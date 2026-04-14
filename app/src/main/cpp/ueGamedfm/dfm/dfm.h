#ifndef DFM_H
#define DFM_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <array>
#include <cstring>
#include "../ilbUE5Struct/UE5DfmStruct.h"

// 前向声明
namespace ue5dfminf { class UE5DfmInterface; }

// =====================================================================
//  三角洲行动 (DFM) — 对局状态监控 + 玩家/物资采集 (C++ 原生实现)
//  从 loot_scan.js (Frida) 转写为 C++ 注入代码
//  Target: com.tencent.tmgp.dfm (ARM64 Android, UE5.4 腾讯定制版)
//
//  遍历路径 (来自 IDA + Frida 验证):
//    GWorld(+0xF8)  → PersistentLevel
//    GWorld(+0x140) → GameState
//    Level(+0x98)   → TArray<Actor*> (Actors 数组)
//    Actor(+0x180)  → RootComponent
//    RootComponent(+0x220/224/228) → WorldPos X/Y/Z
//
//  PickupBase (物品拾取):
//    +0xF20 → InventoryIdName (FName)
//    +0xF28 → InventoryType
//    +0xF30 → StackCount (int32)
//
//  GPCharacter (角色):
//    +0x390   → PlayerState*
//    +0x1088  → GPHealthDataComponent* (HealthComp)
//    +0x1718  → CacheCurWeapon (WeaponBase*)
//    HealthComp+0x270 → HealthSet*
//    HealthSet+0x3C   → Health.CurrentValue (float)
//    HealthSet+0x54   → MaxHealth.CurrentValue (float)
//
//  PlayerState:
//    +0x378 → PlayerName (FString: ptr+0, len+8)
//    +0x660 → TeamID (int32)
//    +0x3F8 → PawnPrivate (Character*)
//
//  GameState:
//    +0x388 → PlayerArray (TArray<PlayerState*>)
//    +0x398 → bHasBegunPlay
//    +0x3C0 → ElapsedTime
// =====================================================================

namespace dfm {

// =====================================================================
//  ResolvedOffsets — 通过 UE5DfmInterface 动态查找的偏移
//  优先使用反射系统查询, 回退到 IDA 分析的默认值
//  所有值在 DfmMatchMonitor::initOffsets() 中填充
// =====================================================================
struct ResolvedOffsets {
    // ── Actor (反射可查) ──
    int32_t Actor_RootComponent   = 0x180;   // Actor.RootComponent

    // ── SceneComponent (反射可查) ──
    int32_t Scene_RelativeLocation = 0x168;  // SceneComponent.RelativeLocation
    int32_t Scene_ComponentToWorld = 0x210;  // SceneComponent.ComponentToWorld (FTransform)

    // ── Pawn (反射可查) ──
    int32_t Pawn_PlayerState      = 0x390;   // Pawn.PlayerState

    // ── PlayerState (反射可查) ──
    int32_t PS_PlayerName         = 0x378;   // PlayerState.PlayerName (FString)
    int32_t PS_PawnPrivate        = 0x3F8;   // PlayerState.PawnPrivate

    // ── GameStateBase (反射可查) ──
    int32_t GS_PlayerArray        = 0x388;   // GameStateBase.PlayerArray
    int32_t GS_bHasBegunPlay      = 0x398;   // GameStateBase.bHasBegunPlay
    int32_t GS_ElapsedTime        = 0x3C0;   // GameStateBase.ReplicatedWorldTimeSeconds
    int32_t GS_MatchState         = 0x3B0;   // GameStateBase.MatchState (FName)

    // ── World (反射可查) ──
    int32_t World_PersistentLevel = 0xF8;    // World.PersistentLevel
    int32_t World_GameState       = 0x140;   // World.GameState
    int32_t World_Levels          = 0x158;   // World.Levels (TArray<Level*>)
    int32_t World_StreamingLevels = 0x90;    // World.StreamingLevelsToConsider

    // ── ULevel (反射可查) ──
    int32_t Level_Actors          = 0x98;    // Level.Actors (TArray<Actor*>)

    // ── Actor.Owner (反射可查) ──
    int32_t Actor_Owner           = 0x120;   // Actor.Owner (AActor*)

    // ── DFM 游戏自定义 (IDA-only, 无法反射) ──
    // GPPlayerState
    int32_t PS_TeamID             = 0x660;   // GPPlayerState.TeamID
    int32_t PS_PlayerName2        = 0x470;   // GPPlayerState.PlayerNamePrivate (回退)

    // PickupBase
    int32_t Pickup_InvIdName      = 0xF20;   // PickupBase.InventoryIdName (FName)
    int32_t Pickup_InvType        = 0xF28;   // PickupBase.InventoryType
    int32_t Pickup_StackCount     = 0xF30;   // PickupBase.StackCount

    // GPCharacterBase
    int32_t Char_HealthComp       = 0x1088;  // GPCharacterBase.GPHealthDataComponent*
    int32_t Char_CurWeapon        = 0x1718;  // GPCharacterBase.CacheCurWeapon

    // GPHealthDataComponent
    int32_t HC_HealthMax          = 0x248;   // GPHealthDataComponent.HealthMAX
    int32_t HC_HealthSet          = 0x270;   // GPHealthDataComponent.HealthSet*

    // GPAttributeSetHealth
    int32_t HS_HealthCur          = 0x3C;    // HealthSet.Health.CurrentValue
    int32_t HS_HealthMax          = 0x54;    // HealthSet.MaxHealth.CurrentValue
    int32_t HS_ArmorCur           = 0x74;    // HealthSet.ArmorHealth.CurrentValue
    int32_t HS_HelmetCur          = 0x9C;    // HealthSet.HelmetArmorHealth.CurrentValue

    // Container (物资箱)
    int32_t Cont_RepItemArray     = 0x1C38;  // InventoryPickup_Container.RepItemArray
    int32_t Cont_ItemsOffset      = 0x108;   // RepItemArray 内 TArray 偏移
    int32_t Cont_PickupBoxType    = 0x1C14;
    int32_t Cont_IsEmpty          = 0x2110;

    // SingleItemContainer
    int32_t SIC_CachedPickups     = 0x1058;
    int32_t SIC_BoxId             = 0xFF8;
    int32_t SIC_FirstOpened       = 0x1018;
    int32_t SIC_Finished          = 0x10E0;

    // LevelStreaming
    int32_t Streaming_LoadedLevel = 0x128;

    // LevelActorContainer
    int32_t Container_Actors      = 0x28;

    // InventoryItemInfo 大小
    int32_t ItemInfoSize          = 0x690;

    // ── 相机系统 (SDK dump 确认) ──
    // Controller
    int32_t Ctrl_ControlRotation   = 0x3D8;   // Controller.ControlRotation (FRotator: Pitch/Yaw/Roll)

    // PlayerController
    int32_t PC_PlayerCameraManager = 0x408;   // PlayerController.PlayerCameraManager

    // PlayerCameraManager
    int32_t PCM_DefaultFOV         = 0x388;   // PlayerCameraManager.DefaultFOV
    int32_t PCM_CameraCachePrivate = 0x2B60;  // PlayerCameraManager.CameraCachePrivate (FCameraCacheEntry)

    // FCameraCacheEntry.POV (FMinimalViewInfo) 内部偏移
    // UE5 LargeWorldCoordinates: FVector=double[3], FRotator=double[3]
    int32_t CamCache_LocationX    = 0x08;     // POV.Location.X (double)
    int32_t CamCache_LocationY    = 0x10;     // POV.Location.Y (double)
    int32_t CamCache_LocationZ    = 0x18;     // POV.Location.Z (double)
    int32_t CamCache_RotPitch     = 0x20;     // POV.Rotation.Pitch (double)
    int32_t CamCache_RotYaw       = 0x28;     // POV.Rotation.Yaw (double)
    int32_t CamCache_RotRoll      = 0x30;     // POV.Rotation.Roll (double)
    int32_t CamCache_FOV          = 0x38;     // POV.FOV (float)

    // ── 本地 PlayerController 查找链 (OwningGameInstance → LocalPlayers) ──
    int32_t World_OwningGameInstance = 0x1A8;   // World.OwningGameInstance
    int32_t GI_LocalPlayers          = 0x38;    // GameInstance.LocalPlayers (TArray<ULocalPlayer*>)
    int32_t LP_PlayerController      = 0x30;    // LocalPlayer.PlayerController

    /// 关键偏移是否有效
    bool isValid() const {
        return Actor_RootComponent > 0 && Pawn_PlayerState > 0 && GS_PlayerArray > 0;
    }
};
// =====================================================================
//  FVector3 — 三维坐标
// =====================================================================
struct FVector3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// =====================================================================
//  MatchState — 对局状态
// =====================================================================
struct MatchState {
    std::string state;
    bool inMatch = false;
    uintptr_t gameStatePtr = 0;
    std::string worldName;
    int32_t elapsedTimeSeconds = -1;
};

// =====================================================================
//  HealthInfo — 血量/护甲数据
// =====================================================================
struct HealthInfo {
    float hp = 0.0f;
    float maxHp = 0.0f;
    float armor = 0.0f;
    float helmet = 0.0f;
};

// =====================================================================
//  LootItem — 地面物资信息
// =====================================================================
struct LootItem {
    std::string itemName;
    std::string className;
    int32_t     itemId = 0;
    int32_t     stackCount = 0;
    FVector3    pos;
};

// =====================================================================
//  ContainerItem — 箱内单个物品
// =====================================================================
struct ContainerItem {
    std::string name;
    int32_t     itemId = 0;
    int32_t     count = 0;
    float       durability = 0.0f;
    float       durabilityMax = 0.0f;
};

// =====================================================================
//  ContainerInfo — 物资箱信息
// =====================================================================
struct ContainerInfo {
    std::string className;
    std::string boxType;
    bool        opened = false;
    bool        finished = false;
    FVector3    pos;
    std::vector<ContainerItem> items;
};

// =====================================================================
//  PlayerInfo — 玩家信息
// =====================================================================
struct PlayerInfo {
    std::string playerName;
    std::string className;
    int32_t     teamId = -1;
    bool        isAI = false;
    float       hp = 0.0f;
    float       maxHp = 0.0f;
    float       armor = 0.0f;
    float       helmet = 0.0f;
    std::string weapon;
    FVector3    pos;
    uintptr_t   characterPtr = 0;
};

// =====================================================================
//  Notification — 屏幕通知 (击杀/伤害/新玩家)
// =====================================================================
struct Notification {
    std::string text;
    uint32_t color = 0xFFFFFFFF;  // ABGR (ImU32 compatible)
    float timeLeft = 3.0f;  // 秒
};

// =====================================================================
//  DrawDfmData — 推送给 GUI 的完整帧数据
// =====================================================================
struct DrawDfmData {
    bool inMatch = false;
    std::string worldName;
    std::string matchState;
    int32_t myTeamId = -1;
    FVector3 myPos;

    // 相机数据 (ESP 3D 投影用)
    float camLocX = 0.0f, camLocY = 0.0f, camLocZ = 0.0f;
    float camPitch = 0.0f, camYaw = 0.0f, camRoll = 0.0f;
    float camFOV = 90.0f;

    std::vector<PlayerInfo>    players;
    std::vector<LootItem>      lootItems;
    std::vector<ContainerInfo> containers;

    int32_t aliveCount = 0;
    int32_t totalCount = 0;

    // 通知消息 (击杀/受伤 等)
    std::vector<Notification> notifications;
};

// =====================================================================
//  SharedDfmData — 线程安全的数据共享 (MatchMonitor → GUI)
// =====================================================================
class SharedDfmData {
public:
    static SharedDfmData& getInstance() {
        static SharedDfmData instance;
        return instance;
    }

    void pushData(const DrawDfmData& data);
    void getData(DrawDfmData& outData);
    bool isInMatch() const { return m_inMatch.load(std::memory_order_acquire); }

private:
    SharedDfmData() = default;
    std::mutex m_mutex;
    DrawDfmData m_buffers[2];
    int m_frontIdx = 0;
    std::atomic<bool> m_inMatch{false};
};

// =====================================================================
//  DfmMatchMonitor — 对局状态监控 + 玩家/物资采集 主类
// =====================================================================
class DfmMatchMonitor {
public:
    DfmMatchMonitor(uintptr_t moduleBase, uintptr_t moduleSize,
                    uint32_t offNamePool, uint32_t offGUObjectArrayNum,
                    uint32_t offGUObjectArrayChunks, uint32_t offGWorld,
                    ue5dfminf::UE5DfmInterface* interface = nullptr);
    ~DfmMatchMonitor();

    /// 启动监控线程
    bool start();
    /// 停止监控
    void stop();
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    // ── 安全内存读取 ──
    static uintptr_t safeReadPtr(uintptr_t addr);
    static int32_t   safeReadS32(uintptr_t addr);
    static uint32_t  safeReadU32(uintptr_t addr);
    static uint8_t   safeReadU8(uintptr_t addr);
    static float     safeReadFloat(uintptr_t addr);
    static bool      safeReadMemory(uintptr_t addr, void* out, size_t size);

    // ── FName 解析 (NamePool 混淆解码) ──
    std::string resolveName(uint32_t id) const;
    std::string readFName(uintptr_t addr) const;
    std::string readObjName(uintptr_t objPtr) const;
    std::string readClassName(uintptr_t objPtr) const;
    static uint8_t amask(int length);

    // ── FString 读取 ──
    static std::string readFString(uintptr_t addr);

    // ── 坐标读取 ──
    bool getActorLocation(uintptr_t actorPtr, FVector3& outLoc) const;

    // ── 对局状态 ──
    MatchState getMatchState() const;
    bool checkInMatch() const;

    // ── 角色/物资扫描 ──
    std::vector<uintptr_t> getAllActors() const;
    void scanActors(const std::vector<uintptr_t>& actors, DrawDfmData& outData);
    HealthInfo getCharacterHealth(uintptr_t actorPtr) const;
    std::string getPlayerName(uintptr_t actorPtr) const;
    int32_t getTeamId(uintptr_t actorPtr) const;
    std::string getWeaponName(uintptr_t actorPtr) const;

    // ── PlayerArray 回退 ──
    void scanFromPlayerArray(DrawDfmData& outData) const;

    // ── 物资箱 ──
    ContainerInfo readContainerInfo(uintptr_t actorPtr, const std::string& cn) const;
    std::vector<ContainerItem> readContainerItems(uintptr_t actorPtr, const std::string& cn) const;

    // ── 类名分类 ──
    static bool isPickupClass(const std::string& cn);
    static bool isCharacterClass(const std::string& cn);
    static bool isContainerClass(const std::string& cn);
    static bool isAICharacter(const std::string& cn);

    // ── 物品名映射 ──
    std::string getItemDisplayName(int32_t itemId) const;

    // ── 本地玩家位置 + 相机 ──
    FVector3 getMyPosition() const;
    uintptr_t findLocalPlayerController() const;
    void fillCameraData(DrawDfmData& outData) const;
    static double safeReadDouble(uintptr_t addr);

    // ── 玩家位置快速更新 ──
    void updatePlayerPositions(DrawDfmData& data) const;

    // ── 击杀/变化检测 ──
    void detectChanges(const DrawDfmData& prev, DrawDfmData& curr);

    // ── 轮询线程 ──
    void pollLoop();

    // ── 偏移初始化 (通过 interface 动态查询) ──
    bool initOffsets();

    // ── 成员变量 ──
    ue5dfminf::UE5DfmInterface* m_interface;  // 可选: 反射查询接口
    ResolvedOffsets m_off;                    // 动态解析的偏移
    uintptr_t m_moduleBase;
    uintptr_t m_moduleSize;
    uint32_t  m_offNamePool;
    uint32_t  m_offGUObjectArrayNum;
    uint32_t  m_offGUObjectArrayChunks;
    uint32_t  m_offGWorld;

    mutable uintptr_t m_cachedPC = 0;      // 缓存的本地 PlayerController
    std::atomic<bool> m_running{false};
    std::thread m_pollThread;

    static constexpr int POLL_INTERVAL_MS = 250;
    static constexpr int SCAN_INTERVAL_MS = 5000;
    static constexpr int PLAYER_UPDATE_INTERVAL_MS = 1000;  // 玩家位置快速更新
};

} // namespace dfm

#endif // DFM_H
