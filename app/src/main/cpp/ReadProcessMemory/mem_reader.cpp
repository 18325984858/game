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
#include <dlfcn.h>

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
// AArch64 上高 8 位是高地址标签位 (TBI=Top Byte Ignore, MTE 也只用高 4 位).
// scudo 分配器返回的堆指针常见形式为 0xB4xxxxxxxxxxxxxx 之类，
// 例如 0xb400007a7b722190 → 实际虚地址 0x00007a7b722190.
// /proc/PID/mem 的 lseek 偏移是原始虚地址，不会自动处理 TBI，
// 所以必须在调 dd skip= 之前先抹掉高字节; 否则偏移远超过
// 进程虚拟地址空间 (48-bit user-VA 上限 0x0000FFFFFFFFFFFF), dd 会立即
// 读到 EOF 返回 0 字节，表现为“读取失败 (无数据)”。
inline uintptr_t untag(uintptr_t addr) {
    return addr & 0x00FFFFFFFFFFFFFFULL;
}

/**
 * 持久化 root shell: 一次 `su`, 多次发命令, 用唯一 marker 分帧.
 * 线程安全: 内部加锁序列化.
 *
 * 超时/授权设计说明:
 *   - spawnLocked() 后会发一条 "echo PING" + DONE marker, 取得
 *     ping 超时 (1.5s, 可调) 作为 su 授权探测. APatch/KernelSU 在
 *     未授权时 su 会永久挂起不输出任何东西, ping 超时即认为
 *     "被拒绝", 记住 authDenied_ = true, 后续调用 fast-fail 返回.
 *   - 调用者可用 isAuthDenied() 查询状态 / 在 UI 展示提示.
 *   - resetAuth() 在用户手动授权后调用 (现阶段 UI 还未提供入口,
 *     用户只需重启 app 即可).
 */
class RootShell {
public:
    static RootShell& I() {
        static RootShell s;
        return s;
    }

    bool isAuthDenied() {
        std::lock_guard<std::mutex> g(mtx_);
        return authDenied_;
    }

    void resetAuth() {
        std::lock_guard<std::mutex> g(mtx_);
        authDenied_ = false;
        killLocked();
    }

    /**
     * 执行一段 shell 命令, 等待其完成 (通过 marker).
     * stdout/stderr 输出会被丢弃 (除非重定向到文件内再读).
     */
    bool exec(const std::string& cmd) {
        std::lock_guard<std::mutex> g(mtx_);
        // 授权被拒 → fast-fail, 不再费 15s 去重试 (调用者会连环出现
        // 在进程枚举/内存读取/按钮点击上)
        if (authDenied_) {
            __android_log_print(ANDROID_LOG_WARN, RTAG,
                "跳过 root 调用: su 未获授权 (请去 APatch/KernelSU 为本 app 授权)");
            return false;
        }
        if (!ensureAliveLocked()) return false;
        return execLocked(cmd, /*timeoutMs=*/15000);
    }

    ~RootShell() {
        killLocked();
    }

private:
    std::mutex mtx_;
    pid_t pid_ = -1;
    int   wfd_ = -1;    // 写给 su 的 stdin
    int   rfd_ = -1;    // 读 su 的 stdout
    bool  authDenied_ = false;

    // 在锁内发一条命令并等 DONE marker, timeoutMs 为总超时.
    bool execLocked(const std::string& cmd, int timeoutMs) {
        std::string line = cmd + "\n" + "echo " + DONE_TAG + "\n";
        ssize_t w = write(wfd_, line.data(), line.size());
        if (w != (ssize_t)line.size()) {
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "write 到 su stdin 失败 w=%zd errno=%d", w, errno);
            killLocked();
            return false;
        }

        char buf[1024];
        std::string acc;
        const int sliceMs = 10;
        int iters = (timeoutMs + sliceMs - 1) / sliceMs;
        for (int i = 0; i < iters; i++) {
            ssize_t n = read(rfd_, buf, sizeof(buf));
            if (n > 0) {
                acc.append(buf, n);
                if (acc.find(DONE_TAG) != std::string::npos) return true;
            } else if (n == 0) {
                // su 进程退出 (execlp 失败 / 未授权被杀 / 正常退出)
                killLocked();
                return false;
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(sliceMs * 1000);
                    continue;
                }
                killLocked();
                return false;
            }
        }
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "等待 DONE marker 超时(%dms), 命令=%s", timeoutMs, cmd.c_str());
        killLocked();
        return false;
    }

    bool ensureAliveLocked() {
        if (pid_ > 0) {
            int status = 0;
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == 0) return true;
            killLocked();
        }
        if (!spawnLocked()) return false;

        // 刚 fork 出的 su, 用 1.5s ping 探测是否已授权 / 是否能响应.
        // 未授权时 APatch/KernelSU 会挂起 su 不输出, ping 肯定超时.
        if (!execLocked("echo P", /*timeoutMs=*/1500)) {
            authDenied_ = true;
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "✘ su ping 失败: 该 app 未获得 root 授权 "
                "(请打开 APatch/KernelSU manager → 超级用户 → 为本 app 授权 → 重启 app)");
            return false;
        }
        return true;
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
            // 关键: 必须 -G 3009 让 su 进程的 supplementary group 含 readproc.
            // Android 13+ 默认 procfs 挂载选项: gid=3009,hidepid=invisible (=2).
            // hidepid=2 时, 即使 uid=0 的 root, 若进程的 任一 group 不包含 3009(readproc),
            // 也无法读取其它 uid 的 /proc/PID/{maps,mem,status,...} (返回 EACCES).
            //
            // 用 -G(--supp-group) 而非 -g(--group): 保留 primary gid=0,
            // 只把 3009 加入 supplementary groups. 这样两边都得:
            //   * 能读 /data/adb/ksu/.tmp/superkey (root:root 0600) 之类 root-only 文件
            //   * 能读 /proc/<other_pid>/{maps,mem} (procfs hidepid 检查会遭到 readproc)
            execlp("su", "su", "-G", "3009", (char*)nullptr);
            // 退化方案: 部分 root 实现可能不支持 -G, 直接拉裸 su.
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

        // 早死检测: 50ms 后看 child 是否已经 _exit (execlp("su") 不存在 / 立即被拒).
        // su 正常情况下会一直等 stdin, 不会自己退出.
        usleep(50 * 1000);
        int status = 0;
        pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "su 子进程立即退出 code=%d (su 未安装 / execlp 失败 / 被 LSM 拒绝)", code);
            close(wfd_); wfd_ = -1;
            close(rfd_); rfd_ = -1;
            pid_ = -1;
            return false;
        }

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
 * 持久化跨进程内存读 daemon 客户端: 负责跟 libpvrhelper.so 经 root su
 * 启动并保持的 stdin/stdout pipe 通信.
 *
 * 替代了上一版用 `RootShell exec helper > /data/local/tmp/<rand>` + fopen 的方案.
 * 反检测收益:
 *   - helper 进程常驻, 只在首次 readChunk 时 fork 一次, 不再每次读都 fork+exec
 *   - cmdline / argv 在 helper 内部被 prctl + memset 抹成 "[kworker/u16:0H]",
 *     /proc/<helper_pid>/{comm,cmdline} 不再含目标 PID/大十六进制地址
 *   - 不再写任何临时文件, /data/local/tmp/<rand>/<rand> 全程不出现
 *   - helper ELF 已 strip, strings 扫不到 "process_vm_readv"/"usage" 等关键字
 *
 * 协议见 PvrHelper/pvr_helper.c 头注释.
 */
class RootHelper {
public:
    static RootHelper& I() { static RootHelper s; return s; }

    /**
     * 读 [addr, addr+size) 到 dst (容量必须 ≥ size). 返回实际读到的字节数.
     *   > 0  : 至少读到一些 (可能 = size 也可能 < size)
     *   = 0  : process_vm_readv 直接失败 (target 已死/addr 不在合法 VMA),
     *          调用方应停止本次循环
     *   < 0  : daemon 通信错误 (pipe 死 / 协议错), 已 killLocked, 下次自动重启
     *
     * size 上限受 helper 内 BUF_BYTES = 1MB 限制, client (readMemory) 已分块.
     */
    ssize_t readChunk(int targetPid, uintptr_t addr, size_t size, uint8_t* dst) {
        std::lock_guard<std::mutex> g(mtx_);
        if (size == 0) return 0;
        if (size > 1024 * 1024) size = 1024 * 1024;

        if (!ensureAliveLocked()) return -1;

        char cmd[160];
        int cl = snprintf(cmd, sizeof(cmd),
                          "R %d 0x%zx %zu\n",
                          targetPid, (size_t)addr, size);
        if (cl <= 0 || writeAllLocked(wfd_, cmd, (size_t)cl) != 0) {
            __android_log_print(ANDROID_LOG_WARN, RTAG,
                "RootHelper write 命令失败, 重启 daemon");
            killLocked();
            return -1;
        }

        char hdr[64];
        int hl = readLineLocked(rfd_, hdr, (int)sizeof(hdr), /*timeoutMs=*/5000);
        if (hl < 0) {
            __android_log_print(ANDROID_LOG_WARN, RTAG,
                "RootHelper 读响应头超时/EOF, 重启 daemon");
            killLocked();
            return -1;
        }
        if (hl < 2) return -1;
        if (hdr[0] == 'E') {
            // helper 调 process_vm_readv 失败 (target 退出 / addr 不在 VMA)
            return 0;
        }
        if (hdr[0] != 'K') {
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "RootHelper 协议错: %s", hdr);
            killLocked();
            return -1;
        }
        size_t n = (size_t)strtoul(hdr + 2, nullptr, 10);
        if (n == 0 || n > size) {
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "RootHelper 长度异常 want=%zu got=%zu", size, n);
            killLocked();
            return -1;
        }
        if (readFullLocked(rfd_, dst, n, /*timeoutMs=*/8000) != 0) {
            __android_log_print(ANDROID_LOG_WARN, RTAG,
                "RootHelper 读 payload 超时 want=%zu", n);
            killLocked();
            return -1;
        }
        return (ssize_t)n;
    }

    void resetLocked() {
        std::lock_guard<std::mutex> g(mtx_);
        killLocked();
    }

    ~RootHelper() { killLocked(); }

private:
    std::mutex mtx_;
    pid_t pid_ = -1;
    int   wfd_ = -1;
    int   rfd_ = -1;

    bool ensureAliveLocked() {
        if (pid_ > 0) {
            int status = 0;
            pid_t r = waitpid(pid_, &status, WNOHANG);
            if (r == 0) return true;
            killLocked();
        }
        return spawnLocked();
    }

    bool spawnLocked() {
        // helper 二进制路径 (P.ddBin) 由 ensureRuntimeReady() 保证已就位.
        const auto& P = paths();

        int in[2]  = {-1, -1};
        int out[2] = {-1, -1};
        if (pipe(in) < 0 || pipe(out) < 0) {
            if (in[0] >= 0) { close(in[0]); close(in[1]); }
            if (out[0] >= 0) { close(out[0]); close(out[1]); }
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);
            return false;
        }

        if (pid == 0) {
            // child: stdin/stdout 接管, stderr 静默 (helper 内部还会再 dup 一次到 /dev/null)
            dup2(in[0],  STDIN_FILENO);
            dup2(out[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                if (devnull > 2) close(devnull);
            }
            close(in[0]); close(in[1]); close(out[0]); close(out[1]);

            // su -G 3009 -c <helperPath>: helper 进程是 su 的 child, 继承
            // primary gid=0 + supplementary 3009. exec 后由 helper 进程取代,
            // /proc/<pid>/comm 由 helper 内 prctl 改名.
            execlp("su", "su", "-G", "3009", "-c", P.ddBin.c_str(), (char*)nullptr);
            // 退化: 不支持 -G
            execlp("su", "su", "-c", P.ddBin.c_str(), (char*)nullptr);
            _exit(127);
        }

        close(in[0]); close(out[1]);
        wfd_ = in[1];
        rfd_ = out[0];
        pid_ = pid;

        // 读端 NONBLOCK, readLineLocked/readFullLocked 走超时轮询
        int fl = fcntl(rfd_, F_GETFL, 0);
        if (fl >= 0) fcntl(rfd_, F_SETFL, fl | O_NONBLOCK);

        // 早死检测: 50ms 内 child 退出 = su/helper 启动失败
        usleep(50 * 1000);
        int status = 0;
        pid_t r = waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "RootHelper 子进程立即退出 code=%d (su 拒授权 / helper 不存在)", code);
            close(wfd_); wfd_ = -1;
            close(rfd_); rfd_ = -1;
            pid_ = -1;
            return false;
        }
        __android_log_print(ANDROID_LOG_INFO, RTAG,
            "RootHelper daemon 启动 pid=%d", pid);
        return true;
    }

    void killLocked() {
        if (wfd_ >= 0) { close(wfd_); wfd_ = -1; }
        if (rfd_ >= 0) { close(rfd_); rfd_ = -1; }
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            int st;
            waitpid(pid_, &st, 0);
            pid_ = -1;
        }
    }

    // 阻塞写, 处理短写
    static int writeAllLocked(int fd, const void* buf, size_t n) {
        const char* b = (const char*)buf;
        while (n > 0) {
            ssize_t w = write(fd, b, n);
            if (w > 0) { b += w; n -= (size_t)w; continue; }
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        return 0;
    }

    // 带超时的读, 处理 EAGAIN (启动时把 rfd_ 设为 NONBLOCK)
    static int readFullLocked(int fd, void* buf, size_t n, int timeoutMs) {
        char* b = (char*)buf;
        const int sliceMs = 5;
        int iters = (timeoutMs + sliceMs - 1) / sliceMs;
        while (n > 0 && iters > 0) {
            ssize_t r = read(fd, b, n);
            if (r > 0) { b += r; n -= (size_t)r; continue; }
            if (r == 0) return -1;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(sliceMs * 1000);
                iters--;
                continue;
            }
            if (errno == EINTR) continue;
            return -1;
        }
        return n == 0 ? 0 : -1;
    }

    // 读一行 (\n 终止), 写入 buf (不含 \n, 末尾置 \0). 返回字符数 (>=0) 或 -1
    static int readLineLocked(int fd, char* buf, int cap, int timeoutMs) {
        int got = 0;
        const int sliceMs = 5;
        int iters = (timeoutMs + sliceMs - 1) / sliceMs;
        while (got < cap - 1 && iters > 0) {
            char c;
            ssize_t r = read(fd, &c, 1);
            if (r == 1) {
                if (c == '\n') { buf[got] = '\0'; return got; }
                buf[got++] = c;
                continue;
            }
            if (r == 0) return -1;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(sliceMs * 1000);
                iters--;
                continue;
            }
            if (errno == EINTR) continue;
            return -1;
        }
        return -1;
    }

    RootHelper() = default;
};

/**
 * 跨进程读取 [addr, addr+size) 到 out 的指定偏移.
 *
 * 当前实现: 全程走持久 root daemon (RootHelper). 见 RootHelper / pvr_helper.c
 * 反检测优化:
 *   - daemon 进程 cmdline/comm 被 prctl + memset 抹成 [kworker/u16:0H]
 *   - 不再写任何临时文件 (上一版的 dd /proc/PID/mem 与短生命周期 helper 都已淘汰)
 *   - 字节通过 stdin/stdout pipe 直接流回, 文件系统层无痕
 */
ssize_t readChunk(int pid, uintptr_t addr, size_t size,
                  std::vector<uint8_t>& out, size_t outOffset) {
    ssize_t r = RootHelper::I().readChunk(pid, addr, size, out.data() + outOffset);
    if (r < 0) {
        // daemon 通信错 (已 killLocked, 下次自动重启)
        __android_log_print(ANDROID_LOG_ERROR, RTAG,
            "RootHelper readChunk 通信失败 pid=%d addr=0x%zx size=%zu",
            pid, (size_t)addr, size);
        return -1;
    }
    if (r == 0) {
        // helper 端 process_vm_readv 失败: target 已退出 / addr 不在合法 VMA
        __android_log_print(ANDROID_LOG_WARN, RTAG,
            "process_vm_readv 0 字节 pid=%d addr=0x%zx (target退出 或 addr 不在合法 VMA)",
            pid, (size_t)addr);
        return -2;
    }
    return r;
}

// 一次性初始化: 创建随机 tmp 目录, 把 libpvrhelper.so 复制到随机命名路径作为 helper.
// libpvrhelper.so 是我们打包在 APK 内的 ARM64 ELF 可执行文件, 用 process_vm_readv
// 跨进程读内存. 之所以借用 dd-style "ddBin" 字段名是历史原因, 路径用途已变.
static void ensureRuntimeReady() {
    static std::once_flag once;
    std::call_once(once, []{
        const auto& P = paths();

        // 通过 dladdr 找到本 .so 所在目录 = nativeLibraryDir
        // 例如 /data/app/~~xxxxx/com.example.dobbyproject-zzz/lib/arm64
        Dl_info info{};
        std::string libDir;
        if (dladdr((void*)&ensureRuntimeReady, &info) && info.dli_fname) {
            std::string fn = info.dli_fname;
            auto slash = fn.find_last_of('/');
            if (slash != std::string::npos) libDir = fn.substr(0, slash);
        }
        if (libDir.empty()) {
            __android_log_print(ANDROID_LOG_ERROR, RTAG,
                "ensureRuntimeReady: dladdr 拿不到 nativeLibraryDir, helper 无法部署");
            return;
        }
        std::string helperSrc = libDir + "/libpvrhelper.so";

        // 注意: 目录权限必须给 others 留一个 +x (搜索位), 否则非 root 的 app 进程
        // 无法 fopen 目录下的临时文件 (即使文件本身已 chmod 666). 这里用 711:
        // others 可进入并按名访问文件, 但无法 ls 列出目录 (无 +r),
        // 仍然能避免反作弊扫 `/data/local/tmp/.*` 时拿到完整文件名清单.
        // 注意: 这里**不能**调 MemReader::runRootShellCapture, 因为后者也会
        //       调 ensureRuntimeReady(), 在 std::call_once 同线程内重入会死锁
        //       (C++ once_flag 同线程递归是未定义/deadlock). 直接走 RootShell.
        std::string capPath = P.dir + "/" + randHex(3);
        std::string cmd =
            "mkdir -p " + P.dir + " && chmod 711 " + P.dir + " && "
            "([ -x " + P.ddBin + " ] || (cp " + helperSrc + " " + P.ddBin +
            " 2>/dev/null && chmod 755 " + P.ddBin + ")) && ls -l " + P.ddBin +
            " > " + capPath + " 2>&1; chmod 666 " + capPath + " 2>/dev/null";
        bool ok = RootShell::I().exec(cmd);
        std::string out;
        if (ok) {
            FILE* fp = fopen(capPath.c_str(), "rb");
            if (fp) {
                char buf[1024]; size_t n;
                while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
                fclose(fp);
            }
            unlink(capPath.c_str());
        }
        __android_log_print(ANDROID_LOG_INFO, RTAG,
            "ensureRuntimeReady ok=%d helperSrc=%s helperDst=%s result=%s",
            (int)ok, helperSrc.c_str(), P.ddBin.c_str(), out.c_str());
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

bool isRootAuthDenied() {
    return RootShell::I().isAuthDenied();
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
    // shell 内置 `while IFS= read -r L; do printf ...; done` 对几万行 maps
    // (UE 游戏常见 8k-30k 行) 性能极差: 每行做一次 read+printf, 总耗时
    // 经测会 >15s 直接撞 RootShell 超时. 改用 cat 一次性 dump.
    // 命令行用变量赋值拼接 ("C=cat; $C $M") 让 ps 中 cmdline 不直接出现
    // "cat /proc/PID/maps" 这种过强的特征字符串.
    snprintf(cmd, sizeof(cmd),
        "C=ca\"\"t; M=/proc/%d/maps; T=%s; $C $M > $T 2>/dev/null; chmod 666 $T 2>/dev/null",
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
