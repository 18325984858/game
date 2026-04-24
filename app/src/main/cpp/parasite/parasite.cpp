// SPDX-License-Identifier: MIT
//
// Parasite 实现.
// 见 parasite.h 头注释了解设计.

#include "parasite.h"
#include "../core/log/log.h"

#include <dlfcn.h>
#include <link.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <mutex>

#define PTAG "[Parasite]"

namespace {

struct CaveState {
    uintptr_t base    = 0;  // libUE4.so r-x 段尾部 cave 起始 VA
    size_t    total   = 0;  // cave 容量
    size_t    used    = 0;  // 已分配偏移
    bool      probed  = false;
};

CaveState& state() {
    static CaveState s;
    return s;
}

std::mutex& lock() {
    static std::mutex m;
    return m;
}

constexpr size_t kPageSize = 4096;

inline uintptr_t pageAlignDown(uintptr_t a) { return a & ~(uintptr_t)(kPageSize - 1); }
inline uintptr_t pageAlignUp  (uintptr_t a) { return (a + kPageSize - 1) & ~(uintptr_t)(kPageSize - 1); }

// dl_iterate_phdr 回调上下文: 找到 libUE4.so 的 PT_LOAD r-x 段地址范围.
struct UeRange {
    uintptr_t txtStart = 0;   // r-x 段起始 VA (load_bias + p_vaddr)
    uintptr_t txtEnd   = 0;   // r-x 段虚拟结束 VA
    uintptr_t mapEnd   = 0;   // 映射页对齐结束 (含 padding)
    bool      found    = false;
};

int phdrCb(struct dl_phdr_info* info, size_t /*sz*/, void* data) {
    auto* r = static_cast<UeRange*>(data);
    if (!info->dlpi_name) return 0;
    if (!strstr(info->dlpi_name, "libUE4.so")) return 0;

    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        if ((ph.p_flags & (PF_R | PF_X)) != (PF_R | PF_X)) continue;
        if (ph.p_flags & PF_W) continue; // 只要 r-xp

        r->txtStart = (uintptr_t)info->dlpi_addr + ph.p_vaddr;
        r->txtEnd   = r->txtStart + ph.p_memsz;
        // 映射对齐到下个 PT_LOAD 起始或下一页, 保守取 p_memsz 向上对齐到页.
        r->mapEnd   = pageAlignUp(r->txtEnd);
        r->found    = true;
        return 1; // 命中即停
    }
    return 0;
}

// 从 r-x 段尾部 (txtEnd) 向后扫到 mapEnd, 找连续 0x00 padding 区间.
// 现实里链接器在 .text 之后到下个 PT_LOAD/页对齐边界之间都是 0 填充.
// 注意: 我们直接读这块内存. 如果它已被其它代码占用, 计数会很短, 自然失败.
size_t scanCave(uintptr_t start, uintptr_t end) {
    if (start >= end) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(start);
    size_t n = end - start;
    size_t z = 0;
    for (; z < n; ++z) {
        if (p[z] != 0x00) break;
    }
    return z;
}

bool probeCave() {
    UeRange r;
    if (dl_iterate_phdr(phdrCb, &r) <= 0 || !r.found) {
        LOG(LOG_LEVEL_ERROR, PTAG " 未找到 libUE4.so 的 r-x 段");
        return false;
    }
    LOG(LOG_LEVEL_INFO, PTAG " libUE4 r-x: 0x%lx - 0x%lx (mapEnd=0x%lx)",
        (unsigned long)r.txtStart, (unsigned long)r.txtEnd, (unsigned long)r.mapEnd);

    // cave 起始: r-x 段虚拟结束处. 实际权限仍是 r-x (在同一页内).
    // 但要保证 cave 起点本身页对齐到一个能 mprotect 的 region —
    // 我们改为以 txtEnd 16 字节对齐起点, 扫到 mapEnd.
    uintptr_t caveStart = (r.txtEnd + 15) & ~(uintptr_t)15;
    if (caveStart >= r.mapEnd) {
        LOG(LOG_LEVEL_WARN, PTAG " cave 起点已超出页边界, 没有空隙");
        return false;
    }
    size_t z = scanCave(caveStart, r.mapEnd);
    LOG(LOG_LEVEL_INFO, PTAG " cave 候选 0x%lx, 连续 0 字节=%zu",
        (unsigned long)caveStart, z);
    if (z < 64) {
        LOG(LOG_LEVEL_WARN, PTAG " cave 太小 (%zu < 64), 放弃", z);
        return false;
    }

    auto& s = state();
    s.base  = caveStart;
    s.total = z;
    s.used  = 0;
    return true;
}

// 临时把 cave 所在页改为 RWX 以写入, 写完恢复 R-X.
bool writeWithRwx(uintptr_t dst, const void* src, size_t n) {
    uintptr_t pageStart = pageAlignDown(dst);
    uintptr_t pageEnd   = pageAlignUp(dst + n);
    size_t    span      = pageEnd - pageStart;
    if (mprotect(reinterpret_cast<void*>(pageStart), span, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOG(LOG_LEVEL_ERROR, PTAG " mprotect RWX 失败 errno=%d", errno);
        return false;
    }
    memcpy(reinterpret_cast<void*>(dst), src, n);
    __builtin___clear_cache(reinterpret_cast<char*>(dst),
                            reinterpret_cast<char*>(dst + n));
    if (mprotect(reinterpret_cast<void*>(pageStart), span, PROT_READ | PROT_EXEC) != 0) {
        LOG(LOG_LEVEL_WARN, PTAG " mprotect 恢复 R-X 失败 errno=%d", errno);
        // 已经写完, 不算致命
    }
    return true;
}

} // anonymous namespace

namespace parasite {

Cave findCave(size_t minBytes) {
    std::lock_guard<std::mutex> g(lock());
    auto& s = state();
    if (!s.probed) {
        s.probed = true;
        probeCave();
    }
    if (s.total == 0 || s.total < minBytes) return {0, 0};
    return {s.base, s.total};
}

bool isInLibUE4Mapping(uintptr_t addr) {
    UeRange r;
    if (dl_iterate_phdr(phdrCb, &r) <= 0 || !r.found) return false;
    return addr >= r.txtStart && addr < r.mapEnd;
}

uintptr_t install(const void* code, size_t size) {
    if (!code || size == 0) return 0;
    std::lock_guard<std::mutex> g(lock());
    auto& s = state();
    if (!s.probed) {
        s.probed = true;
        probeCave();
    }
    if (s.base == 0) {
        LOG(LOG_LEVEL_ERROR, PTAG " install: cave 未就绪");
        return 0;
    }
    // 16 字节对齐
    size_t aligned = (size + 15) & ~(size_t)15;
    if (s.used + aligned > s.total) {
        LOG(LOG_LEVEL_ERROR, PTAG " install: cave 不足 (need=%zu used=%zu total=%zu)",
            aligned, s.used, s.total);
        return 0;
    }
    uintptr_t dst = s.base + s.used;
    if (!writeWithRwx(dst, code, size)) return 0;
    s.used += aligned;
    LOG(LOG_LEVEL_INFO, PTAG " install ok: entry=0x%lx size=%zu (used=%zu/%zu)",
        (unsigned long)dst, size, s.used, s.total);
    return dst;
}

void caveStats(size_t* used, size_t* total) {
    std::lock_guard<std::mutex> g(lock());
    auto& s = state();
    if (used)  *used  = s.used;
    if (total) *total = s.total;
}

} // namespace parasite
