#ifndef DFM_ITEM_REGISTRY_H
#define DFM_ITEM_REGISTRY_H

// =====================================================================
//  DFM 物资名注册表 — 运行时累积 + 持久化
//
//  目的: 当 InteractorName(FText) 在某帧成功解析出本地化中文名时, 把
//        (idName → display, className) 三元组写入文件, 下次启动加载,
//        作为快速 lookup 表。多场对局后能积累出几乎完整的物资命名库,
//        无需读取游戏 DataTable 的内部 TMap (反作弊加密)。
//
//  线程安全: 内部用 mutex; record/lookup 可在 Monitor 与 GUI 任一线程调用。
//  持久化路径: 默认 /sdcard/Android/data/com.example.dobbyproject/files/dfm_items.txt
//  文件格式 (UTF-8 行式, 易手工查看/修改):
//      # comment line
//      <idName>\t<display>\t<className>
//
//  自动保存策略: record() 触发 dirty 标志, 每 30s 由 maybeAutoSave 落盘,
//  避免高频 IO; UI 也可手动调 save() 立即写盘。
// =====================================================================

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace dfm {

class ItemRegistry {
public:
    static ItemRegistry& instance();

    // 初始化: 设置存储目录 (一次性), 并尝试加载已有文件。
    // dir 不存在或无写权限时静默失败 — 仍可继续 record(), 仅 save 会失败。
    void init(const std::string& dir);

    // 记录一条解析成功的物资 (display 不能为空)。
    // 若 idName 已存在且 display 与现有不同, 用新值覆盖 (允许语种切换/版本更新)。
    void record(const std::string& idName,
                const std::string& display,
                const std::string& className);

    // 查询: 返回 display 或空串。线程安全。
    std::string lookup(const std::string& idName) const;

    // 按数字 itemId 查询 (用于容器内物品 — 那里只能拿到 cat*10000+seq, 拿不到 FName)。
    // 注册表内若曾以同样数字 ID 的字符串 idName 记录过, 则命中。
    std::string lookupById(int32_t numId) const;

    // 当前条目数。
    size_t size() const;

    // 强制立即写盘。返回 true 表示成功写入。
    bool save();

private:
    struct Entry {
        std::string display;
        std::string className;
    };

    ItemRegistry() = default;

    bool load();                       // 内部: 从 m_path 加载
    void maybeAutoSave();              // record() 内调; 距上次落盘 > 30s 才写

    mutable std::mutex m_mtx;
    std::unordered_map<std::string, Entry> m_map;
    std::unordered_map<int32_t, std::string> m_byId;   // numId → display (反向索引)
    std::string m_path;                // 完整文件路径 (含目录)
    std::atomic<bool> m_dirty{false};
    std::atomic<int64_t> m_lastSaveMs{0};
    std::atomic<bool> m_initialized{false};
};

} // namespace dfm

#endif // DFM_ITEM_REGISTRY_H
