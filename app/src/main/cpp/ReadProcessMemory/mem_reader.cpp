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
#include <sys/random.h>
#include <sys/syscall.h>

#include <mutex>
#include <algorithm>
#include <vector>

#ifndef SYS_getrandom
#  if defined(__aarch64__) || defined(__arm__)
#    define SYS_getrandom 278
#  endif
#endif

static inline ssize_t compat_getrandom(void* buf, size_t buflen, unsigned int flags) {
    return (ssize_t)syscall(SYS_getrandom, buf, buflen, flags);
}

#define RTAG "[MR]"

namespace {

constexpr size_t PAGE_SIZE  = 4096;
// 单次 dd 读取的最大字节数, 大于此值会切分
constexpr size_t CHUNK_BYTES = 1 * 1024 * 1024;

const char* DONE_TAG  = "__X_DONE__";

// ── 运行时随机化的临时目录与工具二进制路径 ───────────────────────────
// 目的: 不再让 ACE 通过固定路径 /data/local/tmp/so_dump 与命令行特征
//       (dd if=/proc/PID/mem ...) 直接命中。
struct RuntimePaths {
    std::string dir;        // /data/local/tmp/.X<rand>
    std::string memTmp;     // <dir>/m<rand>
    std::string mapsTmp;    // <dir>/p<rand>
    std::string writeTmp;   // <dir>/w<rand>
    std::string ddBin;      // <dir>/<rand>  (renamed copy of /system/bin/dd)
};

static std::string randHex(size_t bytes) {
    std::vector<uint8_t> r(bytes);
    if (compat_getrandom(r.data(), bytes, 0) != (ssize_t)bytes) {
        // 兜底: 用 PID + nano 时间
        for (size_t i = 0; i < bytes; i++) r[i] = (uint8_t)(rand() & 0xFF);
    }
    static const char hex[] = "0123456789abcdef";
    std::string out(bytes * 2, '0');
    for (size_t i = 0; i < bytes; i++) {
        out[i*2]   = hex[(r[i] >> 4) & 0xF];
        out[i*2+1] = hex[ r[i]       & 0xF];
    }
    return out;
}

static const RuntimePaths& paths() {
    static RuntimePaths p = []{
        RuntimePaths q;
        q.dir       = "/data/local/tmp/." + randHex(4);
        q.memTmp    = q.dir + "/" + randHex(3);
        q.mapsTmp   = q.dir + "/" + randHex(3);
        q.writeTmp  = q.dir + "/" + randHex(3);
        q.ddBin     = q.dir + "/" + randHex(3);
        return q;
    }();
    return p;
}

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
 * 把 /proc/PID/mem 的一段 [addr, addr+size) 读到临时文件并 fread 到 out
 * 要求 size <= CHUNK_BYTES, 地址已 untag
 */
ssize_t readChunk(int pid, uintptr_t addr, size_t size,
                  std::vector<uint8_t>& out, size_t outOffset) {
    uintptr_t pageStart  = addr & ~(PAGE_SIZE - 1);
    size_t    pageOff    = addr - pageStart;
    size_t    pageCount  = (pageOff + size + PAGE_SIZE - 1) / PAGE_SIZE;
    const auto& P = paths();

    char cmd[768];
    // 一次 shell 命令: 清 tmp + (重命名后的) dd 拉数据 + chmod
    // 用变量赋值方式拼接 if/of, 让 ps 中的 cmdline 不那么显眼
    snprintf(cmd, sizeof(cmd),
        "rm -f %s; I=/proc/%d/mem; O=%s; %s i\"f\"=$I o\"f\"=$O bs=%zu skip=%zu count=%zu "
        "conv=noerror,sync 2>/dev/null; chmod 666 %s 2>/dev/null",
        P.memTmp.c_str(), pid, P.memTmp.c_str(), P.ddBin.c_str(),
        PAGE_SIZE, pageStart / PAGE_SIZE, pageCount, P.memTmp.c_str());

    if (!RootShell::I().exec(cmd)) {
        return -1;
    }

    FILE* fp = fopen(P.memTmp.c_str(), "rb");
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

// 一次性初始化: 创建随机 tmp 目录, 把 /system/bin/dd 复制为随机名称
// 这样 ps -A | grep dd 不会再命中我们的进程
static void ensureRuntimeReady() {
    static std::once_flag once;
    std::call_once(once, []{
        const auto& P = paths();
        std::string cmd =
            "mkdir -p " + P.dir + " && chmod 700 " + P.dir + " && "
            "([ -x " + P.ddBin + " ] || (cp /system/bin/dd " + P.ddBin +
            " 2>/dev/null || cp /system/bin/toybox " + P.ddBin + " 2>/dev/null) && "
            "chmod 755 " + P.ddBin + ")";
        RootShell::I().exec(cmd);
    });
}

} // anonymous namespace


namespace MemReader {

// 暴露给同模块其他翻译单元 (如 so_dumper) 的持久 root shell 接口,
// 取代各处 system("su -c ...") / popen("su -c ...") 调用。
bool runRootShell(const std::string& cmd) {
    ensureRuntimeReady();
    return RootShell::I().exec(cmd);
}

bool runRootShellCapture(const std::string& cmd, std::string& out) {
    out.clear();
    ensureRuntimeReady();
    const auto& P = paths();
    std::string capPath = P.dir + "/" + randHex(3);
    std::string wrapped = "(" + cmd + ") > " + capPath +
                         " 2>/dev/null; chmod 666 " + capPath + " 2>/dev/null";
    if (!RootShell::I().exec(wrapped)) return false;
    FILE* fp = fopen(capPath.c_str(), "rb");
    if (!fp) return false;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    fclose(fp);
    unlink(capPath.c_str());
    return true;
}

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

    // 首次准备 tmp 目录 + 重命名 dd
    ensureRuntimeReady();

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

bool readMaps(int pid, std::string& out) {
    out.clear();
    if (pid <= 0) return false;

    // 首次准备 tmp 目录
    ensureRuntimeReady();
    const auto& P = paths();

    char cmd[512];
    // 不再用 cat: 改用 shell 内置 read 循环, ps 列表里看不到 cat 命令.
    // 输出经 printf 写到 mapsTmp, 再由本进程 fopen 读回.
    snprintf(cmd, sizeof(cmd),
        "M=/proc/%d/maps; T=%s; : > $T; while IFS= read -r L; do printf '%%s\\n' \"$L\"; done < $M >> $T 2>/dev/null; chmod 666 $T 2>/dev/null",
        pid, P.mapsTmp.c_str());
    if (!RootShell::I().exec(cmd)) {
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "readMaps: RootShell exec 失败 pid=%d", pid);
        return false;
    }

    FILE* fp = fopen(P.mapsTmp.c_str(), "r");
    if (!fp) {
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "readMaps: fopen tmp 失败 pid=%d errno=%d", pid, errno);
        return false;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        out.append(buf, n);
    }
    fclose(fp);
    if (out.empty()) {
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "readMaps: maps 为空 pid=%d (进程可能已退出或无权限)", pid);
        return false;
    }
    return true;
}

bool findRegion(int pid, uintptr_t address, RegionInfo& info) {
    uintptr_t realAddr = untag(address);

    // 复用 readMaps (内部同样走 shell read 循环, 不再 popen cat)
    std::string maps;
    if (!readMaps(pid, maps)) return false;

    bool found = false;
    size_t pos = 0;
    while (pos < maps.size()) {
        size_t nl = maps.find('\n', pos);
        std::string line = maps.substr(pos,
            (nl == std::string::npos ? maps.size() : nl) - pos);
        pos = (nl == std::string::npos) ? maps.size() : nl + 1;
        if (line.empty()) continue;

        uintptr_t s, e;
        char perms[8], path[512]; path[0] = 0;
        unsigned long off, d1, d2, inode;
        int m = sscanf(line.c_str(), "%lx-%lx %4s %lx %lx:%lx %lu %511[^\n]",
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

// ── base64 编码 (标准字母表, 不换行) ──
static std::string b64_encode(const uint8_t* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t)data[i] << 16 | (uint32_t)data[i+1] << 8 | data[i+2];
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6)  & 0x3F];
        out += tbl[ v        & 0x3F];
        i += 3;
    }
    if (i < len) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

ssize_t writeMemory(int pid, uintptr_t address,
                    const std::vector<uint8_t>& data) {
    if (pid <= 0 || data.empty()) return -1;

    // 写入字节数上限 (单条 shell 命令载荷) — 更大时分段
    // 单条命令的 b64 文本约 4/3 * raw + 开销, 这里 1MB 足够应付绝大多数场景
    const size_t MAX_ONCE = 1 * 1024 * 1024;
    size_t total = data.size();
    uintptr_t realAddr = untag(address);
    if (realAddr != address) {
        __android_log_print(ANDROID_LOG_INFO, RTAG,
            "写入去 tag: 0x%zx -> 0x%zx", (size_t)address, (size_t)realAddr);
    }

    // 准备 tmp 目录 + 重命名 dd
    ensureRuntimeReady();
    const auto& P = paths();

    size_t written = 0;
    while (written < total) {
        size_t chunk = std::min(total - written, MAX_ONCE);
        std::string b64 = b64_encode(data.data() + written, chunk);

        // 构造命令:
        //   1. 用 base64 -d 还原二进制到 writeTmp
        //   2. (重命名后的) dd bs=1 seek=ADDR count=SIZE conv=notrunc 到 /proc/PID/mem
        //      bs=1 慢但可按字节精确定位
        //      把 if/of 用变量赋值方式写, 让 cmdline 中不直接出现 'if=/proc/...'
        std::string cmd;
        cmd.reserve(b64.size() + 256);
        cmd += "echo -n '";
        cmd += b64;
        cmd += "' | base64 -d > ";
        cmd += P.writeTmp;
        cmd += " && chmod 666 ";
        cmd += P.writeTmp;
        cmd += " && I=";
        cmd += P.writeTmp;
        cmd += "; O=/proc/";
        char ibuf[64];
        snprintf(ibuf, sizeof(ibuf), "%d", pid);
        cmd += ibuf;
        cmd += "/mem; ";
        cmd += P.ddBin;
        cmd += " i\"f\"=$I o\"f\"=$O bs=1 seek=";
        snprintf(ibuf, sizeof(ibuf), "%zu", (size_t)(realAddr + written));
        cmd += ibuf;
        cmd += " count=";
        snprintf(ibuf, sizeof(ibuf), "%zu", chunk);
        cmd += ibuf;
        // 关键: 一定要 conv=notrunc, 否则 dd 会截断 /proc/PID/mem (在某些 kernel 会拒绝)
        cmd += " conv=notrunc 2>";
        cmd += P.dir;
        cmd += "/e && ";
        // 成功时打印一个占位, 不成功则由外层 marker 感知不到错误码, 但 dd 的 err 可后续取
        cmd += "echo __WRITE_OK__";

        bool ok = RootShell::I().exec(cmd);
        if (!ok) {
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "写入 shell 失败 pid=%d addr=0x%zx size=%zu written=%zu",
                pid, (size_t)realAddr, chunk, written);
            break;
        }

        written += chunk;
    }

    if (written == 0) {
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "写入 0 字节: pid=%d addr=0x%zx size=%zu",
            pid, (size_t)realAddr, total);
        return -2;
    }

    __android_log_print(ANDROID_LOG_INFO, RTAG,
        "写入 pid=%d addr=0x%zx->0x%zx 请求=%zu 实际=%zu",
        pid, (size_t)address, (size_t)realAddr, total, written);
    return (ssize_t)written;
}

} // namespace MemReader
