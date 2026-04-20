/**
 * pvr_helper - 跨进程内存读取 helper, 由 root shell 启动.
 *
 * 用 process_vm_readv(syscall 270) 读取目标进程任意虚地址区间,
 * 写入指定输出文件. 替代 toybox dd /proc/PID/mem 方案 (后者在
 * skip*bs > 2GB 时返回 "Permission denied" 是 toybox dd 的 32-bit
 * lseek bug).
 *
 * 用法: pvr_helper <PID> <HEXADDR> <SIZE> <OUTFILE>
 *   - 自动去掉 aarch64 TBI/MTE tag (高 8 位)
 *   - SIZE 任意大小, 内部分块读取
 *   - 不 commit unmapped 页, 不写零补齐: 部分读返回部分内容
 *   - 退出码: 0=完整成功, 1=参数错, 2=写文件失败, 3=完全失败,
 *            4=部分成功 (输出文件长度 < SIZE)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/uio.h>

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s PID HEXADDR SIZE OUTFILE\n", argv[0]);
        return 1;
    }
    pid_t  pid  = (pid_t)atoi(argv[1]);
    unsigned long long addr = strtoull(argv[2], NULL, 0);
    size_t size = (size_t)strtoul(argv[3], NULL, 0);
    const char* outpath = argv[4];

    // 去掉 aarch64 TBI/MTE 高 8 位 tag, 让 process_vm_readv 拿到真实虚地址
    addr &= 0x00FFFFFFFFFFFFFFULL;

    int fd = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", outpath, strerror(errno));
        return 2;
    }

    // 单次最大 1MB, 内部循环
    const size_t CHUNK = 1024 * 1024;
    void* buf = malloc(CHUNK);
    if (!buf) { close(fd); return 2; }

    size_t total_done = 0;
    while (total_done < size) {
        size_t want = size - total_done;
        if (want > CHUNK) want = CHUNK;
        struct iovec local  = { .iov_base = buf, .iov_len = want };
        struct iovec remote = {
            .iov_base = (void*)(uintptr_t)(addr + total_done),
            .iov_len  = want
        };
        ssize_t n = process_vm_readv(pid, &local, 1, &remote, 1, 0);
        if (n <= 0) {
            // 该块失败. 不写零, 直接退出循环, 让上层用现有长度判断.
            // 写到 stderr 让 RootShell 日志可见.
            fprintf(stderr, "process_vm_readv pid=%d addr=0x%llx len=%zu failed: %s\n",
                    pid, (unsigned long long)(addr + total_done), want, strerror(errno));
            break;
        }
        if (write(fd, buf, n) != n) {
            fprintf(stderr, "write failed: %s\n", strerror(errno));
            free(buf);
            close(fd);
            return 2;
        }
        total_done += (size_t)n;
        if ((size_t)n < want) break;  // partial read 也停, 下个 page 一般也读不到
    }
    free(buf);
    close(fd);

    if (total_done == 0) return 3;
    if (total_done < size) return 4;
    return 0;
}
