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
#include "../Log/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
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
    waitpid(pid, &status, WUNTRACED);

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
// injectRemote — 核心注入函数
// ═══════════════════════════════════════════════════════════════════════════════

int Injector::injectRemote(pid_t pid, const char* soPath, InjectMode mode) {
    LOG(LOG_LEVEL_INFO, "[Injector] 开始注入 pid=%d so=%s mode=%s", pid, soPath, mode == MODE_PUBG ? "PUBG" : "LOL");

    // ── 1. Attach 到目标进程 ──
    if (ptrace(PTRACE_ATTACH, pid, nullptr, nullptr) < 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] PTRACE_ATTACH 失败 (pid=%d): %s", pid, strerror(errno));
        return -1;
    }

    int status = 0;
    waitpid(pid, &status, WUNTRACED);
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
            const char* funcName = "MyStartPointUE4";
            if (ptrace_writedata(pid, remoteMem, funcName, strlen(funcName) + 1) < 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] 写入函数名失败");
                goto cleanup;
            }

            uint64_t dlsymParams[2] = { dlopenResult, remoteMem };
            uint64_t funcAddr = 0;
            if (ptrace_call(pid, remoteDlsymAddr, dlsymParams, 2, &funcAddr) < 0 || funcAddr == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] dlsym 查找 MyStartPointUE4 失败");
                goto cleanup;
            }
            LOG(LOG_LEVEL_INFO, "[Injector] MyStartPointUE4 地址: %llx", (unsigned long long)funcAddr);

            // 获取 libUE4.so 基址
            uint64_t ue4Base = getRemoteModuleBase(pid, "libUE4.so");
            if (ue4Base == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] 无法找到 libUE4.so 基址");
                goto cleanup;
            }
            LOG(LOG_LEVEL_INFO, "[Injector] libUE4.so 基址: %llx", (unsigned long long)ue4Base);

            // 读取 GNames/GUObjectArray/GWorld 指针
            uint64_t pGNames        = ptrace_peekptr(pid, ue4Base + 0x146F9F30);
            uint64_t pGUObjectArray = ue4Base + 0x14706480;  // 结构体地址, 不解引用
            uint64_t pGWorld        = ptrace_peekptr(pid, ue4Base + 0x14988578);

            LOG(LOG_LEVEL_INFO, "[Injector] GNames:        %llx", (unsigned long long)pGNames);
            LOG(LOG_LEVEL_INFO, "[Injector] GUObjectArray: %llx", (unsigned long long)pGUObjectArray);
            LOG(LOG_LEVEL_INFO, "[Injector] GWorld:        %llx", (unsigned long long)pGWorld);

            // 调用 MyStartPointUE4(libUE4Base, pGNames, pGWorld, pGUObjectArray, NULL)
            uint64_t startParams[5] = { ue4Base, pGNames, pGWorld, pGUObjectArray, 0 };
            LOG(LOG_LEVEL_INFO, "[Injector] 调用 MyStartPointUE4...");
            uint64_t startRet = 0;
            if (ptrace_call(pid, funcAddr, startParams, 5, &startRet) < 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] MyStartPointUE4 调用失败");
                goto cleanup;
            }
            LOG(LOG_LEVEL_INFO, "[Injector] ✓ MyStartPointUE4 返回: %lld", (long long)startRet);
        } else {
        // ═══ LOL (il2cpp) 注入路径 ═══
        {
            const char* funcName = "_Z12MyStartPointPvS_S_S_S_";
            if (ptrace_writedata(pid, remoteMem, funcName, strlen(funcName) + 1) < 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] 写入函数名失败");
                goto cleanup;
            }

            uint64_t dlsymParams[2] = { dlopenResult, remoteMem };
            uint64_t myStartPointAddr = 0;
            if (ptrace_call(pid, remoteDlsymAddr, dlsymParams, 2, &myStartPointAddr) < 0 || myStartPointAddr == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] dlsym 查找 MyStartPoint 失败");
                goto cleanup;
            }
            LOG(LOG_LEVEL_INFO, "[Injector] MyStartPoint 地址: %llx", (unsigned long long)myStartPointAddr);

            // ── 7. 获取 libil2cpp.so 基址并读取 IL2CPP 指针 ──
            uint64_t il2cppBase = getRemoteModuleBase(pid, "libil2cpp.so");
            if (il2cppBase == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] 无法找到 libil2cpp.so 基址");
                goto cleanup;
            }
            LOG(LOG_LEVEL_INFO, "[Injector] libil2cpp.so 基址: %llx", (unsigned long long)il2cppBase);

            // 读取 IL2CPP 元数据指针 (与 Frida JS 中的偏移一致)
            uint64_t pCodeRegistration     = ptrace_peekptr(pid, il2cppBase + 0xF45D838);
            uint64_t pMetadataRegistration  = ptrace_peekptr(pid, il2cppBase + 0xF45D840);
            uint64_t pGlobalMetadataHeader  = ptrace_peekptr(pid, il2cppBase + 0xF45D858);
            uint64_t pMetadataImagesTable   = ptrace_peekptr(pid, il2cppBase + 0x1D21140);

            LOG(LOG_LEVEL_INFO, "[Injector] pCodeRegistration:     %llx", (unsigned long long)pCodeRegistration);
            LOG(LOG_LEVEL_INFO, "[Injector] pMetadataRegistration: %llx", (unsigned long long)pMetadataRegistration);
            LOG(LOG_LEVEL_INFO, "[Injector] pGlobalMetadataHeader: %llx", (unsigned long long)pGlobalMetadataHeader);
            LOG(LOG_LEVEL_INFO, "[Injector] pMetadataImagesTable:  %llx", (unsigned long long)pMetadataImagesTable);

            if (pCodeRegistration == 0 || pMetadataRegistration == 0 ||
                pGlobalMetadataHeader == 0 || pMetadataImagesTable == 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] IL2CPP 指针读取失败, 某个值为 0");
                goto cleanup;
            }

            // ── 8. 远程调用 MyStartPoint(il2cppBase, pCodeReg, pMetaReg, pGlobalMeta, pMetaImages) ──
            uint64_t startParams[5] = {
                il2cppBase,
                pCodeRegistration,
                pMetadataRegistration,
                pGlobalMetadataHeader,
                pMetadataImagesTable
            };

            LOG(LOG_LEVEL_INFO, "[Injector] 调用 MyStartPoint...");
            uint64_t startRet = 0;
            if (ptrace_call(pid, myStartPointAddr, startParams, 5, &startRet) < 0) {
                LOG(LOG_LEVEL_ERROR, "[Injector] MyStartPoint 调用失败");
                goto cleanup;
            }
            LOG(LOG_LEVEL_INFO, "[Injector] ✓ MyStartPoint 返回: %lld", (long long)startRet);
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

        result = 0;
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
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            pclose(fp);
            pid_t pid = atoi(buf);
            if (pid > 0) {
                LOG(LOG_LEVEL_INFO, "[Injector] 找到目标进程: %s -> pid=%d (pidof)", packageName, pid);
                return pid;
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
        packageName, soPath, mode == MODE_PUBG ? "PUBG" : "LOL");

    // 等待目标进程启动, 每秒检查一次, 最多等待 15 秒
    pid_t pid = -1;
    for (int i = 0; i < 15; i++) {
        pid = findPidByName(packageName);
        if (pid > 0) break;
        LOG(LOG_LEVEL_INFO, "[Injector] 等待目标进程启动... (%d/15)", i + 1);
        sleep(1);
    }

    if (pid <= 0) {
        LOG(LOG_LEVEL_ERROR, "[Injector] 等待 15 秒后目标进程仍未运行: %s", packageName);
        return -1;
    }

    return injectRemote(pid, soPath, mode);
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif
