/*
 * ═════════════════════════════════════════════════════════════════════
 *  Injector - 多架构 ptrace 远程注入器 (ARM64 / x86_64 / x86)
 * ═════════════════════════════════════════════════════════════════════
 *
 *  编译为独立可执行文件 (libinjector.so), 通过 su 以 root 身份运行。
 *  用法: /data/local/tmp/injector <package_name> <so_path>
 *
 *  接口说明:
 *    injectByPackageName(pkg, so)
 *      ├─ 每秒轮询 pidof, 最多等待 15 秒找到目标 PID
 *      └─ injectRemote(pid, so)  → 执行完整注入流程
 *
 *    injectRemote(pid, so)
 *      ├─ ptrace(ATTACH)         附加到目标进程
 *      ├─ 保存原始寄存器       用于最终恢复
 *      ├─ 远程 mmap             在目标进程分配 4KB 内存
 *      ├─ 写入 SO 路径           POKEDATA 写入远程内存
 *      ├─ 远程 dlopen            加载 libdobbyproject.so
 *      ├─ 远程 dlsym             查找 _Z12MyStartPointPvS_S_S_S_
 *      ├─ 读取 IL2CPP 指针       从 libil2cpp.so 基址偏移处读取:
 *      │   +0xF45D838 → pCodeRegistration
 *      │   +0xF45D840 → pMetadataRegistration
 *      │   +0xF45D858 → pGlobalMetadataHeader
 *      │   +0x1D21140 → pMetadataImagesTable
 *      ├─ 远程调用 MyStartPoint(base, codeReg, metaReg, globalMeta, metaImages)
 *      ├─ 远程 munmap            释放临时内存
 *      └─ 恢复寄存器 + DETACH     目标进程恢复正常执行
 *
 *  远程函数调用原理 (ARM64):
 *    1. GETREGS 读取寄存器
 *    2. 设置 X0-X7 = 参数, PC = 目标函数地址, LR = 0
 *    3. CONT 让目标进程执行
 *    4. 函数返回时 LR=0 触发 SIGSEGV, waitpid 捕获
 *    5. GETREGS 读取 X0 = 返回值
 *
 *  模块地址计算:
 *    remote_func = (local_func - local_module_base) + remote_module_base
 *    通过解析 /proc/pid/maps 获取模块基址
 *
 * ═════════════════════════════════════════════════════════════════════
 */
#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

namespace Injector {

    /// 注入模式
    enum InjectMode {
        MODE_LOL  = 0,   // lol手游 (il2cpp, 调用 MyStartPoint)
        MODE_PUBG = 1,   // 和平精英 (UE4, 调用 MyStartPointUE4)
    };

    /**
     * 通过 ptrace 将指定 SO 注入到目标进程
     * @param pid         目标进程 PID
     * @param soPath      要注入的 SO 文件绝对路径
     * @param mode        注入模式 (MODE_LOL / MODE_PUBG)
     * @return            0 成功, 负值失败
     */
    int injectRemote(pid_t pid, const char* soPath, InjectMode mode = MODE_LOL);

    /**
     * 通过包名查找目标进程 PID
     * @param packageName 包名 (如 "com.tencent.lolm")
     * @return            PID, 失败返回 -1
     */
    pid_t findPidByName(const char* packageName);

    /**
     * 完整注入流程: 查找进程 + ptrace 注入 SO
     * @param packageName 目标包名
     * @param soPath      SO 文件路径
     * @param mode        注入模式 (MODE_LOL / MODE_PUBG)
     * @return            0 成功, 负值失败
     */
    int injectByPackageName(const char* packageName, const char* soPath, InjectMode mode = MODE_LOL);

} // namespace Injector
