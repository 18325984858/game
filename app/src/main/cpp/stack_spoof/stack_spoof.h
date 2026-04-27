// SPDX-License-Identifier: MIT
//
// ARM64 sleep-mask 风格调用栈伪造 (Call Stack Spoofing) — 公共 API.
//
// 目标:
//   在敏感操作 (Sleep / 内存读写 / 关键 hook 调用) 期间, 让任何 frame-pointer 风格
//   的栈回溯器 (libunwindstack / _Unwind_Backtrace / pthread debug) 看到的栈帧
//   全部落在 libc.so / libart.so 等"白名单"模块内, 看不到本注入 .so 的痕迹.
//
// 思路 (sleep-mask 模式 / 被动栈伪造):
//   1. 进入敏感区前, 在线程当前栈顶之下挖一段连续区域作为 "fake frame chain".
//   2. 按 AArch64 AAPCS 帧指针布局填好 N 层 [saved_FP, saved_LR] 对,
//      每层 LR 指向某模块 r-x 段内一条 BL/BLR 之后的合法返回地址.
//   3. 把当前线程的 x29 (FP) 切换到 fake chain 顶端, 调用敏感函数.
//   4. 出来后立刻还原 x29, 不留痕迹.
//
// 适用场景:
//   - 自调用敏感 API 时主动套壳 (类似 Windows 上 SilentMoonwalk 的 spoofed call).
//   - 在 hook 内部对真实业务函数发起调用, 让调用期间被采样的栈"很干净".
//
// 不适用 / 留 TODO:
//   - 异步信号/被外部 SIGPROF 采样: 这个模板只在主动调用窗口有效, 调用结束 FP 就还原了.
//     如果检测端是 sleep 期间持续轮询, 需要把 FP 切到 fake chain, 然后线程主动 nanosleep,
//     这里提供 spoofed_sleep 的便捷接口.
//   - 基于 .eh_frame DWARF unwind 的检测: ARM64 上很多 unwinder 优先用 DWARF, 不看 FP chain.
//     绕这种检测需要伪造 .eh_frame_hdr, 不在本模板范围.
//   - 同时只能有一个伪造调用窗口 (per-thread). 嵌套伪造请扩展 ThreadSpoofState.

#ifndef STACK_SPOOF_H
#define STACK_SPOOF_H

#include <cstddef>
#include <cstdint>

namespace stack_spoof {

/**
 * @brief 一次性初始化伪造返回地址池。
 *
 * 扫描 libc.so / libart.so / libandroid_runtime.so / libutils.so 的 r-x 段,
 * 识别 BL (`0x94000000`) / BLR (`0xD63F0000`) 指令, 把"BL/BLR 后的下一条指令地址"
 * 收集为合法返回地址池 (供后续伪造的 LR 使用)。
 *
 * @param min_frames  池子至少需要凑齐的合法返回地址条数, 不足则视为初始化失败。
 *                    默认 6 (足以填满默认 8 层 fake chain 的大部分 LR, 重复使用)。
 * @return true  池子里地址数 >= min_frames, 后续 spoof 调用可走伪造分支;
 *         false 只在所有候选模块都没扫到时出现, 此时 call_spoofed 退化为直接调用。
 *
 * @note 内部用 std::call_once 保证仅扫描一次, 重复调用安全且廉价。
 *       建议在 JNI_OnLoad 阶段提前调用, 避免第一次 spoof 时被扫描成本拖慢。
 */
bool init(int min_frames = 6);

/**
 * @brief 在伪造的 FP chain 下调用 fn(arg)。
 *
 * 调用过程:
 *   1) 在当前线程的 fake stack (per-thread, mmap 一页) 中铺好 N 层 [fake_FP, fake_LR];
 *   2) 通过 naked 内联汇编 trampoline 切换 x29 (FP) 到 fake chain 顶端;
 *   3) BLR 跳到 fn(arg);
 *   4) 返回后立刻还原 x29, 不留痕迹。
 *
 * 期间任何顺 FP 链行走的栈回溯器 (libunwindstack / _Unwind_Backtrace / Linux
 * frame-pointer unwinder) 看到的栈帧都会落在 libc.so / libart.so 等白名单模块,
 * 看不到本注入 .so 的痕迹。SP 不切换, 故 fn 内的局部变量仍在真实栈上, 安全可用。
 *
 * @param fn   目标函数指针, 必须遵循 AAPCS 标准 ABI; 不能是依赖调用方 FP chain 的
 *             特殊函数 (绝大多数 C/C++ 函数都满足)。
 * @param arg  透传给 fn 的单参数, fn 内部的语义自定义。
 * @return     fn 的返回值 (按指针截断为 void*); 若 fn 为 NULL 或 init 完全失败,
 *             返回 nullptr / 退化为直接调用 fn。
 *
 * @warning  仅对"主动发起调用"窗口有效。若检测端基于 SIGPROF 异步采样, 退出窗口后
 *           x29 已还原, 那一刻被采样到的仍是真实栈。需要持续伪造, 用 spoofed_sleep_ns
 *           或自行扩展。
 * @warning  同一线程不支持嵌套伪造 (chain_top_fp 单槽), 嵌套需要扩展 ThreadSpoofState。
 */
void* call_spoofed(void* (*fn)(void*), void* arg);

/**
 * @brief sleep-mask 便捷封装: 在伪造 FP chain 下执行 nanosleep。
 *
 * 用于"敏感线程睡眠期间被采样的栈也很干净"场景。等价于:
 * @code
 *   stack_spoof::call_spoofed(thunk_calling_nanosleep, &ns);
 * @endcode
 *
 * @param nanoseconds  睡眠纳秒数 (拆分为 timespec.tv_sec / tv_nsec)。
 * @return  nanosleep 的返回值 (0 表示成功, -1 表示被中断或出错)。
 */
int spoofed_sleep_ns(uint64_t nanoseconds);

} // namespace stack_spoof

// ─────────────────────────────────────────────────────────────────────────────
// extern "C" Frida 友好包装: 这些符号以未混淆名字导出到 libdobbyproject.so,
// 供 Frida JS / 第三方注入器通过 Module.findExportByName 直接绑定 NativeFunction.
// 语义与 stack_spoof:: 命名空间内的同名函数一致。
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

/// 等价 stack_spoof::init(min_frames), 返回 1 / 0。
int stack_spoof_init(int min_frames);

/// 等价 stack_spoof::call_spoofed(fn, arg)。
void* stack_spoof_call(void* (*fn)(void*), void* arg);

/// 等价 stack_spoof::spoofed_sleep_ns(nanoseconds)。
int stack_spoof_sleep_ns(unsigned long long nanoseconds);

} // extern "C"

#endif // STACK_SPOOF_H
