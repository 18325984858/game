// SPDX-License-Identifier: MIT
//
// Parasite 自检: 拷贝一段最小 PIC 机器码到 cave, 调用一次, 验证返回值正确
// 且执行点 PC 落在 libUE4.so 范围. 用于 D 方案地基的 smoke test.

#include "parasite.h"
#include "../core/log/log.h"

#include <dlfcn.h>
#include <link.h>
#include <cstring>

#define PTAG "[ParasiteTest]"

namespace {

// ARM64 PIC payload: 返回 0xCAFE.
//   movz w0, #0xCAFE  ; 0x52995FC0  -> w0 = 0xCAFE
//   ret               ; 0xD65F03C0
// 字节序: little-endian 32-bit instructions.
alignas(16) const uint8_t kPayloadReturnCafe[] = {
    0xC0, 0x5F, 0x99, 0x52,   // movz w0, #0xCAFE
    0xC0, 0x03, 0x5F, 0xD6,   // ret
};

bool addrInLibUE4(uintptr_t addr) {
    // 先用 dladdr 走快路径 (位于正常 .text 范围); 如果返回 libUE4 则通过.
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(addr), &info) != 0
        && info.dli_fname && strstr(info.dli_fname, "libUE4.so")) {
        return true;
    }
    // 后退路径: cave 在 ELF p_memsz 后、页对齐 mapEnd 前的 padding 区, dladdr
    // 看不到但物理上仍是 libUE4 的 r-x 映射 (/proc/self/maps 同一行).
    return parasite::isInLibUE4Mapping(addr);
}

} // namespace

namespace parasite {

// 自检入口. 编译时调用一次. 返回 true = D 方案地基工作正常.
bool selfTest() {
    // payload 仅 8 字节, 用 sizeof+对齐余量当门槛即可,
    // 不要走 findCave 默认 4096 的整页门槛 (libUE4 r-x 末尾 padding 通常 < 4KB).
    auto cave = findCave(sizeof(kPayloadReturnCafe) + 16);
    if (cave.addr == 0) {
        LOG(LOG_LEVEL_ERROR, PTAG " findCave 失败");
        return false;
    }
    LOG(LOG_LEVEL_INFO, PTAG " cave 0x%lx size=%zu", (unsigned long)cave.addr, cave.size);

    uintptr_t entry = install(kPayloadReturnCafe, sizeof(kPayloadReturnCafe));
    if (entry == 0) {
        LOG(LOG_LEVEL_ERROR, PTAG " install 失败");
        return false;
    }

    if (!addrInLibUE4(entry)) {
        LOG(LOG_LEVEL_ERROR, PTAG " entry 0x%lx 不在 libUE4.so (D 方案核心目标失败)",
            (unsigned long)entry);
        return false;
    }
    LOG(LOG_LEVEL_INFO, PTAG " entry dladdr -> libUE4.so ✓");

    using Fn = unsigned int(*)();
    Fn fn = reinterpret_cast<Fn>(entry);
    unsigned int rv = fn();
    if (rv != 0xCAFE) {
        LOG(LOG_LEVEL_ERROR, PTAG " payload 返回 0x%x, 期望 0xCAFE", rv);
        return false;
    }
    LOG(LOG_LEVEL_INFO, PTAG " ✓ D 方案 smoke test 通过 (entry=0x%lx 返回 0xCAFE)",
        (unsigned long)entry);
    return true;
}

} // namespace parasite

// C 风格导出: 让其它 .cpp 用 extern bool selfTestParasite(); 直接调用,
// 不必引头/不必管 C++ namespace mangling.
extern "C" bool selfTestParasite() {
    return parasite::selfTest();
}
