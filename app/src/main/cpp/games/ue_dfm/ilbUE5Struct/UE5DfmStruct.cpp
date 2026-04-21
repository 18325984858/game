/**
 * @file    UE5DfmStruct.cpp
 * @brief   UE 5.4 DFM 结构体方法实现
 *          NamePool 条目解析 / 句柄拆分等辅助功能
 */
#include "UE5DfmStruct.h"

namespace ue5dfm {

// ═════════════════════════════════════════════════════════════════════
//  FNameEntryHeader — 名称条目头解析
//
//  Raw 16-bit 布局 (WITHOUT_CASE_PRESERVING_NAME):
//    bit  0      : bIsWide (1=宽字符, 0=ANSI)
//    bit  1..5   : LowercaseProbeHash (5 bits, 哈希探测用)
//    bit  6..15  : Len (10 bits, 字符串长度, 最大 1023)
// ═════════════════════════════════════════════════════════════════════

/** 判断名称是否为宽字符编码 (bit 0) */
bool FNameEntryHeader::isWide() const {
    return Raw & 1;
}

/** 获取字符串长度 (高 10 位, bit 6..15) */
uint16_t FNameEntryHeader::getLen() const {
    return Raw >> 6;
}

/** 获取小写探测哈希 (bit 1..5, 用于 NamePool 哈希桶快速过滤) */
uint16_t FNameEntryHeader::getProbeHash() const {
    return (Raw >> 1) & 0x1F;
}

// ═════════════════════════════════════════════════════════════════════
//  FNameEntry — 名称条目访问
//
//  变长结构:
//    +0x00: FNameEntryHeader (2 bytes)
//    +0x02: AnsiName[] 或 WideName[] (长度由 Header.Len 决定)
//
//  注意: 腾讯 DFM 版 NamePool 对 ANSI payload 做了 NOT+XOR 混淆,
//        读取原始字节后需要额外解码 (见 UE5DfmDumper)
// ═════════════════════════════════════════════════════════════════════

/** 判断是否为宽字符条目 */
bool FNameEntry::isWide() const {
    return Header.isWide();
}

/** 获取名称字符串长度 */
int FNameEntry::getLen() const {
    return Header.getLen();
}

/** 获取 ANSI 名称指针 (紧跟在 Header 之后, +0x02) */
const char* FNameEntry::getAnsiName() const {
    return AnsiName;
}

// ═════════════════════════════════════════════════════════════════════
//  FNameEntryHandle — NamePool 句柄拆分
//
//  FNameEntryId (即 FName.ComparisonIndex) 编码方式:
//    高位 = Block 索引 (在 FNameEntryAllocator.Blocks[] 中的下标)
//    低位 = Offset (块内条目偏移, 乘以 Stride 得到字节偏移)
//
//  DFM 定制: OffsetBits = 18 (标准 UE5.4 = 16)
//    → 每块最多 2^18 = 262144 个条目
//    → Block = id >> 18, Offset = id & 0x3FFFF
// ═════════════════════════════════════════════════════════════════════

/** 将 FNameEntryId 拆分为 Block + Offset 句柄 */
FNameEntryHandle FNameEntryHandle::fromId(uint32_t id, uint32_t offsetBits) {
    FNameEntryHandle h;
    h.Block  = id >> offsetBits;
    h.Offset = id & ((1u << offsetBits) - 1);
    return h;
}

} // namespace ue5dfm
