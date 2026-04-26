// ScreenCapture.cpp - fork+exec /system/bin/screencap, 读 RAW 帧
// 不使用任何 Hook / 不修改目标进程, 仅作为旁路截屏.
#include "ScreenCapture.h"
#include "../core/log/log.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <cstdio>
#include <cstdlib>

namespace ai_overlay {

static bool readN(int fd, void* buf, size_t n) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, p + got, n - got);
        if (r > 0) { got += static_cast<size_t>(r); continue; }
        if (r == 0) return got == n;
        if (errno == EINTR) continue;
        return false;
    }
    return true;
}

bool captureScreen(CaptureFrame& out, bool suMode) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        LOG(LOG_LEVEL_WARN, "[AI/Cap] pipe failed: %s", strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        LOG(LOG_LEVEL_WARN, "[AI/Cap] fork failed: %s", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // 子进程: stdout -> pipe[1]
        ::close(pipefd[0]);
        if (::dup2(pipefd[1], STDOUT_FILENO) < 0) _exit(127);
        ::close(pipefd[1]);
        // stderr 丢弃
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) { ::dup2(devnull, STDERR_FILENO); ::close(devnull); }

        if (suMode) {
            execl("/system/bin/sh", "sh", "-c", "su -c /system/bin/screencap", nullptr);
            execl("/system/bin/sh", "sh", "-c", "su -c screencap", nullptr);
        } else {
            execl("/system/bin/screencap", "screencap", nullptr);
        }
        _exit(127);
    }

    // 父进程: 读 pipe
    ::close(pipefd[1]);

    // RAW 头: w(int32), h(int32), format(int32) [Android <= 10 是 3*int32]
    // Android 11+ 增加了 colorspace, 但前 3 个字段顺序不变, pixel data 依然 RGBA8888
    // 我们先读 3*int32, 再根据格式探测尾随字段.
    uint32_t hdr3[3] = {0,0,0};
    if (!readN(pipefd[0], hdr3, sizeof(hdr3))) {
        LOG(LOG_LEVEL_WARN, "[AI/Cap] read header failed");
        ::close(pipefd[0]);
        int st; waitpid(pid, &st, 0);
        return false;
    }

    int w = static_cast<int>(hdr3[0]);
    int h = static_cast<int>(hdr3[1]);
    int fmt = static_cast<int>(hdr3[2]);

    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 ||
        (fmt != 1 /*RGBA8888*/ && fmt != 5 /*BGRA8888*/)) {
        LOG(LOG_LEVEL_WARN, "[AI/Cap] bad hdr w=%d h=%d fmt=%d", w, h, fmt);
        ::close(pipefd[0]);
        int st; waitpid(pid, &st, 0);
        return false;
    }

    // Android 9+ screencap 头额外有 1 个 uint32 (dataSpace / colorSpace)
    // 通过尝试: 读 4 字节, 若加上像素数据后 EOF 对齐则认为头是 4 个字段.
    // 简化: 直接尝试读 1 个 uint32 (额外字段), 若读不到再认为头只有 3 字段.
    uint32_t maybeColorSpace = 0;
    bool haveExtra = readN(pipefd[0], &maybeColorSpace, sizeof(maybeColorSpace));
    // 如果没读到额外字段, 之前的位置无法回滚 — 处理: 若 haveExtra=false 则 maybeColorSpace 未定义,
    // 但 readN 会返回 false 仅当 EOF/错误, 此时也无像素数据, 直接失败.
    if (!haveExtra) {
        LOG(LOG_LEVEL_WARN, "[AI/Cap] no extra field / no pixel data");
        ::close(pipefd[0]);
        int st; waitpid(pid, &st, 0);
        return false;
    }

    // 读像素 (假设 stride = w*4, 实测 screencap 输出无填充)
    const size_t pxBytes = static_cast<size_t>(w) * h * 4;
    out.width = w;
    out.height = h;
    out.stride = w * 4;
    out.format = fmt;
    out.pixels.resize(pxBytes);

    if (!readN(pipefd[0], out.pixels.data(), pxBytes)) {
        LOG(LOG_LEVEL_WARN, "[AI/Cap] read pixels failed (need %zu)", pxBytes);
        out.pixels.clear();
        ::close(pipefd[0]);
        int st; waitpid(pid, &st, 0);
        return false;
    }

    ::close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);

    // BGRA -> RGBA 就地交换
    if (fmt == 5) {
        uint8_t* p = out.pixels.data();
        for (size_t i = 0; i < pxBytes; i += 4) {
            uint8_t b = p[i];
            p[i] = p[i+2];
            p[i+2] = b;
        }
        out.format = 1;
    }

    return true;
}

} // namespace ai_overlay
