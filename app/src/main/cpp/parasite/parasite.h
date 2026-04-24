// SPDX-License-Identifier: MIT
//
// Parasite (D 方案 / 寄生执行) 公共 API.
//
// 目标: 把指定 hook 函数物理拷贝进 libUE4.so 的 r-x 段尾部 padding (cave),
//       使其 PC/LR 永远落在 libUE4.so 范围内, 绕过 ACE 周期栈回溯白名单检测.
//
// 当前骨架支持:
//   1. 在 libUE4.so 找 cave (尾部 0 填充 padding ≥ 阈值)
//   2. 把一段 position-independent 机器码 (无 ADRP/ADR/literal load/外部 BL)
//      拷贝到 cave, mprotect r-xp.
//   3. 返回 cave 内的执行入口 (落在 libUE4.so 范围内).
//
// 当前骨架不支持 (业务 hook 需自行解决):
//   - ARM64 PC-relative 指令 (ADRP/ADR/BL/B/CBZ/TBZ/literal LDR) 重定位.
//     拷过去的代码必须是 PIC 且对外只走间接调用 (BLR x16 之类).
//   - 写入 libUE4.so .ARM.exidx 让 _Unwind_VRS 解出 PC 仍在 libUE4. 当前 cave
//     在 libUE4 .text 内, dladdr 已经会返回 libUE4, 但若 ACE 同时校验
//     unwind table 存在 cave entry, 还得再写 .ARM.exidx (留 TODO).
//   - 寄生线程 (借 libUE4 tick callback). 当前留给业务侧手动调用 install
//     拿到 entry 后挂入对应的 hook 点.
//
// 使用方式 (典型):
//   uintptr_t entry = parasite::install(my_payload, my_payload_end - my_payload);
//   if (entry) {
//       // entry 是 cave 内一个可执行地址, 落在 libUE4.so r-x 段, 以函数指针调用即可.
//       reinterpret_cast<void(*)()>(entry)();
//   }

#ifndef PARASITE_H
#define PARASITE_H

#include <cstddef>
#include <cstdint>

namespace parasite {

// 找到 libUE4.so 的 r-x 段尾部 padding, 返回 cave 起始地址 (绝对 VA) 与可用字节数.
// 如果找不到合适 cave, 返回 {0, 0}.
struct Cave {
    uintptr_t addr;
    size_t    size;
};

// 扫描 libUE4.so r-x 段尾部连续 0x00 字节, 长度 >= minBytes 才算合格 cave.
// 默认要求至少 4KB.
Cave findCave(size_t minBytes = 4096);

// 把 [code, code+size) 的 PIC 代码拷贝到 cave, mprotect r-xp, 返回执行入口.
// cave 内会按 16 字节对齐分配, 多次调用累积 (cave 头部已用偏移由内部维护).
// 失败返回 0.
uintptr_t install(const void* code, size_t size);

// 调试: 当前 cave 已用字节数 / 总容量.
void caveStats(size_t* used, size_t* total);

// 判断一个地址是否落在 libUE4.so 的物理 r-x 映射区间内
// (包括尾页 padding, 位于 p_memsz 之后但在 page-aligned mapEnd 之前).
// 这是 cave 合法性的真实准则; dladdr 走 ELF 符号表会误判超出 p_memsz 的
// padding 不属于本库, 不能用.
bool isInLibUE4Mapping(uintptr_t addr);

// 自检: 拷贝一段 PIC 代码 (return 0xCAFE) 到 cave 调用一次, 验证 entry
// dladdr 落在 libUE4.so 且执行结果正确. 见 parasite_test.cpp.
bool selfTest();

} // namespace parasite

#endif // PARASITE_H
