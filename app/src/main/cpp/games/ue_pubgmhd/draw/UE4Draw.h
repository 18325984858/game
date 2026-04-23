#ifndef UE4_DRAW_H
#define UE4_DRAW_H

#include <imgui/imgui.h>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <array>
#include <string>
#include <cstdint>

// =====================================================================
//  UE4 ESP 绘制系统 — PUBG Mobile 玩家坐标绘制 + 控制菜单
//
//  历史错误总结:
//  [BUG-1] 无锁三缓冲导致堆损坏 (Scudo misaligned pointer)
//    原因: DrawGameData 含 std::vector/std::string, Writer 通过 exchange
//          拿回 buffer 后赋值会析构 Reader 正在引用的容器
//    修复: 回退为 mutex 双缓冲, Reader 在锁内拷贝到本地变量
//  [BUG-2] 三缓冲闪烁 (Writer 无新数据时 Reader ping-pong)
//    原因: acquireRead() 每帧无条件 exchange, 好坏数据交替
//    修复: 已随三缓冲移除而解决
// =====================================================================

namespace ue4draw {

enum class DrawBoneId : uint8_t {
    Pelvis = 0,
    SpineLower,
    SpineUpper,
    Neck,
    Head,
    LeftUpperArm,
    LeftLowerArm,
    LeftHand,
    RightUpperArm,
    RightLowerArm,
    RightHand,
    LeftThigh,
    LeftCalf,
    LeftFoot,
    RightThigh,
    RightCalf,
    RightFoot,
    Count,
};

static constexpr size_t kTrackedBoneCount = static_cast<size_t>(DrawBoneId::Count);

struct DrawBonePoint {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// =====================================================================
//  共享玩家数据 (MatchMonitor -> 渲染线程)
// =====================================================================
struct DrawPlayerInfo {
    uint32_t    playerKey = 0;
    int32_t     teamID = 0;
    std::string playerName;
    bool        isAI = false;
    bool        isAlive = true;
    float       health = 0.0f;
    float       healthMax = 100.0f;
    int32_t     kills = 0;
    float       posX = 0.0f;
    float       posY = 0.0f;
    float       posZ = 0.0f;
    bool        isTeammate = false;
    std::array<DrawBonePoint, kTrackedBoneCount> bones{};
    uint32_t    boneMask = 0;
};

struct DrawGameData {
    bool inMatch = false;
    std::string worldName;
    std::string matchState;
    int32_t myTeamID = -1;
    float myPosX = 0.0f;
    float myPosY = 0.0f;
    float myPosZ = 0.0f;
    // 相机信息 (来自 PlayerCameraManager.CameraCache)
    float camLocX = 0.0f, camLocY = 0.0f, camLocZ = 0.0f;
    float camPitch = 0.0f, camYaw = 0.0f, camRoll = 0.0f;
    float camFOV = 90.0f;
    int aliveCount = 0;
    int totalCount = 0;
    std::vector<DrawPlayerInfo> players;
};

// =====================================================================
//  SharedUE4Data — 线程安全数据桥接 (单例, mutex 保护)
//
//  使用 mutex + 双缓冲: Writer 写后台 buffer 后交换前后台索引
//  Reader 拷贝前台 buffer 到本地后立即释放锁
//  DrawGameData 含 std::vector/std::string, 无锁三缓冲会因
//  赋值时析构正在被 Reader 引用的容器导致堆损坏 (Scudo misaligned ptr)
// =====================================================================
class SharedUE4Data {
    using Clock = std::chrono::steady_clock;
public:
    static SharedUE4Data& getInstance() {
        static SharedUE4Data instance;
        return instance;
    }
    SharedUE4Data(const SharedUE4Data&) = delete;
    SharedUE4Data& operator=(const SharedUE4Data&) = delete;

    /// Writer 端: 写入最新数据 (仅 MatchMonitor 线程调用)
    void pushData(const DrawGameData& data) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const int writeIdx = 1 - m_frontIndex;
        m_buffers[writeIdx] = data;
        m_frontIndex = writeIdx;
        m_inMatch.store(data.inMatch, std::memory_order_release);
        m_lastPushTime.store(Clock::now().time_since_epoch().count(), std::memory_order_release);
    }

    /// Reader 端: 拷贝最新数据到 outData (仅 GUI 线程调用)
    void getData(DrawGameData& outData) {
        std::lock_guard<std::mutex> lock(m_mutex);
        outData = m_buffers[m_frontIndex];
    }

    /// 获取上次 pushData 距今的毫秒数 (用于检测数据源是否存活)
    int64_t msSinceLastPush() const {
        auto last = m_lastPushTime.load(std::memory_order_acquire);
        if (last == 0) return -1;  // 从未推送过
        auto now = Clock::now().time_since_epoch().count();
        auto diff = now - last;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::duration(diff)).count();
    }

    bool isInMatch() const { return m_inMatch.load(std::memory_order_acquire); }
    void setInMatch(bool v) { m_inMatch.store(v, std::memory_order_release); }

    /// GUI ↔ 后台线程: 自瞄开关 (GUI 线程写, MatchMonitor 线程读)
    bool isAimbotEnabled() const { return m_aimbotEnabled.load(std::memory_order_acquire); }
    void setAimbotEnabled(bool v) { m_aimbotEnabled.store(v, std::memory_order_release); }

    /// GUI ↔ 后台线程: 恢复内存请求 (GUI 线程写, MatchMonitor 线程读并清除)
    bool isRestoreRequested() const { return m_restoreRequested.load(std::memory_order_acquire); }
    void requestRestore() { m_restoreRequested.store(true, std::memory_order_release); }
    void clearRestoreRequest() { m_restoreRequested.store(false, std::memory_order_release); }
    bool isMemoryRestored() const { return m_memoryRestored.load(std::memory_order_acquire); }
    void setMemoryRestored(bool v) { m_memoryRestored.store(v, std::memory_order_release); }

private:
    SharedUE4Data() = default;
    std::mutex m_mutex;
    std::array<DrawGameData, 2> m_buffers{};
    int m_frontIndex = 0;
    std::atomic<bool> m_inMatch{false};
    std::atomic<bool> m_aimbotEnabled{false};
    std::atomic<bool> m_restoreRequested{false};
    std::atomic<bool> m_memoryRestored{false};
    std::atomic<int64_t> m_lastPushTime{0};  // Clock::duration::count()
};

// =====================================================================
//  UE4Overlay — ImGui 覆盖层绘制
// =====================================================================
class UE4Overlay {
public:
    void drawOverlay(const DrawGameData& data);

private:
    // ---- 菜单状态 ----
    bool m_menuExpanded    = true;
    bool m_enableESP       = true;     // ESP 方框
    bool m_enableSkeleton  = true;     // 骨架线
    bool m_enableSnapline  = true;     // 射线
    bool m_enableHP        = true;     // 血条
    bool m_enableName      = true;     // 名字
    bool m_enableDistance   = true;     // 距离
    bool m_enableTeammate  = false;    // 显示队友
    bool m_enableMinimap   = true;     // 小地图
    bool m_enableFallbackESP = true;   // 投影失败时绘制屏边箭头
    bool m_enablePlayerList = true;    // 玩家坐标/血量调试面板
    bool m_enableTouchPoint = true;    // 手指按下绘制触点
    bool m_enableAimbot     = false;    // 自瞄锁定目标 (默认关闭)
    float m_minimapSize    = 200.0f;   // 小地图大小
    float m_minimapRangeMeters = 180.0f; // 小地图半径对应的现实距离 (米)
    float m_espMaxDist     = 500.0f;   // ESP 最大显示距离 (米)
    int m_lastPreciseESP   = 0;
    int m_lastFallbackESP  = 0;

    // ---- 子绘制 ----
    void drawMenu(const DrawGameData& data);
    int drawESP(const DrawGameData& data, float screenW, float screenH);
    int drawMinimap(const DrawGameData& data, float screenW, float screenH);

    // ---- 工具 ----
    static ImU32 hpColor(float ratio);
    static float distance3D(float x1, float y1, float z1, float x2, float y2, float z2);
    static bool worldToScreen(const DrawGameData& cam, float wx, float wy, float wz,
                              float screenW, float screenH, float& sx, float& sy);
};

} // namespace ue4draw

#endif // UE4_DRAW_H
