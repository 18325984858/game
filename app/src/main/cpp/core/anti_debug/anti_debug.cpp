/*
 * anti_debug.cpp — 轻量反调试 / 反 Frida / 反 Xposed 自检线程
 *
 * 设计目标:
 *   1. 零业务侵入: 一个后台 detached 线程, 不阻塞 UI.
 *   2. 命中即 abort: 不静默 (sleep + 假装正常), 让逆向者看到清晰的 SIGABRT,
 *      也方便正常用户上报 crash log.
 *   3. 检测项保守, 只标记高确定性信号, 避免误伤 (如 root 但未挂调试器的用户).
 *
 * 检测项:
 *   - /proc/self/status 中 TracerPid != 0  →  调试器 / ptrace 类工具
 *   - /proc/self/maps 出现 frida-agent / gum-js-loop / xposed / lspd / linjector
 *     →  动态注入框架
 *
 * 不做的事:
 *   - 不做 ptrace(PTRACE_TRACEME) (会和系统调试器/zygote 冲突, 收益不大).
 *   - 不检测 root: 这是用户自由, 不属于"代码保护".
 *
 * 由 CMake 选项 ENABLE_ANTI_DEBUG 编译进来. 关闭时整个 .o 不参与链接.
 */

#include "anti_debug.h"

#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>

#include "../log/log.h"

namespace {

constexpr const char* kTag = "AntiDbg";
constexpr int kPollIntervalSec = 2;

std::atomic<bool> g_started{false};

bool TracerPidPositive() {
    FILE* fp = fopen("/proc/self/status", "r");
    if (!fp) return false;
    char line[256];
    bool hit = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            int pid = atoi(line + 10);
            if (pid > 0) hit = true;
            break;
        }
    }
    fclose(fp);
    return hit;
}

bool MapsContainsInjectionFramework() {
    static const char* const kBadNeedles[] = {
        "frida-agent",
        "gum-js-loop",
        "gmain",
        "linjector",
        "lspd",
        "EdXposed",
        "LSPosed",
        nullptr,
    };
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;
    char line[1024];
    bool hit = false;
    while (fgets(line, sizeof(line), fp)) {
        for (const char* const* p = kBadNeedles; *p; ++p) {
            if (strstr(line, *p)) { hit = true; break; }
        }
        if (hit) break;
    }
    fclose(fp);
    return hit;
}

void* WatcherMain(void*) {
    pthread_setname_np(pthread_self(), "ad-watch");
    for (;;) {
        if (TracerPidPositive()) {
            LOG(LOG_LEVEL_ERROR, "%s tracer detected, aborting", kTag);
            abort();
        }
        if (MapsContainsInjectionFramework()) {
            LOG(LOG_LEVEL_ERROR, "%s injection framework detected, aborting", kTag);
            abort();
        }
        sleep(kPollIntervalSec);
    }
    return nullptr;
}

}  // namespace

void StartAntiDebugWatcher() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, &WatcherMain, nullptr) != 0) {
        // 创建失败不致命, 仅记录, 不影响业务.
        LOG(LOG_LEVEL_WARN, "%s pthread_create failed", kTag);
        g_started.store(false);
    }
    pthread_attr_destroy(&attr);
}

OBFU_ATTRS_END
