/**
 * dfm_items_baked.h — DFM 物品 ID → 中文名静态映射表
 *
 * 说明:
 *   * DFM 的物品中文名走 UE5 FText StringTable 路径, 运行时无法跨线程读取
 *     (强制 GameThread, 跨线程调内部 API 会 abort).
 *   * 本表是离线收集的 (idName -> 中文显示名) 一对一映射, 烤死编译进 .so.
 *   * 命中时直接返回, 未命中走 getItemDisplayName 大类映射兜底 (旧逻辑).
 *
 * 收集方式:
 *   * 运行时截图见到一个物品就把 (itemId, 中文名) 加进来.
 *   * idName = 11 位数字 ItemID, 也可以同时用 numId (int64) 索引.
 *   * 双向冗余: kBakedById  (string key)  +  kBakedByNumId (int64 key)
 *     reslove 时两边都查, 无论上层来源是 FName string 还是 cat*1e7+seq 整数.
 *
 * 添加规则:
 *   * 一行一条, 留 // 注释指明来源截图日期 / 物品类别, 方便后续维护.
 *   * 不要乱填: 错误的中文会污染 GUI; 不确定就别加.
 */
#ifndef DFM_ITEMS_BAKED_H
#define DFM_ITEMS_BAKED_H

#include <string>
#include <unordered_map>
#include <cstdint>

namespace dfm {

// 11 位 ItemID 字符串 (PickupBase.InventoryIdName FName) → 中文名
inline const std::unordered_map<std::string, const char*>& bakedItemNamesById() {
    static const std::unordered_map<std::string, const char*> kMap = {
        // ── 来源: 2026-05-04/05 截图 ──
        {"15020010021", "军用医疗包"},        // 收集品 - 红色急救包 (idName 来自 pickupFTextProbe)
        {"15020010034", "军用医疗包"},        // 同款不同序号
        {"12886303888", "强效注射器"},        // 容器内
        {"34361244359", "手术剪刀"},          // 容器内 (大类34=??, 实测 +60 注射类)
        // {"180300000031", "..."},          // 旧截图 [枪部件], 待补全
    };
    return kMap;
}

// 数字 ItemID (int64) → 中文名. 与上表内容一致, 但 key 是数字类型,
// 用于 readContainerItems 路径 (那里直接拿到 cat*1e7+seq).
inline const std::unordered_map<int64_t, const char*>& bakedItemNamesByNumId() {
    static const std::unordered_map<int64_t, const char*> kMap = {
        {15020010021LL, "军用医疗包"},
        {15020010034LL, "军用医疗包"},
        {12886303888LL, "强效注射器"},
        {34361244359LL, "手术剪刀"},
    };
    return kMap;
}

// 查询: 优先 numId, 失败回退 string. 命中返回非空 std::string.
inline std::string bakedItemNameLookup(int64_t numId, const std::string& idName) {
    if (numId > 0) {
        const auto& m2 = bakedItemNamesByNumId();
        auto it = m2.find(numId);
        if (it != m2.end()) return it->second;
    }
    if (!idName.empty()) {
        const auto& m1 = bakedItemNamesById();
        auto it = m1.find(idName);
        if (it != m1.end()) return it->second;
    }
    return std::string();
}

} // namespace dfm

#endif // DFM_ITEMS_BAKED_H
