#ifndef UE_NRC_H
#define UE_NRC_H

// =====================================================================
//  NRC (洛克王国手游) — 对局数据结构 / 共享数据 / Match Monitor
//  目标包名: com.tencent.nrc, UE 4.26
//  说明: 当前版本仅提供 SDK Dump 工具, 对局监控未适配实游戏字段,
//        故所有 DrawNrcData 字段均为占位, Monitor 线程仅推送空帧.
// =====================================================================

#include <cstdint>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nrc {

// 单个玩家信息 (占位)
struct PlayerInfo {
    uintptr_t ptr        = 0;
    float     x = 0.f, y = 0.f, z = 0.f;
    float     hp = 0.f, hpMax = 100.f;
    float     dist = 0.f;
    bool      isTeammate = false;
    std::string name;
};

// 单个物资信息 (占位)
struct LootInfo {
    uintptr_t ptr        = 0;
    float     x = 0.f, y = 0.f, z = 0.f;
    float     dist = 0.f;
    std::string name;
};

// 屏幕通知 (占位)
struct Notification {
    std::string text;
    std::chrono::steady_clock::time_point t{};
};

// 主数据包 (镜像 dfm::DrawDfmData 的最小集)
struct DrawNrcData {
    bool       inMatch = false;
    std::string worldName;

    // 摄像机
    float camX = 0.f, camY = 0.f, camZ = 0.f;
    float camFov = 90.f;
    float camRotPitch = 0.f, camRotYaw = 0.f, camRotRoll = 0.f;
    float camMatrix[16] = {};
    bool  camValid = false;

    // 列表
    std::vector<PlayerInfo>   players;
    std::vector<LootInfo>     lootItems;
    std::vector<Notification> notifications;
};

// =====================================================================
//  共享数据单例 (Monitor 写, GUI 读)
// =====================================================================
class SharedNrcData {
public:
    static SharedNrcData& getInstance();

    void pushData(const DrawNrcData& d);
    void getData(DrawNrcData& out) const;

    /// 自上次 push 以来经过的毫秒数, -1 表示从未 push
    int64_t getMsSinceLastPush() const;

private:
    SharedNrcData() = default;
    mutable std::mutex m_mutex;
    DrawNrcData        m_data;
    std::chrono::steady_clock::time_point m_lastPush{};
    bool               m_hasData = false;
};

// =====================================================================
//  对局监控 (占位实现)
// =====================================================================
class NrcMatchMonitor {
public:
    NrcMatchMonitor(uintptr_t moduleBase, uintptr_t moduleSize,
                    uint32_t  offNamePool,
                    uint32_t  offGUObjectArrayNum,
                    uint32_t  offGUObjectArrayChunks,
                    uint32_t  offGWorld);
    ~NrcMatchMonitor();

    bool start();
    void stop();
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    uintptr_t m_moduleBase            = 0;
    uintptr_t m_moduleSize            = 0;
    uint32_t  m_offNamePool           = 0;
    uint32_t  m_offGUObjectArrayNum   = 0;
    uint32_t  m_offGUObjectArrayChunks= 0;
    uint32_t  m_offGWorld             = 0;

    std::atomic<bool> m_running{false};
    std::thread       m_thread;

    void threadMain();
};

} // namespace nrc

#endif // UE_NRC_H
