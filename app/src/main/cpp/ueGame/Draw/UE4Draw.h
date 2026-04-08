#ifndef UE4_DRAW_H
#define UE4_DRAW_H

#include <imgui/imgui.h>
#include <atomic>
#include <vector>
#include <array>
#include <string>
#include <cstdint>

// =====================================================================
//  UE4 ESP 绘制系统 — PUBG Mobile 玩家坐标绘制 + 控制菜单
// =====================================================================

namespace ue4draw {

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
//  SharedUE4Data — 无锁三缓冲数据桥接 (单例, SPSC lock-free)
//
//  设计模式: Triple Buffer (三缓冲)
//  - Writer(MatchMonitor) 写入 writeSlot, 完成后与 readySlot 原子交换
//  - Reader(GUI线程) 将 readSlot 与 readySlot 原子交换, 然后直接引用 readSlot
//  - 三个 slot 互不重叠, Writer 永远不会写到 Reader 正在读的 buffer
//  - 完全消除 mutex 竞争, Reader 不会阻塞 Writer, 反之亦然
// =====================================================================
class SharedUE4Data {
public:
    static SharedUE4Data& getInstance() {
        static SharedUE4Data instance;
        return instance;
    }
    SharedUE4Data(const SharedUE4Data&) = delete;
    SharedUE4Data& operator=(const SharedUE4Data&) = delete;

    /// Writer 端: 写入最新数据 (仅 MatchMonitor 线程调用)
    void pushData(const DrawGameData& data) {
        m_buffers[m_writeSlot] = data;
        // 将写完的 buffer 提交为 ready, 拿回空闲 buffer 作为下次写入目标
        m_writeSlot = m_readySlot.exchange(m_writeSlot, std::memory_order_acq_rel);
        m_inMatch.store(data.inMatch, std::memory_order_release);
    }

    /// Reader 端: 获取最新数据的只读引用 (仅 GUI 线程调用)
    /// 返回的引用在下一次 acquireRead() 之前保持有效
    const DrawGameData& acquireRead() {
        m_readSlot = m_readySlot.exchange(m_readSlot, std::memory_order_acq_rel);
        return m_buffers[m_readSlot];
    }

    /// 兼容旧接口: 拷贝到 outData
    void getData(DrawGameData& outData) {
        outData = acquireRead();
    }

    bool isInMatch() const { return m_inMatch.load(std::memory_order_acquire); }
    void setInMatch(bool v) { m_inMatch.store(v, std::memory_order_release); }

private:
    SharedUE4Data() = default;
    std::array<DrawGameData, 3> m_buffers{};
    int m_writeSlot = 0;                     // Writer 私有, 无需原子
    int m_readSlot  = 1;                     // Reader 私有, 无需原子
    std::atomic<int> m_readySlot{2};         // Writer/Reader 共享交换点
    std::atomic<bool> m_inMatch{false};
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
    bool m_enableSnapline  = true;     // 射线
    bool m_enableHP        = true;     // 血条
    bool m_enableName      = true;     // 名字
    bool m_enableDistance   = true;     // 距离
    bool m_enableTeammate  = false;    // 显示队友
    bool m_enableMinimap   = true;     // 小地图
    bool m_enableFallbackESP = true;   // 投影失败时绘制屏边箭头
    bool m_enablePlayerList = true;    // 玩家坐标/血量调试面板
    bool m_enableTouchPoint = true;    // 手指按下绘制触点
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
