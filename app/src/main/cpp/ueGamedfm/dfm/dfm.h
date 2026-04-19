#ifndef DFM_H
#define DFM_H

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
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
//    Actor(+0x268)  → RootComponent
//    RootComponent(+ComponentToWorld+0x10/0x14/0x18) → WorldPos X/Y/Z
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
    // ── Actor (反射可查; 默认值与 sdk_dump 对齐) ──
    // sdk_dump.txt: bReplicateMovement@0x90 (位字段, bit5), ReplicatedMovement@0x9C, RootComponent@0x180
    int32_t Actor_bReplicateMovement = 0x90;   // Actor.bReplicateMovement (位字段)
    int32_t Actor_ReplicatedMovement = 0x9C;   // Actor.ReplicatedMovement (RepMovement, Size=0x38)
    int32_t Actor_RootComponent      = 0x180;  // Actor.RootComponent (SceneComponent*)

    // ── SceneComponent (反射可查) ──
    int32_t Scene_AttachParent     = 0x108;  // SceneComponent.AttachParent (SceneComponent*) — sdk_dump
    int32_t Scene_RelativeLocation = 0x168;  // SceneComponent.RelativeLocation (EncVector, Size=0x10)
    int32_t Scene_ComponentToWorld = 0x210;  // SceneComponent.ComponentToWorld (FTransform, Translation@+0x10)

    // ── 位置相关字段 ──
    // IntCharacter.CosmeticData_Server_Character_OnFire (+0x810) 内的 CameraViewLoc (+0xE8)
    // 绝对偏移: Actor + 0x810 + 0xE8 = Actor + 0x8F8
    int32_t Char_CameraViewLoc     = 0x8F8;  // IntCharacter 内嵌 NetworkCosmeticData 的 CameraViewLoc (plain Vector)

    // ── 骨骼系统 (SDK dump 确认) ──
    // CHARACTER.Mesh → SkeletalMeshComponent (Pawn.Mesh at CHARACTER+0x3D0)
    int32_t Char_Mesh              = 0x3D0;   // CHARACTER.Mesh (SkeletalMeshComponent*, SDK: 0x3D0)
    int32_t Char_FPPMesh           = 0xA80;   // CharacterBase.FPPMesh (SkeletalMeshComponent*, SDK: 0xA80)
    int32_t STBase_AvatarComponent = 0x3B98;  // STExtraBaseCharacter.AvatarComponent
    int32_t STBase_FPPComp         = 0x4168;  // STExtraBaseCharacter.FPPComp
    int32_t FPPComp_AvatarComp     = 0x380;   // BaseFPPComponent._AvatarComp
    // SkeletalMeshComponent 内部字段 (sdk_dump 验证, 当前 DFM 版本)
    // 旧版默认 (0x7F0/0x710/0x9D8/0x9C8) 与当前 SDK 错位 0x10 ~ 0x108;
    // 关键: Skel_CachedCompSpace 旧值 0x9D8 实际指向 CachedBoneSpaceTransforms
    // (父空间局部 transform), 把局部坐标乘以 ComponentToWorld 后所有骨骼坍缩到
    // mesh 组件原点 → 屏幕上骨架画成一个点。反作弊会剥离这些字段名,
    // TRY_RESOLVE_MULTI 反射查不到时静默保留默认值, 必须把默认值改对。
    int32_t SkinnedMesh_SkeletalMesh = 0x6E8; // SkinnedMeshComponent.SkeletalMesh (sdk_dump: 0x6E8)
    int32_t SkinnedMesh_MasterPoseComponent = 0x714; // SkinnedMeshComponent.MasterPoseComponent (sdk_dump: 0x714)
    int32_t Skel_CachedCompSpace   = 0x9E8;   // SkeletalMeshComponent.CachedComponentSpaceTransforms (sdk_dump: 0x9E8)
    int32_t Skel_BoneSpaceTransforms = 0x9D8; // SkeletalMeshComponent.CachedBoneSpaceTransforms (sdk_dump: 0x9D8)
    int32_t Avatar_MasterBoneComponent = 0x300; // AvatarComponent.MasterBoneComponent
    int32_t Avatar_MeshComponentList = 0x528;   // AvatarComponent.meshComponentList
    int32_t Avatar_AvatarEntityList = 0x880;    // AvatarComponent.AvatarEntityList
    int32_t Avatar_DefaultAvatarSubSystemList = 0xDD0; // AvatarComponent.DefaultAvatarSubSystemList
    int32_t Avatar_LocalSubSystemList = 0xE30;  // AvatarComponent.LocalSubSystemList
    int32_t Avatar_LocalActiveSubSystemList = 0xE40; // AvatarComponent.LocalActiveSubSystemList
    int32_t Avatar_SkeletalMeshCompPool = 0xF10; // AvatarComponent.SkeletalMeshCompPool
    int32_t AvatarFuncBranch_SkeletonMappingComp = 0x68; // AvatarFuncBranch_NewFPP.SkeletonMappingComp

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
    int32_t World_NetDriver       = 0x30;    // World.NetDriver (NetDriver*) — sdk_dump
    int32_t World_DemoNetDriver   = 0xE8;    // World.DemoNetDriver (回放网络)
    // ── NetDriver / NetConnection / ActorChannel (sdk_dump 验证) ──
    // 通过 NetDriver 链路枚举服务端真正复制到客户端的活跃 actor;
    // 解决 Persistent/Streaming Level.Actors[] 只含静态/占位 actor、漏掉
    // 服务器按需复制的真人/AI 的问题。
    int32_t Net_ServerConnection  = 0x88;    // NetDriver.ServerConnection (NetConnection*)
    int32_t Net_ClientConnections = 0x90;    // NetDriver.ClientConnections (TArray<NetConnection*>)
    int32_t NC_OpenChannels       = 0x70;    // NetConnection.OpenChannels (TArray<UChannel*>)
    int32_t AChan_Actor           = 0x70;    // ActorChannel.Actor (AActor*)

    // ── ULevel (反射可查) ──
    int32_t Level_Actors          = 0x98;    // Level.Actors (TArray<Actor*>)

    // ── Actor.Owner (反射可查) ──
    int32_t Actor_Owner           = 0x120;   // Actor.Owner (AActor*)

    // ── DFM 游戏自定义 (IDA-only, 无法反射) ──
    // GPPlayerState (sdk_dump 验证)
    int32_t PS_TeamID             = 0x658;   // GPPlayerState.TeamID — sdk_dump: 0x658
    int32_t PS_PlayerName2        = 0x470;   // GPPlayerState.PlayerNamePrivate (回退)

    // PickupBase (inherits InteractorBase) — sdk_dump 验证
    int32_t Interactor_Name        = 0x790;   // InteractorBase.InteractorName (FText, Size=0x18) — sdk_dump: 0x790
    int32_t Pickup_InvIdName      = 0xF78;   // PickupBase.InventoryIdName (FName) — sdk_dump: 0xF78
    int32_t Pickup_InvType        = 0xF80;   // PickupBase.InventoryType (TSubclassOf<Class>) — sdk_dump: 0xF80
    int32_t Pickup_StackCount     = 0xF88;   // PickupBase.StackCount (int32) — sdk_dump: 0xF88

    // GPCharacterBase (sdk_dump 验证)
    int32_t Char_HealthComp       = 0x1068;  // GPCharacterBase.HealthComp (GPHealthDataComponent*) — sdk_dump: 0x1068
    int32_t Char_CurWeapon        = 0x1708;  // GPCharacterBase.CacheCurWeapon — sdk_dump: 0x1708

    // GPHealthDataComponent (sdk_dump 验证)
    int32_t HC_HealthMax          = 0x258;   // GPHealthDataComponent.HealthMAX — sdk_dump: 0x258
    int32_t HC_HealthSet          = 0x280;   // GPHealthDataComponent.HealthSet* — sdk_dump: 0x280

    // GPAttributeSetHealth
    int32_t HS_HealthCur          = 0x3C;    // HealthSet.Health.CurrentValue
    int32_t HS_HealthMax          = 0x54;    // HealthSet.MaxHealth.CurrentValue
    int32_t HS_ArmorCur           = 0x74;    // HealthSet.ArmorHealth.CurrentValue
    int32_t HS_HelmetCur          = 0x9C;    // HealthSet.HelmetArmorHealth.CurrentValue

    // Container (物资箱) — sdk_dump 验证
    int32_t Cont_RepItemArray     = 0x1DE0;  // InventoryPickup_Container.RepItemArray (ItemArray, Size=0x120) — sdk_dump: 0x1DE0
    int32_t Cont_ItemsOffset      = 0x108;   // RepItemArray 内 TArray 偏移 (内部布局, dump 不直接给出)
    int32_t Cont_PickupBoxType    = 0x1DBC;  // InventoryPickup_Container.PickupBoxType (EPickupBoxType) — sdk_dump: 0x1DBC
    int32_t Cont_ExtraRepInfo     = 0x1DD8;  // InventoryPickup_Container.ExtraRepInfo (bFirstOpen) — sdk_dump: 0x1DD8
    int32_t Cont_IsEmpty          = 0x22D0;  // InventoryPickup_Container.bIsEmpty — sdk_dump: 0x22D0

    // SingleItemContainer (Interactor_SingleItemContainer) — sdk_dump 验证
    int32_t SIC_CachedPickups     = 0x10B0;  // Interactor_SingleItemContainer.CachedPickups — sdk_dump: 0x10B0
    int32_t SIC_BoxId             = 0x1050;  // Interactor_SingleItemContainer.boxId — sdk_dump: 0x1050
    int32_t SIC_FirstOpened       = 0x1070;  // 估算: 旧偏移 0x1018 + 父类增长量 0x58
    int32_t SIC_Finished          = 0x1138;  // 估算: 旧偏移 0x10E0 + 父类增长量 0x58

    // LevelStreaming
    int32_t Streaming_LoadedLevel = 0x128;

    // LevelActorContainer
    int32_t Level_ActorCluster    = 0xC8;    // Level → ActorCluster 容器指针
    int32_t Container_Actors      = 0x28;

    // InventoryItemInfo 大小
    int32_t ItemInfoSize          = 0x690;

    // ── 相机系统 (SDK dump 确认) ──
    // Controller
    int32_t Ctrl_ControlRotation   = 0x3D8;   // Controller.ControlRotation (FRotator: Pitch/Yaw/Roll, Size=0xC)
    int32_t Ctrl_Pawn              = 0x3A0;   // Controller.Pawn (SDK: 0x3A0)
    int32_t Ctrl_PlayerState       = 0x378;   // Controller.PlayerState (SDK: 0x378)

    // PlayerController
    int32_t PC_AcknowledgedPawn    = 0x3F0;   // PlayerController.AcknowledgedPawn (SDK: 0x3F0)
    int32_t PC_PlayerCameraManager = 0x408;   // PlayerController.PlayerCameraManager
    int32_t PC_TargetViewRotation  = 0x41C;   // PlayerController.TargetViewRotation (FRotator, SDK: 0x41C)

    // PlayerCameraManager (SDK: class Size=0x4020)
    int32_t PCM_DefaultFOV         = 0x388;   // PlayerCameraManager.DefaultFOV (float)
    int32_t PCM_CameraCache        = 0x3E0;   // PlayerCameraManager.CameraCache (FCameraCacheEntry, Size=0x9D0)
    int32_t PCM_LastFrameCache     = 0xDB0;   // PlayerCameraManager.LastFrameCameraCache
    int32_t PCM_ViewTarget         = 0x1780;  // PlayerCameraManager.ViewTarget (TViewTarget, Size=0x9E0)
    int32_t PCM_CameraCachePrivate = 0x2B60;  // PlayerCameraManager.CameraCachePrivate (FCameraCacheEntry)
    int32_t PCM_LastFrameCachePriv = 0x3530;  // PlayerCameraManager.LastFrameCameraCachePrivate

    // FCameraCacheEntry 内部偏移 (SDK 确认):
    //   +0x00: float Timestamp
    //   +0x10: MinimalViewInfo POV (Size=0x9C0)
    //     POV+0x00: EncVector Location (0x10) — 加密, 不可直接读
    //     POV+0x10: Rotator Rotation (0xC)  — Pitch/Yaw/Roll float
    //     POV+0x1C: float FOV
    //     POV+0x20: float DesiredFOV
    // CacheEntry 绝对偏移 = 0x10(POV起始) + 字段偏移
    int32_t CamCache_RotPitch     = 0x20;     // CacheEntry+0x10+0x10
    int32_t CamCache_RotYaw       = 0x24;     // CacheEntry+0x10+0x14
    int32_t CamCache_RotRoll      = 0x28;     // CacheEntry+0x10+0x18
    int32_t CamCache_FOV          = 0x2C;     // CacheEntry+0x10+0x1C
    int32_t CamCache_DesiredFOV   = 0x30;     // CacheEntry+0x10+0x20

    // TViewTarget 内部偏移 (SDK 确认):
    //   +0x00: Actor* Target
    //   +0x10: MinimalViewInfo POV (Size=0x9C0) — 同上布局
    int32_t ViewTarget_RotPitch   = 0x20;     // ViewTarget+0x10+0x10
    int32_t ViewTarget_RotYaw     = 0x24;
    int32_t ViewTarget_RotRoll    = 0x28;
    int32_t ViewTarget_FOV        = 0x2C;     // ViewTarget+0x10+0x1C

    // ── 本地 PlayerController 查找链 (OwningGameInstance → LocalPlayers) ──
    int32_t World_OwningGameInstance = 0x190;   // World.OwningGameInstance (SDK: 0x190)
    int32_t GI_LocalPlayers          = 0x38;    // GameInstance.LocalPlayers (TArray<ULocalPlayer*>)
    int32_t LP_PlayerController      = 0x30;    // Player.PlayerController (SDK: 0x30)

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

    // 骨骼位置 (世界坐标, 由 Worker 线程填充)
    // 索引对应 UE5 标准人形骨骼:
    //   0=Head, 1=Neck, 2=Spine3(chest), 3=Spine1(belly),
    //   4=Pelvis(hip), 5=RShoulder, 6=RElbow, 7=RHand,
    //   8=LShoulder, 9=LElbow, 10=LHand,
    //   11=RThigh, 12=RKnee, 13=RFoot,
    //   14=LThigh, 15=LKnee, 16=LFoot
    static constexpr int BONE_COUNT = 17;
    FVector3 bones[BONE_COUNT];
    bool     bonesValid = false;
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

    // 自最后一次 pushData 起经过的毫秒数 (用于检测游戏退出/数据停止)
    // 若从未 push 过, 返回 -1
    int64_t getMsSinceLastPush() const;

private:
    SharedDfmData() = default;
    std::mutex m_mutex;
    DrawDfmData m_buffers[2];
    int m_frontIdx = 0;
    std::atomic<bool> m_inMatch{false};
    std::atomic<int64_t> m_lastPushMs{-1};  // steady_clock 毫秒, -1 = 从未推送
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
    struct ReplicatedMovementSnapshot {
        FVector3 location{};
        uint8_t encByte = 0;
        bool movementEnabled = false;
        bool hasUsablePlainLocation = false;
    };

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
    std::string readFText(uintptr_t addr) const;

    // ── 坐标读取 ──
    bool getActorLocation(uintptr_t actorPtr, FVector3& outLoc) const;
    bool getCharacterSocketLocation(uintptr_t characterPtr, FVector3& outLoc) const;
    ReplicatedMovementSnapshot sampleReplicatedMovement(uintptr_t actorPtr) const;
    bool tryGetCachedActorLocation(uintptr_t actorPtr, FVector3& outLoc) const;
    void rememberActorLocation(uintptr_t actorPtr, const FVector3& location) const;
    bool shouldAcceptRootCompensation(uintptr_t actorPtr, float x, float y) const;

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

    // ── 骨骼系统 ──
    bool fillPlayerBones(PlayerInfo& player) const;
    uintptr_t resolveObjectField(uintptr_t fieldAddr) const;
    uintptr_t followMasterPoseChain(uintptr_t meshComp) const;
    uintptr_t getSkeletalMeshAsset(uintptr_t meshComp) const;
    int32_t collectSparseMapValues(uintptr_t mapBase, std::vector<uintptr_t>& outValues,
                                   int32_t maxEntries) const;
    int32_t collectObjectArrayValues(uintptr_t arrayAddr, std::vector<uintptr_t>& outValues,
                                     int32_t maxEntries) const;
    uintptr_t scanObjectForMeshComponent(uintptr_t objectPtr, int scanBytes) const;
    uintptr_t resolveBestBoneMeshComponent(uintptr_t characterPtr) const;
    int32_t getCachedTransformCount(uintptr_t meshComp) const;
    struct BoneAssetCacheEntry {
        std::array<int32_t, PlayerInfo::BONE_COUNT> trackedBoneIndices{};
        std::array<ue5dfm::FName, PlayerInfo::BONE_COUNT> trackedBoneNames{};
        std::array<uint8_t, PlayerInfo::BONE_COUNT> trackedBoneNameValid{};
        int matchedCount = 0;
    };
    bool getSocketLocationFromMesh(uintptr_t meshComp, const ue5dfm::FName& socketName, FVector3& outLoc) const;
    // 通过 sub_D982DB0 trampoline 解密 SceneComponent.ComponentToWorld。
    // 返回 true 时 outTransform 是 stub 内部 scratch 指向的 FTransform 副本; 调用方
    // 必须立即拷贝, 不要长时间持有 (内部 buffer 会在下次调用时被覆盖)。
    bool decryptSceneComponentToWorld(uintptr_t sceneComp, ue5dfm::FTransform& outTransform) const;
    bool decryptSceneComponentLocation(uintptr_t sceneComp, FVector3& outLoc) const;
    bool resolveTrackedBoneIndices(uintptr_t skeletalMeshAssetPtr, BoneAssetCacheEntry& outEntry) const;
    int matchBoneNamesFromFNameArray(uintptr_t dataPtr, int count, BoneAssetCacheEntry& entry) const;
    int matchBoneNamesFromBoneInfoArray(uintptr_t dataPtr, int count, int stride,
                                        BoneAssetCacheEntry& entry) const;

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
    mutable std::unordered_map<uintptr_t, BoneAssetCacheEntry> m_boneAssetCache;
    // 探测出的 SkeletalMeshComponent.CachedComponentSpaceTransforms 偏移按 mesh 缓存
    mutable std::mutex m_compSpaceOffsetMu;
    mutable std::unordered_map<uintptr_t, int32_t> m_compSpaceOffsetCache;
    struct RootCompensationOwner {
        uintptr_t actorPtr = 0;
        std::chrono::steady_clock::time_point seenAt{};
    };
    mutable std::unordered_map<uintptr_t, FVector3> m_lastKnownActorPositions;
    mutable std::unordered_map<uint64_t, RootCompensationOwner> m_rootCompensationOwners;

    mutable uintptr_t m_cachedPC = 0;      // 缓存的本地 PlayerController
    std::atomic<bool> m_running{false};
    std::thread m_pollThread;

    static constexpr int POLL_INTERVAL_MS = 16;          // 主循环间隔 (~60fps, 相机+位置高频更新)
    static constexpr int SCAN_INTERVAL_MS = 5000;        // 完整 Actor 扫描间隔
    // 玩家位置/骨骼必须每帧刷新, 否则相机一动世界→屏幕投影就漂移 (旧值=1s 前坐标)
    static constexpr int PLAYER_UPDATE_INTERVAL_MS = POLL_INTERVAL_MS;
};

} // namespace dfm

#endif // DFM_H
