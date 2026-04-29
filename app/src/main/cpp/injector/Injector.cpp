/*
 * ═════════════════════════════════════════════════════════════════════
 *  Injector.cpp - ARM64 ptrace 注入实现
 * ═════════════════════════════════════════════════════════════════════
 *
 *  注入流程:
 *
 *  injectByPackageName("com.tencent.lolm", "/data/.../libdobbyproject.so")
 *    │
 *    ├─ findPidByName()   ─── su -c 'pidof com.tencent.lolm'
 *    │   └─ 每秒重试, 最多 15 次
 *    │
 *    └─ injectRemote(pid, soPath)
 *        │
 *        ├─ [1] ptrace(PTRACE_ATTACH, pid)
 *        │   └─ waitpid 等待目标暂停
 *        │
 *        ├─ [2] GETREGS 保存原始寄存器 (origRegs)
 *        │
 *        ├─ [3] 远程调用 mmap(NULL, 0x1000, RWX, ANON|PRIVATE, -1, 0)
 *        │   ├─ 通过 getRemoteFuncAddr 计算目标进程中 mmap 地址
 *        │   │   └─ remote_mmap = (local_mmap - local_libc_base) + remote_libc_base
 *        │   └─ 返回 remoteMem (4KB 可执行内存)
 *        │
 *        ├─ [4] POKEDATA 写入 SO 路径到 remoteMem
 *        │
 *        ├─ [5] 远程调用 dlopen(remoteMem, RTLD_NOW)
 *        │   ├─ 通过 libdl.so 偏移计算目标 dlopen 地址
 *        │   └─ 返回 dlopenResult (SO handle)
 *        │
 *        ├─ [6] 远程调用 dlsym(handle, "_Z12MyStartPointPvS_S_S_S_")
 *        │   ├─ 函数名写入 remoteMem
 *        │   └─ 返回 myStartPointAddr
 *        │
 *        ├─ [7] 解析 /proc/pid/maps 获取 libil2cpp.so 基址
 *        │   └─ PEEKDATA 读取 4 个 IL2CPP 元数据指针:
 *        │       base+0xF45D838 → pCodeRegistration
 *        │       base+0xF45D840 → pMetadataRegistration
 *        │       base+0xF45D858 → pGlobalMetadataHeader
 *        │       base+0x1D21140 → pMetadataImagesTable
 *        │
 *        ├─ [8] 远程调用 MyStartPoint(il2cppBase, codeReg, metaReg, globalMeta, metaImages)
 *        │   ├─ X0=il2cppBase  X1=codeReg  X2=metaReg  X3=globalMeta  X4=metaImages
 *        │   ├─ PC=myStartPointAddr  LR=0
 *        │   ├─ CONT → 目标执行 MyStartPoint
 *        │   └─ LR=0 触发 SIGSEGV → waitpid 捕获 → 读取 X0 返回值
 *        │
 *        ├─ [9] 远程调用 munmap(remoteMem, 0x1000) 释放临时内存
 *        │
 *        └─ [10] SETREGS 恢复原始寄存器 + ptrace(DETACH)
 *             └─ 目标进程恢复正常执行, 注入完成
 *
 * ═════════════════════════════════════════════════════════════════════
 */
#include "Injector.h"
#include "../core/log/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdlib>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/uio.h>     // struct iovec

#if defined(__aarch64__)
#include <asm/ptrace.h>   // struct user_pt_regs (ARM64)
#elif defined(__arm__)
#include <asm/ptrace.h>   // struct pt_regs (ARM32)
#elif defined(__x86_64__) || defined(__i386__)
#include <sys/user.h>     // struct user_regs_struct (x86/x86_64)
#endif

// ═══════════════════════════════════════════════════════════════════════════════
// 多架构 ptrace 寄存器操作封装
// ═══════════════════════════════════════════════════════════════════════════════

#if defined(__aarch64__)

struct pt_regs_arch {
    uint64_t regs[31];  // X0-X30
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

static int ptrace_getregs(pid_t pid, pt_regs_arch* regs) {
    struct iovec iov;
    iov.iov_base = regs;
    iov.iov_len = sizeof(*regs);
    return ptrace(PTRACE_GETREGSET, pid, (void*)1 /*NT_PRSTATUS*/, &iov);
}

static int ptrace_setregs(pid_t pid, const pt_regs_arch* regs) {
    struct iovec iov;
    iov.iov_base = (void*)regs;
    iov.iov_len = sizeof(*regs);
    return ptrace(PTRACE_SETREGSET, pid, (void*)1 /*NT_PRSTATUS*/, &iov);
}

#elif defined(__arm__)

typedef struct pt_regs pt_regs_arch;

static int ptrace_getregs(pid_t pid, pt_regs_arch* regs) {
    return ptrace(PTRACE_GETREGS, pid, nullptr, regs);
}

static int ptrace_setregs(pid_t pid, const pt_regs_arch* regs) {
    return ptrace(PTRACE_SETREGS, pid, nullptr, regs);
}

#elif defined(__x86_64__)

typedef struct user_regs_struct pt_regs_arch;

static int ptrace_getregs(pid_t pid, pt_regs_arch* regs) {
    return ptrace(PTRACE_GETREGS, pid, nullptr, regs);
}

static int ptrace_setregs(pid_t pid, const pt_regs_arch* regs) {
    return ptrace(PTRACE_SETREGS, pid, nullptr, regs);
}

#elif defined(__i386__)

typedef struct user_regs_struct pt_regs_arch;

static int ptrace_getregs(pid_t pid, pt_regs_arch* regs) {
    return ptrace(PTRACE_GETREGS, pid, nullptr, regs);
}

static int ptrace_setregs(pid_t pid, const pt_regs_arch* regs) {
    return ptrace(PTRACE_SETREGS, pid, nullptr, regs);
}

#else
#error "Unsupported architecture for Injector"
#endif

#ifndef __WALL
#define __WALL 0x40000000
#endif

#ifndef PTRACE_SEIZE
#define PTRACE_SEIZE 0x4206
#endif

#ifndef PTRACE_INTERRUPT
#define PTRACE_INTERRUPT 0x4207
#endif

static int wait_for_trace_stop(pid_t pid, int* status, const char* stage, int timeoutMs) {
    if (!status) return -1;
    constexpr int kStepUs = 20 * 1000;
    int waitedMs = 0;
    *status = 0;

    while (waitedMs <= timeoutMs) {
        int ret = waitpid(-1, status, WUNTRACED | __WALL | WNOHANG);
        if (ret > 0) {
            if (ret != pid) {
                LOG(LOG_LEVEL_WARN, "[Injector] %s wait got pid=%d, expect=%d status=0x%x",
                    stage, ret, pid, *status);
                continue;
            }
            if (WIFSTOPPED(*status)) {
                LOG(LOG_LEVEL_INFO, "[Injector] %s wait stop ok sig=%d status=0x%x",
                    stage, WSTOPSIG(*status), *status);
                return 0;
            }
            LOG(LOG_LEVEL_ERROR, "[Injector] %s wait unexpected status=0x%x", stage, *status);
            return -1;
        }
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG(LOG_LEVEL_ERROR, "[Injector] %s waitpid failed: %s", stage, strerror(errno));
            return -1;
        }
        usleep(kStepUs);
        waitedMs += kStepUs / 1000;
    }

    LOG(LOG_LEVEL_ERROR, "[Injector] %s wait timeout pid=%d timeout=%dms", stage, pid, timeoutMs);
    return -1;
}

static int ptrace_attach_and_wait(pid_t pid, int* status) {
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) == 0) {
        if (wait_for_trace_stop(pid, status, "attach", 8000) == 0) {
            return 0;
        }
        LOG(LOG_LEVEL_ERROR, "[Injector] PTRACE_ATTACH 后等待目标停止失败, 尝试 detach pid=%d", pid);
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        usleep(50 * 1000);
    } else {
        LOG(LOG_LEVEL_ERROR, "[Injector] PTRACE_ATTACH 失败 (pid=%d): %s", pid, strerror(errno));
    }

    LOG(LOG_LEVEL_WARN, "[Injector] 尝试 PTRACE_SEIZE/PTRACE_INTERRUPT pid=%d", pid);
    if (ptrace(PTRACE_SEIZE, pid, nullptr, nullptr) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] PTRACE_SEIZE 失败 (pid=%d): %s", pid, strerror(errno));
        return -1;
    }
    if (ptrace(PTRACE_INTERRUPT, pid, nullptr, nullptr) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] PTRACE_INTERRUPT 失败 (pid=%d): %s", pid, strerror(errno));
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -1;
    }
    if (wait_for_trace_stop(pid, status, "seize", 8000) == 0) {
        return 0;
    }

    LOG(LOG_LEVEL_ERROR, "[Injector] PTRACE_SEIZE 后等待目标停止失败, 尝试 detach pid=%d", pid);
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 远程内存读写
// ═══════════════════════════════════════════════════════════════════════════════

static int ptrace_writedata(pid_t pid, uint64_t dest, const void* data, size_t size) {
    const uint64_t* src = (const uint64_t*)data;
    size_t count = size / sizeof(uint64_t);
    size_t remainder = size % sizeof(uint64_t);

    for (size_t i = 0; i < count; i++) {
        if (ptrace(PTRACE_POKEDATA, pid, (void*)(dest + i * sizeof(uint64_t)), (void*)src[i]) < 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] POKEDATA failed at %llx: %s",
                (unsigned long long)(dest + i * sizeof(uint64_t)), strerror(errno));
            return -1;
        }
    }

    if (remainder > 0) {
        uint64_t val = 0;
        // 先读出原始数据，保留未覆盖的字节
        val = ptrace(PTRACE_PEEKDATA, pid, (void*)(dest + count * sizeof(uint64_t)), nullptr);
        memcpy(&val, (const uint8_t*)data + count * sizeof(uint64_t), remainder);
        if (ptrace(PTRACE_POKEDATA, pid, (void*)(dest + count * sizeof(uint64_t)), (void*)val) < 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] POKEDATA remainder failed: %s", strerror(errno));
            return -1;
        }
    }

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 远程读取指针值 (PTRACE_PEEKDATA)
// ═══════════════════════════════════════════════════════════════════════════════

static uint64_t ptrace_peekptr(pid_t pid, uint64_t addr) {
    errno = 0;
    uint64_t val = (uint64_t)ptrace(PTRACE_PEEKDATA, pid, (void*)addr, nullptr);
    if (errno != 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] PEEKDATA failed at %llx: %s",
            (unsigned long long)addr, strerror(errno));
        return 0;
    }
    return val;
}

static bool ptrace_read_string(pid_t pid, uint64_t addr, char* out, size_t outSize) {
    if (!out || outSize == 0 || addr == 0) return false;
    size_t written = 0;
    while (written + 1 < outSize) {
        errno = 0;
        uint64_t word = (uint64_t)ptrace(PTRACE_PEEKDATA, pid, (void*)(addr + written), nullptr);
        if (errno != 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] PEEKDATA string failed at %llx: %s",
                (unsigned long long)(addr + written), strerror(errno));
            break;
        }

        for (size_t i = 0; i < sizeof(word) && written + 1 < outSize; ++i) {
            char ch = (char)((word >> (i * 8)) & 0xff);
            out[written++] = ch;
            if (ch == '\0') return true;
        }
    }
    out[outSize - 1] = '\0';
    return written > 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 解析目标进程中某个模块的基地址
// ═══════════════════════════════════════════════════════════════════════════════

static uint64_t getRemoteModuleBase(pid_t pid, const char* moduleName) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, moduleName)) {
            uint64_t base = 0;
            sscanf(line, "%llx-", (unsigned long long*)&base);
            fclose(fp);
            return base;
        }
    }
    fclose(fp);
    return 0;
}

// 获取目标进程中模块的大小 (所有映射段的最大结束地址 - 基址)
static uint64_t getRemoteModuleSize(pid_t pid, const char* moduleName) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

    uint64_t minStart = UINT64_MAX, maxEnd = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, moduleName)) {
            uint64_t start = 0, end = 0;
            sscanf(line, "%llx-%llx", (unsigned long long*)&start, (unsigned long long*)&end);
            if (start < minStart) minStart = start;
            if (end > maxEnd) maxEnd = end;
        }
    }
    fclose(fp);
    return (maxEnd > minStart) ? (maxEnd - minStart) : 0;
}

static pid_t waitForTargetProcessReady(const char* packageName, Injector::InjectMode mode) {
    constexpr int kDefaultWaitSeconds = 15;
    constexpr int kPubgWaitSeconds = 45;
    constexpr int kDfmWaitSeconds  = 90;   // DFM UE5.4 引擎初始化较慢, 需要更长等待
    constexpr int kNrcWaitSeconds  = 60;   // NRC UE 4.26
    constexpr int kPubgStableSamples = 3;
    constexpr int kDfmStableSamples  = 5;  // DFM 要求连续 5 次检测到 libUE4.so 才视为稳定
    constexpr int kNrcStableSamples  = 3;

    // DFM 和 PUBG 都需要等待 libUE4.so 加载
    const bool needUe4Wait = (mode == Injector::MODE_PUBG || mode == Injector::MODE_DFM || mode == Injector::MODE_NRC);
    const int maxWaitSeconds = (mode == Injector::MODE_DFM) ? kDfmWaitSeconds :
                               (mode == Injector::MODE_NRC) ? kNrcWaitSeconds :
                               (mode == Injector::MODE_PUBG) ? kPubgWaitSeconds : kDefaultWaitSeconds;
    const int requiredStableSamples = (mode == Injector::MODE_DFM) ? kDfmStableSamples :
                                      (mode == Injector::MODE_NRC) ? kNrcStableSamples : kPubgStableSamples;
    pid_t lastPid = -1;
    int stableSamples = 0;

    for (int attempt = 1; attempt <= maxWaitSeconds; ++attempt) {
        pid_t pid = Injector::findPidByName(packageName);
        if (pid <= 0) {
            lastPid = -1;
            stableSamples = 0;
            LOG(LOG_LEVEL_INFO, "[Injector] 等待目标进程启动... (%d/%d)", attempt, maxWaitSeconds);
            sleep(1);
            continue;
        }

        if (!needUe4Wait) {
            return pid;
        }

        const bool ue4Loaded = getRemoteModuleBase(pid, "libUE4.so") != 0;
        if (pid != lastPid) {
            lastPid = pid;
            stableSamples = ue4Loaded ? 1 : 0;
        } else if (ue4Loaded) {
            ++stableSamples;
        } else {
            stableSamples = 0;
        }

        const char* modeTag = (mode == Injector::MODE_DFM) ? "DFM" :
                              (mode == Injector::MODE_NRC) ? "NRC" : "PUBG";
        LOG(LOG_LEVEL_INFO,
            "[Injector] %s 就绪检查 pid=%d libUE4=%s stable=%d/%d (%d/%d)",
            modeTag,
            pid,
            ue4Loaded ? "yes" : "no",
            stableSamples,
            requiredStableSamples,
            attempt,
            maxWaitSeconds);

        if (ue4Loaded && stableSamples >= requiredStableSamples) {
            return pid;
        }

        sleep(1);
    }

    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 获取本进程某个模块的基地址
// ═══════════════════════════════════════════════════════════════════════════════

static uint64_t getLocalModuleBase(const char* moduleName) {
    return getRemoteModuleBase(getpid(), moduleName);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 计算目标进程中函数的实际地址
// (本进程的函数地址 - 本进程模块基址 + 目标进程模块基址)
// ═══════════════════════════════════════════════════════════════════════════════

static uint64_t getRemoteFuncAddr(pid_t pid, const char* moduleName, void* localFuncAddr) {
    uint64_t localBase = getLocalModuleBase(moduleName);
    uint64_t remoteBase = getRemoteModuleBase(pid, moduleName);
    if (localBase == 0 || remoteBase == 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] module '%s' base: local=%llx remote=%llx",
            moduleName, (unsigned long long)localBase, (unsigned long long)remoteBase);
        return 0;
    }
    uint64_t offset = (uint64_t)localFuncAddr - localBase;
    return remoteBase + offset;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 远程调用: 在目标进程中执行一个函数并获取返回值
// ═══════════════════════════════════════════════════════════════════════════════

static int ptrace_call(pid_t pid, uint64_t funcAddr, uint64_t* params, int paramCount, uint64_t* retVal) {
    pt_regs_arch regs;
    if (ptrace_getregs(pid, &regs) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] GETREGS failed: %s", strerror(errno));
        return -1;
    }

#if defined(__aarch64__)
    // ARM64: 前8个参数通过 X0-X7 传递
    for (int i = 0; i < paramCount && i < 8; i++) {
        regs.regs[i] = params[i];
    }
    regs.pc = funcAddr;
    // 设置 LR (X30) 为 0, 函数返回时触发 SIGSEGV
    regs.regs[30] = 0;

#elif defined(__arm__)
    // ARM32 AAPCS: R0-R3 传递前 4 个参数, 多余的压栈
    for (int i = 0; i < paramCount && i < 4; i++) {
        regs.uregs[i] = (unsigned long)params[i];
    }
    if (paramCount > 4) {
        regs.ARM_sp -= (paramCount - 4) * 4;
        for (int i = 4; i < paramCount; i++) {
            uint32_t val32 = (uint32_t)params[i];
            ptrace(PTRACE_POKEDATA, pid, (void*)(unsigned long)(regs.ARM_sp + (i - 4) * 4), (void*)(unsigned long)val32);
        }
    }
    regs.ARM_pc = (unsigned long)funcAddr;
    // 设置 LR 为 0, 函数返回时触发 SIGSEGV
    regs.ARM_lr = 0;

#elif defined(__x86_64__)
    // x86_64 System V ABI: RDI, RSI, RDX, RCX, R8, R9
    if (paramCount > 0) regs.rdi = params[0];
    if (paramCount > 1) regs.rsi = params[1];
    if (paramCount > 2) regs.rdx = params[2];
    if (paramCount > 3) regs.rcx = params[3];
    if (paramCount > 4) regs.r8  = params[4];
    if (paramCount > 5) regs.r9  = params[5];
    // 在栈上压入返回地址 0, ret 时触发 SIGSEGV
    regs.rsp -= 8;
    ptrace(PTRACE_POKEDATA, pid, (void*)regs.rsp, (void*)0);
    regs.rip = funcAddr;

#elif defined(__i386__)
    // x86 cdecl: 所有参数从右往左压栈
    for (int i = paramCount - 1; i >= 0; i--) {
        regs.esp -= 4;
        uint32_t val32 = (uint32_t)params[i];
        ptrace(PTRACE_POKEDATA, pid, (void*)(unsigned long)regs.esp, (void*)(unsigned long)val32);
    }
    // 压入返回地址 0
    regs.esp -= 4;
    ptrace(PTRACE_POKEDATA, pid, (void*)(unsigned long)regs.esp, (void*)0);
    regs.eip = (uint32_t)funcAddr;
#endif

    if (ptrace_setregs(pid, &regs) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] SETREGS failed: %s", strerror(errno));
        return -1;
    }

    // 让目标进程继续执行
    if (ptrace(PTRACE_CONT, pid, nullptr, nullptr) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] CONT failed: %s", strerror(errno));
        return -1;
    }

    // 等待目标进程停止 (返回地址 0 触发 SIGSEGV)
    int status = 0;
    if (wait_for_trace_stop(pid, &status, "remote-call", 10000) < 0) {
        return -1;
    }

    if (WIFSTOPPED(status)) {
        if (ptrace_getregs(pid, &regs) < 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] GETREGS after call failed: %s", strerror(errno));
            return -1;
        }
#if defined(__aarch64__)
        if (retVal) *retVal = regs.regs[0];  // X0 = 返回值
#elif defined(__arm__)
        if (retVal) *retVal = regs.ARM_r0;   // R0 = 返回值
#elif defined(__x86_64__)
        if (retVal) *retVal = regs.rax;      // RAX = 返回值
#elif defined(__i386__)
        if (retVal) *retVal = regs.eax;      // EAX = 返回值
#endif
    } else {
        LOG(LOG_LEVEL_ERROR, "[Injector] 目标进程异常退出, status=%d", status);
        return -1;
    }

    return 0;
}

// ═══════════════════════════════════════════════════════════════════════════════
// 模式分支共用 helpers — 把 4 个分支里重复的 dlsym/模块基址/StartPoint 调用抽出。
// 每个 helper 失败时打日志并返回 false/0; 调用方只需 `if (!helper(...)) goto cleanup;`。
// ═══════════════════════════════════════════════════════════════════════════════

/** 远程 dlsym 查找入口函数地址。失败返回 0 并打日志。 */
static uint64_t resolveStartPoint(pid_t pid, uint64_t dlopenResult, uint64_t remoteMem,
                                  uint64_t remoteDlsymAddr, const char* funcName) {
    if (ptrace_writedata(pid, remoteMem, funcName, strlen(funcName) + 1) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] 写入函数名失败");
        return 0;
    }
    uint64_t dlsymParams[2] = { dlopenResult, remoteMem };
    uint64_t funcAddr = 0;
    if (ptrace_call(pid, remoteDlsymAddr, dlsymParams, 2, &funcAddr) < 0 || funcAddr == 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] dlsym 查找 %s 失败", funcName);
        return 0;
    }
    LOG(LOG_LEVEL_INFO, "[Injector] %s 地址: %llx", funcName, (unsigned long long)funcAddr);
    return funcAddr;
}

/** 取目标 SO 基址 + 大小, 同时打日志。base==0 视为失败返回 false。 */
static bool loadModuleInfo(pid_t pid, const char* libName, uint64_t* base, uint64_t* size) {
    *base = getRemoteModuleBase(pid, libName);
    if (*base == 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] 无法找到 %s 基址", libName);
        return false;
    }
    *size = getRemoteModuleSize(pid, libName);
    LOG(LOG_LEVEL_INFO, "[Injector] %s 基址: %llx 大小: 0x%llx",
        libName, (unsigned long long)*base, (unsigned long long)*size);
    return true;
}

/** 远程调用 MyStartPoint*, 打日志, 返回值非 0 才算成功。 */
static bool invokeStartPoint(pid_t pid, uint64_t funcAddr, const char* funcName,
                             uint64_t* params, int nParams) {
    LOG(LOG_LEVEL_INFO, "[Injector] 调用 %s...", funcName);
    uint64_t startRet = 0;
    if (ptrace_call(pid, funcAddr, params, nParams, &startRet) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] %s 调用失败", funcName);
        return false;
    }
    LOG(LOG_LEVEL_INFO, "[Injector] ✓ %s 返回: %lld", funcName, (long long)startRet);
    if (startRet == 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] %s 返回 0, 视为失败", funcName);
        return false;
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// injectRemote — 核心注入函数
// ═══════════════════════════════════════════════════════════════════════════════

int Injector::injectRemote(pid_t pid, const char* soPath, InjectMode mode) {
    LOG(LOG_LEVEL_INFO, "[Injector] 开始注入 pid=%d so=%s mode=%s", pid, soPath,
        mode == MODE_PUBG ? "PUBG" : (mode == MODE_DFM ? "DFM" : (mode == MODE_NRC ? "NRC" : "LOL")));

    // ── 1. Attach 到目标进程 ──
    int status = 0;
    if (ptrace_attach_and_wait(pid, &status) < 0) {
        return -1;
    }
    LOG(LOG_LEVEL_INFO, "[Injector] 已附加到进程 %d", pid);

    // ── 2. 保存原始寄存器 ──
    pt_regs_arch origRegs;
    if (ptrace_getregs(pid, &origRegs) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] 保存寄存器失败: %s", strerror(errno));
        ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
        return -2;
    }

    int result = -1;

    // ── 3. 远程调用 mmap 分配内存 ──
    uint64_t remoteMmapAddr = getRemoteFuncAddr(pid, "libc.so", (void*)mmap);
    if (remoteMmapAddr == 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] 无法找到远程 mmap 地址");
        goto detach;
    }
    LOG(LOG_LEVEL_INFO, "[Injector] 远程 mmap 地址: %llx", (unsigned long long)remoteMmapAddr);

    {
        uint64_t mmapParams[6] = {
            0,                          // addr = NULL
            0x1000,                     // length = 4096
            PROT_READ | PROT_WRITE | PROT_EXEC,  // prot
            MAP_ANONYMOUS | MAP_PRIVATE,          // flags
            0,                          // fd = -1 (用0因为MAP_ANONYMOUS)
            0                           // offset
        };
        // fd 应该传 -1, 但 ptrace 传递时需要用无符号表示
        mmapParams[4] = (uint64_t)-1;

        uint64_t remoteMem = 0;
        if (ptrace_call(pid, remoteMmapAddr, mmapParams, 6, &remoteMem) < 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] 远程 mmap 调用失败");
            goto detach;
        }
        if (remoteMem == (uint64_t)MAP_FAILED || remoteMem == 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] 远程 mmap 返回无效地址: %llx", (unsigned long long)remoteMem);
            goto detach;
        }
        LOG(LOG_LEVEL_INFO, "[Injector] 远程内存分配成功: %llx", (unsigned long long)remoteMem);

        // ── 4. 写入 SO 路径到远程内存 ──
        size_t pathLen = strlen(soPath) + 1;
        if (ptrace_writedata(pid, remoteMem, soPath, pathLen) < 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] 写入 SO 路径失败");
            goto detach;
        }
        LOG(LOG_LEVEL_INFO, "[Injector] SO 路径已写入远程内存");

        // ── 5. 远程调用 dlopen 加载 SO ──
        // Android linker 中 dlopen 在 libdl.so 或 linker64 中
        uint64_t remoteDlopenAddr = getRemoteFuncAddr(pid, "libdl.so", (void*)dlopen);
        if (remoteDlopenAddr == 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] 无法找到远程 dlopen 地址");
            goto detach;
        }
        LOG(LOG_LEVEL_INFO, "[Injector] 远程 dlopen 地址: %llx", (unsigned long long)remoteDlopenAddr);

        uint64_t dlopenParams[2] = {
            remoteMem,     // filename (SO 路径)
            RTLD_NOW       // flags
        };

        uint64_t dlopenResult = 0;
        if (ptrace_call(pid, remoteDlopenAddr, dlopenParams, 2, &dlopenResult) < 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] 远程 dlopen 调用失败");
            goto detach;
        }

        if (dlopenResult == 0) {
            // dlopen 失败, 尝试获取 dlerror
            LOG(LOG_LEVEL_ERROR, "[Injector] dlopen 返回 NULL, SO 加载失败!");
            uint64_t remoteDlerrorAddr = getRemoteFuncAddr(pid, "libdl.so", (void*)dlerror);
            if (remoteDlerrorAddr != 0) {
                uint64_t errStrAddr = 0;
                ptrace_call(pid, remoteDlerrorAddr, nullptr, 0, &errStrAddr);
                LOG(LOG_LEVEL_ERROR, "[Injector] dlerror 地址: %llx", (unsigned long long)errStrAddr);
                char errBuf[512] = {0};
                if (ptrace_read_string(pid, errStrAddr, errBuf, sizeof(errBuf)) && errBuf[0] != '\0') {
                    LOG(LOG_LEVEL_ERROR, "[Injector] dlerror: %s", errBuf);
                } else {
                    LOG(LOG_LEVEL_ERROR, "[Injector] dlerror 内容为空或读取失败");
                }
            }
            goto detach;
        }

        LOG(LOG_LEVEL_INFO, "[Injector] ✓ dlopen 成功! handle=%llx", (unsigned long long)dlopenResult);

        // ── 6. 远程调用 dlsym 查找入口函数 ──
        uint64_t remoteDlsymAddr = getRemoteFuncAddr(pid, "libdl.so", (void*)dlsym);
        if (remoteDlsymAddr == 0) {
            LOG(LOG_LEVEL_ERROR, "[Injector] 无法找到远程 dlsym 地址");
            goto cleanup;
        }

        if (mode == MODE_PUBG) {
            // ═══ PUBG (UE4) 注入路径 ═══
            //
            // 全局偏移 (相对于 libUE4.so 基址):
            //   GNames:        +0x146F9F30 (需解引用)
            //   GUObjectArray: +0x14706480 (结构体地址, 不解引用)
            //   GWorld:        +0x14988578 (需解引用)
            //
            static constexpr uint32_t PUBG_OFF_GNAMES         = 0x154CE510;
            static constexpr uint32_t PUBG_OFF_GUOBJECTARRAY  = 0x14CEC650;
            static constexpr uint32_t PUBG_OFF_GWORLD         = 0x14CEE938;

            const char* funcName = "MyStartPointPUBG";
            uint64_t funcAddr = resolveStartPoint(pid, dlopenResult, remoteMem, remoteDlsymAddr, funcName);
            if (funcAddr == 0) goto cleanup;

            uint64_t ue4Base = 0, ue4Size = 0;
            if (!loadModuleInfo(pid, "libUE4.so", &ue4Base, &ue4Size)) goto cleanup;

            // 读取 GNames/GUObjectArray/GWorld 指针
            uint64_t pGNames        = ptrace_peekptr(pid, ue4Base + PUBG_OFF_GNAMES);
            uint64_t pGUObjectArray = ue4Base + PUBG_OFF_GUOBJECTARRAY;  // 结构体地址, 不解引用
            uint64_t pGWorld        = ue4Base + PUBG_OFF_GWORLD;         // 全局变量地址, 不解引用

            LOG(LOG_LEVEL_INFO, "[Injector] GNames:        %llx", (unsigned long long)pGNames);
            LOG(LOG_LEVEL_INFO, "[Injector] GUObjectArray: %llx", (unsigned long long)pGUObjectArray);
            LOG(LOG_LEVEL_INFO, "[Injector] GWorld:        %llx", (unsigned long long)pGWorld);

            if (ue4Size == 0 || pGNames == 0 || pGWorld == 0) {
                LOG(LOG_LEVEL_ERROR,
                    "[Injector] PUBG 全局指针未就绪 ue4Size=0x%llx GNames=%llx GWorld=%llx",
                    (unsigned long long)ue4Size,
                    (unsigned long long)pGNames,
                    (unsigned long long)pGWorld);
                goto cleanup;
            }

            // 调用 MyStartPointPUBG(libUE4Base, pGNames, pGWorld, pGUObjectArray, moduleSize, NULL)
            uint64_t startParams[6] = { ue4Base, pGNames, pGWorld, pGUObjectArray, ue4Size, 0 };
            if (!invokeStartPoint(pid, funcAddr, funcName, startParams, 6)) goto cleanup;
            result = 0;
        } else if (mode == MODE_DFM) {
            // ═══ DFM (UE5.4 三角洲) 注入路径 ═══
            //
            // 全局偏移 (相对于 libUE4.so 基址):
            //   NamePool:          +0x1B88EA00
            //   GUObjectArray.Num: +0x1B8B579C
            //   GUObjectArray.Chunks: +0x1B8B57A8
            //   GWorld:            +0x1BBAA930
            //
            static constexpr uint32_t DFM_OFF_NAMEPOOL          = 0x1B88EA00;
            static constexpr uint32_t DFM_OFF_GUOBJECTARRAY_NUM = 0x1B8B579C;
            static constexpr uint32_t DFM_OFF_GUOBJECTARRAY_CHUNKS = 0x1B8B57A8;
            static constexpr uint32_t DFM_OFF_GWORLD            = 0x1BBAA930;

            const char* funcName = "MyStartPointDFM";
            uint64_t funcAddr = resolveStartPoint(pid, dlopenResult, remoteMem, remoteDlsymAddr, funcName);
            if (funcAddr == 0) goto cleanup;

            uint64_t ue4Base = 0, ue4Size = 0;
            if (!loadModuleInfo(pid, "libUE4.so", &ue4Base, &ue4Size)) goto cleanup;
            if (ue4Size == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] DFM ue4Size=0");
                goto cleanup;
            }

            // 直接传递偏移值 (不加 ue4Base, C++ 层使用 base + offset 计算)
            uint64_t pNamePool          = DFM_OFF_NAMEPOOL;                  // NamePool 偏移
            uint64_t pGUObjectArrayNum  = DFM_OFF_GUOBJECTARRAY_NUM;          // GUObjectArray.NumElements 偏移
            uint64_t pGUObjectArrayChunks = DFM_OFF_GUOBJECTARRAY_CHUNKS;     // GUObjectArray.Chunks 偏移
            uint64_t pGWorld            = DFM_OFF_GWORLD;                     // GWorld 偏移

            LOG(LOG_LEVEL_INFO, "[Injector] DFM offsets: NP=0x%llx Num=0x%llx Chunks=0x%llx GW=0x%llx",
                (unsigned long long)pNamePool, (unsigned long long)pGUObjectArrayNum,
                (unsigned long long)pGUObjectArrayChunks, (unsigned long long)pGWorld);

            // 调用 MyStartPointDFM(libUE4Base, offNamePool, offGWorld, offGUObjArrayNum, offGUObjArrayChunks, moduleSize, NULL)
            uint64_t startParams[7] = {
                ue4Base,                // X0: plibUE4ModeBase - libUE4.so 基址
                pNamePool,              // X1: pGNames - NamePool 偏移
                pGWorld,                // X2: pGWorld - GWorld 偏移
                pGUObjectArrayNum,      // X3: pGUObjectArray - NumElements 偏移
                pGUObjectArrayChunks,   // X4: pGUObjectArrayChunks - Chunks 偏移
                ue4Size,                // X5: moduleSize
                0                       // X6: pData - 预留
            };
            if (!invokeStartPoint(pid, funcAddr, funcName, startParams, 7)) goto cleanup;
            result = 0;
        } else if (mode == MODE_NRC) {
            // ═══ NRC (UE 4.26 洛克王国手游) 注入路径 ═══
            //
            // 全局偏移 (相对于 libUE4.so 基址, 来自 ue_dump_all.js):
            //   NamePool:               +0x0D9A4B40
            //   GUObjectArray base:     +0x0D9C06B8
            //   GUObjectArray.Chunks:   +0x0D9C06C8 (= base + 0x10)
            //   GUObjectArray.Num:      +0x0D9C06DC (= base + 0x24)
            //   GWorld:                 +0x0DF3A198
            //
            static constexpr uint32_t NRC_OFF_NAMEPOOL             = 0x0D9A4B40;
            static constexpr uint32_t NRC_OFF_GUOBJECTARRAY_NUM    = 0x0D9C06B8 + 0x24;
            static constexpr uint32_t NRC_OFF_GUOBJECTARRAY_CHUNKS = 0x0D9C06B8 + 0x10;
            static constexpr uint32_t NRC_OFF_GWORLD               = 0x0DF3A198;

            const char* funcName = "MyStartPointNRC";
            uint64_t funcAddr = resolveStartPoint(pid, dlopenResult, remoteMem, remoteDlsymAddr, funcName);
            if (funcAddr == 0) goto cleanup;

            uint64_t ue4Base = 0, ue4Size = 0;
            if (!loadModuleInfo(pid, "libUE4.so", &ue4Base, &ue4Size)) goto cleanup;
            if (ue4Size == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] NRC ue4Size=0");
                goto cleanup;
            }

            uint64_t pNamePool             = NRC_OFF_NAMEPOOL;
            uint64_t pGUObjectArrayNum     = NRC_OFF_GUOBJECTARRAY_NUM;
            uint64_t pGUObjectArrayChunks  = NRC_OFF_GUOBJECTARRAY_CHUNKS;
            uint64_t pGWorld               = NRC_OFF_GWORLD;

            LOG(LOG_LEVEL_INFO, "[Injector] NRC offsets: NP=0x%llx Num=0x%llx Chunks=0x%llx GW=0x%llx",
                (unsigned long long)pNamePool, (unsigned long long)pGUObjectArrayNum,
                (unsigned long long)pGUObjectArrayChunks, (unsigned long long)pGWorld);

            // MyStartPointNRC(base, NamePool, GWorld, Num, Chunks, moduleSize, NULL)
            uint64_t startParams[7] = {
                ue4Base,
                pNamePool,
                pGWorld,
                pGUObjectArrayNum,
                pGUObjectArrayChunks,
                ue4Size,
                0
            };
            if (!invokeStartPoint(pid, funcAddr, funcName, startParams, 7)) goto cleanup;
            result = 0;
        } else {
        // ═══ LOL (il2cpp) 注入路径 ═══
        {
            //
            // 全局偏移 (相对于 libil2cpp.so 基址):
            //   CodeRegistration:     +0x0F45D838 (需解引用)
            //   MetadataRegistration: +0x0F45D840 (需解引用)
            //   GlobalMetadataHeader: +0x0F45D858 (需解引用)
            //   MetadataImagesTable:  +0x1D21140  (需解引用)
            //
            static constexpr uint32_t LOL_OFF_CODE_REG    = 0x0F45D838;
            static constexpr uint32_t LOL_OFF_META_REG    = 0x0F45D840;
            static constexpr uint32_t LOL_OFF_GLOBAL_META = 0x0F45D858;
            static constexpr uint32_t LOL_OFF_META_IMAGES = 0x1D21140;

            const char* funcName = "MyStartPointLOL";
            uint64_t myStartPointAddr = resolveStartPoint(pid, dlopenResult, remoteMem, remoteDlsymAddr, funcName);
            if (myStartPointAddr == 0) goto cleanup;

            // ── 7. 获取 libil2cpp.so 基址 ──
            uint64_t il2cppBase = getRemoteModuleBase(pid, "libil2cpp.so");
            if (il2cppBase == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] 无法找到 libil2cpp.so 基址");
                goto cleanup;
            }
            LOG(LOG_LEVEL_INFO, "[Injector] libil2cpp.so 基址: %llx", (unsigned long long)il2cppBase);

            // 直接传递偏移值 (不加 il2cppBase, C++ 层使用 base + offset 解引用)
            uint64_t pCodeRegistration     = LOL_OFF_CODE_REG;
            uint64_t pMetadataRegistration  = LOL_OFF_META_REG;
            uint64_t pGlobalMetadataHeader  = LOL_OFF_GLOBAL_META;
            uint64_t pMetadataImagesTable   = LOL_OFF_META_IMAGES;

            LOG(LOG_LEVEL_INFO, "[Injector] LOL offsets: CodeReg=0x%llx MetaReg=0x%llx GlobalMeta=0x%llx MetaImages=0x%llx",
                (unsigned long long)pCodeRegistration, (unsigned long long)pMetadataRegistration,
                (unsigned long long)pGlobalMetadataHeader, (unsigned long long)pMetadataImagesTable);

            // ── 8. 远程调用 MyStartPointLOL(il2cppBase, offCodeReg, offMetaReg, offGlobalMeta, offMetaImages) ──
            uint64_t startParams[5] = {
                il2cppBase,
                pCodeRegistration,
                pMetadataRegistration,
                pGlobalMetadataHeader,
                pMetadataImagesTable
            };

            if (!invokeStartPoint(pid, myStartPointAddr, "MyStartPoint", startParams, 5)) goto cleanup;
            result = 0;
        }
        } // end LOL branch

cleanup:
        // ── 9. 远程调用 munmap 释放临时内存 ──
        uint64_t remoteMunmapAddr = getRemoteFuncAddr(pid, "libc.so", (void*)munmap);
        if (remoteMunmapAddr != 0) {
            uint64_t munmapParams[2] = { remoteMem, 0x1000 };
            ptrace_call(pid, remoteMunmapAddr, munmapParams, 2, nullptr);
            LOG(LOG_LEVEL_INFO, "[Injector] 远程临时内存已释放");
        }
    }

detach:
    // ── 7. 恢复原始寄存器并 Detach ──
    ptrace_setregs(pid, &origRegs);
    ptrace(PTRACE_DETACH, pid, nullptr, nullptr);
    LOG(LOG_LEVEL_INFO, "[Injector] 已从进程 %d 分离, 结果=%d", pid, result);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// findPidByName — 通过包名查找 PID
// ═══════════════════════════════════════════════════════════════════════════════

pid_t Injector::findPidByName(const char* packageName) {
    // 方式1: 直接扫描 /proc (注入器已以 root 身份运行, 无需 su)
    DIR* dir = opendir("/proc");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            bool isDigit = true;
            for (const char* c = entry->d_name; *c; c++) {
                if (*c < '0' || *c > '9') { isDigit = false; break; }
            }
            if (!isDigit) continue;

            char cmdlinePath[256];
            snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%s/cmdline", entry->d_name);
            FILE* fp = fopen(cmdlinePath, "r");
            if (!fp) continue;

            char cmdline[256] = {0};
            fgets(cmdline, sizeof(cmdline), fp);
            fclose(fp);

            if (strcmp(cmdline, packageName) == 0) {
                pid_t pid = atoi(entry->d_name);
                closedir(dir);
                LOG(LOG_LEVEL_INFO, "[Injector] 找到目标进程: %s -> pid=%d", packageName, pid);
                return pid;
            }
        }
        closedir(dir);
    }

    // 方式2: 回退到 pidof (注入器已是 root, 不需要 su)
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "pidof %s", packageName);
    FILE* fp = popen(cmd, "r");
    if (fp) {
        char buf[256] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            char* save = nullptr;
            for (char* token = strtok_r(buf, " \t\r\n", &save);
                 token != nullptr;
                 token = strtok_r(nullptr, " \t\r\n", &save)) {
                pid_t pid = static_cast<pid_t>(strtol(token, nullptr, 10));
                if (pid <= 0) continue;

                char cmdlinePath[256];
                snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%d/cmdline", pid);
                FILE* cmdlineFp = fopen(cmdlinePath, "r");
                if (!cmdlineFp) continue;

                char cmdline[256] = {0};
                fgets(cmdline, sizeof(cmdline), cmdlineFp);
                fclose(cmdlineFp);

                if (strcmp(cmdline, packageName) == 0) {
                    LOG(LOG_LEVEL_INFO, "[Injector] 找到目标进程: %s -> pid=%d (pidof)", packageName, pid);
                    return pid;
                }
                LOG(LOG_LEVEL_INFO, "[Injector] 跳过 pidof 非主进程: pid=%d cmdline=%s", pid, cmdline);
            }
        } else {
            pclose(fp);
        }
    }

    LOG(LOG_LEVEL_ERROR, "[Injector] 未找到进程: %s", packageName);
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════════════
// injectByPackageName — 完整注入: 查找 PID + ptrace 注入
// ═══════════════════════════════════════════════════════════════════════════════

int Injector::injectByPackageName(const char* packageName, const char* soPath, InjectMode mode) {
    LOG(LOG_LEVEL_INFO, "[Injector] 开始注入 package=%s so=%s mode=%s",
        packageName, soPath,
        mode == MODE_PUBG ? "PUBG" : (mode == MODE_DFM ? "DFM" : (mode == MODE_NRC ? "NRC" : "LOL")));

    pid_t pid = waitForTargetProcessReady(packageName, mode);

    if (pid <= 0) {
        if (mode == MODE_PUBG || mode == MODE_DFM || mode == MODE_NRC) {
            LOG(LOG_LEVEL_ERROR, "[Injector] %s 目标进程未在就绪窗口内稳定并加载 libUE4.so: %s",
                mode == MODE_DFM ? "DFM" : "PUBG", packageName);
        } else {
            LOG(LOG_LEVEL_ERROR, "[Injector] 等待 15 秒后目标进程仍未运行: %s", packageName);
        }
        return -1;
    }

    return injectRemote(pid, soPath, mode);
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif
