#ifndef PUBGMHD_H
#define PUBGMHD_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>
#include <mutex>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cstring>
#include "../engine/UE4Struct.h"
#include "../draw/PubgmhdDraw.h"

// 前向声明
namespace ue4inf { class UE4Interface; }
namespace ue4draw { struct DrawGameData; struct DrawPlayerInfo; struct DrawWorldObject; }

// =====================================================================
//  PUBG Mobile 和平精英 — 对局状态监控 + 玩家坐标采集 (C++ 原生实现)
//  从 frida_match_monitor.js 转写
//  Target: com.tencent.tmgp.pubgmhd (ARM64 Android, UE4.18 腾讯定制版)
//
//  历史错误总结:
//  [BUG-4] BatchMemReader “全有或全无”导致遍历不到数据
//    原因: read() 失败时 continue 跳过整个玩家; 字段超出缓冲返回 0
//    修复: get() 自动回退到 safeReadMemory 单独读取
//  [BUG-5] 指针数组批量读取无回退
//    原因: TArray 内存跨页边界时整块 memcpy 失败, psPtrs 全零
//    修复: 改为逐个 safeReadPtr 读取指针数组
//  [BUG-6] writeMemFloat/writeMemU8 裸写入崩溃
//    原因: 写入已释放的 Actor 内存 → SIGSEGV, s_safeReadActive=0 无法恢复
//    修复: 新增 safeWriteMemory() 与 safeReadMemory 相同的 sigsetjmp 保护
//  [BUG-7] GNames/GUObjectArray 裸指针解引用 (13 处)
//    原因: getNameByIndex/scanCharacters/pollPlayers 中直接 ptr->field
//    修复: 全部改为 safeReadPtr/safeReadS32 + 结构体偏移计算
//  [BUG-8] readFString() 裸解引用 wstr[i]
//    原因: UTF-16 字符串指针被游戏释放后访问 → SIGSEGV
//    修复: safeReadMemory 一次性读取到栈缓冲区
//  [BUG-9] installSafeReadGuard 非线程安全
//    原因: bool 检查+设置无原子性, 多线程可能重复安装信号处理器
//    修复: 改为 std::call_once
//  [BUG-10] 信号处理器链冲突
//    原因: uestart.cpp 调用 pubgmhd 的 safeReadMemory 安装处理器 A,
//          UE4Dumper 又安装处理器 B 覆盖 A → siglongjmp 跳错 jmpbuf
//    修复: uestart 改用 /proc/self/mem pread() 探测, 不涉及信号处理器
//  [BUG-11] 开局网络异常断开 (“网络波动异常”)
//    原因: patchActorNetCull 在 加载/飞机/跳伞 阶段写入 NetCullDistSq
//    修复: 仅在 matchState=="InProgress" 时才允许写入
// =====================================================================

namespace pubgmhd {

// =====================================================================
//  引擎常量
// =====================================================================
static constexpr int POLL_INTERVAL_MS         = 250;
// PLAYER_POLL_INTERVAL_MS: 玩家列表轮询间隔.
//   - 8ms (~125Hz): 频次过高, 被反作弊判定为异常采样.
//   - 16ms (~60Hz): 与游戏帧率上限对齐, 每帧只取一次最新数据;
//     fast path 用 BatchMemReader 把每个玩家 6 次 safeRead 合并为 1 次,
//     总 syscall 量反而降低.
//   - 30ms (~33Hz): 旧值, ESP 在转身/快速移动时有可见抖动.
static constexpr int PLAYER_POLL_INTERVAL_MS  = 16;
static constexpr int MONITOR_IDLE_SLEEP_MS    = 2;
static constexpr int STATE_LOG_INTERVAL_MS    = 1000;
static constexpr int PLAYER_LOG_INTERVAL_MS   = 1000;
// 骨架抓取节流 (per-player): 骨架解析每个玩家每帧上百次跨进程读, 降到 200ms 大幅减压
static constexpr int SKELETON_RESOLVE_INTERVAL_MS = 200;
static constexpr size_t TRACKED_BONE_COUNT    = 17;

// =====================================================================
//  ResolvedOffsets — 通过 UE4Interface 动态查找的游戏特定偏移
//  所有值在 MatchMonitor::initOffsets() 中由反射系统解析填充
// =====================================================================
struct ResolvedOffsets {
    // UWorld
    int32_t World_GameState             = -1;
    int32_t World_AuthorityGameMode     = -1;
    int32_t World_PersistentLevel       = -1;
    int32_t World_ActiveLevelActors     = -1;
    int32_t World_Levels                = -1;

    // ULevel / LevelActorContainer
    int32_t Level_ActorCluster          = -1;
    int32_t LevelActorContainer_Actors  = -1;

    // GameStateBase
    int32_t GS_bHasBegunPlay            = -1;
    int32_t GS_ElapsedTime              = -1;
    int32_t GS_PlayerArray              = -1;
    int32_t GS_MatchState               = -1;  // FName, dump.cs: GameState.MatchState

    // UAEGameState
    int32_t GS_PlayerNum                = -1;
    int32_t GS_TotalPlayerNum           = -1;
    int32_t GS_GameType                 = -1;
    int32_t GS_AlivePlayerNum           = -1;
    int32_t GS_AliveRealPlayerNum       = -1;

    // PlayerState / UAEPlayerState / STExtraPlayerState
    int32_t PS_PlayerName               = -1;
    int32_t PS_PlayerKey                = -1;
    int32_t PS_bAIPlayer                = -1;
    int32_t PS_TeamID                   = -1;
    int32_t PS_Kills                    = -1;
    int32_t PS_LiveState                = -1;
    int32_t PS_CharacterOwner           = -1;
    int32_t PS_PlayerHealth             = -1;
    int32_t PS_PlayerHealthMax          = -1;
    int32_t PS_SelfLocAndRot            = -1;

    // Actor
    int32_t Actor_RootComponent         = -1;
    int32_t Actor_NetCullDistSq         = -1;

    // Pawn
    int32_t Pawn_PlayerState            = -1;

    // SceneComponent
    int32_t SceneComp_ComponentToWorld  = -1;
    int32_t SceneComp_Translation       = -1;

    // UAEPlayerController
    int32_t PC_bIsObserver              = -1;
    int32_t PC_bIsObserverInBattle      = -1;
    int32_t PC_bIsObserverHost          = -1;

    // 本地玩家链路 (World -> GameInstance -> LocalPlayers[0] -> PlayerController)
    int32_t World_OwningGameInstance    = -1;  // World.OwningGameInstance
    int32_t GI_LocalPlayers             = -1;  // GameInstance.LocalPlayers (TArray<ULocalPlayer*>)
    int32_t Player_PlayerController     = -1;  // UPlayer.PlayerController
    int32_t PC_AcknowledgedPawn         = -1;  // PlayerController.AcknowledgedPawn
    int32_t PC_PlayerState              = -1;  // Controller.PlayerState

    // Controller
    int32_t Ctrl_ControlRotation        = -1;  // FRotator (Pitch, Yaw, Roll)
    int32_t STPC_LastFrameCacheControlRotation = -1;
    int32_t STPC_CachedViewControlRotation     = -1;
    int32_t STPC_CurrentActiveCameraCache      = -1;

    // Actor (通用)
    int32_t Actor_Owner                 = -1;  // AActor::Owner

    // PlayerController (相机相关)
    int32_t PC_PlayerCameraManager      = -1;  // PlayerController.PlayerCameraManager

    // PlayerCameraManager
    int32_t PCM_PCOwner                 = -1;
    int32_t PCM_CameraCache             = -1;  // PlayerCameraManager.CameraCache
    int32_t PCM_DefaultFOV              = -1;  // PlayerCameraManager.DefaultFOV

    // Character (UAECharacter / STExtraCharacter / STExtraBaseCharacter)
    int32_t Char_Health                 = -1;
    int32_t Char_HealthMax              = -1;
    int32_t Char_TeamID                 = -1;
    int32_t Char_PlayerKey              = -1;
    int32_t Char_PlayerName             = -1;
    int32_t Char_Mesh                   = -1;
    int32_t Char_bDead                  = -1;
    int32_t Char_bMarkScopeIn           = -1;  // Character.bMarkScopeIn (开镜状态)
    int32_t Char_CurrentNetCullDistSq   = -1;

    // STExtraBaseCharacter
    int32_t STBase_AvatarComponent      = -1;
    int32_t STBase_FPPComp              = -1;
    int32_t STBase_DefaultCharacterMesh = -1;
    int32_t STBase_LastSkeletalMesh     = -1;
    int32_t STBase_STExtraPlayerState   = -1;

    // AvatarComponent
    int32_t Avatar_MasterBoneComponent  = -1;
    int32_t Avatar_SkeletalMeshCompPool = -1;
    int32_t Avatar_MeshComponentList    = -1;
    int32_t Avatar_EntityTickList       = -1;
    int32_t Avatar_AvatarEntityList     = -1;

    // Skeletal / bone chain
    int32_t SkinnedMesh_MasterPoseComponent          = -1;
    int32_t SkinnedMesh_SkeletalMesh                    = -1;
    int32_t SkeletalMeshComp_CachedComponentSpaceTransforms = -1;
    int32_t SkeletalMeshAsset_Skeleton                 = -1;
    int32_t Skeleton_RefBoneNames                      = -1;

    // Character — CharacterMovement 组件指针
    int32_t Char_CharacterMovement      = -1;  // Character.CharacterMovement (UCharacterMovementComponent*)

    // CharacterMovementComponent — 人物移动速度
    int32_t CMC_MaxWalkSpeed            = -1;  // +0x264
    int32_t CMC_MaxWalkSpeedCrouched    = -1;  // +0x268
    int32_t CMC_MaxSwimSpeed            = -1;  // +0x26C
    int32_t CMC_MaxFlySpeed             = -1;  // +0x270
    int32_t CMC_MaxAcceleration         = -1;  // +0x278
    int32_t CMC_GravityScale            = -1;  // +0x20C
    int32_t CMC_JumpZVelocity           = -1;  // +0x214
    int32_t CMC_Velocity                = -1;  // MovementComponent.Velocity +0x13C

    // STExtraShootWeaponBulletBase — 子弹相关
    int32_t Bullet_PMComp               = -1;  // STExtraShootWeaponBulletBase.PMComp
    int32_t Bullet_LaunchGravityScale   = -1;  // STExtraShootWeaponBulletBase.LaunchGravityScale
    int32_t Bullet_MaxNoGravityRange    = -1;  // STExtraShootWeaponBulletBase.MaxNoGravityRange
    int32_t Bullet_ShootDir             = -1;  // STExtraShootWeaponBulletBase.ShootDir

    // ProjectileMovementComponent — 弹道运动
    int32_t PMC_InitialSpeed            = -1;  // +0x164
    int32_t PMC_MaxSpeed                = -1;  // +0x168
    int32_t PMC_Velocity                = -1;  // MovementComponent.Velocity +0x13C
    int32_t PMC_ProjectileGravityScale  = -1;  // +0x180

    // BulletTrackComponent — 后坐力
    int32_t BTC_CurRecoilValue          = -1;
    int32_t BTC_VerticalRecoilTarget    = -1;
    int32_t BTC_HorizontalRecoilTarget  = -1;
    int32_t BTC_VerticalRecoveryTarget  = -1;
    int32_t BTC_PoseRecoilFactor        = -1;
    int32_t BTC_AccessoriesVRecoilFactor = -1;
    int32_t BTC_VerticalRecoilFactorModifier = -1;
    int32_t BTC_AccessoriesHRecoilFactor = -1;
    int32_t BTC_HorizontalRecoilFactorModifier = -1;
    int32_t BTC_AccVerticalRecoilTarget = -1;

    // Weapon — 当前武器
    int32_t Char_CurWeapon                = -1;  // STExtraBaseCharacter.CurWeapon
    int32_t Weapon_BulletTrackComp        = -1;  // STExtraShootWeapon.BulletTrackComp

    // PickUpWrapperActor — 地面物资
    int32_t PickUp_DefineID            = -1;   // 0x6F0 ItemDefineID (Type+0, SpecID+4, bValid+8)
    int32_t PickUp_Count               = -1;   // 0x708 int32
    int32_t PickUp_bHasBeenPickedUp    = -1;   // 0x70C bool
    int32_t PickUp_bIsInBox            = -1;   // 0x70E bool

    // STExtraVehicleBase — 载具
    int32_t Vehicle_VehicleType         = -1;  // 0x7AE uint8 ESTExtraVehicleType
    int32_t Vehicle_VehicleHealthState  = -1;  // 0xB84 uint8 ESTExtraVehicleHealthState
    int32_t Vehicle_TeamID              = -1;  // 0x1594 int32
    int32_t Vehicle_Fuel                = -1;  // 0x3374 float CommonComponent_Fuel

    // PickUpListWrapperActor — 多物品箱 (PickUpWrapperActor 派生)
    int32_t PickUpList_DataList         = -1;  // 0xD98 TArray<PickUpItemData>; stride 0x38
                                                //   - +0x00 ItemDefineID (Type@+0, SubID@+4)
                                                //   - +0x18 int32 Count
                                                //   - +0x30 int32 InstanceID

    /// 所有关键偏移是否已成功解析
    bool isValid() const;
};

// =====================================================================
//  FVector3 — 简单三维坐标
// =====================================================================
struct FVector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// =====================================================================
//  BatchMemReader — 批量内存读取器 (Flyweight 模式, 自动回退)
//
//  设计模式: Flyweight + Facade
//  - 一次 safeReadMemory 调用读取目标结构体到本地缓冲
//  - 缓冲内字段: 零系统调用, 直接 memcpy 提取
//  - 缓冲外字段或批量读取失败: 自动回退到 safeReadMemory 单独读取
//  - 调用者无需关心是否命中缓冲, get() 始终返回正确值
// =====================================================================
class BatchMemReader {
public:
    static constexpr size_t kMaxBatchSize = 2048;

    BatchMemReader() = default;

    /// 从 baseAddr 开始批量读取 size 字节到本地缓冲
    /// 即使返回 false (缓冲失败或超限), get() 仍可通过回退路径正常工作
    bool read(uintptr_t baseAddr, size_t size) {
        m_base = baseAddr;  // 始终记录基址, 用于回退
        m_size = 0;
        m_valid = false;
        if (baseAddr < 0x10000 || size == 0) {
            return false;
        }
        const size_t readSize = (size <= kMaxBatchSize) ? size : kMaxBatchSize;
        m_valid = safeReadMemoryStatic(baseAddr, m_buf, readSize);
        if (m_valid) {
            m_size = readSize;
        }
        return m_valid;
    }

    bool isValid() const { return m_valid; }
    uintptr_t base() const { return m_base; }

    /// 从缓冲中提取指定偏移处的 T 值
    /// 如果偏移超出缓冲范围, 自动回退到单独 safeReadMemory
    template<typename T>
    T get(int32_t offset) const {
        T val{};
        if (offset < 0) return val;
        if (m_valid && static_cast<size_t>(offset) + sizeof(T) <= m_size) {
            // 快速路径: 从本地缓冲提取
            std::memcpy(&val, m_buf + offset, sizeof(T));
        } else if (m_base >= 0x10000) {
            // 回退路径: 单独读取 (缓冲未覆盖或批量读取失败)
            safeReadMemoryStatic(m_base + static_cast<uintptr_t>(offset), &val, sizeof(T));
        }
        return val;
    }

    uintptr_t getPtr(int32_t offset) const { return get<uintptr_t>(offset); }
    int32_t   getS32(int32_t offset) const { return get<int32_t>(offset); }
    uint32_t  getU32(int32_t offset) const { return get<uint32_t>(offset); }
    uint8_t   getU8(int32_t offset)  const { return get<uint8_t>(offset); }
    float     getFloat(int32_t offset) const { return get<float>(offset); }

    /// 提取 FVector3 (三个连续 float)
    FVector3 getVec3(int32_t offset) const {
        FVector3 v;
        v.x = getFloat(offset);
        v.y = getFloat(offset + 4);
        v.z = getFloat(offset + 8);
        return v;
    }

private:
    static bool safeReadMemoryStatic(uintptr_t addr, void* out, size_t size);
    uint8_t   m_buf[kMaxBatchSize]{};
    uintptr_t m_base = 0;
    size_t    m_size = 0;
    bool      m_valid = false;
};

// =====================================================================
//  LiveCameraSnapshot — 渲染线程零延迟相机刷新
//
//  问题: 玩家轮询 (含相机解析) 每 16ms 一次. 渲染线程每帧 (可达 90~120Hz)
//        都要画 ESP, 用的相机姿态可能旧到 16ms. 玩家快速滑屏旋转视角时,
//        ESP 框会明显滞后于目标几像素.
//
//  方案: 慢路径 (poll thread) 一旦选定最佳 ViewInfo, 把 (loc/rot/fov) 三个
//        地址 publish() 到本类. 快路径 (render thread) 每帧调 refresh(),
//        只做 ~7 次 safeReadFloat (~微秒级), 把 DrawGameData 的 cam* 字段
//        刷新到当前 game-tick 的最新值.
// =====================================================================
class LiveCameraSnapshot {
public:
    static LiveCameraSnapshot& instance();

    // 慢路径: 选定相机源后发布地址 (rotOverrideAddr 非 0 表示 rot 来自 controller)
    void publish(uintptr_t locAddr, uintptr_t rotAddr, uintptr_t fovAddr,
                 uintptr_t rotOverrideAddr, float defaultFov);
    void clear();

    // 快路径: 渲染线程在 drawOverlay 前调用; 成功时覆盖 data 的 cam* 字段
    bool refresh(ue4draw::DrawGameData& data) const;

private:
    LiveCameraSnapshot() = default;
    std::atomic<uintptr_t> m_locAddr{0};
    std::atomic<uintptr_t> m_rotAddr{0};
    std::atomic<uintptr_t> m_fovAddr{0};
    std::atomic<uintptr_t> m_rotOverrideAddr{0};
    std::atomic<float>     m_defaultFov{90.0f};
};

enum class PlayerSource : uint8_t {
    PlayerArray = 0,
    CharacterScan = 1,
};

// =====================================================================
//  PlayerNode — 玩家信息节点 (双向链表 + HashMap)
// =====================================================================
struct PlayerNode {
    uint32_t    playerKey = 0;
    int32_t     teamID = 0;
    std::string playerName;
    bool        isAI = false;
    uint8_t     liveState = 0;     // 0=存活, 1=死亡
    float       health = 0.0f;
    float       healthMax = 0.0f;
    int32_t     kills = 0;
    FVector3    pos;
    uintptr_t   characterPtr = 0;
    PlayerSource source = PlayerSource::PlayerArray;
    uint32_t    lastSeenCharacterScanEpoch = 0;

    // 骨骼缓存: 保留最后一次有效的骨骼数据, 防止闪烁
    struct CachedBone { float x, y, z; };
    std::array<CachedBone, 17> cachedBones{};
    uint32_t    cachedBoneMask = 0;
    uint64_t    cachedBoneTimestampMs = 0;  // 最后有效骨骼的时间戳
    uint64_t    lastSkeletonAttemptMs = 0;  // 上次尝试 fillPlayerSkeleton 的时间 (节流)

    PlayerNode* prev = nullptr;
    PlayerNode* next = nullptr;
};

// =====================================================================
//  PlayerList — 玩家双向链表 + 快速查找
// =====================================================================
class PlayerList {
public:
    PlayerList() = default;
    ~PlayerList();

    void clear();
    PlayerNode* findByKey(uint32_t playerKey);
    PlayerNode* upsert(uint32_t playerKey, const PlayerNode& data);
    void remove(uint32_t playerKey);
    int size() const { return m_size; }
    PlayerNode* head() const { return m_head; }

private:
    PlayerNode* m_head = nullptr;
    PlayerNode* m_tail = nullptr;
    int m_size = 0;
    std::unordered_map<uint32_t, PlayerNode*> m_map;
};

// =====================================================================
//  EObserverType — 观战类型枚举
// =====================================================================
enum class EObserverType : int {
    None = 0,               // 普通玩家
    InSpectating = 1,       // 死亡后观战
    GlobalObserver = 2,     // 全局观战
    FriendObserver = 3,     // 好友观战
    Spectator = 4,          // 观众
};

const char* observerTypeName(EObserverType type);

// =====================================================================
//  MatchState — 对局状态信息
// =====================================================================
struct MatchState {
    std::string state;
    bool inMatch = false;
    bool needsPlayerConfirmation = false;
    uintptr_t gameStatePtr = 0;
    std::string worldName;
    int32_t playerArrayNum = -1;
    int32_t elapsedTimeSeconds = -1;
};

// =====================================================================
//  BulletInfo — 子弹速度/重力数据
// =====================================================================
struct BulletInfo {
    bool valid = false;
    float initialSpeed = 0.0f;          // ProjectileMovementComponent.InitialSpeed
    float maxSpeed = 0.0f;              // ProjectileMovementComponent.MaxSpeed
    FVector3 velocity;                  // ProjectileMovementComponent.Velocity
    float projectileGravityScale = 0.0f; // ProjectileMovementComponent.ProjectileGravityScale
    float launchGravityScale = 0.0f;    // STExtraShootWeaponBulletBase.LaunchGravityScale
    int32_t maxNoGravityRange = 0;      // STExtraShootWeaponBulletBase.MaxNoGravityRange
    FVector3 shootDir;                  // STExtraShootWeaponBulletBase.ShootDir
};

// =====================================================================
//  RecoilInfo — 枪械后坐力数据
// =====================================================================
struct RecoilInfo {
    bool valid = false;
    float curRecoilValue = 0.0f;        // BulletTrackComponent.CurRecoilValue
    float verticalRecoilTarget = 0.0f;  // BulletTrackComponent.VerticalRecoilTarget
    float horizontalRecoilTarget = 0.0f; // BulletTrackComponent.HorizontalRecoilTarget
    float verticalRecoveryTarget = 0.0f; // BulletTrackComponent.VerticalRecoveryTarget
    float poseRecoilFactor = 0.0f;      // BulletTrackComponent.PoseRecoilFactor
    float accVRecoilFactor = 0.0f;      // BulletTrackComponent.AccessoriesVRecoilFactor
    float vRecoilFactorModifier = 0.0f; // BulletTrackComponent.VerticalRecoilFactorModifier
    float accHRecoilFactor = 0.0f;      // BulletTrackComponent.AccessoriesHRecoilFactor
    float hRecoilFactorModifier = 0.0f; // BulletTrackComponent.HorizontalRecoilFactorModifier
    float accVerticalRecoilTarget = 0.0f; // BulletTrackComponent.AccVerticalRecoilTarget
};

// =====================================================================
//  CharacterSpeedInfo — 人物移动速度数据
// =====================================================================
struct CharacterSpeedInfo {
    bool valid = false;
    float maxWalkSpeed = 0.0f;          // CharacterMovementComponent.MaxWalkSpeed
    float maxWalkSpeedCrouched = 0.0f;  // CharacterMovementComponent.MaxWalkSpeedCrouched
    float maxSwimSpeed = 0.0f;          // CharacterMovementComponent.MaxSwimSpeed
    float maxFlySpeed = 0.0f;           // CharacterMovementComponent.MaxFlySpeed
    float maxAcceleration = 0.0f;       // CharacterMovementComponent.MaxAcceleration
    float gravityScale = 0.0f;          // CharacterMovementComponent.GravityScale
    float jumpZVelocity = 0.0f;         // CharacterMovementComponent.JumpZVelocity
    FVector3 velocity;                  // MovementComponent.Velocity (当前速度向量)
};

// =====================================================================
//  WeaponBulletParams — 武器子弹参数 (用于弹道物理预测)
// =====================================================================
struct WeaponBulletParams {
    bool valid = false;
    float bulletSpeed = 75000.0f;       // cm/s (默认 ~750 m/s)
    float gravityScale = 1.0f;          // ProjectileMovementComponent.ProjectileGravityScale
    float launchGravityScale = 1.0f;    // STExtraShootWeaponBulletBase.LaunchGravityScale
    float maxNoGravityRange = 0.0f;     // cm, 此范围内不受重力影响
};

// =====================================================================
//  MatchMonitor — 对局状态监控 + 玩家坐标采集 主类
// =====================================================================
class MatchMonitor {
public:
    MatchMonitor(uintptr_t moduleBase, uintptr_t gNames, uintptr_t gWorld,
                 uintptr_t gUObjectArray, uint64_t moduleSize,
                 ue4inf::UE4Interface& interface,
                 const std::string& logDir = "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/",
                 const std::string& logFile = "player_log.txt");
    ~MatchMonitor();

    /// 启动监控 (创建后台轮询线程)
    bool start();

    /// 停止监控
    void stop();

    /// 是否正在运行
    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

private:
    // ---- 安全内存读取 ----
    static uintptr_t safeReadPtr(uintptr_t addr);
    static int32_t   safeReadS32(uintptr_t addr);
    static uint32_t  safeReadU32(uintptr_t addr);
    static uint8_t   safeReadU8(uintptr_t addr);
    static float     safeReadFloat(uintptr_t addr);

    // ---- 安全内存写入 ----
    static bool writeMemU8(uintptr_t addr, uint8_t val);
    static bool writeMemFloat(uintptr_t addr, float val);

    // ---- FName 解析 ----
    std::string getNameByIndex(int index);
    std::string readFName(uintptr_t addr);
    std::string readObjName(uintptr_t objPtr);
    std::string readClassName(uintptr_t objPtr);

    // ---- FString 读取 (UE4 Android: UTF-16LE) ----
    static std::string readFString(uintptr_t addr);

    // ---- 物资真实名称 (调用 libUE4.so 内部 sub_A3B7AC8) ----
    // 在 stack_spoof FP-chain 伪造 + sigsetjmp 熔断保护下调用游戏内部
    // 配置表查名函数, 把 ItemDefineID.TypeSpecificID (itemID) 翻译成
    // 玩家可见的真实物资名 (UTF-16 FString -> UTF-8). 命中/失败均缓存,
    // 同一 itemID 不会重复进入伪造调用窗口.
    bool tryGetItemNameNative(int32_t itemID, std::string& out);

    // 动态解析 sub_A3B7AC8 的运行时地址 (0 表示失败).
    // 优先路径: UE4Interface 反射拿到 ItemUtilsV2::GetItemNameV2 thunk,
    //   再解码 thunk 的第一条 BL imm26 → sub_A3B7AC8 (BP thunk 模板就是
    //   "拆 FFrame 取 itemID → BL 真正查表函数 → 拷 FString 到 Result").
    // 兜底: m_moduleBase + 硬编码 RVA 0xA3B7AC8 (dump.cs 当前版本).
    // 结果永久缓存到 m_itemNameFnAddr; 失败也缓存为 sentinel ~0 不重试.
    uintptr_t resolveItemNameFnAddress();

    // 动态解析物资配置表 UClass 缓存槽 (libUE4.so 内 unk_150ED768) 的运行时
    // 地址 (0 表示失败). 沿调用链解码 ARM64 指令: sub_A3B7AC8 → 第 1 条 BL =
    // sub_981FDB4 → 第 1 条 BL = sub_98322E0 → 第 1 对 ADRP+ADD = 槽地址.
    // 失败回退 m_moduleBase + 硬编码 RVA 0x150ED768. 进程级原子缓存零重试.
    // 该槽用于在 tryGetItemNameNative 入口判断游戏线程是否已自行初始化配置表
    // (零 = 未初始化, 此时调用 sub_A3B7AC8 会触发 sub_AED16BC FindObject
    // 从工作线程发起 UE 反射加载 → SIGSEGV).
    uintptr_t resolveItemConfigCacheSlot();

    // ---- 对局状态 ----
    MatchState getMatchState();

    // 通过遍历 GUObjectArray 定位真实 GameState 实例
    // (腾讯 PUBG 的 World+0xAC0 不指向真 GameState; 必须扫对象表按类匹配)
    // 命中后缓存; 缓存失效或首次/未命中时全表扫, 空扫结果 2s 内不再重复.
    uintptr_t findCurrentGameStateInstance();

    // ---- Actor 位置 ----
    bool getActorLocation(uintptr_t actorPtr, FVector3& outLoc);
    bool fillPlayerSkeleton(uintptr_t characterPtr, ue4draw::DrawPlayerInfo& outPlayer);

    // ---- 类继承链检查 ----
    bool isSubclassOf(uintptr_t classPtr, const char* targetName);

    // ---- 观战类型 ----
    uintptr_t getLocalPlayerController();
    uintptr_t findPlayerCameraManagerInstance();
    EObserverType detectObserverType();
    bool setObserverType(EObserverType type);

    // ---- 网络可见范围修改 ----
    void patchActorNetCull(uintptr_t actorPtr);

    // ---- 内存恢复 (反检测) ----
    void restoreAllModifiedMemory();   // 恢复所有修改过的游戏内存值

    // ---- GUObjectArray 扫描 Character ----
    int scanCharacters();

    // ---- GUObjectArray 扫描场景物体 (物资/载具/空投) ----
    int scanWorldObjects(std::vector<ue4draw::DrawWorldObject>& outObjects);

    // ---- PlayerArray 遍历更新 ----
    int updatePlayerList(uintptr_t gameStatePtr);

    // ---- 高频轻量刷新 ----
    void refreshTrackedPlayersFast();
    void fillCameraSnapshot(ue4draw::DrawGameData& drawData);

    // ---- 子弹/后坐力/速度数据获取 ----
    BulletInfo getBulletInfo(uintptr_t bulletActorPtr);
    RecoilInfo getRecoilInfo(uintptr_t bulletTrackCompPtr);
    CharacterSpeedInfo getCharacterSpeedInfo(uintptr_t characterPtr);

    // ---- 弹道物理模拟 ----
    FVector3 predictBallisticAimPoint(const FVector3& shooterPos, const FVector3& targetPos,
                                       const FVector3& targetVelocity, const WeaponBulletParams& params);
    WeaponBulletParams getLocalWeaponBulletParams();
    FVector3 getTargetVelocity(uintptr_t characterPtr);

    // ---- 自瞄 ----
    void aimAtNearestEnemy();

    // ---- 轮询线程 ----
    void pollMatchStateLoop();
    void pollPlayers();

    // ---- 日志 ----
    void openLog();
    void writeLog(const char* line);
    void writeSkeletonLog(const char* line);
    void writeSkeletonLogf(const char* fmt, ...);
    void closeLog();

    // ---- 偏移解析 ----
    bool initOffsets();

    // ---- 批量读取偏移范围 (initOffsets 后计算) ----
    void computeBatchReadBounds();
    size_t m_psReadSize = 0;     // PlayerState 需要批量读取的字节数
    size_t m_charReadSize = 0;   // Character 需要批量读取的字节数

    struct BoneAssetCacheEntry {
        std::array<int32_t, TRACKED_BONE_COUNT> trackedBoneIndices{};
        int matchedCount = 0;
    };

    bool resolveTrackedBoneIndices(uintptr_t skeletalMeshAssetPtr, BoneAssetCacheEntry& outEntry);
    int matchBoneNamesFromFNameArray(uintptr_t dataPtr, int count, BoneAssetCacheEntry& entry);
    int matchBoneNamesFromBoneInfoArray(uintptr_t dataPtr, int count, int stride, BoneAssetCacheEntry& entry);

    // ---- 成员变量 ----
    ue4inf::UE4Interface& m_interface;  // UE4 反射查询接口
    ResolvedOffsets m_off;              // 动态解析的游戏偏移
    uintptr_t m_moduleBase;
    uintptr_t m_gNames;       // GNames 数组 (已解引用)
    uintptr_t m_gWorld;       // GWorld 全局变量地址
    uintptr_t m_gUObjectArray; // GUObjectArray 全局变量地址
    uint64_t  m_moduleSize;
    int       m_numNames = 0;
    std::string m_logDir;
    std::string m_logFile;

    // 物资查表辅助: resolveItemNameFnAddress 一次性把 (fn, slot) 同步写入,
    // resolveItemConfigCacheSlot 直接从这里读, 避免重复扫描 thunk.
    std::atomic<uintptr_t> m_itemConfigSlotCache{0};

    std::atomic<bool> m_running{false};
    std::thread   m_pollThread;         // 轮询线程 (joinable, 非 detach)

    // GameState 实例发现缓存
    uintptr_t m_cachedGSPtr = 0;                          // 上次命中的 GameState UObject
    std::unordered_map<uintptr_t,bool> m_gsClassSet;      // UClass* -> isSubclassOf(GameStateBase)
    uint64_t  m_lastEmptyGSScanMs = 0;                    // 最近一次空扫时间戳, 用于节流
    uintptr_t m_cachedPCMPtr = 0;                          // PlayerCameraManager 兜底缓存
    std::unordered_map<uintptr_t,bool> m_pcmClassSet;      // UClass* -> isSubclassOf(PlayerCameraManager)
    uint64_t  m_lastPCMScanMs = 0;                         // 最近一次相机管理器空扫时间戳
    std::string   m_lastMatchState;
    std::string   m_currentMatchState;   // 当前对局状态 (InProgress/WaitingToStart/Aircraft 等)
    bool          m_isInMatch = false;
    int32_t       m_myTeamID = -1;
    uint32_t      m_myPlayerKey = 0;
    uintptr_t     m_myPawn = 0;        // 本地玩家 Pawn (AcknowledgedPawn), 兜底自我识别
    int           m_lastReportedArrayNum = -1;
    int           m_lastReportedTotal = -1;
    int32_t       m_currentMatchElapsedSeconds = -1;
    int           m_lastLoadThrottlePhase = -1;
    uint64_t      m_matchEnterTickMs = 0;
    uint64_t      m_lastCharacterScanMs = 0;
    int           m_characterScanChunkIndex = 0;
    int           m_characterScanItemIndex = 0;
    uint32_t      m_characterScanEpoch = 0;
    uint32_t      m_lastCompletedCharacterScanEpoch = 0;

    PlayerList m_playerList;
    std::unordered_map<uintptr_t, uint64_t> m_lastNetCullPatchMs;
    // NetCullDist 原始值存储 (actorPtr -> {origNetCull, origCurrentNetCull, playerKey})
    struct NetCullOriginal {
        float netCullDistSq = 0.0f;
        float currentNetCullDistSq = 0.0f;
        uint32_t playerKey = 0;          // 用于验证地址未被复用
    };
    std::unordered_map<uintptr_t, NetCullOriginal> m_netCullOriginals;
    std::atomic<bool> m_memoryRestored{false};  // 已恢复标志, 原子操作防竞态
    std::unordered_map<uintptr_t, bool> m_characterClassSet;
    std::unordered_map<int, std::string> m_nameCache;
    std::unordered_map<uintptr_t, BoneAssetCacheEntry> m_boneAssetCache;
    uint64_t m_lastBoneCacheClearMs = 0;  // 上次清理骨骼缓存的时间

    // 场景物体扫描缓存 (UClass* -> kind, 0=ignore, 1=PickUp, 2=Vehicle, 3=Airdrop)
    std::unordered_map<uintptr_t, uint8_t> m_worldObjClassSet;
    uint64_t m_lastWorldObjScanMs = 0;
    std::vector<ue4draw::DrawWorldObject> m_cachedWorldObjects;
    // 分片扫描状态: 把全表扫描拆到多次 poll 调用避免阻塞线程
    int32_t m_worldObjScanIndex = 0;
    std::vector<ue4draw::DrawWorldObject> m_worldObjScanBuffer;
    bool    m_worldObjScanInProgress = false;

    // Aimbot (统一使用 SharedUE4Data::isAimbotEnabled() 作为开关)
    int           m_aimbotTargetBone = 3;  // 默认瞄脖子 (TRACKED_BONE_COUNT 索引: 3=neck, 4=head)
    uint32_t      m_aimbotLockedKey = 0;   // 当前锁定目标的 playerKey (0=未锁定)
    float         m_aimbotSmoothing = 8.0f; // 平滑系数 (越大越平滑, 1=瞬移)
    uint64_t      m_lastAimbotWriteMs = 0;  // 上次写入 ControlRotation 的时间 (限频)

    // 弹道预测缓存
    WeaponBulletParams m_cachedBulletParams;
    uint64_t      m_lastBulletParamReadMs = 0;
    std::unordered_map<uintptr_t, WeaponBulletParams> m_weaponParamsCache;  // 按武器指针缓存

    // Aimbot 日志
    FILE*         m_aimbotLogFp = nullptr;
    std::mutex    m_aimbotLogMutex;
    void          openAimbotLog();
    void          writeAimbotLog(const char* fmt, ...);
    void          closeAimbotLog();
    bool          isLocalPlayerScoping();  // 读取本地角色 bMarkScopeIn

    FILE* m_logFp = nullptr;
    int   m_logLineCount = 0;
    std::mutex m_logMutex;
};

} // namespace pubgmhd

#endif // PUBGMHD_H
