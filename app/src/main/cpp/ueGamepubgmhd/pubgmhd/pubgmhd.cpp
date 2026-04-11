#include "pubgmhd.h"
#include "../libUE4Struct/ilbUE4Struct.h"
#include "../interface/interface.h"
#include "../Draw/UE4Draw.h"
#include "../../Log/log.h"

#include <thread>
#include <chrono>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <csetjmp>
#include <cstddef>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

namespace pubgmhd {

using Clock = std::chrono::steady_clock;

namespace {

constexpr uintptr_t kUObjectClassPrivateOffset = 0x10;
constexpr uintptr_t kUObjectNamePrivateOffset = 0x18;
constexpr uintptr_t kUStructSuperStructOffset = 0x30;
constexpr uintptr_t kFNameComparisonIndexOffset = 0x0;
constexpr uintptr_t kFNameNumberOffset = 0x4;
constexpr int kMaxBoneNameCount = 2048;
constexpr int kMinRenderableBoneMatches = 6;

struct RemoteQuat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct RemoteTransform {
    RemoteQuat rotation;
    FVector3 translation;
    float translationPad = 0.0f;
    FVector3 scale3D{1.0f, 1.0f, 1.0f};
    float scalePad = 0.0f;
};
static_assert(sizeof(RemoteTransform) == 0x30, "RemoteTransform size mismatch");

struct RemoteWeakObjectPtr {
    int32_t objectIndex = -1;
    int32_t objectSerialNumber = 0;
};
static_assert(sizeof(RemoteWeakObjectPtr) == 0x8, "RemoteWeakObjectPtr size mismatch");

struct RemoteContainerArray {
    uintptr_t data = 0;
    int32_t num = 0;
    int32_t max = 0;
};
static_assert(sizeof(RemoteContainerArray) == 0x10, "RemoteContainerArray size mismatch");

template<typename T>
bool isUsableRemoteArray(const ue4::TArray<T>& array, int maxNum) {
    const uintptr_t dataPtr = reinterpret_cast<uintptr_t>(array.Data);
    return dataPtr >= 0x10000 && array.Num > 0 && array.Num <= maxNum && array.Max >= array.Num;
}

using BoneAliasList = std::array<const char*, 8>;

const std::array<BoneAliasList, TRACKED_BONE_COUNT> kTrackedBoneAliases = {{
    BoneAliasList{"pelvis", "root", "hips", "bip001pelvis", "bip01pelvis", nullptr, nullptr, nullptr},
    BoneAliasList{"spine01", "spine1", "spine", "bip001spine", "bip01spine", nullptr, nullptr, nullptr},
    BoneAliasList{"spine03", "spine3", "spine02", "spine2", "spine03jnt", "bip001spine1", "bip001spine2", "bip01spine2"},
    BoneAliasList{"neck01", "neck", "neck02", "bip001neck", "bip01neck", nullptr, nullptr, nullptr},
    BoneAliasList{"head", "head01", "head02", "bip001head", "bip01head", nullptr, nullptr, nullptr},
    BoneAliasList{"upperarml", "lupperarm", "leftarm", "leftupperarm", "claviclel", "bip001lupperarm", "bip01lupperarm", nullptr},
    BoneAliasList{"lowerarml", "llowerarm", "leftforearm", "leftlowerarm", "forearml", "bip001lforearm", "bip01lforearm", nullptr},
    BoneAliasList{"handl", "lhand", "lefthand", "bip001lhand", "bip01lhand", nullptr, nullptr, nullptr},
    BoneAliasList{"upperarmr", "rupperarm", "rightarm", "rightupperarm", "clavicler", "bip001rupperarm", "bip01rupperarm", nullptr},
    BoneAliasList{"lowerarmr", "rlowerarm", "rightforearm", "rightlowerarm", "forearmr", "bip001rforearm", "bip01rforearm", nullptr},
    BoneAliasList{"handr", "rhand", "righthand", "bip001rhand", "bip01rhand", nullptr, nullptr, nullptr},
    BoneAliasList{"thighl", "lthigh", "leftupleg", "leftthigh", "bip001lthigh", "bip01lthigh", nullptr, nullptr},
    BoneAliasList{"calfl", "lcalf", "leftleg", "leftlowerleg", "bip001lcalf", "bip01lcalf", nullptr, nullptr},
    BoneAliasList{"footl", "lfoot", "leftfoot", "bip001lfoot", "bip01lfoot", nullptr, nullptr, nullptr},
    BoneAliasList{"thighr", "rthigh", "rightupleg", "rightthigh", "bip001rthigh", "bip01rthigh", nullptr, nullptr},
    BoneAliasList{"calfr", "rcalf", "rightleg", "rightlowerleg", "bip001rcalf", "bip01rcalf", nullptr, nullptr},
    BoneAliasList{"footr", "rfoot", "rightfoot", "bip001rfoot", "bip01rfoot", nullptr, nullptr, nullptr},
}};

static thread_local sigjmp_buf s_safeReadJmpBuf;
static thread_local volatile sig_atomic_t s_safeReadActive = 0;
static struct sigaction s_oldSigsegvAction{};
static struct sigaction s_oldSigbusAction{};
static std::once_flag s_safeReadGuardOnce;

void safeReadSignalHandler(int sig, siginfo_t* info, void* ctx) {
    if (s_safeReadActive) {
        s_safeReadActive = 0;
        siglongjmp(s_safeReadJmpBuf, sig);
    }

    struct sigaction* old = (sig == SIGSEGV) ? &s_oldSigsegvAction : &s_oldSigbusAction;
    if ((old->sa_flags & SA_SIGINFO) != 0) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

void installSafeReadGuard() {
    std::call_once(s_safeReadGuardOnce, []() {
        struct sigaction sa{};
        sa.sa_sigaction = safeReadSignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &s_oldSigsegvAction);
        sigaction(SIGBUS, &sa, &s_oldSigbusAction);
    });
}

bool safeReadMemory(uintptr_t addr, void* out, size_t size) {
    if (out == nullptr || size == 0 || addr < 0x10000) {
        return false;
    }

    installSafeReadGuard();
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
        s_safeReadActive = 0;
        return false;
    }

    s_safeReadActive = 1;
    memcpy(out, reinterpret_cast<const void*>(addr), size);
    s_safeReadActive = 0;
    return true;
}

// 安全写入: 与 safeReadMemory 同样的 sigsetjmp 保护
// 写入已释放的 Actor 内存时捕获 SIGSEGV/SIGBUS 而非崩溃
bool safeWriteMemory(uintptr_t addr, const void* src, size_t size) {
    if (src == nullptr || size == 0 || addr < 0x10000) {
        return false;
    }

    installSafeReadGuard();
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
        s_safeReadActive = 0;
        return false;
    }

    s_safeReadActive = 1;
    memcpy(reinterpret_cast<void*>(addr), src, size);
    s_safeReadActive = 0;
    return true;
}

bool isFiniteVector(const FVector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

FVector3 crossProduct(const FVector3& lhs, const FVector3& rhs) {
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

FVector3 scaleVector(const FVector3& value, const FVector3& scale) {
    return {value.x * scale.x, value.y * scale.y, value.z * scale.z};
}

FVector3 rotateVector(const RemoteQuat& rotation, const FVector3& value) {
    const FVector3 quatVector{rotation.x, rotation.y, rotation.z};
    const FVector3 uv = crossProduct(quatVector, value);
    const FVector3 uuv = crossProduct(quatVector, uv);
    return {
        value.x + ((uv.x * rotation.w) + uuv.x) * 2.0f,
        value.y + ((uv.y * rotation.w) + uuv.y) * 2.0f,
        value.z + ((uv.z * rotation.w) + uuv.z) * 2.0f,
    };
}

FVector3 transformPosition(const RemoteTransform& transform, const FVector3& localPosition) {
    FVector3 safeScale = transform.scale3D;
    if (!std::isfinite(safeScale.x) || std::fabs(safeScale.x) < 0.0001f) safeScale.x = 1.0f;
    if (!std::isfinite(safeScale.y) || std::fabs(safeScale.y) < 0.0001f) safeScale.y = 1.0f;
    if (!std::isfinite(safeScale.z) || std::fabs(safeScale.z) < 0.0001f) safeScale.z = 1.0f;

    const FVector3 scaled = scaleVector(localPosition, safeScale);
    const FVector3 rotated = rotateVector(transform.rotation, scaled);
    return {
        rotated.x + transform.translation.x,
        rotated.y + transform.translation.y,
        rotated.z + transform.translation.z,
    };
}

std::string normalizeBoneName(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        if (!std::isalnum(ch)) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

bool endsWithText(const std::string& value, const char* suffix) {
    if (nullptr == suffix) {
        return false;
    }
    const size_t suffixLength = std::strlen(suffix);
    return value.size() >= suffixLength
        && value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
}

bool matchesTrackedBoneName(size_t slot, const std::string& normalizedName) {
    for (const char* alias : kTrackedBoneAliases[slot]) {
        if (nullptr == alias) {
            break;
        }
        if (normalizedName == alias
            || endsWithText(normalizedName, alias)
            || (std::strlen(alias) >= 6 && normalizedName.find(alias) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

bool hasUsablePlayerPosition(const FVector3& value) {
    return isFiniteVector(value)
        && (std::fabs(value.x) > 1.0f || std::fabs(value.y) > 1.0f || std::fabs(value.z) > 1.0f)
        && std::fabs(value.x) < 1.0e8f
        && std::fabs(value.y) < 1.0e8f
        && std::fabs(value.z) < 1.0e8f;
}

bool isPlayerInNormalState(const PlayerNode& player) {
    if (player.playerKey == 0) {
        return false;
    }
    if (player.liveState != 0) {
        return false;
    }
    if (!std::isfinite(player.health)) {
        return false;
    }
    if (player.health <= 0.0f) {
        return false;
    }
    return true;
}

constexpr auto kSlowPlayerRefreshInterval = std::chrono::milliseconds(120);
enum class LoadThrottlePhase : int {
    Early = 0,
    Transition = 1,
    Normal = 2,
};

constexpr int32_t kEarlyLoadPhaseSeconds = 120;
constexpr int32_t kTransitionLoadPhaseSeconds = 180;
constexpr auto kTransitionSlowPlayerRefreshInterval = std::chrono::milliseconds(160);
constexpr auto kEarlySlowPlayerRefreshInterval = std::chrono::milliseconds(220);

uint64_t nowMonotonicMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

LoadThrottlePhase getLoadThrottlePhase(int32_t matchElapsedSeconds) {
    if (matchElapsedSeconds < 0 || matchElapsedSeconds < kEarlyLoadPhaseSeconds) {
        return LoadThrottlePhase::Early;
    }
    if (matchElapsedSeconds < kTransitionLoadPhaseSeconds) {
        return LoadThrottlePhase::Transition;
    }
    return LoadThrottlePhase::Normal;
}

const char* loadThrottlePhaseName(LoadThrottlePhase phase) {
    switch (phase) {
        case LoadThrottlePhase::Early:
            return "EARLY";
        case LoadThrottlePhase::Transition:
            return "TRANSITION";
        case LoadThrottlePhase::Normal:
            return "NORMAL";
        default:
            return "UNKNOWN";
    }
}

std::chrono::milliseconds getSlowRefreshInterval(int32_t matchElapsedSeconds) {
    switch (getLoadThrottlePhase(matchElapsedSeconds)) {
        case LoadThrottlePhase::Early:
            return kEarlySlowPlayerRefreshInterval;
        case LoadThrottlePhase::Transition:
            return kTransitionSlowPlayerRefreshInterval;
        case LoadThrottlePhase::Normal:
        default:
            return kSlowPlayerRefreshInterval;
    }
}

int getCharacterScanBudget(int32_t matchElapsedSeconds) {
    switch (getLoadThrottlePhase(matchElapsedSeconds)) {
        case LoadThrottlePhase::Early:
            return 0;
        case LoadThrottlePhase::Transition:
            return 32768;
        case LoadThrottlePhase::Normal:
        default:
            return 65536;
    }
}

int getCharacterScanIntervalMs(int32_t matchElapsedSeconds) {
    switch (getLoadThrottlePhase(matchElapsedSeconds)) {
        case LoadThrottlePhase::Early:
            return -1;
        case LoadThrottlePhase::Transition:
            return 800;
        case LoadThrottlePhase::Normal:
        default:
            return 0;
    }
}

uint64_t getNetCullPatchIntervalMs(int32_t matchElapsedSeconds) {
    switch (getLoadThrottlePhase(matchElapsedSeconds)) {
        case LoadThrottlePhase::Early:
            return 5000;
        case LoadThrottlePhase::Transition:
            return 2000;
        case LoadThrottlePhase::Normal:
        default:
            return 1000;
    }
}

} // namespace

bool shouldLogEvery(Clock::time_point& lastLogTime, std::chrono::milliseconds interval) {
    const auto now = Clock::now();
    if (lastLogTime.time_since_epoch().count() != 0 && now - lastLogTime < interval) {
        return false;
    }
    lastLogTime = now;
    return true;
}

// =====================================================================
//  观战类型名称
// =====================================================================
const char* observerTypeName(EObserverType type) {
    switch (type) {
        case EObserverType::None:           return "None";
        case EObserverType::InSpectating:   return "InSpectating";
        case EObserverType::GlobalObserver: return "GlobalObserver";
        case EObserverType::FriendObserver: return "FriendObserver";
        case EObserverType::Spectator:      return "Spectator";
        default:                            return "Unknown";
    }
}

// =====================================================================
//  PlayerList 实现
// =====================================================================
PlayerList::~PlayerList() { clear(); }

void PlayerList::clear() {
    PlayerNode* cur = m_head;
    while (cur) {
        PlayerNode* next = cur->next;
        delete cur;
        cur = next;
    }
    m_head = nullptr;
    m_tail = nullptr;
    m_size = 0;
    m_map.clear();
}

PlayerNode* PlayerList::findByKey(uint32_t playerKey) {
    auto it = m_map.find(playerKey);
    return (it != m_map.end()) ? it->second : nullptr;
}

PlayerNode* PlayerList::upsert(uint32_t playerKey, const PlayerNode& data) {
    PlayerNode* node = findByKey(playerKey);
    if (node) {
        node->teamID     = data.teamID;
        node->playerName = data.playerName;
        node->isAI       = data.isAI;
        node->liveState  = data.liveState;
        node->health     = data.health;
        node->healthMax  = data.healthMax;
        node->kills      = data.kills;
        node->pos        = data.pos;
        node->characterPtr = data.characterPtr;
        return node;
    }
    node = new PlayerNode();
    node->playerKey  = playerKey;
    node->teamID     = data.teamID;
    node->playerName = data.playerName;
    node->isAI       = data.isAI;
    node->liveState  = data.liveState;
    node->health     = data.health;
    node->healthMax  = data.healthMax;
    node->kills      = data.kills;
    node->pos        = data.pos;
    node->characterPtr = data.characterPtr;

    if (!m_head) {
        m_head = node;
        m_tail = node;
    } else {
        node->prev = m_tail;
        m_tail->next = node;
        m_tail = node;
    }
    m_size++;
    m_map[playerKey] = node;
    return node;
}

void PlayerList::remove(uint32_t playerKey) {
    PlayerNode* node = findByKey(playerKey);
    if (!node) return;
    if (node->prev) node->prev->next = node->next;
    else m_head = node->next;
    if (node->next) node->next->prev = node->prev;
    else m_tail = node->prev;
    m_size--;
    m_map.erase(playerKey);
    delete node;
}

// =====================================================================
//  ResolvedOffsets::isValid 检查关键偏移是否都已解
// =====================================================================
bool ResolvedOffsets::isValid() const {
    // 核心偏移: 没有这些就无法运行
    bool core = World_GameState >= 0
             && GS_PlayerArray >= 0
             && Actor_RootComponent >= 0;
    // 玩家数据: 没有这些就无法采集玩家信息
    bool player = PS_PlayerKey >= 0
               && PS_TeamID >= 0
               && Char_Health >= 0
               && Char_HealthMax >= 0;
    return core && player;
}

// =====================================================================
//  initOffsets 通过 UE4Interface 动态查找所有游戏偏
// =====================================================================
// 辅助: 查找偏移, 失败时打印警告
#define RESOLVE_OFFSET(target, className, fieldName) do { \
    int32_t _off = m_interface.getFieldOffsetInHierarchy(className, fieldName); \
    if (_off >= 0) { target = _off; \
        LOG(LOG_LEVEL_INFO, "[InitOffsets] %s.%s = 0x%X", className, fieldName, _off); } \
    else { LOG(LOG_LEVEL_WARN, "[InitOffsets] 未找到 %s.%s", className, fieldName); } \
} while(0)

// 辅助: 尝试多个类名查找同一字段, 沿继承链搜索 (第一个匹配即返回)
#define RESOLVE_OFFSET_MULTI(target, fieldName, ...) do { \
    const char* _classes[] = { __VA_ARGS__ }; \
    std::string _owner; \
    for (auto* _cn : _classes) { \
        const ue4inf::UEFieldInfo* _fi = m_interface.findFieldInHierarchy(_cn, fieldName, &_owner); \
        if (_fi) { target = _fi->offset; \
            LOG(LOG_LEVEL_INFO, "[InitOffsets] %s.%s = 0x%X (via %s)", _cn, fieldName, _fi->offset, _owner.c_str()); \
            break; } \
    } \
    if (target < 0) { LOG(LOG_LEVEL_WARN, "[InitOffsets] 未找到 %s (尝试了 %zu 个类+继承链)", fieldName, sizeof(_classes)/sizeof(_classes[0])); } \
} while(0)

uintptr_t resolveWeakObjectPtr(uintptr_t guObjectArrayPtr, uintptr_t weakPtrAddr) {
    if (guObjectArrayPtr < 0x10000 || weakPtrAddr < 0x10000) {
        return 0;
    }

    RemoteWeakObjectPtr weakPtr{};
    if (!safeReadMemory(weakPtrAddr, &weakPtr, sizeof(weakPtr))
        || weakPtr.objectIndex < 0
        || weakPtr.objectSerialNumber <= 0) {
        return 0;
    }

    int32_t numChunks = 0;
    int32_t totalNum = 0;
    if (!safeReadMemory(guObjectArrayPtr + 0xF8, &numChunks, sizeof(numChunks))
        || !safeReadMemory(guObjectArrayPtr + 0x100, &totalNum, sizeof(totalNum))
        || numChunks <= 0
        || numChunks > 1000
        || weakPtr.objectIndex >= totalNum) {
        return 0;
    }

    int32_t remainingIndex = weakPtr.objectIndex;
    for (int32_t chunkIndex = 0; chunkIndex < numChunks; ++chunkIndex) {
        int32_t chunkCount = 0;
        if (!safeReadMemory(guObjectArrayPtr + 0xE8 + static_cast<uintptr_t>(chunkIndex) * sizeof(int32_t),
                            &chunkCount,
                            sizeof(chunkCount))
            || chunkCount <= 0) {
            continue;
        }

        if (remainingIndex >= chunkCount) {
            remainingIndex -= chunkCount;
            continue;
        }

        uintptr_t chunkBase = 0;
        if (!safeReadMemory(guObjectArrayPtr + 0xC8 + static_cast<uintptr_t>(chunkIndex) * sizeof(uintptr_t),
                            &chunkBase,
                            sizeof(chunkBase))
            || chunkBase < 0x10000) {
            return 0;
        }

        ue4::FUObjectItem item{};
        const uintptr_t itemAddr = chunkBase + static_cast<uintptr_t>(remainingIndex) * sizeof(ue4::FUObjectItem);
        if (!safeReadMemory(itemAddr, &item, sizeof(item))) {
            return 0;
        }

        if (item.SerialNumber != weakPtr.objectSerialNumber) {
            return 0;
        }

        return reinterpret_cast<uintptr_t>(item.Object);
    }

    return 0;
}

bool MatchMonitor::initOffsets() {
    LOG(LOG_LEVEL_INFO, "[InitOffsets] 开始通过反射解析偏移...");

    // UWorld
    RESOLVE_OFFSET(m_off.World_GameState,         "World", "GameState");
    RESOLVE_OFFSET(m_off.World_AuthorityGameMode,  "World", "AuthorityGameMode");

    // GameState / GameStateBase — MatchState 声明在 GameState 而非 GameStateBase
    RESOLVE_OFFSET_MULTI(m_off.GS_MatchState,      "MatchState",         "GameState", "GameStateBase", "UAEGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_bHasBegunPlay,   "bHasBegunPlay",      "GameStateBase", "GameState", "UAEGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_ElapsedTime,     "ElapsedTime",        "GameStateBase", "GameState", "UAEGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_PlayerArray,     "PlayerArray",        "GameStateBase", "GameState", "UAEGameState");

    // UAEGameState — 字段可能在父类或子类上
    RESOLVE_OFFSET_MULTI(m_off.GS_PlayerNum,       "PlayerNum",          "UAEGameState", "GameState", "STExtraGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_TotalPlayerNum,  "TotalPlayerNum",     "UAEGameState", "GameState", "STExtraGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_GameType,        "GameType",           "UAEGameState", "GameState", "STExtraGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_AlivePlayerNum,  "AlivePlayerNum",     "UAEGameState", "STExtraGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_AliveRealPlayerNum, "AliveRealPlayerNum", "UAEGameState", "STExtraGameState");

    // PlayerState
    RESOLVE_OFFSET_MULTI(m_off.PS_PlayerName,      "PlayerName",         "PlayerState", "UAEPlayerState");

    // UAEPlayerState
    RESOLVE_OFFSET_MULTI(m_off.PS_PlayerKey,       "PlayerKey",          "UAEPlayerState", "STExtraPlayerState");
    RESOLVE_OFFSET_MULTI(m_off.PS_bAIPlayer,       "bAIPlayer",          "UAEPlayerState", "STExtraPlayerState");
    RESOLVE_OFFSET_MULTI(m_off.PS_TeamID,          "TeamID",             "UAEPlayerState", "STExtraPlayerState", "PlayerState");

    // STExtraPlayerState
    RESOLVE_OFFSET_MULTI(m_off.PS_Kills,           "Kills",              "STExtraPlayerState", "UAEPlayerState", "PlayerState");
    RESOLVE_OFFSET_MULTI(m_off.PS_LiveState,       "LiveState",          "STExtraPlayerState", "UAEPlayerState");
    RESOLVE_OFFSET_MULTI(m_off.PS_CharacterOwner,  "CharacterOwner",     "STExtraPlayerState", "UAEPlayerState");
    RESOLVE_OFFSET_MULTI(m_off.PS_PlayerHealth,    "PlayerHealth",       "STExtraPlayerState", "UAEPlayerState");
    RESOLVE_OFFSET_MULTI(m_off.PS_PlayerHealthMax, "PlayerHealthMax",    "STExtraPlayerState", "UAEPlayerState");
    RESOLVE_OFFSET_MULTI(m_off.PS_SelfLocAndRot,   "SelfLocAndRot",      "STExtraPlayerState", "UAEPlayerState");

    // Actor
    RESOLVE_OFFSET(m_off.Actor_RootComponent,     "Actor", "RootComponent");
    RESOLVE_OFFSET_MULTI(m_off.Actor_NetCullDistSq, "NetCullDistanceSquared", "Actor", "Character", "Pawn");

    // SceneComponent — 使用 ComponentToWorld.Translation (世界坐标, 非 RelativeLocation)
    // ComponentToWorld 是 FTransform, Translation 在 FTransform+0x10
    {
        int32_t ctw = m_interface.getFieldOffsetInHierarchy("SceneComponent", "ComponentToWorld");
        if (ctw >= 0) {
            m_off.SceneComp_ComponentToWorld = ctw;
            m_off.SceneComp_Translation = ctw + 0x10; // FTransform.Translation offset
            LOG(LOG_LEVEL_INFO, "[InitOffsets] SceneComponent.ComponentToWorld+0x10 = 0x%X", m_off.SceneComp_Translation);
        } else {
            LOG(LOG_LEVEL_WARN, "[InitOffsets] ComponentToWorld 未找到, 回退 0x200");
            m_off.SceneComp_ComponentToWorld = 0x1F0;
            m_off.SceneComp_Translation = 0x200;
        }
    }

    // UAEPlayerController
    RESOLVE_OFFSET_MULTI(m_off.PC_bIsObserver,         "bIsObserver",         "UAEPlayerController", "STExtraPlayerController");
    RESOLVE_OFFSET_MULTI(m_off.PC_bIsObserverInBattle, "bIsObserverInBattle", "UAEPlayerController", "STExtraPlayerController");
    RESOLVE_OFFSET_MULTI(m_off.PC_bIsObserverHost,     "bIsObserverHost",     "UAEPlayerController", "STExtraPlayerController");

    // Controller — ControlRotation (FRotator: Pitch, Yaw, Roll)
    RESOLVE_OFFSET_MULTI(m_off.Ctrl_ControlRotation,   "ControlRotation",     "Controller", "PlayerController", "UAEPlayerController", "STExtraPlayerController");

    // UAECharacter
    RESOLVE_OFFSET_MULTI(m_off.Char_TeamID,        "TeamID",             "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_PlayerKey,     "PlayerKey",          "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_PlayerName,    "PlayerName",         "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_Mesh,          "Mesh",               "Character", "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");

    // STExtraCharacter
    RESOLVE_OFFSET_MULTI(m_off.Char_Health,        "Health",             "STExtraCharacter", "UAECharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_HealthMax,     "HealthMax",          "STExtraCharacter", "UAECharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_bDead,         "bDead",              "STExtraCharacter", "UAECharacter", "STExtraBaseCharacter");

    // Character — bMarkScopeIn (开镜状态, 用于自瞄条件判断)
    RESOLVE_OFFSET_MULTI(m_off.Char_bMarkScopeIn,  "bMarkScopeIn",       "Character", "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");

    // STExtraBaseCharacter
    RESOLVE_OFFSET_MULTI(m_off.Char_CurrentNetCullDistSq, "CurrentNetCullDistanceSquared", "STExtraBaseCharacter", "STExtraCharacter", "UAECharacter");
    RESOLVE_OFFSET_MULTI(m_off.STBase_AvatarComponent, "AvatarComponent", "STExtraBaseCharacter", "STExtraCharacter");
    RESOLVE_OFFSET_MULTI(m_off.STBase_FPPComp,      "FPPComp",           "STExtraBaseCharacter", "STExtraCharacter");
    RESOLVE_OFFSET_MULTI(m_off.STBase_DefaultCharacterMesh, "DefaultCharacterMesh", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.STBase_LastSkeletalMesh, "LastSkeletalMesh", "STExtraBaseCharacter");

    // AvatarComponent
    RESOLVE_OFFSET_MULTI(m_off.Avatar_MasterBoneComponent, "MasterBoneComponent", "AvatarComponent");
    RESOLVE_OFFSET_MULTI(m_off.Avatar_SkeletalMeshCompPool, "SkeletalMeshCompPool", "AvatarComponent");
    RESOLVE_OFFSET_MULTI(m_off.Avatar_MeshComponentList, "meshComponentList", "AvatarComponent");
    RESOLVE_OFFSET_MULTI(m_off.Avatar_EntityTickList, "EntityTickList", "AvatarComponent");
    RESOLVE_OFFSET_MULTI(m_off.Avatar_AvatarEntityList, "AvatarEntityList", "AvatarComponent");

    // Skeletal / bone chain
    RESOLVE_OFFSET_MULTI(m_off.SkinnedMesh_MasterPoseComponent, "MasterPoseComponent", "SkinnedMeshComponent", "SkeletalMeshComponent");
    RESOLVE_OFFSET_MULTI(m_off.SkinnedMesh_SkeletalMesh, "SkeletalMesh", "SkinnedMeshComponent", "SkeletalMeshComponent");
    RESOLVE_OFFSET_MULTI(m_off.SkeletalMeshComp_CachedComponentSpaceTransforms, "CachedComponentSpaceTransforms", "SkeletalMeshComponent");
    RESOLVE_OFFSET_MULTI(m_off.SkeletalMeshAsset_Skeleton, "Skeleton", "SkeletalMesh");
    RESOLVE_OFFSET_MULTI(m_off.Skeleton_RefBoneNames, "RefBoneNames", "Skeleton");

    // Character → CharacterMovement 组件指针
    RESOLVE_OFFSET_MULTI(m_off.Char_CharacterMovement, "CharacterMovement", "Character", "UAECharacter", "STExtraCharacter");

    // CharacterMovementComponent — 人物移动速度
    RESOLVE_OFFSET_MULTI(m_off.CMC_MaxWalkSpeed,         "MaxWalkSpeed",         "CharacterMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.CMC_MaxWalkSpeedCrouched, "MaxWalkSpeedCrouched", "CharacterMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.CMC_MaxSwimSpeed,         "MaxSwimSpeed",         "CharacterMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.CMC_MaxFlySpeed,          "MaxFlySpeed",          "CharacterMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.CMC_MaxAcceleration,      "MaxAcceleration",      "CharacterMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.CMC_GravityScale,         "GravityScale",         "CharacterMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.CMC_JumpZVelocity,        "JumpZVelocity",        "CharacterMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.CMC_Velocity,             "Velocity",             "MovementComponent", "CharacterMovementComponent");

    // STExtraShootWeaponBulletBase — 子弹 Actor
    RESOLVE_OFFSET_MULTI(m_off.Bullet_PMComp,            "PMComp",              "STExtraShootWeaponBulletBase");
    RESOLVE_OFFSET_MULTI(m_off.Bullet_LaunchGravityScale, "LaunchGravityScale", "STExtraShootWeaponBulletBase");
    RESOLVE_OFFSET_MULTI(m_off.Bullet_MaxNoGravityRange, "MaxNoGravityRange",   "STExtraShootWeaponBulletBase");
    RESOLVE_OFFSET_MULTI(m_off.Bullet_ShootDir,          "ShootDir",            "STExtraShootWeaponBulletBase");

    // ProjectileMovementComponent — 弹道运动
    RESOLVE_OFFSET_MULTI(m_off.PMC_InitialSpeed,          "InitialSpeed",          "ProjectileMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.PMC_MaxSpeed,              "MaxSpeed",              "ProjectileMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.PMC_Velocity,              "Velocity",              "MovementComponent", "ProjectileMovementComponent");
    RESOLVE_OFFSET_MULTI(m_off.PMC_ProjectileGravityScale, "ProjectileGravityScale", "ProjectileMovementComponent");

    // BulletTrackComponent — 后坐力
    RESOLVE_OFFSET_MULTI(m_off.BTC_CurRecoilValue,          "CurRecoilValue",          "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_VerticalRecoilTarget,    "VerticalRecoilTarget",    "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_HorizontalRecoilTarget,  "HorizontalRecoilTarget",  "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_VerticalRecoveryTarget,  "VerticalRecoveryTarget",  "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_PoseRecoilFactor,        "PoseRecoilFactor",        "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_AccessoriesVRecoilFactor, "AccessoriesVRecoilFactor", "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_VerticalRecoilFactorModifier, "VerticalRecoilFactorModifier", "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_AccessoriesHRecoilFactor, "AccessoriesHRecoilFactor", "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_HorizontalRecoilFactorModifier, "HorizontalRecoilFactorModifier", "BulletTrackComponent");
    RESOLVE_OFFSET_MULTI(m_off.BTC_AccVerticalRecoilTarget, "AccVerticalRecoilTarget", "BulletTrackComponent");

    LOG(LOG_LEVEL_INFO, "[InitOffsets] 解析完成, isValid=%d", m_off.isValid());
    LOG(LOG_LEVEL_INFO, "[InitOffsets] World.GameState=0x%X GS.PlayerArray=0x%X PS.PlayerKey=0x%X",
        m_off.World_GameState, m_off.GS_PlayerArray, m_off.PS_PlayerKey);
    LOG(LOG_LEVEL_INFO, "[InitOffsets] Actor.RootComponent=0x%X Char.Health=0x%X Char.HealthMax=0x%X",
        m_off.Actor_RootComponent, m_off.Char_Health, m_off.Char_HealthMax);
    LOG(LOG_LEVEL_INFO, "[InitOffsets] Char.bMarkScopeIn=0x%X (ADS detection)", m_off.Char_bMarkScopeIn);
    LOG(LOG_LEVEL_INFO, "[InitOffsets] CMC: MaxWalkSpeed=0x%X GravityScale=0x%X Velocity=0x%X",
        m_off.CMC_MaxWalkSpeed, m_off.CMC_GravityScale, m_off.CMC_Velocity);
    LOG(LOG_LEVEL_INFO, "[InitOffsets] PMC: InitialSpeed=0x%X MaxSpeed=0x%X GravityScale=0x%X",
        m_off.PMC_InitialSpeed, m_off.PMC_MaxSpeed, m_off.PMC_ProjectileGravityScale);
    LOG(LOG_LEVEL_INFO, "[InitOffsets] BTC: VRecoil=0x%X HRecoil=0x%X CurRecoil=0x%X",
        m_off.BTC_VerticalRecoilTarget, m_off.BTC_HorizontalRecoilTarget, m_off.BTC_CurRecoilValue);

    return m_off.isValid();
}

#undef RESOLVE_OFFSET
#undef RESOLVE_OFFSET_MULTI

// =====================================================================
//  安全内存读取
// =====================================================================
uintptr_t MatchMonitor::safeReadPtr(uintptr_t addr) {
    uintptr_t val = 0;
    safeReadMemory(addr, &val, sizeof(val));
    return val;
}

int32_t MatchMonitor::safeReadS32(uintptr_t addr) {
    int32_t val = 0;
    safeReadMemory(addr, &val, sizeof(val));
    return val;
}

uint32_t MatchMonitor::safeReadU32(uintptr_t addr) {
    uint32_t val = 0;
    safeReadMemory(addr, &val, sizeof(val));
    return val;
}

uint8_t MatchMonitor::safeReadU8(uintptr_t addr) {
    uint8_t val = 0;
    safeReadMemory(addr, &val, sizeof(val));
    return val;
}

float MatchMonitor::safeReadFloat(uintptr_t addr) {
    float val = 0.0f;
    safeReadMemory(addr, &val, sizeof(val));
    return val;
}

// =====================================================================
//  BatchMemReader 静态桥接
// =====================================================================
bool BatchMemReader::safeReadMemoryStatic(uintptr_t addr, void* out, size_t size) {
    return safeReadMemory(addr, out, size);
}

// =====================================================================
//  偏移范围计算 — 确定 PlayerState / Character 批量读取所需的字节数
// =====================================================================
void MatchMonitor::computeBatchReadBounds() {
    auto maxOff = [](std::initializer_list<int32_t> offsets, size_t fieldSize) -> size_t {
        int32_t mx = 0;
        for (int32_t o : offsets) {
            if (o > mx) mx = o;
        }
        return (mx > 0) ? static_cast<size_t>(mx) + fieldSize : 0;
    };

    // PlayerState: 包含所有 PS_* 偏移中最大值 + padding
    m_psReadSize = maxOff({
        m_off.PS_PlayerKey, m_off.PS_TeamID, m_off.PS_bAIPlayer,
        m_off.PS_LiveState, m_off.PS_PlayerHealth, m_off.PS_PlayerHealthMax,
        m_off.PS_Kills, m_off.PS_CharacterOwner,
        m_off.PS_SelfLocAndRot >= 0 ? m_off.PS_SelfLocAndRot + 12 : 0  // FVector3 = 12 bytes
    }, 8);  // 8 bytes for pointer fields

    // Character: 包含所有 Char_* 偏移 + Actor_RootComponent
    m_charReadSize = maxOff({
        m_off.Char_Health, m_off.Char_HealthMax, m_off.Char_bDead,
        m_off.Char_PlayerKey, m_off.Char_TeamID,
        m_off.Actor_RootComponent, m_off.Actor_NetCullDistSq,
        m_off.Char_CurrentNetCullDistSq
    }, 8);

    // BatchMemReader::read() 会自动将超出 kMaxBatchSize 的部分截断,
    // get() 对未命中缓冲的字段自动回退到单独读取, 无需在此 clamp
    LOG(LOG_LEVEL_INFO, "[BatchRead] PS 批量读取范围: %zu bytes (buf=%zu), Char 批量读取范围: %zu bytes (buf=%zu)",
        m_psReadSize, BatchMemReader::kMaxBatchSize, m_charReadSize, BatchMemReader::kMaxBatchSize);
}

// =====================================================================
//  安全内存写入 (信号保护, 防止写入已释放内存时崩溃)
// =====================================================================
bool MatchMonitor::writeMemU8(uintptr_t addr, uint8_t val) {
    if (addr == 0) return false;
    return safeWriteMemory(addr, &val, sizeof(val));
}

bool MatchMonitor::writeMemFloat(uintptr_t addr, float val) {
    if (addr == 0) return false;
    return safeWriteMemory(addr, &val, sizeof(val));
}

// =====================================================================
//  FName 解析
// =====================================================================
std::string MatchMonitor::getNameByIndex(int index) {
    auto it = m_nameCache.find(index);
    if (it != m_nameCache.end()) return it->second;
    if (index < 0 || index >= m_numNames) return "";

    // 通过 safeRead 访问 GNames 数组, 避免游戏重分配时裸解引用崩溃
    uintptr_t namesBase = m_gNames;
    int ci = index / ue4::NAMES_ELEMENTS_PER_CHUNK;
    int wi = index % ue4::NAMES_ELEMENTS_PER_CHUNK;

    // Chunks[ci] 是指针数组, 每个元素 8 字节
    uintptr_t chkPtr = safeReadPtr(namesBase + static_cast<uintptr_t>(ci) * 8);
    if (chkPtr == 0 || chkPtr < 0x10000) return "";

    // chk[wi] 是 FNameEntry* 数组
    uintptr_t entryPtr = safeReadPtr(chkPtr + static_cast<uintptr_t>(wi) * 8);
    if (entryPtr == 0 || entryPtr < 0x10000) return "";

    // FNameEntry: +0x08 = Index (bit0=IsWide), +0x0C = AnsiName
    int32_t entryIndex = safeReadS32(entryPtr + 0x08);
    bool isWide = (entryIndex & 1) != 0;

    std::string name;
    if (!isWide) {
        // 读取 ANSI 字符串 (entryPtr + 0x0C)
        char buf[256] = {};
        safeReadMemory(entryPtr + 0x0C, buf, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        name = buf;
    } else {
        // Wide (UTF-32)
        uintptr_t a = entryPtr + 0x0C;
        for (int i = 0; i < 512; i++) {
            uint32_t c = safeReadU32(a + i * 4);
            if (c == 0) break;
            if (c < 128) name += static_cast<char>(c);
            else name += '?';
        }
    }
    if (!name.empty()) m_nameCache[index] = name;
    return name;
}

std::string MatchMonitor::readFName(uintptr_t addr) {
    if (addr == 0 || addr < 0x10000) return "<invalid>";
    int32_t idx = safeReadS32(addr + kFNameComparisonIndexOffset);
    int32_t num = safeReadS32(addr + kFNameNumberOffset);
    if (idx < 0 || idx >= m_numNames) return "<invalid>";
    std::string base = getNameByIndex(idx);
    if (base.empty()) return "<invalid>";
    if (num == 0) return base;
    return base + "_" + std::to_string(num - 1);
}

std::string MatchMonitor::readObjName(uintptr_t objPtr) {
    if (objPtr == 0 || objPtr < 0x10000) return "<invalid>";
    return readFName(objPtr + kUObjectNamePrivateOffset);
}

std::string MatchMonitor::readClassName(uintptr_t objPtr) {
    if (objPtr == 0 || objPtr < 0x10000) return "<no_class>";
    uintptr_t clsPtr = safeReadPtr(objPtr + kUObjectClassPrivateOffset);
    if (clsPtr == 0 || clsPtr < 0x10000) return "<no_class>";
    return readObjName(clsPtr);
}

// =====================================================================
//  FString 读取 (UE4 Android: UTF-16LE -> UTF-8)
// =====================================================================
std::string MatchMonitor::readFString(uintptr_t addr) {
    uintptr_t dataPtr = safeReadPtr(addr);
    int32_t num = safeReadS32(addr + 8);
    if (dataPtr == 0 || num <= 0 || num > 256) return "";

    // 一次性安全读取整个 UTF-16 缓冲区, 避免裸解引用崩溃
    const size_t byteLen = static_cast<size_t>(num) * 2;
    uint16_t buf[256] = {};
    if (!safeReadMemory(dataPtr, buf, byteLen)) return "";

    std::string result;
    for (int i = 0; i < num - 1; i++) {
        uint16_t c = buf[i];
        if (c == 0) break;
        if (c < 128) {
            result += static_cast<char>(c);
        } else if (c < 0x800) {
            result += static_cast<char>(0xC0 | (c >> 6));
            result += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (c >> 12));
            result += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return result;
}

// =====================================================================
//  对局状态读
// =====================================================================
MatchState MatchMonitor::getMatchState() {
    MatchState ms;
    uintptr_t worldPtr = safeReadPtr(m_gWorld);
    if (worldPtr == 0 || worldPtr < 0x10000) {
        ms.state = "NO_WORLD";
        return ms;
    }
    ms.worldName = readObjName(worldPtr);
    if (m_off.World_GameState < 0) { ms.state = "Unknown"; return ms; }
    uintptr_t gsPtr = safeReadPtr(worldPtr + m_off.World_GameState);
    ms.gameStatePtr = gsPtr;

    if (gsPtr != 0 && gsPtr > 0x10000 && m_off.GS_MatchState >= 0) {
        ms.state = readFName(gsPtr + m_off.GS_MatchState);
    } else {
        ms.state = "Unknown";
    }

    if (gsPtr != 0 && gsPtr > 0x10000 && m_off.GS_ElapsedTime >= 0) {
        const int32_t elapsedTime = safeReadS32(gsPtr + m_off.GS_ElapsedTime);
        if (elapsedTime >= 0 && elapsedTime < 7200) {
            ms.elapsedTimeSeconds = elapsedTime;
        }
    }

    // 判断是否在对局: 排除大厅/UI 地图
    ms.inMatch = ms.worldName.find("Editor_login") == std::string::npos
              && ms.worldName.find("UImap") == std::string::npos
              && ms.worldName.find("Lobby") == std::string::npos
              && ms.worldName != "None"
              && ms.worldName.find("invalid") == std::string::npos
              && gsPtr != 0;
    return ms;
}

// =====================================================================
//  Actor 位置读取
// =====================================================================
bool MatchMonitor::getActorLocation(uintptr_t actorPtr, FVector3& outLoc) {
    if (actorPtr == 0) return false;
    uintptr_t rootComp = safeReadPtr(actorPtr + m_off.Actor_RootComponent);
    if (rootComp == 0 || rootComp < 0x10000) return false;
    int off = m_off.SceneComp_Translation;
    if (off < 0) return false;
    outLoc.x = safeReadFloat(rootComp + off);
    outLoc.y = safeReadFloat(rootComp + off + 4);
    outLoc.z = safeReadFloat(rootComp + off + 8);
    if (outLoc.x == 0 && outLoc.y == 0 && outLoc.z == 0) return false;
    if (std::fabs(outLoc.x) > 1e8f || std::fabs(outLoc.y) > 1e8f) return false;
    return true;
}

// 从 FName 数组匹配骨骼索引
int MatchMonitor::matchBoneNamesFromFNameArray(uintptr_t dataPtr, int count, BoneAssetCacheEntry& entry) {
    entry.trackedBoneIndices.fill(-1);
    entry.matchedCount = 0;
    if (dataPtr < 0x10000 || count <= 0 || count > kMaxBoneNameCount) return 0;

    for (int index = 0; index < count; ++index) {
        ue4::FName boneName{};
        if (!safeReadMemory(dataPtr + static_cast<uintptr_t>(index) * sizeof(ue4::FName),
                            &boneName, sizeof(boneName)))
            continue;
        if (boneName.ComparisonIndex < 0 || boneName.ComparisonIndex >= m_numNames) continue;

        const std::string rawName = getNameByIndex(boneName.ComparisonIndex);
        const std::string normalizedName = normalizeBoneName(rawName);
        if (normalizedName.empty()) continue;

        for (size_t slot = 0; slot < TRACKED_BONE_COUNT; ++slot) {
            if (entry.trackedBoneIndices[slot] >= 0) continue;
            if (matchesTrackedBoneName(slot, normalizedName)) {
                entry.trackedBoneIndices[slot] = index;
                entry.matchedCount++;
                break;
            }
        }
        if (entry.matchedCount == static_cast<int>(TRACKED_BONE_COUNT)) break;
    }
    return entry.matchedCount;
}

// 从 FMeshBoneInfo 数组 (stride字节, FName在偏移0) 匹配骨骼索引
int MatchMonitor::matchBoneNamesFromBoneInfoArray(uintptr_t dataPtr, int count, int stride, BoneAssetCacheEntry& entry) {
    entry.trackedBoneIndices.fill(-1);
    entry.matchedCount = 0;
    if (dataPtr < 0x10000 || count <= 0 || count > kMaxBoneNameCount || stride < 12) return 0;

    for (int index = 0; index < count; ++index) {
        ue4::FName boneName{};
        if (!safeReadMemory(dataPtr + static_cast<uintptr_t>(index) * stride,
                            &boneName, sizeof(boneName)))
            continue;
        if (boneName.ComparisonIndex < 0 || boneName.ComparisonIndex >= m_numNames) continue;

        const std::string rawName = getNameByIndex(boneName.ComparisonIndex);
        const std::string normalizedName = normalizeBoneName(rawName);
        if (normalizedName.empty()) continue;

        for (size_t slot = 0; slot < TRACKED_BONE_COUNT; ++slot) {
            if (entry.trackedBoneIndices[slot] >= 0) continue;
            if (matchesTrackedBoneName(slot, normalizedName)) {
                entry.trackedBoneIndices[slot] = index;
                entry.matchedCount++;
                break;
            }
        }
        if (entry.matchedCount == static_cast<int>(TRACKED_BONE_COUNT)) break;
    }
    return entry.matchedCount;
}

bool MatchMonitor::resolveTrackedBoneIndices(uintptr_t skeletalMeshAssetPtr, BoneAssetCacheEntry& outEntry) {
    outEntry.trackedBoneIndices.fill(-1);
    outEntry.matchedCount = 0;
    if (skeletalMeshAssetPtr < 0x10000) return false;

    const auto cached = m_boneAssetCache.find(skeletalMeshAssetPtr);
    if (cached != m_boneAssetCache.end()) {
        outEntry = cached->second;
        return outEntry.matchedCount > 0;
    }

    BoneAssetCacheEntry entry;
    entry.trackedBoneIndices.fill(-1);

    // 方法1: SkeletalMesh+0x238 FReferenceSkeleton (全身骨骼, stride=16)
    constexpr uintptr_t kRefBoneInfoOffset = 0x238;
    const uintptr_t boneInfoData = safeReadPtr(skeletalMeshAssetPtr + kRefBoneInfoOffset);
    const int32_t boneInfoNum = safeReadS32(skeletalMeshAssetPtr + kRefBoneInfoOffset + 8);
    if (boneInfoData >= 0x10000 && boneInfoNum > 0 && boneInfoNum <= kMaxBoneNameCount) {
        matchBoneNamesFromBoneInfoArray(boneInfoData, boneInfoNum, 16, entry);
        if (entry.matchedCount >= kMinRenderableBoneMatches) {
            m_boneAssetCache[skeletalMeshAssetPtr] = entry;
            outEntry = entry;
            return true;
        }
        // stride=16 不够, 试 stride=8 (纯 FName 数组)
        BoneAssetCacheEntry trial;
        if (matchBoneNamesFromFNameArray(boneInfoData, boneInfoNum, trial) > entry.matchedCount)
            entry = trial;
    }

    // 方法2: Skeleton → RefBoneNames
    const int32_t skelOff = (m_off.SkeletalMeshAsset_Skeleton >= 0) ? m_off.SkeletalMeshAsset_Skeleton : 0x48;
    const int32_t refOff = (m_off.Skeleton_RefBoneNames >= 0) ? m_off.Skeleton_RefBoneNames : 0x280;
    const uintptr_t skeletonPtr = safeReadPtr(skeletalMeshAssetPtr + skelOff);
    if (skeletonPtr >= 0x10000) {
        ue4::TArray<ue4::FName> refBoneNames{};
        if (safeReadMemory(skeletonPtr + refOff, &refBoneNames, sizeof(refBoneNames))
            && isUsableRemoteArray(refBoneNames, kMaxBoneNameCount)) {
            BoneAssetCacheEntry trial;
            if (matchBoneNamesFromFNameArray(reinterpret_cast<uintptr_t>(refBoneNames.Data),
                                             refBoneNames.Num, trial) > entry.matchedCount)
                entry = trial;
        }
    }

    m_boneAssetCache[skeletalMeshAssetPtr] = entry;
    outEntry = entry;
    return entry.matchedCount > 0;
}

// =====================================================================
//  fillPlayerSkeleton — 填充玩家骨骼世界坐标
//
//  流程: 收集候选组件 → 选最多 transforms 的 → 匹配骨骼名 → 坐标变换
// =====================================================================
bool MatchMonitor::fillPlayerSkeleton(uintptr_t characterPtr, ue4draw::DrawPlayerInfo& outPlayer) {
    static Clock::time_point s_lastSkeletonLogTime;

    // dump.cs 静态偏移 (反射失败时的回退)
    constexpr int32_t kFB_Char_Mesh         = 0x650;
    constexpr int32_t kFB_ComponentToWorld   = 0x1F0;
    constexpr int32_t kFB_SkeletalMesh       = 0x7F0;
    constexpr int32_t kFB_MasterPose         = 0x7F8;
    constexpr int32_t kFB_CachedTransforms   = 0xBB8;
    constexpr int32_t kFB_CachedBoneTrans    = 0xBA8;
    constexpr int32_t kFB_AvatarComp         = 0x3B98;
    constexpr int32_t kFB_FPPComp            = 0x4168;
    constexpr int32_t kFB_DefaultMesh        = 0x4630;
    constexpr int32_t kFB_LastSkelMesh       = 0x4790;
    constexpr int32_t kFB_MasterBone         = 0x300;
    constexpr int32_t kFB_MeshCompList       = 0x528;
    constexpr int32_t kFB_SkelPool           = 0xF10;
    constexpr int32_t kFB_FPPAvatar          = 0x380;

    outPlayer.boneMask = 0;
    if (characterPtr < 0x10000) return false;

    // 限制骨骼资产缓存大小 (防止 GC 后旧指针堆积), 超过上限懒清理
    if (m_boneAssetCache.size() > 128) {
        m_boneAssetCache.clear();
    }
    // 限制名称缓存大小防止长时间运行后内存膨胀
    if (m_nameCache.size() > 50000) {
        m_nameCache.clear();
    }

    auto off = [](int32_t reflected, int32_t fallback) -> int32_t {
        return (reflected >= 0) ? reflected : fallback;
    };

    const int32_t oMesh      = off(m_off.Char_Mesh, kFB_Char_Mesh);
    const int32_t oCtw       = off(m_off.SceneComp_ComponentToWorld, kFB_ComponentToWorld);
    const int32_t oSkelMesh  = off(m_off.SkinnedMesh_SkeletalMesh, kFB_SkeletalMesh);
    const int32_t oMasterP   = off(m_off.SkinnedMesh_MasterPoseComponent, kFB_MasterPose);
    const int32_t oCached    = off(m_off.SkeletalMeshComp_CachedComponentSpaceTransforms, kFB_CachedTransforms);
    const int32_t oAvatar    = off(m_off.STBase_AvatarComponent, kFB_AvatarComp);
    const int32_t oMasterB   = off(m_off.Avatar_MasterBoneComponent, kFB_MasterBone);

    // ---- 辅助 ----
    auto readTransformArray = [&](uintptr_t comp, ue4::TArray<RemoteTransform>& out) -> bool {
        if (comp < 0x10000) return false;
        if (safeReadMemory(comp + oCached, &out, sizeof(out)) && isUsableRemoteArray(out, kMaxBoneNameCount))
            return true;
        if (safeReadMemory(comp + kFB_CachedBoneTrans, &out, sizeof(out)) && isUsableRemoteArray(out, kMaxBoneNameCount))
            return true;
        return false;
    };

    auto followMasterPose = [&](uintptr_t comp) -> uintptr_t {
        if (comp < 0x10000) return comp;
        for (int d = 0; d < 4; ++d) {
            uintptr_t m = resolveWeakObjectPtr(m_gUObjectArray, comp + static_cast<uintptr_t>(oMasterP));
            if (m < 0x10000) break;
            comp = m;
        }
        return comp;
    };

    // ---- 收集候选组件 ----
    struct Candidate { uintptr_t comp; int count; };
    std::vector<Candidate> candidates;
    candidates.reserve(12);

    auto tryAdd = [&](uintptr_t comp) {
        if (comp < 0x10000) return;
        for (const auto& c : candidates) if (c.comp == comp) return;
        ue4::TArray<RemoteTransform> arr{};
        int n = readTransformArray(comp, arr) ? arr.Num : 0;
        candidates.push_back({comp, n});
    };

    // 1. Character.Mesh + MasterPose 链
    const uintptr_t charMesh = safeReadPtr(characterPtr + oMesh);
    tryAdd(charMesh);
    uintptr_t masterRoot = followMasterPose(charMesh);
    if (masterRoot != charMesh) tryAdd(masterRoot);

    // 2. AvatarComponent → MasterBoneComponent + meshComponentList + SkelPool
    const uintptr_t avatar = safeReadPtr(characterPtr + oAvatar);
    if (avatar >= 0x10000) {
        uintptr_t masterBone = safeReadPtr(avatar + oMasterB);
        tryAdd(masterBone);
        uintptr_t mbRoot = followMasterPose(masterBone);
        if (mbRoot != masterBone) tryAdd(mbRoot);

        // meshComponentList TSparseMap
        const int32_t oMeshList = off(m_off.Avatar_MeshComponentList, kFB_MeshCompList);
        const uintptr_t mapBase = avatar + static_cast<uintptr_t>(oMeshList);
        const uintptr_t entries = safeReadPtr(mapBase);
        const int32_t maxIdx = safeReadS32(mapBase + 0x28);
        if (entries >= 0x10000 && maxIdx > 0 && maxIdx <= 256) {
            const uintptr_t secFlags = safeReadPtr(mapBase + 0x20);
            for (int32_t wi = 0; wi < (maxIdx + 31) / 32; ++wi) {
                uint32_t flags = 0;
                if (wi < 4) safeReadMemory(mapBase + 0x10 + wi * 4, &flags, 4);
                else if (secFlags >= 0x10000) safeReadMemory(secFlags + wi * 4, &flags, 4);
                for (int b = 0; b < 32 && flags; ++b, flags >>= 1) {
                    if (!(flags & 1)) continue;
                    int32_t idx = wi * 32 + b;
                    if (idx >= maxIdx) break;
                    tryAdd(safeReadPtr(entries + static_cast<uintptr_t>(idx) * 24 + 8));
                }
            }
        }

        // SkeletalMeshCompPool
        const int32_t oPool = off(m_off.Avatar_SkeletalMeshCompPool, kFB_SkelPool);
        ue4::TArray<uintptr_t> pool{};
        if (safeReadMemory(avatar + oPool, &pool, sizeof(pool)) && isUsableRemoteArray(pool, 64)) {
            uintptr_t pd = reinterpret_cast<uintptr_t>(pool.Data);
            for (int i = 0; i < pool.Num; ++i)
                tryAdd(safeReadPtr(pd + i * 8));
        }
    }

    // 3. FPPComp → _AvatarComp → MasterBone
    const uintptr_t fpp = safeReadPtr(characterPtr + off(m_off.STBase_FPPComp, kFB_FPPComp));
    if (fpp >= 0x10000) {
        int32_t fppAvatarOff = kFB_FPPAvatar;
        const std::string fppCls = readClassName(fpp);
        if (!fppCls.empty() && fppCls[0] != '<') {
            for (const char* fn : {"_AvatarComp", "AvatarComp", "AvatarComponent"}) {
                if (auto* fi = m_interface.findFieldInHierarchy(fppCls, fn)) { fppAvatarOff = fi->offset; break; }
            }
        }
        uintptr_t fppAvatar = safeReadPtr(fpp + fppAvatarOff);
        if (fppAvatar >= 0x10000 && fppAvatar != avatar)
            tryAdd(safeReadPtr(fppAvatar + oMasterB));
    }

    if (candidates.empty()) return false;

    // ---- 按 transformCount 降序 ----
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.count > b.count; });

    // ---- 收集 SkeletalMesh 资产 ----
    std::vector<uintptr_t> assets;
    assets.reserve(candidates.size() + 2);
    auto addAsset = [&](uintptr_t ptr) {
        if (ptr >= 0x10000 && std::find(assets.begin(), assets.end(), ptr) == assets.end())
            assets.push_back(ptr);
    };
    for (const auto& c : candidates) addAsset(safeReadPtr(c.comp + oSkelMesh));
    addAsset(safeReadPtr(characterPtr + off(m_off.STBase_DefaultCharacterMesh, kFB_DefaultMesh)));
    addAsset(safeReadPtr(characterPtr + off(m_off.STBase_LastSkeletalMesh, kFB_LastSkelMesh)));

    // ---- 匹配: 找最优 (组件 × 资产) ----
    // ---- 匹配并一次性拷贝骨骼变换到栈缓冲 (避免 TOCTOU 竞争) ----
    static constexpr int kMaxTransformSlots = 256;
    RemoteTransform localTransforms[kMaxTransformSlots];
    int bestCount = 0, bestMatched = 0;
    RemoteTransform bestCtw{};
    BoneAssetCacheEntry bestMap{};
    bestMap.trackedBoneIndices.fill(-1);
    bool bestCopied = false;

    for (const auto& c : candidates) {
        if (c.count < 2 || c.count > kMaxTransformSlots) continue;
        ue4::TArray<RemoteTransform> arr{};
        if (!readTransformArray(c.comp, arr)) continue;
        if (arr.Num > kMaxTransformSlots) continue;
        RemoteTransform ctw{};
        if (!safeReadMemory(c.comp + oCtw, &ctw, sizeof(ctw))) continue;

        for (uintptr_t asset : assets) {
            BoneAssetCacheEntry map;
            if (!resolveTrackedBoneIndices(asset, map)) continue;
            int avail = 0;
            for (int32_t bi : map.trackedBoneIndices)
                if (bi >= 0 && bi < arr.Num) avail++;
            if (avail > bestMatched) {
                bestCount = arr.Num; bestMatched = avail;
                bestCtw = ctw; bestMap = map;
                // 一次性拷贝全部变换到栈, 避免后续读取时指针被 UE4 重分配
                bestCopied = safeReadMemory(
                    reinterpret_cast<uintptr_t>(arr.Data),
                    localTransforms,
                    static_cast<size_t>(arr.Num) * sizeof(RemoteTransform));
            }
        }
    }

    if (bestMatched < kMinRenderableBoneMatches || !bestCopied) {
        return false;
    }

    // ---- 读取骨骼变换 → 世界坐标 ----
    int resolved = 0;
    const float rootX = outPlayer.posX, rootY = outPlayer.posY, rootZ = outPlayer.posZ;
    for (size_t slot = 0; slot < TRACKED_BONE_COUNT; ++slot) {
        int32_t bi = bestMap.trackedBoneIndices[slot];
        if (bi < 0 || bi >= bestCount) continue;
        const RemoteTransform& bt = localTransforms[bi];
        FVector3 wp = transformPosition(bestCtw, bt.translation);
        if (!hasUsablePlayerPosition(wp)) continue;
        // 骨骼一致性: 距离玩家根位置过远说明数据已失效
        float dx = wp.x - rootX, dy = wp.y - rootY, dz = wp.z - rootZ;
        if (dx*dx + dy*dy + dz*dz > 500.0f * 500.0f) continue;
        outPlayer.bones[slot] = {wp.x, wp.y, wp.z};
        outPlayer.boneMask |= (1u << static_cast<uint32_t>(slot));
        resolved++;
    }

    if (outPlayer.boneMask == 0) return false;

    if (shouldLogEvery(s_lastSkeletonLogTime, std::chrono::milliseconds(5000)))
        LOG(LOG_LEVEL_INFO, "[Skeleton] ok key=%u resolved=%d/%d transforms=%d mask=0x%X",
            outPlayer.playerKey, resolved, bestMatched, bestCount, outPlayer.boneMask);
    return true;
}

// =====================================================================
//  类继承链检
// =====================================================================
bool MatchMonitor::isSubclassOf(uintptr_t classPtr, const char* targetName) {
    uintptr_t currentPtr = classPtr;
    int depth = 0;
    while (currentPtr != 0 && currentPtr >= 0x10000 && depth < 20) {
        std::string name = readObjName(currentPtr);
        if (name == targetName) return true;
        currentPtr = safeReadPtr(currentPtr + kUStructSuperStructOffset);
        depth++;
    }
    return false;
}


// =====================================================================
//  观战类型检
// =====================================================================
uintptr_t MatchMonitor::getLocalPlayerController() {
    uintptr_t worldPtr = safeReadPtr(m_gWorld);
    if (worldPtr == 0 || worldPtr < 0x10000) return 0;
    if (m_off.World_GameState < 0 || m_off.GS_PlayerArray < 0) return 0;
    uintptr_t gsPtr = safeReadPtr(worldPtr + m_off.World_GameState);
    if (gsPtr == 0 || gsPtr < 0x10000) return 0;
    uintptr_t arrayData = safeReadPtr(gsPtr + m_off.GS_PlayerArray);
    int32_t arrayNum = safeReadS32(gsPtr + m_off.GS_PlayerArray + 8);
    if (arrayData == 0 || arrayNum <= 0) return 0;
    uintptr_t psPtr = safeReadPtr(arrayData); // PlayerArray[0] = 本地玩家 PlayerState
    if (psPtr == 0) return 0;
    return safeReadPtr(psPtr + 0x98); // AActor::Owner = PlayerController
}

EObserverType MatchMonitor::detectObserverType() {
    uintptr_t pc = getLocalPlayerController();
    if (pc == 0) {
        LOG(LOG_LEVEL_INFO, "无法获取本地 PlayerController");
        return EObserverType::None;
    }
    bool bIsObserver = safeReadU8(pc + m_off.PC_bIsObserver) != 0;
    bool bInBattle = safeReadU8(pc + m_off.PC_bIsObserverInBattle) != 0;
    bool bIsHost = safeReadU8(pc + m_off.PC_bIsObserverHost) != 0;

    EObserverType obsType = EObserverType::None;
    if (bIsObserver) {
        if (bIsHost) obsType = EObserverType::GlobalObserver;
        else if (bInBattle) obsType = EObserverType::InSpectating;
        else obsType = EObserverType::Spectator;
    }

    LOG(LOG_LEVEL_INFO, "[ObserverType] EObserverType_%s (%d) bIsObserver=%d bInBattle=%d bIsHost=%d",
        observerTypeName(obsType), static_cast<int>(obsType), bIsObserver, bInBattle, bIsHost);

    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[ObserverType] EObserverType_%s (%d)", observerTypeName(obsType), static_cast<int>(obsType));
    writeLog(logBuf);

    return obsType;
}

bool MatchMonitor::setObserverType(EObserverType type) {
    uintptr_t pc = getLocalPlayerController();
    if (pc == 0) {
        LOG(LOG_LEVEL_ERROR, "[SetObserver] 无法获取 PlayerController");
        return false;
    }
    switch (type) {
        case EObserverType::None:
            writeMemU8(pc + m_off.PC_bIsObserver, 0);
            writeMemU8(pc + m_off.PC_bIsObserverInBattle, 0);
            writeMemU8(pc + m_off.PC_bIsObserverHost, 0);
            break;
        case EObserverType::InSpectating:
            writeMemU8(pc + m_off.PC_bIsObserver, 1);
            writeMemU8(pc + m_off.PC_bIsObserverInBattle, 1);
            writeMemU8(pc + m_off.PC_bIsObserverHost, 0);
            break;
        case EObserverType::GlobalObserver:
            writeMemU8(pc + m_off.PC_bIsObserver, 1);
            writeMemU8(pc + m_off.PC_bIsObserverInBattle, 0);
            writeMemU8(pc + m_off.PC_bIsObserverHost, 1);
            break;
        case EObserverType::FriendObserver:
        case EObserverType::Spectator:
            writeMemU8(pc + m_off.PC_bIsObserver, 1);
            writeMemU8(pc + m_off.PC_bIsObserverInBattle, 0);
            writeMemU8(pc + m_off.PC_bIsObserverHost, 0);
            break;
        default:
            LOG(LOG_LEVEL_ERROR, "[SetObserver] 无效类型: %d", static_cast<int>(type));
            return false;
    }
    LOG(LOG_LEVEL_INFO, "[SetObserver] 已设置为 EObserverType_%s (%d)",
        observerTypeName(type), static_cast<int>(type));

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "[SetObserver] -> EObserverType_%s (%d)", observerTypeName(type), static_cast<int>(type));
    writeLog(logBuf);
    return true;
}

// =====================================================================
//  网络可见范围修改
// =====================================================================
void MatchMonitor::patchActorNetCull(uintptr_t actorPtr) {
    if (actorPtr == 0) return;
    // 仅在 InProgress 状态写入 NetCullDist, 避免在加载/飞机/跳伞阶段触发网络异常断开
    if (m_currentMatchState != "InProgress") return;
    const uint64_t nowMs = nowMonotonicMs();
    const uint64_t patchIntervalMs = getNetCullPatchIntervalMs(m_currentMatchElapsedSeconds);
    auto it = m_lastNetCullPatchMs.find(actorPtr);
    if (it != m_lastNetCullPatchMs.end() && nowMs >= it->second && nowMs - it->second < patchIntervalMs) {
        return;
    }
    m_lastNetCullPatchMs[actorPtr] = nowMs;

    if (m_off.Actor_NetCullDistSq >= 0)
        writeMemFloat(actorPtr + m_off.Actor_NetCullDistSq, MAX_CULL_DIST_SQ);
    if (m_off.Char_CurrentNetCullDistSq >= 0)
        writeMemFloat(actorPtr + m_off.Char_CurrentNetCullDistSq, MAX_CULL_DIST_SQ);
}

// =====================================================================
//  GUObjectArray 扫描所Character
// =====================================================================
int MatchMonitor::scanCharacters() {
    const int scanBudget = getCharacterScanBudget(m_currentMatchElapsedSeconds);
    const int scanIntervalMs = getCharacterScanIntervalMs(m_currentMatchElapsedSeconds);
    if (scanBudget <= 0 || scanIntervalMs < 0) {
        return 0;
    }

    if (scanIntervalMs > 0) {
        const uint64_t nowMs = nowMonotonicMs();
        if (m_lastCharacterScanMs != 0 && nowMs >= m_lastCharacterScanMs
            && nowMs - m_lastCharacterScanMs < static_cast<uint64_t>(scanIntervalMs)) {
            return 0;
        }
        m_lastCharacterScanMs = nowMs;
    }

    // 通过 safeRead 访问 GUObjectArray, 避免游戏重分配时裸解引用崩溃
    uintptr_t objArrayAddr = m_gUObjectArray;
    // FUObjectArray: NumChunks @ +0xF8, TotalNumElements @ +0x100
    int numChunks = safeReadS32(objArrayAddr + 0xF8);
    int totalNum  = safeReadS32(objArrayAddr + 0x100);
    if (numChunks <= 0 || numChunks > 1000 || totalNum <= 0 || totalNum > 5000000) return 0;

    int newCharsFound = 0;

    if (m_characterScanChunkIndex < 0 || m_characterScanChunkIndex >= numChunks) {
        m_characterScanChunkIndex = 0;
        m_characterScanItemIndex = 0;
    }

    if (m_characterScanChunkIndex == 0 && m_characterScanItemIndex == 0) {
        ++m_characterScanEpoch;
        if (m_characterScanEpoch == 0) {
            m_characterScanEpoch = 1;
        }
    }

    int processedItems = 0;
    int visitedChunks = 0;

    while (processedItems < scanBudget && visitedChunks < numChunks) {
        const int ci = m_characterScanChunkIndex;
        // ChunkPtrs @ +0xC8, 每个指针 8 字节
        uintptr_t chunkBase = safeReadPtr(objArrayAddr + 0xC8 + static_cast<uintptr_t>(ci) * 8);
        // ChunkElementCounts @ +0xE8, 每个 int32 4 字节
        const int chunkCount = safeReadS32(objArrayAddr + 0xE8 + static_cast<uintptr_t>(ci) * 4);

        if (chunkBase == 0 || chunkBase < 0x10000 || chunkCount <= 0) {
            m_characterScanChunkIndex = (ci + 1) % numChunks;
            m_characterScanItemIndex = 0;
            visitedChunks++;
            continue;
        }

        int wi = m_characterScanItemIndex;
        if (wi < 0 || wi >= chunkCount) {
            wi = 0;
        }

        // FUObjectItem 大小 = 24 字节 (Object* @ +0x00)
        static constexpr size_t kFUObjectItemSize = 24;
        BatchMemReader charBatch;

        while (wi < chunkCount && processedItems < scanBudget) {
            // 通过 safeReadPtr 读取 FUObjectItem.Object
            uintptr_t objPtr = safeReadPtr(chunkBase + static_cast<uintptr_t>(wi) * kFUObjectItemSize);
            processedItems++;
            wi++;
            if (objPtr == 0 || objPtr < 0x10000) continue;
            uintptr_t classPtr = safeReadPtr(objPtr + kUObjectClassPrivateOffset);
            if (classPtr == 0) continue;

            auto it = m_characterClassSet.find(classPtr);
            if (it != m_characterClassSet.end()) {
                if (!it->second) continue;
            } else {
                bool isChar = isSubclassOf(classPtr, "STExtraBaseCharacter");
                m_characterClassSet[classPtr] = isChar;
                if (!isChar) continue;
            }

            // ----- 批量读取 Character (best-effort, get() 自动回退) -----
            charBatch.read(objPtr, m_charReadSize);

            uint32_t playerKey = charBatch.getU32(m_off.Char_PlayerKey);
            if (playerKey == 0) continue;

            PlayerNode* existing = m_playerList.findByKey(playerKey);
            if (existing != nullptr && existing->source == PlayerSource::PlayerArray) {
                continue;
            }

            int32_t teamID = charBatch.getS32(m_off.Char_TeamID);
            float health = charBatch.getFloat(m_off.Char_Health);
            float healthMax = charBatch.getFloat(m_off.Char_HealthMax);
            bool bDead = (charBatch.getU8(m_off.Char_bDead) & 1) != 0;
            std::string playerName = readFString(objPtr + m_off.Char_PlayerName);

            FVector3 loc;
            // 位置: 通过 RootComponent -> SceneComponent 获取
            if (m_off.Actor_RootComponent >= 0) {
                const uintptr_t rootComp = charBatch.getPtr(m_off.Actor_RootComponent);
                if (rootComp != 0 && m_off.SceneComp_Translation >= 0) {
                    loc.x = safeReadFloat(rootComp + m_off.SceneComp_Translation);
                    loc.y = safeReadFloat(rootComp + m_off.SceneComp_Translation + 4);
                    loc.z = safeReadFloat(rootComp + m_off.SceneComp_Translation + 8);
                }
            } else {
                getActorLocation(objPtr, loc);
            }

            if (healthMax <= 0) continue;

            patchActorNetCull(objPtr);

            PlayerNode data;
            data.teamID = teamID;
            data.playerName = playerName;
            data.isAI = false;
            data.liveState = bDead ? 1 : 0;
            data.health = health;
            data.healthMax = healthMax;
            data.kills = 0;
            data.pos = loc;
            data.characterPtr = objPtr;
            data.source = PlayerSource::CharacterScan;
            PlayerNode* node = m_playerList.upsert(playerKey, data);
            if (node) {
                node->source = PlayerSource::CharacterScan;
                node->lastSeenCharacterScanEpoch = m_characterScanEpoch;
            }
            newCharsFound++;
        }

        if (wi >= chunkCount) {
            m_characterScanChunkIndex = (ci + 1) % numChunks;
            m_characterScanItemIndex = 0;
            visitedChunks++;
            if (m_characterScanChunkIndex == 0) {
                m_lastCompletedCharacterScanEpoch = m_characterScanEpoch;
            }
        } else {
            m_characterScanChunkIndex = ci;
            m_characterScanItemIndex = wi;
            break;
        }
    }

    return newCharsFound;
}

// =====================================================================
//  遍历 PlayerArray, 更新双向链表
// =====================================================================
int MatchMonitor::updatePlayerList(uintptr_t gameStatePtr) {
    uintptr_t arrayData = safeReadPtr(gameStatePtr + m_off.GS_PlayerArray);
    int32_t arrayNum = safeReadS32(gameStatePtr + m_off.GS_PlayerArray + 8);

    // 读取 GameState 的全局玩家计数 (偏移可能未解析, 安全检查)
    int32_t totalPlayerNum = m_off.GS_TotalPlayerNum >= 0 ? safeReadS32(gameStatePtr + m_off.GS_TotalPlayerNum) : 0;
    int32_t playerNum = m_off.GS_PlayerNum >= 0 ? safeReadS32(gameStatePtr + m_off.GS_PlayerNum) : 0;
    int32_t aliveNum = m_off.GS_AlivePlayerNum >= 0 ? safeReadS32(gameStatePtr + m_off.GS_AlivePlayerNum) : 0;
    int32_t aliveRealNum = m_off.GS_AliveRealPlayerNum >= 0 ? safeReadS32(gameStatePtr + m_off.GS_AliveRealPlayerNum) : 0;

    if (arrayNum != m_lastReportedArrayNum || totalPlayerNum != m_lastReportedTotal) {
        LOG(LOG_LEVEL_INFO, "[PlayerCount] PlayerArray=%d TotalPlayerNum=%d PlayerNum=%d AlivePlayerNum=%d AliveRealPlayerNum=%d",
            arrayNum, totalPlayerNum, playerNum, aliveNum, aliveRealNum);
        m_lastReportedArrayNum = arrayNum;
        m_lastReportedTotal = totalPlayerNum;
    }

    if (arrayData == 0 || arrayNum <= 0 || arrayNum > 500) return 0;

    // ----- 读取全部 PlayerState 指针 -----
    // 去掉批量读取: TArray 指针数组可能跨页边界导致整块 memcpy 失败,
    // 而逐个 safeReadPtr 可以跳过单个无效条目而不丢失全部玩家
    std::vector<uintptr_t> psPtrs(static_cast<size_t>(arrayNum), 0);
    for (int i = 0; i < arrayNum; i++) {
        psPtrs[i] = safeReadPtr(arrayData + i * 8);
    }

    std::unordered_map<uint32_t, bool> seenKeys;
    int updated = 0;
    BatchMemReader psBatch;   // 复用, 避免每次循环重新构造
    BatchMemReader charBatch;

    for (int i = 0; i < arrayNum; i++) {
        const uintptr_t psPtr = psPtrs[i];
        if (psPtr == 0) continue;

        // ----- 批量读取 PlayerState (best-effort, get() 自动回退) -----
        psBatch.read(psPtr, m_psReadSize);

        // 从本地缓冲提取字段 (命中缓冲=零开销, 未命中=自动回退到单独读取)
        const uint32_t playerKey = psBatch.getU32(m_off.PS_PlayerKey);
        if (playerKey == 0) continue;

        int32_t teamID = psBatch.getS32(m_off.PS_TeamID);
        bool isAI = psBatch.getU8(m_off.PS_bAIPlayer) != 0;
        uint8_t liveState = psBatch.getU8(m_off.PS_LiveState);
        float health = psBatch.getFloat(m_off.PS_PlayerHealth);
        float healthMax = psBatch.getFloat(m_off.PS_PlayerHealthMax);
        int32_t kills = psBatch.getS32(m_off.PS_Kills);
        // FString 需要跟随指针, 仍需单独读取
        std::string playerName = readFString(psPtr + m_off.PS_PlayerName);

        // 优先通过 CharacterOwner -> RootComponent 获取精确位置
        FVector3 loc;
        const uintptr_t charOwner = psBatch.getPtr(m_off.PS_CharacterOwner);
        if (charOwner != 0) {
            // ----- 批量读取 Character (best-effort, get() 自动回退) -----
            charBatch.read(charOwner, m_charReadSize);

            if (m_off.Char_Health >= 0) {
                const float charHealth = charBatch.getFloat(m_off.Char_Health);
                if (std::isfinite(charHealth) && charHealth >= 0.0f) {
                    health = charHealth;
                }
            }
            if (m_off.Char_HealthMax >= 0) {
                const float charHealthMax = charBatch.getFloat(m_off.Char_HealthMax);
                if (std::isfinite(charHealthMax) && charHealthMax > 0.0f) {
                    healthMax = charHealthMax;
                }
            }
            if (m_off.Char_bDead >= 0) {
                const bool bDead = (charBatch.getU8(m_off.Char_bDead) & 1) != 0;
                liveState = bDead ? 1 : 0;
            }
            // 位置: 通过 RootComponent -> SceneComponent 获取
            if (m_off.Actor_RootComponent >= 0) {
                const uintptr_t rootComp = charBatch.getPtr(m_off.Actor_RootComponent);
                if (rootComp != 0 && m_off.SceneComp_Translation >= 0) {
                    loc.x = safeReadFloat(rootComp + m_off.SceneComp_Translation);
                    loc.y = safeReadFloat(rootComp + m_off.SceneComp_Translation + 4);
                    loc.z = safeReadFloat(rootComp + m_off.SceneComp_Translation + 8);
                }
            } else {
                getActorLocation(charOwner, loc);
            }
            patchActorNetCull(charOwner);
        }
        // 回退SelfLocAndRot
        if (loc.x == 0 && loc.y == 0 && loc.z == 0 && m_off.PS_SelfLocAndRot >= 0) {
            loc = psBatch.getVec3(m_off.PS_SelfLocAndRot);
        }

        PlayerNode data;
        data.teamID     = teamID;
        data.playerName = playerName;
        data.isAI       = isAI;
        data.liveState  = liveState;
        data.health     = health;
        data.healthMax  = healthMax;
        data.kills      = kills;
        data.pos        = loc;
        data.characterPtr = charOwner;
        PlayerNode* node = m_playerList.upsert(playerKey, data);
        if (node) {
            node->source = PlayerSource::PlayerArray;
        }
        seenKeys[playerKey] = true;
        updated++;

        // 记录本地玩家 key / TeamID
        if (i == 0) {
            if (teamID > 0) {
                m_myTeamID = teamID;
            }
            m_myPlayerKey = playerKey;
        }
    }

    // 移除已退出的玩家
    std::vector<uint32_t> toRemove;
    PlayerNode* cur = m_playerList.head();
    while (cur) {
        if (seenKeys.find(cur->playerKey) == seenKeys.end()) {
            if (cur->source == PlayerSource::PlayerArray) {
                toRemove.push_back(cur->playerKey);
            } else if (m_lastCompletedCharacterScanEpoch > 0
                && cur->lastSeenCharacterScanEpoch < m_lastCompletedCharacterScanEpoch) {
                toRemove.push_back(cur->playerKey);
            }
        }
        cur = cur->next;
    }
    for (uint32_t key : toRemove) m_playerList.remove(key);

    // 扫描 GUObjectArray 获取附近的 Character (包括敌人)，改为跨多次轮询的分片扫描。
    scanCharacters();

    return updated;
}

void MatchMonitor::refreshTrackedPlayersFast() {
    PlayerNode* cur = m_playerList.head();
    while (cur) {
        if (cur->characterPtr != 0 && cur->characterPtr >= 0x10000) {
            FVector3 loc{};
            if (getActorLocation(cur->characterPtr, loc) && hasUsablePlayerPosition(loc)) {
                cur->pos = loc;
            }

            if (m_off.Char_Health >= 0) {
                const float charHealth = safeReadFloat(cur->characterPtr + m_off.Char_Health);
                if (std::isfinite(charHealth) && charHealth >= 0.0f) {
                    cur->health = charHealth;
                }
            }
            if (m_off.Char_HealthMax >= 0) {
                const float charHealthMax = safeReadFloat(cur->characterPtr + m_off.Char_HealthMax);
                if (std::isfinite(charHealthMax) && charHealthMax > 0.0f) {
                    cur->healthMax = charHealthMax;
                }
            }
            if (m_off.Char_bDead >= 0) {
                const bool bDead = (safeReadU8(cur->characterPtr + m_off.Char_bDead) & 1) != 0;
                cur->liveState = bDead ? 1 : 0;
            } else if (std::isfinite(cur->health) && cur->health <= 0.0f) {
                cur->liveState = 1;
            }
        } else if (std::isfinite(cur->health) && cur->health <= 0.0f) {
            cur->liveState = 1;
        }

        cur = cur->next;
    }
}

void MatchMonitor::fillCameraSnapshot(ue4draw::DrawGameData& drawData) {
    const uintptr_t pc = getLocalPlayerController();
    if (pc == 0) {
        return;
    }

    const uintptr_t pcm = safeReadPtr(pc + 0x658);  // PlayerController.PlayerCameraManager
    if (pcm == 0) {
        return;
    }

    auto readMinimalViewInfo = [&](uintptr_t viewInfoPtr,
                                   float& locX,
                                   float& locY,
                                   float& locZ,
                                   float& pitch,
                                   float& yaw,
                                   float& roll,
                                   float& fov) {
        locX = safeReadFloat(viewInfoPtr + 0x0);
        locY = safeReadFloat(viewInfoPtr + 0x4);
        locZ = safeReadFloat(viewInfoPtr + 0x8);
        pitch = safeReadFloat(viewInfoPtr + 0x18);
        yaw = safeReadFloat(viewInfoPtr + 0x1C);
        roll = safeReadFloat(viewInfoPtr + 0x20);
        fov = safeReadFloat(viewInfoPtr + 0x30);
    };
    auto hasFiniteCameraPose = [](float locX,
                                  float locY,
                                  float locZ,
                                  float pitch,
                                  float yaw,
                                  float roll) {
        return std::isfinite(locX) && std::isfinite(locY) && std::isfinite(locZ)
            && (std::fabs(locX) > 1.0f || std::fabs(locY) > 1.0f || std::fabs(locZ) > 1.0f)
            && std::isfinite(pitch) && std::isfinite(yaw) && std::isfinite(roll);
    };
    auto hasValidFov = [](float fov) {
        return std::isfinite(fov) && fov >= 30.0f && fov <= 170.0f;
    };

    float camLocX = 0.0f;
    float camLocY = 0.0f;
    float camLocZ = 0.0f;
    float camPitch = 0.0f;
    float camYaw = 0.0f;
    float camRoll = 0.0f;
    float camFov = 0.0f;

    // CameraCacheEntry.POV @ PCM+0x650, MinimalViewInfo.FOV @ +0x30 => PCM+0x680
    readMinimalViewInfo(pcm + 0x650, camLocX, camLocY, camLocZ, camPitch, camYaw, camRoll, camFov);
    if (!hasFiniteCameraPose(camLocX, camLocY, camLocZ, camPitch, camYaw, camRoll)) {
        // CachedViewPOV is a direct MinimalViewInfo at PCM+0x2120.
        readMinimalViewInfo(pcm + 0x2120, camLocX, camLocY, camLocZ, camPitch, camYaw, camRoll, camFov);
    }

    if (!hasValidFov(camFov)) {
        camFov = safeReadFloat(pcm + 0x5E0);
    }

    if (hasFiniteCameraPose(camLocX, camLocY, camLocZ, camPitch, camYaw, camRoll)) {
        drawData.camLocX = camLocX;
        drawData.camLocY = camLocY;
        drawData.camLocZ = camLocZ;
        drawData.camPitch = camPitch;
        drawData.camYaw = camYaw;
        drawData.camRoll = camRoll;
        drawData.camFOV = hasValidFov(camFov) ? camFov : 90.0f;
    }
}

// =====================================================================
//  Aimbot 日志 — 写入独立文件方便排查
// =====================================================================
void MatchMonitor::openAimbotLog() {
    std::lock_guard<std::mutex> lock(m_aimbotLogMutex);
    if (m_aimbotLogFp) return;
    mkdir(m_logDir.c_str(), 0777);
    std::string path = m_logDir + "aimbot_log.txt";
    m_aimbotLogFp = fopen(path.c_str(), "a");
    if (m_aimbotLogFp) {
        fprintf(m_aimbotLogFp, "=== Aimbot Log Start ===\n");
        fflush(m_aimbotLogFp);
    }
}

void MatchMonitor::writeAimbotLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(m_aimbotLogMutex);
    if (!m_aimbotLogFp) return;
    auto now = Clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    fprintf(m_aimbotLogFp, "[%lld] ", static_cast<long long>(ms));
    va_list args;
    va_start(args, fmt);
    vfprintf(m_aimbotLogFp, fmt, args);
    va_end(args);
    fprintf(m_aimbotLogFp, "\n");
    fflush(m_aimbotLogFp);
}

void MatchMonitor::closeAimbotLog() {
    std::lock_guard<std::mutex> lock(m_aimbotLogMutex);
    if (m_aimbotLogFp) {
        fprintf(m_aimbotLogFp, "=== Aimbot Log End ===\n");
        fclose(m_aimbotLogFp);
        m_aimbotLogFp = nullptr;
    }
}

// =====================================================================
//  开镜检测 — 读取本地角色的 bMarkScopeIn
// =====================================================================
bool MatchMonitor::isLocalPlayerScoping() {
    if (m_off.Char_bMarkScopeIn < 0) return false;
    PlayerNode* myNode = (m_myPlayerKey != 0) ? m_playerList.findByKey(m_myPlayerKey) : nullptr;
    if (!myNode) myNode = m_playerList.head();
    if (!myNode || myNode->characterPtr == 0) return false;
    uint8_t scopeVal = safeReadU8(myNode->characterPtr + static_cast<uintptr_t>(m_off.Char_bMarkScopeIn));
    return scopeVal != 0;
}

// =====================================================================
//  角度工具
// =====================================================================
namespace {
float normalizeAngle180(float a) {
    a = std::fmod(a + 180.0f, 360.0f);
    if (a < 0.0f) a += 360.0f;
    return a - 180.0f;
}
float lerpAngle(float from, float to, float t) {
    float diff = normalizeAngle180(to - from);
    return from + diff * t;
}
} // anonymous namespace

// =====================================================================
//  getBulletInfo — 从子弹 Actor 获取弹速/重力数据
//
//  参数: bulletActorPtr = STExtraShootWeaponBulletBase 实例指针
//  链路: BulletActor → PMComp(+0x5C8) → ProjectileMovementComponent
//        BulletActor → LaunchGravityScale(+0x5D8)
//        BulletActor → MaxNoGravityRange(+0x5D4)
//        BulletActor → ShootDir(+0x610)
// =====================================================================
BulletInfo MatchMonitor::getBulletInfo(uintptr_t bulletActorPtr) {
    BulletInfo info;
    if (bulletActorPtr == 0 || bulletActorPtr < 0x10000) {
        LOG(LOG_LEVEL_WARN, "[BulletInfo] bulletActorPtr 无效: %p", (void*)bulletActorPtr);
        return info;
    }

    // 从子弹 Actor 读取直属字段
    const int32_t offLaunchGrav = (m_off.Bullet_LaunchGravityScale >= 0) ? m_off.Bullet_LaunchGravityScale : 0x5D8;
    const int32_t offMaxNoGrav  = (m_off.Bullet_MaxNoGravityRange >= 0) ? m_off.Bullet_MaxNoGravityRange : 0x5D4;
    const int32_t offShootDir   = (m_off.Bullet_ShootDir >= 0) ? m_off.Bullet_ShootDir : 0x610;

    info.launchGravityScale = safeReadFloat(bulletActorPtr + offLaunchGrav);
    info.maxNoGravityRange  = safeReadS32(bulletActorPtr + offMaxNoGrav);
    info.shootDir.x = safeReadFloat(bulletActorPtr + offShootDir);
    info.shootDir.y = safeReadFloat(bulletActorPtr + offShootDir + 4);
    info.shootDir.z = safeReadFloat(bulletActorPtr + offShootDir + 8);

    // 获取 ProjectileMovementComponent 指针
    const int32_t offPMComp = (m_off.Bullet_PMComp >= 0) ? m_off.Bullet_PMComp : 0x5C8;
    const uintptr_t pmcPtr = safeReadPtr(bulletActorPtr + offPMComp);
    if (pmcPtr == 0 || pmcPtr < 0x10000) {
        LOG(LOG_LEVEL_WARN, "[BulletInfo] PMComp 为空 bullet=%p offset=0x%X", (void*)bulletActorPtr, offPMComp);
        info.valid = true; // 子弹自身字段有效, 只是没有 PMC
        return info;
    }

    // 从 ProjectileMovementComponent 读取弹速/重力
    const int32_t offInitSpeed = (m_off.PMC_InitialSpeed >= 0) ? m_off.PMC_InitialSpeed : 0x164;
    const int32_t offMaxSpeed  = (m_off.PMC_MaxSpeed >= 0) ? m_off.PMC_MaxSpeed : 0x168;
    const int32_t offVelocity  = (m_off.PMC_Velocity >= 0) ? m_off.PMC_Velocity : 0x13C;
    const int32_t offGravScale = (m_off.PMC_ProjectileGravityScale >= 0) ? m_off.PMC_ProjectileGravityScale : 0x180;

    info.initialSpeed          = safeReadFloat(pmcPtr + offInitSpeed);
    info.maxSpeed              = safeReadFloat(pmcPtr + offMaxSpeed);
    info.velocity.x            = safeReadFloat(pmcPtr + offVelocity);
    info.velocity.y            = safeReadFloat(pmcPtr + offVelocity + 4);
    info.velocity.z            = safeReadFloat(pmcPtr + offVelocity + 8);
    info.projectileGravityScale = safeReadFloat(pmcPtr + offGravScale);
    info.valid = true;

    LOG(LOG_LEVEL_INFO, "[BulletInfo] initSpeed=%.1f maxSpeed=%.1f vel=(%.1f,%.1f,%.1f) gravScale=%.2f launchGrav=%.2f noGravRange=%d",
        info.initialSpeed, info.maxSpeed,
        info.velocity.x, info.velocity.y, info.velocity.z,
        info.projectileGravityScale, info.launchGravityScale, info.maxNoGravityRange);

    return info;
}

// =====================================================================
//  getRecoilInfo — 从 BulletTrackComponent 获取后坐力数据
//
//  参数: bulletTrackCompPtr = BulletTrackComponent 实例指针
//        (通过武器的组件列表获取, 或通过 GUObjectArray 扫描)
//  链路: WeaponActor → BulletTrackComponent
//        BulletTrackComponent.CurRecoilValue      (+0x1C0)
//        BulletTrackComponent.VerticalRecoilTarget (+0x1C4)
//        BulletTrackComponent.HorizontalRecoilTarget (+0x1C8)
// =====================================================================
RecoilInfo MatchMonitor::getRecoilInfo(uintptr_t bulletTrackCompPtr) {
    RecoilInfo info;
    if (bulletTrackCompPtr == 0 || bulletTrackCompPtr < 0x10000) {
        LOG(LOG_LEVEL_WARN, "[RecoilInfo] bulletTrackCompPtr 无效: %p", (void*)bulletTrackCompPtr);
        return info;
    }

    const int32_t offCurRecoil   = (m_off.BTC_CurRecoilValue >= 0)          ? m_off.BTC_CurRecoilValue          : 0x1C0;
    const int32_t offVRecoil     = (m_off.BTC_VerticalRecoilTarget >= 0)    ? m_off.BTC_VerticalRecoilTarget    : 0x1C4;
    const int32_t offHRecoil     = (m_off.BTC_HorizontalRecoilTarget >= 0)  ? m_off.BTC_HorizontalRecoilTarget  : 0x1C8;
    const int32_t offVRecovery   = (m_off.BTC_VerticalRecoveryTarget >= 0)  ? m_off.BTC_VerticalRecoveryTarget  : 0x1CC;
    const int32_t offPoseRecoil  = (m_off.BTC_PoseRecoilFactor >= 0)        ? m_off.BTC_PoseRecoilFactor        : 0x1D4;
    const int32_t offAccVRecoil  = (m_off.BTC_AccessoriesVRecoilFactor >= 0) ? m_off.BTC_AccessoriesVRecoilFactor : 0x1D8;
    const int32_t offVModifier   = (m_off.BTC_VerticalRecoilFactorModifier >= 0) ? m_off.BTC_VerticalRecoilFactorModifier : 0x1E0;
    const int32_t offAccHRecoil  = (m_off.BTC_AccessoriesHRecoilFactor >= 0) ? m_off.BTC_AccessoriesHRecoilFactor : 0x1E4;
    const int32_t offHModifier   = (m_off.BTC_HorizontalRecoilFactorModifier >= 0) ? m_off.BTC_HorizontalRecoilFactorModifier : 0x1F0;
    const int32_t offAccVTarget  = (m_off.BTC_AccVerticalRecoilTarget >= 0) ? m_off.BTC_AccVerticalRecoilTarget : 0x5B0;

    info.curRecoilValue          = safeReadFloat(bulletTrackCompPtr + offCurRecoil);
    info.verticalRecoilTarget    = safeReadFloat(bulletTrackCompPtr + offVRecoil);
    info.horizontalRecoilTarget  = safeReadFloat(bulletTrackCompPtr + offHRecoil);
    info.verticalRecoveryTarget  = safeReadFloat(bulletTrackCompPtr + offVRecovery);
    info.poseRecoilFactor        = safeReadFloat(bulletTrackCompPtr + offPoseRecoil);
    info.accVRecoilFactor        = safeReadFloat(bulletTrackCompPtr + offAccVRecoil);
    info.vRecoilFactorModifier   = safeReadFloat(bulletTrackCompPtr + offVModifier);
    info.accHRecoilFactor        = safeReadFloat(bulletTrackCompPtr + offAccHRecoil);
    info.hRecoilFactorModifier   = safeReadFloat(bulletTrackCompPtr + offHModifier);
    info.accVerticalRecoilTarget = safeReadFloat(bulletTrackCompPtr + offAccVTarget);
    info.valid = true;

    LOG(LOG_LEVEL_INFO, "[RecoilInfo] curRecoil=%.2f vTarget=%.2f hTarget=%.2f vRecovery=%.2f pose=%.2f accV=%.2f vMod=%.2f accH=%.2f hMod=%.2f accVTgt=%.2f",
        info.curRecoilValue, info.verticalRecoilTarget, info.horizontalRecoilTarget,
        info.verticalRecoveryTarget, info.poseRecoilFactor, info.accVRecoilFactor,
        info.vRecoilFactorModifier, info.accHRecoilFactor, info.hRecoilFactorModifier,
        info.accVerticalRecoilTarget);

    return info;
}

// =====================================================================
//  getCharacterSpeedInfo — 从角色获取移动速度/重力数据
//
//  参数: characterPtr = Character Actor 实例指针
//  链路: Character → CharacterMovement(+0x658) → CharacterMovementComponent
//        CMC.MaxWalkSpeed         (+0x264)
//        CMC.MaxWalkSpeedCrouched (+0x268)
//        CMC.GravityScale         (+0x20C)
//        CMC.JumpZVelocity        (+0x214)
//        CMC.Velocity             (+0x13C, 继承自 MovementComponent)
// =====================================================================
CharacterSpeedInfo MatchMonitor::getCharacterSpeedInfo(uintptr_t characterPtr) {
    CharacterSpeedInfo info;
    if (characterPtr == 0 || characterPtr < 0x10000) {
        LOG(LOG_LEVEL_WARN, "[SpeedInfo] characterPtr 无效: %p", (void*)characterPtr);
        return info;
    }

    // 获取 CharacterMovementComponent 指针
    const int32_t offCMC = (m_off.Char_CharacterMovement >= 0) ? m_off.Char_CharacterMovement : 0x658;
    const uintptr_t cmcPtr = safeReadPtr(characterPtr + offCMC);
    if (cmcPtr == 0 || cmcPtr < 0x10000) {
        LOG(LOG_LEVEL_WARN, "[SpeedInfo] CharacterMovement 为空 char=%p offset=0x%X", (void*)characterPtr, offCMC);
        return info;
    }

    const int32_t offWalk      = (m_off.CMC_MaxWalkSpeed >= 0)         ? m_off.CMC_MaxWalkSpeed         : 0x264;
    const int32_t offCrouch    = (m_off.CMC_MaxWalkSpeedCrouched >= 0) ? m_off.CMC_MaxWalkSpeedCrouched : 0x268;
    const int32_t offSwim      = (m_off.CMC_MaxSwimSpeed >= 0)         ? m_off.CMC_MaxSwimSpeed         : 0x26C;
    const int32_t offFly       = (m_off.CMC_MaxFlySpeed >= 0)          ? m_off.CMC_MaxFlySpeed          : 0x270;
    const int32_t offAccel     = (m_off.CMC_MaxAcceleration >= 0)      ? m_off.CMC_MaxAcceleration      : 0x278;
    const int32_t offGrav      = (m_off.CMC_GravityScale >= 0)         ? m_off.CMC_GravityScale         : 0x20C;
    const int32_t offJump      = (m_off.CMC_JumpZVelocity >= 0)        ? m_off.CMC_JumpZVelocity        : 0x214;
    const int32_t offVelocity  = (m_off.CMC_Velocity >= 0)             ? m_off.CMC_Velocity             : 0x13C;

    info.maxWalkSpeed         = safeReadFloat(cmcPtr + offWalk);
    info.maxWalkSpeedCrouched = safeReadFloat(cmcPtr + offCrouch);
    info.maxSwimSpeed         = safeReadFloat(cmcPtr + offSwim);
    info.maxFlySpeed          = safeReadFloat(cmcPtr + offFly);
    info.maxAcceleration      = safeReadFloat(cmcPtr + offAccel);
    info.gravityScale         = safeReadFloat(cmcPtr + offGrav);
    info.jumpZVelocity        = safeReadFloat(cmcPtr + offJump);
    info.velocity.x           = safeReadFloat(cmcPtr + offVelocity);
    info.velocity.y           = safeReadFloat(cmcPtr + offVelocity + 4);
    info.velocity.z           = safeReadFloat(cmcPtr + offVelocity + 8);
    info.valid = true;

    LOG(LOG_LEVEL_INFO, "[SpeedInfo] walk=%.1f crouch=%.1f swim=%.1f fly=%.1f accel=%.1f grav=%.2f jump=%.1f vel=(%.1f,%.1f,%.1f)",
        info.maxWalkSpeed, info.maxWalkSpeedCrouched, info.maxSwimSpeed, info.maxFlySpeed,
        info.maxAcceleration, info.gravityScale, info.jumpZVelocity,
        info.velocity.x, info.velocity.y, info.velocity.z);

    return info;
}

// =====================================================================
//  自瞄 — 开镜时锁定200米内血量最少的敌人直至死亡
//
//  修复:
//    - 角度平滑插值, 防止瞬间跳转导致相机抖动
//    - 限频 30ms 写入一次 ControlRotation, 避免与游戏引擎争抢
//    - 骨骼数据新鲜度检查 (>2秒视为过期)
//    - 读取当前 ControlRotation 做差值插值而非直接覆盖
// =====================================================================
void MatchMonitor::aimAtNearestEnemy() {
    if (!m_aimbotEnabled) return;
    // 读取菜单开关 (GUI 线程通过 SharedUE4Data 设置)
    if (!ue4draw::SharedUE4Data::getInstance().isAimbotEnabled()) {
        if (m_aimbotLockedKey != 0) {
            m_aimbotLockedKey = 0;
        }
        return;
    }
    if (m_off.Ctrl_ControlRotation < 0) return;

    if (!m_aimbotLogFp) openAimbotLog();

    // 条件1: 必须开镜
    const bool scoping = isLocalPlayerScoping();
    if (!scoping) {
        if (m_aimbotLockedKey != 0) {
            writeAimbotLog("[Aimbot] Scope out, clearing locked target key=%u", m_aimbotLockedKey);
            m_aimbotLockedKey = 0;
        }
        return;
    }

    // 限频: 至少 30ms 写入一次, 防止与引擎争抢
    const uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    if (nowMs - m_lastAimbotWriteMs < 30) return;

    const uintptr_t pc = getLocalPlayerController();
    if (pc == 0) return;

    // 读取当前 ControlRotation (用于平滑插值)
    const uintptr_t ctrlRotAddr = pc + static_cast<uintptr_t>(m_off.Ctrl_ControlRotation);
    const float curPitch = safeReadFloat(ctrlRotAddr + 0);
    const float curYaw   = safeReadFloat(ctrlRotAddr + 4);
    if (!std::isfinite(curPitch) || !std::isfinite(curYaw)) return;

    // 读取当前相机位置作为射线起点
    const int32_t pcmOff2 = (m_off.PC_PlayerCameraManager >= 0) ? m_off.PC_PlayerCameraManager : 0x658;
    const uintptr_t pcm = safeReadPtr(pc + pcmOff2);
    if (pcm == 0) return;

    const int32_t camCacheOff = (m_off.PCM_CameraCache >= 0) ? m_off.PCM_CameraCache : 0x640;
    const int32_t camPovOff = camCacheOff + 0x10;
    const float camX = safeReadFloat(pcm + camPovOff + 0x0);
    const float camY = safeReadFloat(pcm + camPovOff + 0x4);
    const float camZ = safeReadFloat(pcm + camPovOff + 0x8);
    if (!std::isfinite(camX) || !std::isfinite(camY) || !std::isfinite(camZ)) return;
    if (std::fabs(camX) < 1.0f && std::fabs(camY) < 1.0f && std::fabs(camZ) < 1.0f) return;

    constexpr float kMaxDistUU = 20000.0f;                  // 200米
    constexpr float kMaxDistSq = kMaxDistUU * kMaxDistUU;
    constexpr float kMinDistSq = 100.0f;
    constexpr uint64_t kMaxBoneAgeMs = 2000;                 // 骨骼数据有效期
    const float smoothT = std::clamp(1.0f / m_aimbotSmoothing, 0.05f, 1.0f);

    const int boneIdx = m_aimbotTargetBone;  // 4=head

    // ---- 辅助: 从 PlayerNode 获取瞄准坐标 ----
    auto getTargetPos = [&](const PlayerNode* node, FVector3& outTgt) -> bool {
        // 优先用骨骼 (检查新鲜度)
        if (boneIdx >= 0 && boneIdx < static_cast<int>(TRACKED_BONE_COUNT)
            && (node->cachedBoneMask & (1u << boneIdx)) != 0
            && (nowMs - node->cachedBoneTimestampMs) < kMaxBoneAgeMs) {
            const float tx = node->cachedBones[boneIdx].x;
            const float ty = node->cachedBones[boneIdx].y;
            const float tz = node->cachedBones[boneIdx].z;
            if (std::isfinite(tx) && std::isfinite(ty) && std::isfinite(tz)
                && (std::fabs(tx) > 1.0f || std::fabs(ty) > 1.0f)) {
                outTgt = {tx, ty, tz};
                return true;
            }
        }
        // 回退: 角色位置 + 头部高度偏移
        if (std::isfinite(node->pos.x) && std::isfinite(node->pos.y) && std::isfinite(node->pos.z)
            && (std::fabs(node->pos.x) > 1.0f || std::fabs(node->pos.y) > 1.0f)) {
            outTgt = {node->pos.x, node->pos.y, node->pos.z + 60.0f};
            return true;
        }
        return false;
    };

    // ---- 辅助: 计算到目标的距离平方和方向角 ----
    auto calcAim = [&](const FVector3& tgt, float& outDistSq, float& outPitch, float& outYaw) -> bool {
        const float dx = tgt.x - camX, dy = tgt.y - camY, dz = tgt.z - camZ;
        outDistSq = dx * dx + dy * dy + dz * dz;
        if (outDistSq < kMinDistSq || outDistSq > kMaxDistSq) return false;
        const float d2D = std::sqrt(dx * dx + dy * dy);
        if (d2D < 0.01f) return false;
        outYaw   = std::atan2(dy, dx) * (180.0f / 3.14159265f);
        outPitch = std::atan2(dz, d2D) * (180.0f / 3.14159265f);
        return std::isfinite(outYaw) && std::isfinite(outPitch);
    };

    // ---- 检查当前锁定目标是否仍然有效 ----
    if (m_aimbotLockedKey != 0) {
        PlayerNode* locked = m_playerList.findByKey(m_aimbotLockedKey);
        if (locked && locked->liveState == 0 && locked->health > 0.0f) {
            FVector3 tgt{};
            float distSq = 0.0f, aimPitch = 0.0f, aimYaw = 0.0f;
            if (getTargetPos(locked, tgt) && calcAim(tgt, distSq, aimPitch, aimYaw)) {
                // 平滑插值
                float finalPitch = lerpAngle(curPitch, aimPitch, smoothT);
                float finalYaw   = lerpAngle(curYaw,   aimYaw,   smoothT);
                writeMemFloat(ctrlRotAddr + 0, finalPitch);
                writeMemFloat(ctrlRotAddr + 4, finalYaw);
                m_lastAimbotWriteMs = nowMs;

                static Clock::time_point s_lastLockLog;
                if (shouldLogEvery(s_lastLockLog, std::chrono::milliseconds(2000))) {
                    writeAimbotLog("[Aimbot] Tracking key=%u hp=%.0f dist=%.0fm",
                        m_aimbotLockedKey, locked->health, std::sqrt(distSq) / 100.0f);
                }
                return;
            }
        }
        // 锁定失效
        writeAimbotLog("[Aimbot] Lost lock key=%u, re-selecting", m_aimbotLockedKey);
        m_aimbotLockedKey = 0;
    }

    // ---- 选择新目标: 200米内血量最少的敌人 ----
    float bestHP = 1e18f;
    float bestDistSq = 1e18f;
    PlayerNode* bestNode = nullptr;
    FVector3 bestTarget{};

    PlayerNode* cur = m_playerList.head();
    while (cur) {
        if (m_myPlayerKey != 0 && cur->playerKey == m_myPlayerKey) { cur = cur->next; continue; }
        if (m_myTeamID > 0 && cur->teamID == m_myTeamID) { cur = cur->next; continue; }
        if (cur->liveState != 0 || cur->health <= 0.0f) { cur = cur->next; continue; }

        FVector3 tgt{};
        float distSq = 0.0f, aimPitch = 0.0f, aimYaw = 0.0f;
        if (getTargetPos(cur, tgt) && calcAim(tgt, distSq, aimPitch, aimYaw)) {
            if (cur->health < bestHP || (cur->health == bestHP && distSq < bestDistSq)) {
                bestHP = cur->health;
                bestDistSq = distSq;
                bestNode = cur;
                bestTarget = tgt;
            }
        }
        cur = cur->next;
    }

    if (!bestNode) return;

    m_aimbotLockedKey = bestNode->playerKey;
    writeAimbotLog("[Aimbot] New lock: key=%u name=%s hp=%.0f dist=%.0fm",
        bestNode->playerKey, bestNode->playerName.c_str(), bestNode->health,
        std::sqrt(bestDistSq) / 100.0f);

    // 首次锁定也用平滑 (不瞬移)
    float aimPitch = 0.0f, aimYaw = 0.0f, distSq = 0.0f;
    if (calcAim(bestTarget, distSq, aimPitch, aimYaw)) {
        float finalPitch = lerpAngle(curPitch, aimPitch, smoothT);
        float finalYaw   = lerpAngle(curYaw,   aimYaw,   smoothT);
        writeMemFloat(ctrlRotAddr + 0, finalPitch);
        writeMemFloat(ctrlRotAddr + 4, finalYaw);
        m_lastAimbotWriteMs = nowMs;
    }
}

// =====================================================================
//  日志工具
// =====================================================================
void MatchMonitor::openLog() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    if (!g_runtimeLogEnabled) return;
    if (m_logFp) return;
    mkdir(m_logDir.c_str(), 0777);
    std::string path = m_logDir + m_logFile;
    m_logFp = fopen(path.c_str(), "w");
    m_logLineCount = 0;
    if (m_logFp) {
        writeLog("=== PUBG Mobile Player Log ===");
        writeLog("");
    }
}

void MatchMonitor::writeLog(const char* line) {
    if (!g_runtimeLogEnabled) return;
    if (!m_logFp) return;
    fprintf(m_logFp, "%s\n", line);
    m_logLineCount++;
    if (m_logLineCount % 50 == 0) fflush(m_logFp);
}

void MatchMonitor::writeSkeletonLog(const char* line) {
    if (nullptr == line || '\0' == line[0]) return;

    std::lock_guard<std::mutex> lock(m_logMutex);
    mkdir(m_logDir.c_str(), 0777);

    std::string path = m_logDir + "skeleton_log.txt";
    FILE* fp = fopen(path.c_str(), "a");
    if (!fp) {
        fp = fopen("/data/local/tmp/ue4_skeleton_log.txt", "a");
        if (!fp) return;
    }

    fprintf(fp, "%s\n", line);
    fflush(fp);
    fclose(fp);
}

void MatchMonitor::writeSkeletonLogf(const char* fmt, ...) {
    if (nullptr == fmt || '\0' == fmt[0]) return;

    char message[1024] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    char line[1200] = {};
    snprintf(line,
             sizeof(line),
             "[%llu] %s",
             static_cast<unsigned long long>(nowMonotonicMs()),
             message);
    writeSkeletonLog(line);
}

void MatchMonitor::closeLog() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    if (!m_logFp) return;
    writeLog("");
    writeLog("=== Log ended ===");
    fclose(m_logFp);
    LOG(LOG_LEVEL_INFO, "日志已保存: %s%s (%d lines)",
        m_logDir.c_str(), m_logFile.c_str(), m_logLineCount);
    m_logFp = nullptr;
    m_logLineCount = 0;
}

// =====================================================================
//  玩家数据轮询
// =====================================================================
void MatchMonitor::pollPlayers() {
    static Clock::time_point s_lastPlayerLogTime;
    static Clock::time_point s_lastSlowRefreshTime;

    const bool shouldDumpPlayerLog = shouldLogEvery(s_lastPlayerLogTime, std::chrono::milliseconds(PLAYER_LOG_INTERVAL_MS));

    // GNames: NumElements @ m_gNames + 0x1400
    m_numNames = safeReadS32(m_gNames + 0x1400);
    MatchState ms = getMatchState();
    if (!ms.inMatch) return;

    if (ms.elapsedTimeSeconds >= 0) {
        m_currentMatchElapsedSeconds = ms.elapsedTimeSeconds;
    } else if (m_matchEnterTickMs != 0) {
        m_currentMatchElapsedSeconds = static_cast<int32_t>((nowMonotonicMs() - m_matchEnterTickMs) / 1000ULL);
    } else {
        m_currentMatchElapsedSeconds = -1;
    }

    // 开局静默期: 状态未达到 InProgress 前不进行玩家数据读写,
    // 让游戏完成资源加载、飞机航线、跳伞等流程
    m_currentMatchState = ms.state;
    const bool isInProgress = (ms.state == "InProgress");
    if (!isInProgress) {
        static Clock::time_point s_lastGraceLogTime;
        if (shouldLogEvery(s_lastGraceLogTime, std::chrono::milliseconds(3000))) {
            LOG(LOG_LEVEL_INFO, "[Grace] 等待对局进入 InProgress (当前: %s, 已经过: %ds), 跳过玩家读取",
                ms.state.c_str(), m_currentMatchElapsedSeconds);
        }
        return;
    }

    const LoadThrottlePhase loadPhase = getLoadThrottlePhase(m_currentMatchElapsedSeconds);
    const int loadPhaseValue = static_cast<int>(loadPhase);
    if (loadPhaseValue != m_lastLoadThrottlePhase) {
        LOG(LOG_LEVEL_INFO, "[Throttle] Phase=%s Elapsed=%d", loadThrottlePhaseName(loadPhase), m_currentMatchElapsedSeconds);
        m_lastLoadThrottlePhase = loadPhaseValue;
    }

    const auto slowRefreshInterval = getSlowRefreshInterval(m_currentMatchElapsedSeconds);

    const bool shouldRunSlowPath = m_playerList.size() <= 0
        || m_myPlayerKey == 0
        || shouldLogEvery(s_lastSlowRefreshTime, slowRefreshInterval);

    if (shouldRunSlowPath) {
        const int count = updatePlayerList(ms.gameStatePtr);
        if (count <= 0 && m_playerList.size() <= 0) {
            return;
        }
    } else {
        refreshTrackedPlayersFast();
    }

    if (m_playerList.size() <= 0) {
        return;
    }

    // 推送数据到绘制层
    ue4draw::DrawGameData drawData;
    drawData.inMatch = true;
    drawData.worldName = ms.worldName;
    drawData.matchState = ms.state;
    drawData.myTeamID = m_myTeamID;
    drawData.totalCount = m_playerList.size();
    drawData.players.reserve(static_cast<size_t>(m_playerList.size()));

    // 获取自己的位置 (PlayerArray[0])
    PlayerNode* myNode = m_myPlayerKey != 0 ? m_playerList.findByKey(m_myPlayerKey) : nullptr;
    if (!myNode) {
        myNode = m_playerList.head();
    }
    if (myNode) {
        drawData.myPosX = myNode->pos.x;
        drawData.myPosY = myNode->pos.y;
        drawData.myPosZ = myNode->pos.z;
    }
    fillCameraSnapshot(drawData);

    // 自瞄: 在刷新位置和相机数据后执行
    aimAtNearestEnemy();

    int aliveCount = 0;
    int aliveTeam = 0;
    int aliveEnemy = 0;
    std::vector<PlayerNode*> enemies;
    std::vector<PlayerNode*> teammates;
    if (shouldDumpPlayerLog) {
        enemies.reserve(static_cast<size_t>(m_playerList.size()));
        teammates.reserve(static_cast<size_t>(m_playerList.size()));
    }

    PlayerNode* cur = m_playerList.head();
    while (cur) {
        const bool isNormalState = isPlayerInNormalState(*cur);
        if (!isNormalState) {
            cur = cur->next;
            continue;
        }

        const bool isTeammate = (m_myTeamID > 0 && cur->teamID == m_myTeamID);
        aliveCount++;
        if (isTeammate) {
            aliveTeam++;
            if (shouldDumpPlayerLog) {
                teammates.push_back(cur);
            }
        } else {
            aliveEnemy++;
            if (shouldDumpPlayerLog) {
                enemies.push_back(cur);
            }
        }

        ue4draw::DrawPlayerInfo dp;
        dp.playerKey = cur->playerKey;
        dp.teamID = cur->teamID;
        dp.playerName = cur->playerName;
        dp.isAI = cur->isAI;
        dp.isAlive = true;
        dp.health = cur->health;
        dp.healthMax = cur->healthMax;
        dp.kills = cur->kills;
        dp.posX = cur->pos.x;
        dp.posY = cur->pos.y;
        dp.posZ = cur->pos.z;
        dp.isTeammate = isTeammate;

        // 骨骼: 尝试从活数据填充, 失败则用缓存 (防闪烁, 支持骑马)
        bool freshBones = fillPlayerSkeleton(cur->characterPtr, dp);
        if (freshBones) {
            // 只在新数据骨骼数 >= 缓存时才更新 (防止部分数据覆盖完整缓存)
            int newBoneCount = 0;
            uint32_t m = dp.boneMask;
            while (m) { newBoneCount += m & 1; m >>= 1; }
            int cachedBoneCount = 0;
            m = cur->cachedBoneMask;
            while (m) { cachedBoneCount += m & 1; m >>= 1; }

            if (newBoneCount >= cachedBoneCount) {
                for (size_t i = 0; i < 17; ++i)
                    cur->cachedBones[i] = {dp.bones[i].x, dp.bones[i].y, dp.bones[i].z};
                cur->cachedBoneMask = dp.boneMask;
                cur->cachedBoneTimestampMs = nowMonotonicMs();
            } else {
                // 新数据不如缓存完整, 用缓存
                for (size_t i = 0; i < 17; ++i)
                    dp.bones[i] = {cur->cachedBones[i].x, cur->cachedBones[i].y, cur->cachedBones[i].z};
                dp.boneMask = cur->cachedBoneMask;
                cur->cachedBoneTimestampMs = nowMonotonicMs(); // 延长缓存有效期
            }
        } else if (cur->cachedBoneMask != 0) {
            // 使用缓存的骨骼 (最多保留 2000ms, 平滑过渡)
            const uint64_t age = nowMonotonicMs() - cur->cachedBoneTimestampMs;
            if (age < 2000) {
                for (size_t i = 0; i < 17; ++i)
                    dp.bones[i] = {cur->cachedBones[i].x, cur->cachedBones[i].y, cur->cachedBones[i].z};
                dp.boneMask = cur->cachedBoneMask;
            } else {
                cur->cachedBoneMask = 0;  // 缓存过期
            }
        }

        drawData.players.push_back(dp);
        cur = cur->next;
    }

    drawData.aliveCount = aliveCount;

    if (shouldDumpPlayerLog) {
        LOG(LOG_LEVEL_INFO, "[Players] %d alive (%d team + %d enemy) / %d total",
            aliveCount, aliveTeam, aliveEnemy, m_playerList.size());

        int playersWithBones = 0;
        int totalBonePoints = 0;
        for (const auto& player : drawData.players) {
            if (player.boneMask == 0) {
                continue;
            }
            playersWithBones++;

            uint32_t mask = player.boneMask;
            while (mask != 0) {
                totalBonePoints += static_cast<int>(mask & 1u);
                mask >>= 1u;
            }
        }

        writeSkeletonLogf("[SkeletonSummary] world=%s state=%s players=%zu alive=%d withBones=%d totalBonePoints=%d",
                          drawData.worldName.c_str(),
                          drawData.matchState.c_str(),
                          drawData.players.size(),
                          drawData.aliveCount,
                          playersWithBones,
                          totalBonePoints);

        char logBuf[512];
        const int limit = (enemies.size() < 40) ? static_cast<int>(enemies.size()) : 40;
        for (int i = 0; i < limit; i++) {
            PlayerNode* p = enemies[i];
            LOG(LOG_LEVEL_INFO, "[Enemy] T%d %s %.0f/%.0fHP (%.0f, %.0f, %.0f) K:%d %s",
                 p->teamID, p->isAI ? "AI" : "Real",
                 p->health, p->healthMax,
                 p->pos.x, p->pos.y, p->pos.z,
                 p->kills, p->playerName.c_str());
            snprintf(logBuf, sizeof(logBuf), " ★ T%d %.0f/%.0fHP (%.0f, %.0f, %.0f) %s",
                     p->teamID, p->health, p->healthMax,
                     p->pos.x, p->pos.y, p->pos.z, p->playerName.c_str());
            writeLog(logBuf);
        }
        for (int i = 0; i < static_cast<int>(teammates.size()); i++) {
            PlayerNode* p = teammates[i];
            LOG(LOG_LEVEL_INFO, "[Team] T%d %s %.0f/%.0fHP (%.0f, %.0f, %.0f) K:%d %s",
                 p->teamID, p->isAI ? "AI" : "Real",
                 p->health, p->healthMax,
                 p->pos.x, p->pos.y, p->pos.z,
                 p->kills, p->playerName.c_str());
            snprintf(logBuf, sizeof(logBuf), " ○ T%d %.0f/%.0fHP (%.0f, %.0f, %.0f) %s",
                     p->teamID, p->health, p->healthMax,
                     p->pos.x, p->pos.y, p->pos.z, p->playerName.c_str());
            writeLog(logBuf);
        }
        writeLog("");
    }

    ue4draw::SharedUE4Data::getInstance().pushData(drawData);
}

// =====================================================================
//  对局状态轮询循(后台线程)
// =====================================================================
void MatchMonitor::pollMatchStateLoop() {
    LOG(LOG_LEVEL_INFO, "Monitor thread started (state %dms, player %dms)",
        POLL_INTERVAL_MS, PLAYER_POLL_INTERVAL_MS);

    Clock::time_point lastStatePollTime;
    Clock::time_point lastPlayerPollTime;
    Clock::time_point lastStateLogTime;
    MatchState lastKnownState{};

    while (m_running) {
        const auto now = Clock::now();
        bool stateChanged = false;

        if (lastStatePollTime.time_since_epoch().count() == 0
            || now - lastStatePollTime >= std::chrono::milliseconds(POLL_INTERVAL_MS)) {
            lastStatePollTime = now;

            // GNames: NumElements @ m_gNames + 0x1400
            m_numNames = safeReadS32(m_gNames + 0x1400);

            MatchState ms = getMatchState();
            lastKnownState = ms;

            bool wasInMatch = m_isInMatch;
            stateChanged = (ms.state != m_lastMatchState) || (ms.inMatch != wasInMatch);
            if (stateChanged) {
                m_isInMatch = ms.inMatch;

                if (m_isInMatch && !wasInMatch) {
                    LOG(LOG_LEVEL_INFO, "Entered match! State=%s World=%s", ms.state.c_str(), ms.worldName.c_str());
                    ue4draw::SharedUE4Data::getInstance().setInMatch(true);
                    m_playerList.clear();
                    m_lastNetCullPatchMs.clear();
                    m_characterClassSet.clear();
                    m_myTeamID = -1;
                    m_myPlayerKey = 0;
                    m_lastReportedArrayNum = -1;
                    m_lastReportedTotal = -1;
                    m_currentMatchElapsedSeconds = -1;
                    m_lastLoadThrottlePhase = -1;
                    m_matchEnterTickMs = nowMonotonicMs();
                    m_lastCharacterScanMs = 0;
                    m_characterScanChunkIndex = 0;
                    m_characterScanItemIndex = 0;
                    m_characterScanEpoch = 0;
                    m_lastCompletedCharacterScanEpoch = 0;
                    m_currentMatchState.clear();
                    openLog();
                    writeSkeletonLogf("=== Skeleton Log Start pid=%d state=%s world=%s ===",
                                      getpid(),
                                      ms.state.c_str(),
                                      ms.worldName.c_str());
                    char logBuf[256];
                    snprintf(logBuf, sizeof(logBuf), ">>> * Entered match State=%s World=%s", ms.state.c_str(), ms.worldName.c_str());
                    writeLog(logBuf);
                    writeLog("");
                    detectObserverType();
                } else if (!m_isInMatch && wasInMatch) {
                    LOG(LOG_LEVEL_INFO, "★ 离开对局! 共追踪 %d 名玩家", m_playerList.size());
                    writeSkeletonLogf("=== Skeleton Log End trackedPlayers=%d ===", m_playerList.size());
                    ue4draw::DrawGameData emptyData;
                    emptyData.inMatch = false;
                    ue4draw::SharedUE4Data::getInstance().pushData(emptyData);
                    closeLog();
                    closeAimbotLog();
                    m_aimbotLockedKey = 0;
                    m_playerList.clear();
                    m_lastNetCullPatchMs.clear();
                    m_characterClassSet.clear();
                    m_myTeamID = -1;
                    m_myPlayerKey = 0;
                    m_currentMatchElapsedSeconds = -1;
                    m_lastLoadThrottlePhase = -1;
                    m_matchEnterTickMs = 0;
                    m_lastCharacterScanMs = 0;
                    m_characterScanChunkIndex = 0;
                    m_characterScanItemIndex = 0;
                    m_characterScanEpoch = 0;
                    m_lastCompletedCharacterScanEpoch = 0;
                    m_currentMatchState.clear();
                }
                m_lastMatchState = ms.state;
            }
        }

        if (m_isInMatch && (lastPlayerPollTime.time_since_epoch().count() == 0
            || now - lastPlayerPollTime >= std::chrono::milliseconds(PLAYER_POLL_INTERVAL_MS))) {
            lastPlayerPollTime = now;
            pollPlayers();
        }

        if (stateChanged || shouldLogEvery(lastStateLogTime, std::chrono::milliseconds(STATE_LOG_INTERVAL_MS))) {
            std::string status = m_isInMatch ? "★ 对局中" : "○ 非对局";
            LOG(LOG_LEVEL_INFO, "[%s] State=%s World=%s Players=%d",
                status.c_str(), lastKnownState.state.c_str(), lastKnownState.worldName.c_str(), m_playerList.size());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(MONITOR_IDLE_SLEEP_MS));
    }

    LOG(LOG_LEVEL_INFO, "监控线程退出");
}

// =====================================================================
//  MatchMonitor 构析构/start/stop
// =====================================================================
MatchMonitor::MatchMonitor(uintptr_t moduleBase, uintptr_t gNames, uintptr_t gWorld,
                           uintptr_t gUObjectArray, uint64_t moduleSize,
                           ue4inf::UE4Interface& interface,
                           const std::string& logDir, const std::string& logFile)
    : m_interface(interface)
    , m_moduleBase(moduleBase)
    , m_gNames(gNames)
    , m_gWorld(gWorld)
    , m_gUObjectArray(gUObjectArray)
    , m_moduleSize(moduleSize)
    , m_logDir(logDir)
    , m_logFile(logFile)
{
}

MatchMonitor::~MatchMonitor() {
    stop();
}

bool MatchMonitor::start() {
    if (m_running) {
        LOG(LOG_LEVEL_INFO, "监控已在运行");
        return true;
    }

    // 验证 GNames (NumElements @ m_gNames + 0x1400)
    m_numNames = safeReadS32(m_gNames + 0x1400);
    if (m_numNames <= 0) {
        LOG(LOG_LEVEL_ERROR, "GNames 无效, numNames=%d", m_numNames);
        return false;
    }
    LOG(LOG_LEVEL_INFO, "Base=%p GNames=%p numNames=%d GWorld=%p GUObjectArray=%p",
        (void*)m_moduleBase, (void*)m_gNames, m_numNames, (void*)m_gWorld, (void*)m_gUObjectArray);

    // 验证 entry[0] == "None"
    std::string entry0 = getNameByIndex(0);
    LOG(LOG_LEVEL_INFO, "Entry[0]='%s' %s", entry0.c_str(), (entry0 == "None") ? "OK" : "BAD");

    // 通过 UE4Interface 动态解析所有游戏偏移
    if (!initOffsets()) {
        LOG(LOG_LEVEL_ERROR, "偏移解析失败, 无法启动监控");
        return false;
    }
    computeBatchReadBounds();

    m_running = true;
    m_lastMatchState = "";
    m_isInMatch = false;
    m_playerList.clear();
    m_lastNetCullPatchMs.clear();
    m_currentMatchElapsedSeconds = -1;
    m_lastLoadThrottlePhase = -1;
    m_matchEnterTickMs = 0;
    m_lastCharacterScanMs = 0;
    m_characterScanChunkIndex = 0;
    m_characterScanItemIndex = 0;
    m_characterScanEpoch = 0;
    m_lastCompletedCharacterScanEpoch = 0;

    std::thread(&MatchMonitor::pollMatchStateLoop, this).detach();
    LOG(LOG_LEVEL_INFO, "=== 对局监控+玩家采集已启动 ===");
    return true;
}

void MatchMonitor::stop() {
    if (!m_running) return;
    m_running = false;
    // 等待线程安全退出 (轮询周期 + 余量)
    std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS + 200));

    // 推送空数据清除绘制层残留, 防止退出后 overlay 仍显示旧帧
    ue4draw::DrawGameData emptyData;
    emptyData.inMatch = false;
    ue4draw::SharedUE4Data::getInstance().pushData(emptyData);

    closeLog();
    closeAimbotLog();
    LOG(LOG_LEVEL_INFO, "=== 监控已停止 ===");
}

} // namespace pubgmhd

OBFU_ATTRS_END
