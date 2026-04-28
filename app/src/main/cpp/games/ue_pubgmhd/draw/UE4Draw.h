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

// 雷达 X/Y 坐标的 "未设置" 哨兵值. 因为坐标允许负数, 不能再用 -1 区分,
// 故采用一个不可能的极小值. 任何 <= 该阈值的存储值视为 "用默认位置".
constexpr float kMinimapPosUnset = -100000.0f;

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

// =====================================================================
//  场景物体 (物资/武器/载具/空投) - 由 GUObjectArray 扫描得到
// =====================================================================
enum class DrawWorldObjectKind : uint8_t {
    Item     = 0,  // 普通物资 (背包/护甲/药品/配件等 PickUp)
    Weapon   = 1,  // 地面枪械 PickUp (DefineID.Type 为武器分类)
    Vehicle  = 2,  // 载具 STExtraVehicleBase
    Airdrop  = 3,  // 空投箱
    DeathBox = 4,  // 玩家死亡掉落箱 PlayerTombBox
};

struct DrawWorldObject {
    DrawWorldObjectKind kind = DrawWorldObjectKind::Item;
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    int32_t typeId = 0;       // ItemDefineID.Type / VehicleType
    int32_t subTypeId = 0;    // ItemDefineID.TypeSpecificID
    int32_t healthState = 0;  // 仅载具: ESTExtraVehicleHealthState
    float   fuel = -1.0f;     // 仅载具: 当前油量 (0~100, <0 表示未读)
    bool    inBox = false;    // 仅物资: 在箱子里, 默认过滤除非开启透视
    std::string label;        // 显示名 (中文短名)
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
    std::vector<DrawWorldObject> worldObjects;
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

    /// Writer 端: 移动写入 (热路径用; 避免 vector<PlayerData> 深拷贝)
    void pushData(DrawGameData&& data) {
        std::lock_guard<std::mutex> lock(m_mutex);
        const int writeIdx = 1 - m_frontIndex;
        const bool inMatch = data.inMatch;
        m_buffers[writeIdx] = std::move(data);
        m_frontIndex = writeIdx;
        m_inMatch.store(inMatch, std::memory_order_release);
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
    // ---- 菜单状态 ---- (除 AI 外默认全部关闭)
    bool m_enableESP       = false;    // ESP 方框
    bool m_enableSkeleton  = false;    // 骨架线
    // 骨架颜色 (RGB 0..1) + 线宽; 启用 m_skeletonAutoScale 时远距离自动加粗
    float m_skeletonColor[3] = {0.31f, 1.0f, 1.0f};   // 默认青色
    float m_skeletonThickness = 1.5f;                 // 基础线宽 (像素)
    float m_skelJointRadius   = 3.0f;                 // 关节圆半径
    bool  m_skeletonAutoScale = true;                 // 远距离自动加粗
    bool m_enableSnapline  = false;    // 射线
    bool m_enableHP        = false;    // 血条
    bool m_enableName      = false;    // 名字
    bool m_enableDistance   = false;   // 距离
    bool m_enableTeammate  = false;    // 显示队友
    bool m_enableMinimap   = false;    // 小地图
    bool m_enableFallbackESP = false;  // 投影失败时绘制屏边箭头
    bool m_enablePlayerList = false;   // 玩家坐标/血量调试面板
    bool m_enableTouchPoint = false;   // 手指按下绘制触点
    bool m_enableAimbot     = false;   // 自瞄锁定目标
    bool m_enableItemESP    = false;   // 场景物资 (背包/护甲/药品)
    bool m_enableWeaponESP  = false;   // 地面枪械
    bool m_enableVehicleESP = false;   // 载具
    bool m_enableAirdropESP = false;   // 空投
    bool m_enableDeathBoxESP = false;  // 玩家死亡掉落箱
    bool m_enableBoxContentESP = false; // 透视箱内物品 (开启后空投/死亡箱/多物品包裹里的 PickUp 也绘制)
    float m_worldObjMaxDist = 300.0f;  // 物资/载具最大显示距离 (米)
    float m_minimapSize    = 400.0f;   // 小地图大小
    float m_minimapRangeMeters = 200.0f; // 小地图半径对应的现实距离 (米)
    // 雷达位置 (左上角像素坐标). 支持负数 (使雷达可部分移出屏幕).
    // <= kMinimapPosUnset 表示 "未设置, 使用默认位置".
    float m_minimapPosX    = kMinimapPosUnset;
    float m_minimapPosY    = kMinimapPosUnset;
    bool  m_minimapLocked  = true;     // 锁定: 禁止拖拽,菜单滑块也禁用
    bool  m_menuCollapsed  = false;    // 菜单收起态 (类 DFM 风格)
    // 折叠/展开切换时记录的菜单位置, 保证两个窗口位置一致 (UE 提供视觉延续性)
    ImVec2 m_menuLastPos   = ImVec2(10.0f, 10.0f);
    bool   m_menuPosCaptured = false;
    float m_espMaxDist     = 500.0f;   // ESP 最大显示距离 (米)
    int m_lastPreciseESP   = 0;
    int m_lastFallbackESP  = 0;

    // ---- AI 屏幕检测 (NCNN NanoDet, 通过 screencap 旁路截屏, 无任何 Hook) ----
    bool  m_enableAIDetect = true;              // 总开关 (默认开)
    float m_aiScoreThr     = 0.25f;             // 置信度阈值
    int   m_aiIntervalMs   = 120;               // 推理间隔 (~8 fps)
    bool  m_aiOnlyPerson   = true;              // 仅显示 person 类(COCO 0)
    float m_aiBoxColor[3]  = {1.0f, 0.4f, 0.1f}; // 默认橙红
    float m_aiBoxThickness = 2.0f;
    bool  m_aiDrawScore    = true;

    // ---- AI 辅助瞄准 (仅基于 AI 检测结果, 不读内存) ----
    bool  m_aiAimEnable    = true;              // 总开关 (默认开视觉锁定)
    bool  m_aiAimVisualOnly= true;              // true=仅视觉锁定指示; false=触屏注入
    float m_aiAimFovRadius = 500.0f;            // FOV 圆半径(像素)
    float m_aiAimHeadRatio = 0.18f;             // 目标头部位置占框高度的比例(0=顶,1=底)
    float m_aiAimSmoothing = 0.55f;             // 平滑系数(0=瞬移,1=完全不动)
    float m_aiAimSensitivityX = 1.0f;           // 像素→触屏拖拽 X 灵敏度倍数
    float m_aiAimSensitivityY = 1.0f;           //                         Y
    int   m_aiAimMinScore  = 15;                // 最低置信度(*100, NanoDet 在游戏画面上识别偏低)
    bool  m_aiAimOnlyPerson= true;              // 只锁定 person 类
    bool  m_aiAimRequireTrigger = false;        // 视觉模式下默认不需要按住

    // ---- 子绘制 ----
    void drawMenu(const DrawGameData& data);
    int drawESP(const DrawGameData& data, float screenW, float screenH);
    int drawMinimap(const DrawGameData& data, float screenW, float screenH);
    int drawWorldObjects(const DrawGameData& data, float screenW, float screenH);
    int drawAIDetections(float screenW, float screenH);

    // ---- 工具 ----
    static ImU32 hpColor(float ratio);
    static float distance3D(float x1, float y1, float z1, float x2, float y2, float z2);
    static bool worldToScreen(const DrawGameData& cam, float wx, float wy, float wz,
                              float screenW, float screenH, float& sx, float& sy);
};

} // namespace ue4draw

#endif // UE4_DRAW_H
