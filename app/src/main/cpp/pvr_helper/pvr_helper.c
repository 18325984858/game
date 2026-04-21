/**
 * pvr_helper - 跨进程内存读取常驻 daemon, 由 RootHelper 通过 root 持久 pipe 启动.
 *
 * 设计原则 (反检测优化):
 *   1) 进程伪装: prctl(PR_SET_NAME) + memset(argv) 把 /proc/<pid>/{comm,cmdline}
 *      改成与内核线程混淆的样子, 不再含目标 PID / 大十六进制地址.
 *   2) 无临时文件: 不再写 /data/local/tmp/<rand>, 改为 stdin 收命令,
 *      stdout 流式回字节. 文件系统层完全无痕.
 *   3) 无可读字符串: 不调用 fprintf/strerror/usage, 协议错误只回单字符 + errno.
 *      配合 CMake `-Wl,-s` strip ELF, 静态 strings 扫不到 process_vm_readv 上下文.
 *
 * 协议 (line-based 请求 / binary 响应):
 *   client → helper:  "R <pid_dec> <addr_hex_or_dec> <size_dec>\n"   (process_vm_readv)
 *                  或 "M <pid_dec> <addr_hex_or_dec> <size_dec>\n"   (pread64 /proc/<pid>/mem)
 *   helper → client:  "K <n>\n" + <n> raw bytes        (成功)
 *                  或 "E <errno>\n"                    (读取失败)
 *   client 关闭 stdin → helper 自然 EOF 退出.
 *
 * 单次 size 上限 = 1MB (内部缓冲), client 自行分块.
 *
 * 跨进程读路径:
 *   R - process_vm_readv(syscall 270): 主路径, 不 attach, 不写 TracerPid,
 *       目标进程的 /proc/self/{maps,status} 完全无变化.
 *   M - pread64('/proc/<pid>/mem'): 备用路径, 当某些 anti-cheat (Tencent ACE)
 *       hook process_vm_readv 让 R-X 段静默返回全 0 时, /proc/<pid>/mem 的
 *       pread64 通常未被 hook, 能拿到真实代码段。fd 按 pid 缓存。
 *       由 client 显式选择: 调用方决定何时切到这条路径。 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <sys/prctl.h>

#define BUF_BYTES (1024 * 1024)

/* aarch64 process_vm_readv 系统调用号 = 270.
 * 直接走 syscall(SYS_process_vm_readv,...) 而非 libc 包装函数,
 * 避免 ELF .dynsym / .dynstr 中出现 "process_vm_readv" 字符串
 * 给静态 strings 扫描留特征. */
#ifndef __NR_process_vm_readv
#define __NR_process_vm_readv 270
#endif

static inline ssize_t pvr(pid_t pid,
                          const struct iovec* lv, unsigned long ln,
                          const struct iovec* rv, unsigned long rn,
                          unsigned long flags) {
    return (ssize_t)syscall(__NR_process_vm_readv, pid, lv, ln, rv, rn, flags);
}

/* /proc/<pid>/mem fd 缓存: 避免每次请求都 open 一次.
 * 大多数会话只 dump 一个目标进程, 一个槽位足够。
 * pid 变化时关闭旧 fd 后重 open。 */
static int g_mem_pid = -1;
static int g_mem_fd  = -1;

static int open_proc_mem(pid_t pid) {
    if (g_mem_pid == (int)pid && g_mem_fd >= 0) return g_mem_fd;
    if (g_mem_fd >= 0) { close(g_mem_fd); g_mem_fd = -1; }
    /* 路径运行时拼接 (避免静态 strings 扫到 "/proc/%d/mem" 字面量) */
    static const unsigned char enc_pfx[] = {
        /* "/proc/" 每字节 XOR 0x5A */
        0x75,0x2A,0x28,0x39,0x39,0x75,0x5A
    };
    static const unsigned char enc_sfx[] = {
        /* "/mem\0" 每字节 XOR 0x5A */
        0x75,0x37,0x3F,0x29,0x5A
    };
    char pfx[sizeof(enc_pfx)], sfx[sizeof(enc_sfx)];
    for (size_t i = 0; i < sizeof(enc_pfx); i++) pfx[i] = (char)(enc_pfx[i] ^ 0x5A);
    for (size_t i = 0; i < sizeof(enc_sfx); i++) sfx[i] = (char)(enc_sfx[i] ^ 0x5A);
    char path[64];
    int n = snprintf(path, sizeof(path), "%s%d%s", pfx, (int)pid, sfx);
    if (n <= 0 || n >= (int)sizeof(path)) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    g_mem_pid = (int)pid;
    g_mem_fd  = fd;
    return fd;
}

/* fallback: 通过 /proc/<pid>/mem + pread64 读取.
 * 返回成功读取字节数, <=0 表示失败。 */
static ssize_t mem_pread(pid_t pid, uintptr_t addr, void* dst, size_t sz) {
    int fd = open_proc_mem(pid);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < sz) {
        ssize_t r = pread64(fd, (char*)dst + done, sz - done, (off_t)(addr + done));
        if (r > 0) { done += (size_t)r; continue; }
        if (r < 0 && errno == EINTR) continue;
        if (done == 0) return r;   /* 一字节没读到, 报告错误 */
        break;                     /* 读了一部分, 返回已读量 */
    }
    return (ssize_t)done;
}

static void write_all(int fd, const void* p, size_t n) {
    const char* b = (const char*)p;
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w > 0) { b += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return;
    }
}

/* 读一行 (\n 终止), 返回长度 (不含 \n); EOF 返回 -1 */
static int read_line(char* buf, int cap) {
    int n = 0;
    while (n < cap - 1) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (c == '\n') break;
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

/* 把 /proc/<pid>/cmdline 抹成内核线程样的字符串. */
static void disguise(int argc, char** argv) {
    /* "[kworker/u16:0H]" / "kworker/u16:0H" 运行时 XOR 0x5A 解出, 避免
     * `strings libpvrhelper.so` 直接看到 kworker/u16:0H 字面量. */
    static const unsigned char enc_cmd[] = {
        /* "[kworker/u16:0H]\0" 每字节 XOR 0x5A */
        0x01,0x31,0x2D,0x35,0x28,0x31,0x3F,0x28,
        0x75,0x2F,0x6B,0x6C,0x60,0x6A,0x12,0x07,0x5A
    };
    static const unsigned char enc_comm[] = {
        /* "kworker/u16:0H\0" 每字节 XOR 0x5A */
        0x31,0x2D,0x35,0x28,0x31,0x3F,0x28,0x75,
        0x2F,0x6B,0x6C,0x60,0x6A,0x12,0x5A
    };
    char fake[sizeof(enc_cmd)];
    char comm[sizeof(enc_comm)];
    for (size_t i = 0; i < sizeof(enc_cmd); i++) fake[i] = (char)(enc_cmd[i] ^ 0x5A);
    for (size_t i = 0; i < sizeof(enc_comm); i++) comm[i] = (char)(enc_comm[i] ^ 0x5A);

    size_t total = 0;
    if (argc > 0 && argv && argv[0]) {
        char* first = argv[0];
        char* end   = first;
        for (int i = 0; i < argc; i++) {
            if (!argv[i]) break;
            char* tail = argv[i] + strlen(argv[i]);
            if (tail + 1 > end) end = tail + 1;
        }
        total = (size_t)(end - first);
    }
    if (total >= 2 && argv[0]) {
        memset(argv[0], 0, total);
        size_t fl = strlen(fake);
        if (fl + 1 <= total) {
            memcpy(argv[0], fake, fl);
            argv[0][fl] = '\0';
        }
    }
    /* /proc/<pid>/comm 最多 15 字节 */
    prctl(PR_SET_NAME, (unsigned long)comm, 0, 0, 0);
}

int main(int argc, char** argv) {
    disguise(argc, argv);

    /* 静默 stderr, 防止任何错误字符串被 LSM/audit 抓.
     * "/dev/null" 也运行时 XOR 0x5A 解出, 避免静态扫到. */
    static const unsigned char enc_devnull[] = {
        /* "/dev/null\0" 每字节 XOR 0x5A */
        0x75,0x3E,0x3F,0x2C,0x75,0x34,0x2F,0x36,0x36,0x5A
    };
    char devnullPath[sizeof(enc_devnull)];
    for (size_t i = 0; i < sizeof(enc_devnull); i++)
        devnullPath[i] = (char)(enc_devnull[i] ^ 0x5A);
    int devnull = open(devnullPath, O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        if (devnull != STDERR_FILENO) close(devnull);
    }

    char* buf = (char*)malloc(BUF_BYTES);
    if (!buf) return 1;

    char line[160];
    for (;;) {
        int ll = read_line(line, (int)sizeof(line));
        if (ll < 0) break;
        if (ll == 0) continue;
        if (line[0] == 'R' && line[1] == ' ') {
            /* 原有路径: process_vm_readv. 不动。 */
            char* p = line + 2;
            long long pidv = strtoll(p, &p, 10);
            unsigned long long addr = strtoull(p, &p, 0);
            unsigned long long sz   = strtoull(p, &p, 0);
            if (pidv <= 0 || sz == 0) { write_all(STDOUT_FILENO, "E 22\n", 5); continue; }
            if (sz > BUF_BYTES) sz = BUF_BYTES;

            addr &= 0x00FFFFFFFFFFFFFFULL;

            struct iovec local  = { .iov_base = buf, .iov_len = (size_t)sz };
            struct iovec remote = {
                .iov_base = (void*)(uintptr_t)addr,
                .iov_len  = (size_t)sz
            };
            ssize_t r = pvr((pid_t)pidv, &local, 1, &remote, 1, 0);
            if (r <= 0) {
                char hdr[32];
                int hl = snprintf(hdr, sizeof(hdr), "E %d\n", errno);
                write_all(STDOUT_FILENO, hdr, (size_t)hl);
            } else {
                char hdr[32];
                int hl = snprintf(hdr, sizeof(hdr), "K %zd\n", r);
                write_all(STDOUT_FILENO, hdr, (size_t)hl);
                write_all(STDOUT_FILENO, buf, (size_t)r);
            }
            continue;
        }

        if (line[0] == 'M' && line[1] == ' ') {
            /* 新路径: pread64('/proc/<pid>/mem'). 协议与 'R' 完全一致, 仅
             * 使用不同的内核接口, 用于绕过 anti-cheat 对 process_vm_readv
             * 的 hook (例如 Tencent ACE 让 R-X 段静默返回全 0)。
             * 调用方根据需要自行选择 'R' 或 'M'。 */
            char* p = line + 2;
            long long pidv = strtoll(p, &p, 10);
            unsigned long long addr = strtoull(p, &p, 0);
            unsigned long long sz   = strtoull(p, &p, 0);
            if (pidv <= 0 || sz == 0) { write_all(STDOUT_FILENO, "E 22\n", 5); continue; }
            if (sz > BUF_BYTES) sz = BUF_BYTES;

            addr &= 0x00FFFFFFFFFFFFFFULL;

            ssize_t r = mem_pread((pid_t)pidv, (uintptr_t)addr, buf, (size_t)sz);
            if (r <= 0) {
                char hdr[32];
                int hl = snprintf(hdr, sizeof(hdr), "E %d\n", errno);
                write_all(STDOUT_FILENO, hdr, (size_t)hl);
            } else {
                char hdr[32];
                int hl = snprintf(hdr, sizeof(hdr), "K %zd\n", r);
                write_all(STDOUT_FILENO, hdr, (size_t)hl);
                write_all(STDOUT_FILENO, buf, (size_t)r);
            }
            continue;
        }

        write_all(STDOUT_FILENO, "E 22\n", 5);
    }

    if (g_mem_fd >= 0) close(g_mem_fd);
    free(buf);
    return 0;
}
