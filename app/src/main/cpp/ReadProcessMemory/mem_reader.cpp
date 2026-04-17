/**
 * @file    mem_reader.cpp
 * @brief   读取任意进程内存 (同 SoDumper 套路: su + dd 到 /data/local/tmp 再 fread)
 *          - 支持 aarch64 MTE/TBI 指针 tag (自动清 tag)
 *          - 持久化 root shell (RootShell), 避免每次 popen 开销
 *          - 按页对齐 + conv=noerror,sync 容错
 *          - 大块 (>CHUNK_BYTES) 自动分段拼接
 *          - 提供地址归属查询 / 字节模式搜索
 */
#include "mem_reader.h"
#include "../Log/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <android/log.h>

#include <mutex>
#include <algorithm>

#define RTAG "[MemReader]"

namespace {

constexpr size_t PAGE_SIZE  = 4096;
// 单次 dd 读取的最大字节数, 大于此值会切分
constexpr size_t CHUNK_BYTES = 1 * 1024 * 1024;

const char* TMP_DIR   = "/data/local/tmp/so_dump";
const char* TMP_FILE  = "/data/local/tmp/so_dump/_memread_tmp";
const char* DONE_TAG  = "__MEMREAD_DONE__";

// ── 去除 aarch64 MTE/TBI 指针 tag ──
inline uintptr_t untag(uintptr_t addr) {
    return addr & 0x00FFFFFFFFFFFFFFULL;
}

/**
 * 持久化 root shell: 一次 `su`, 多次发命令, 用唯一 marker 分帧.
 * 线程安全: 内部加锁序列化.
 */
class RootShell {
public:
    static RootShell& I() {
        static RootShell s;
        return s;
    }

    /**
     * 执行一段 shell 命令, 等待其完成 (通过 marker).
     * stdout/stderr 输出会被丢弃 (除非重定向到文件内再读).
     */
    bool exec(const std::string& cmd) {
        std::lock_guard<std::mutex> g(mtx_);
        if (!ensureAliveLocked()) return false;

        std::string line = cmd + "\n" + "echo " + DONE_TAG + "\n";
        ssize_t w = write(wfd_, line.data(), line.size());
        if (w != (ssize_t)line.size()) {
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "write 到 su stdin 失败 w=%zd errno=%d", w, errno);
            killLocked();
            return false;
        }

        // 读直到收到 DONE marker
        char buf[1024];
        std::string acc;
        size_t tagLen = strlen(DONE_TAG);
        // 超时防护: 最多 15s 等一条命令 (dd 大块也够用)
        for (int iter = 0; iter < 1500; iter++) {
            ssize_t n = read(rfd_, buf, sizeof(buf));
            if (n > 0) {
                acc.append(buf, n);
                if (acc.find(DONE_TAG) != std::string::npos) {
                    return true;
                }
            } else if (n == 0) {
                // shell 已退出
                killLocked();
                return false;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(10 * 1000);
                    continue;
                }
                killLocked();
                return false;
            }
        }
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "等待 DONE marker 超时, 命令=%s", cmd.c_str());
        killLocked();
        return false;
    }

    ~RootShell() {
        killLocked();
    }

private:
    std::mutex mtx_;
    pid_t pid_ = -1;
    int   wfd_ = -1;    // 写给 su 的 stdin
    int   rfd_ = -1;    // 读 su 的 stdout

    bool ensureAliveLocked() {
        if (pid_ > 0) {
            // 检查还活着
            int status = 0;
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == 0) return true;
            // 已退出, 清理
            killLocked();
        }
        return spawnLocked();
    }

    bool spawnLocked() {
        int in[2]  = {-1, -1};
        int out[2] = {-1, -1};
        if (pipe(in) < 0 || pipe(out) < 0) {
            __android_log_print(ANDROID_LOG_ERROR, RTAG, "pipe 失败 errno=%d", errno);
            if (in[0] >= 0) { close(in[0]); close(in[1]); }
            if (out[0] >= 0) { close(out[0]); close(out[1]); }
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            __android_log_print(ANDROID_LOG_ERROR, RTAG, "fork 失败 errno=%d", errno);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            return false;
        }

        if (pid == 0) {
            // child
            dup2(in[0],  STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            dup2(out[1], STDERR_FILENO);
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            execlp("su", "su", (char*)nullptr);
            _exit(127);
        }

        // parent
        close(in[0]); close(out[1]);
        wfd_ = in[1];
        rfd_ = out[0];
        pid_ = pid;

        // 设为非阻塞读
        int fl = fcntl(rfd_, F_GETFL, 0);
        if (fl >= 0) fcntl(rfd_, F_SETFL, fl | O_NONBLOCK);

        __android_log_print(ANDROID_LOG_INFO, RTAG, "持久 root shell 启动 pid=%d", pid);
        return true;
    }

    void killLocked() {
        if (wfd_ >= 0) { close(wfd_); wfd_ = -1; }
        if (rfd_ >= 0) { close(rfd_); rfd_ = -1; }
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            int status;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
    }

    RootShell() { spawnLocked(); }
};

/**
 * 把 /proc/PID/mem 的一段 [addr, addr+size) 读到 TMP_FILE 并 fread 到 out
 * 要求 size <= CHUNK_BYTES, 地址已 untag
 */
ssize_t readChunk(int pid, uintptr_t addr, size_t size,
                  std::vector<uint8_t>& out, size_t outOffset) {
    uintptr_t pageStart  = addr & ~(PAGE_SIZE - 1);
    size_t    pageOff    = addr - pageStart;
    size_t    pageCount  = (pageOff + size + PAGE_SIZE - 1) / PAGE_SIZE;

    char cmd[512];
    // 一次 shell 命令: 清 tmp + dd 拉数据 + chmod
    snprintf(cmd, sizeof(cmd),
        "rm -f %s; dd if=/proc/%d/mem of=%s bs=%zu skip=%zu count=%zu "
        "conv=noerror,sync 2>/dev/null; chmod 666 %s 2>/dev/null",
        TMP_FILE, pid, TMP_FILE, PAGE_SIZE,
        pageStart / PAGE_SIZE, pageCount, TMP_FILE);

    if (!RootShell::I().exec(cmd)) {
        return -1;
    }

    FILE* fp = fopen(TMP_FILE, "rb");
    if (!fp) {
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "fopen tmp 失败 errno=%d %s", errno, strerror(errno));
        return -2;
    }
    if (pageOff > 0) fseek(fp, (long)pageOff, SEEK_SET);
    size_t got = fread(out.data() + outOffset, 1, size, fp);
    fclose(fp);
    return (ssize_t)got;
}

} // anonymous namespace


namespace MemReader {

ssize_t readMemory(int pid, uintptr_t address, size_t size,
                   std::vector<uint8_t>& out) {
    out.clear();
    if (pid <= 0 || size == 0) return -1;

    // 单次上限保护 (可按需放大)
    const size_t HARD_LIMIT = 64 * 1024 * 1024; // 64 MB
    if (size > HARD_LIMIT) size = HARD_LIMIT;

    uintptr_t realAddr = untag(address);
    if (realAddr != address) {
        __android_log_print(ANDROID_LOG_INFO, RTAG,
            "去 tag: 0x%zx -> 0x%zx", (size_t)address, (size_t)realAddr);
    }

    // 首次准备 tmp 目录
    static std::once_flag s_mkdir;
    std::call_once(s_mkdir, []{
        RootShell::I().exec(
            std::string("mkdir -p ") + TMP_DIR + " && chmod 777 " + TMP_DIR);
    });

    out.resize(size);
    size_t totalGot = 0;
    uintptr_t cur = realAddr;
    size_t left = size;
    while (left > 0) {
        size_t chunk = std::min(left, CHUNK_BYTES);
        ssize_t got = readChunk(pid, cur, chunk, out, totalGot);
        if (got <= 0) {
            // 出错或无数据, 若已拿到部分则截断
            break;
        }
        totalGot += (size_t)got;
        cur      += (uintptr_t)got;
        left     -= (size_t)got;
        if ((size_t)got < chunk) {
            // 段结束或不可读页填充 0 但短读, 继续下一轮页地址仍递增
            // (conv=sync 补 0, 一般与 chunk 相等; <chunk 说明后续不可读)
            break;
        }
    }

    if (totalGot == 0) {
        out.clear();
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "读取失败: pid=%d addr=0x%zx->0x%zx size=%zu",
            pid, (size_t)address, (size_t)realAddr, size);
        return -3;
    }
    if (totalGot < size) out.resize(totalGot);

    __android_log_print(ANDROID_LOG_INFO, RTAG,
        "读取 pid=%d addr=0x%zx->0x%zx 请求=%zu 实际=%zu",
        pid, (size_t)address, (size_t)realAddr, size, totalGot);
    return (ssize_t)totalGot;
}

bool findRegion(int pid, uintptr_t address, RegionInfo& info) {
    uintptr_t realAddr = untag(address);
    char mapsPath[64];
    snprintf(mapsPath, sizeof(mapsPath), "/proc/%d/maps", pid);

    // 读 maps 可能需要 root (别人的进程), 直接通过持久 shell cat 到 tmp
    const char* mapsTmp = "/data/local/tmp/so_dump/_maps_tmp";
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "cat %s 2>/dev/null > %s; chmod 666 %s 2>/dev/null",
        mapsPath, mapsTmp, mapsTmp);
    if (!RootShell::I().exec(cmd)) return false;

    FILE* fp = fopen(mapsTmp, "r");
    if (!fp) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        uintptr_t s, e;
        char perms[8], path[512]; path[0] = 0;
        unsigned long off, d1, d2, inode;
        int m = sscanf(line, "%lx-%lx %4s %lx %lx:%lx %lu %511[^\n]",
                       &s, &e, perms, &off, &d1, &d2, &inode, path);
        if (m < 7) continue;
        if (realAddr >= s && realAddr < e) {
            info.baseAddr = s;
            info.endAddr  = e;
            info.perms    = perms;
            std::string pathStr = path;
            while (!pathStr.empty() && (pathStr.front() == ' ' || pathStr.front() == '\t'))
                pathStr.erase(pathStr.begin());
            info.path = pathStr;
            if (pathStr.empty())            info.name = "<anon>";
            else if (pathStr[0] == '[')     info.name = pathStr;
            else if (pathStr[0] == '/') {
                size_t sl = pathStr.rfind('/');
                info.name = (sl != std::string::npos) ? pathStr.substr(sl+1) : pathStr;
            } else                          info.name = pathStr;
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

std::vector<uintptr_t> searchPattern(int pid,
                                     uintptr_t rangeStart,
                                     uintptr_t rangeEnd,
                                     const std::vector<uint8_t>& pattern,
                                     const std::vector<uint8_t>& mask,
                                     size_t maxHits) {
    std::vector<uintptr_t> hits;
    if (pattern.empty() || pattern.size() != mask.size()) return hits;
    rangeStart = untag(rangeStart);
    rangeEnd   = untag(rangeEnd);
    if (rangeEnd <= rangeStart) return hits;

    // 以 1MB 为粒度分块读取; 相邻块保留 (pattern.size()-1) 字节重叠以处理跨边界命中
    const size_t WINDOW = CHUNK_BYTES;
    const size_t PATN   = pattern.size();
    std::vector<uint8_t> buf(WINDOW + PATN);
    size_t tailKeep = 0;
    uintptr_t cur = rangeStart;

    while (cur < rangeEnd && hits.size() < maxHits) {
        size_t want = std::min<size_t>(WINDOW, rangeEnd - cur);
        std::vector<uint8_t> chunk;
        ssize_t got = readMemory(pid, cur, want, chunk);
        if (got <= 0) {
            // 当前范围不可读, 跳过一页继续
            cur = (cur + PAGE_SIZE) & ~(PAGE_SIZE - 1);
            tailKeep = 0;
            continue;
        }

        // 合并上一块末尾 tailKeep 字节 (处理跨 chunk 边界)
        std::vector<uint8_t> scan;
        if (tailKeep) {
            scan.insert(scan.end(), buf.begin(), buf.begin() + tailKeep);
        }
        scan.insert(scan.end(), chunk.begin(), chunk.end());

        // 朴素搜索 (chunk 内数据不大, 且带掩码不好做 KMP)
        if (scan.size() >= PATN) {
            for (size_t i = 0; i + PATN <= scan.size(); i++) {
                bool ok = true;
                for (size_t j = 0; j < PATN; j++) {
                    uint8_t m = mask[j];
                    if (m && ((scan[i + j] ^ pattern[j]) & m)) { ok = false; break; }
                }
                if (ok) {
                    // 对应在 process 中的地址 = cur - tailKeep + i
                    uintptr_t hit = cur - tailKeep + i;
                    hits.push_back(hit);
                    if (hits.size() >= maxHits) break;
                }
            }
        }

        // 保存下一轮的 tailKeep
        tailKeep = std::min<size_t>(scan.size(), PATN - 1);
        if (tailKeep) {
            buf.assign(scan.end() - tailKeep, scan.end());
        }
        cur += (uintptr_t)got;
    }

    __android_log_print(ANDROID_LOG_INFO, RTAG,
        "搜索完成 pid=%d 范围=[0x%zx,0x%zx) 命中=%zu",
        pid, (size_t)rangeStart, (size_t)rangeEnd, hits.size());
    return hits;
}

} // namespace MemReader
