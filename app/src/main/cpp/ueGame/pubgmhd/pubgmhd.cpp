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

// å®‰å…¨å†™å…¥: ä¸Ž safeReadMemory åŒæ ·çš„ sigsetjmp ä¿æŠ¤
// å†™å…¥å·²é‡Šæ”¾çš„ Actor å†…å­˜æ—¶æ•èŽ· SIGSEGV/SIGBUS è€Œéžå´©æºƒ
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
//  è§‚æˆ˜ç±»åž‹åç§°
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
//  PlayerList å®žçŽ°
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
//  ResolvedOffsets::isValid æ£€æŸ¥å…³é”®åç§»æ˜¯å¦éƒ½å·²è§£
// =====================================================================
bool ResolvedOffsets::isValid() const {
    // æ ¸å¿ƒåç§»: æ²¡æœ‰è¿™äº›å°±æ— æ³•è¿è¡Œ
    bool core = World_GameState >= 0
             && GS_PlayerArray >= 0
             && Actor_RootComponent >= 0;
    // çŽ©å®¶æ•°æ®: æ²¡æœ‰è¿™äº›å°±æ— æ³•é‡‡é›†çŽ©å®¶ä¿¡æ¯
    bool player = PS_PlayerKey >= 0
               && PS_TeamID >= 0
               && Char_Health >= 0
               && Char_HealthMax >= 0;
    return core && player;
}

// =====================================================================
//  initOffsets é€šè¿‡ UE4Interface åŠ¨æ€æŸ¥æ‰¾æ‰€æœ‰æ¸¸æˆå
// =====================================================================
// è¾…åŠ©: æŸ¥æ‰¾åç§», å¤±è´¥æ—¶æ‰“å°è­¦å‘Š
#define RESOLVE_OFFSET(target, className, fieldName) do { \
    int32_t _off = m_interface.getFieldOffsetInHierarchy(className, fieldName); \
    if (_off >= 0) { target = _off; \
        LOG(LOG_LEVEL_INFO, "[InitOffsets] %s.%s = 0x%X", className, fieldName, _off); } \
    else { LOG(LOG_LEVEL_WARN, "[InitOffsets] æœªæ‰¾åˆ° %s.%s", className, fieldName); } \
} while(0)

// è¾…åŠ©: å°è¯•å¤šä¸ªç±»åæŸ¥æ‰¾åŒä¸€å­—æ®µ, æ²¿ç»§æ‰¿é“¾æœç´¢ (ç¬¬ä¸€ä¸ªåŒ¹é…å³è¿”å›ž)
#define RESOLVE_OFFSET_MULTI(target, fieldName, ...) do { \
    const char* _classes[] = { __VA_ARGS__ }; \
    std::string _owner; \
    for (auto* _cn : _classes) { \
        const ue4inf::UEFieldInfo* _fi = m_interface.findFieldInHierarchy(_cn, fieldName, &_owner); \
        if (_fi) { target = _fi->offset; \
            LOG(LOG_LEVEL_INFO, "[InitOffsets] %s.%s = 0x%X (via %s)", _cn, fieldName, _fi->offset, _owner.c_str()); \
            break; } \
    } \
    if (target < 0) { LOG(LOG_LEVEL_WARN, "[InitOffsets] æœªæ‰¾åˆ° %s (å°è¯•äº† %zu ä¸ªç±»+ç»§æ‰¿é“¾)", fieldName, sizeof(_classes)/sizeof(_classes[0])); } \
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
    LOG(LOG_LEVEL_INFO, "[InitOffsets] å¼€å§‹é€šè¿‡åå°„è§£æžåç§»...");

    // UWorld
    RESOLVE_OFFSET(m_off.World_GameState,         "World", "GameState");
    RESOLVE_OFFSET(m_off.World_AuthorityGameMode,  "World", "AuthorityGameMode");

    // GameState / GameStateBase â€” MatchState å£°æ˜Žåœ¨ GameState è€Œéž GameStateBase
    RESOLVE_OFFSET_MULTI(m_off.GS_MatchState,      "MatchState",         "GameState", "GameStateBase", "UAEGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_bHasBegunPlay,   "bHasBegunPlay",      "GameStateBase", "GameState", "UAEGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_ElapsedTime,     "ElapsedTime",        "GameStateBase", "GameState", "UAEGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_PlayerArray,     "PlayerArray",        "GameStateBase", "GameState", "UAEGameState");

    // UAEGameState â€” å­—æ®µå¯èƒ½åœ¨çˆ¶ç±»æˆ–å­ç±»ä¸Š
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

    // SceneComponent â€” ä½¿ç”¨ ComponentToWorld.Translation (ä¸–ç•Œåæ ‡, éž RelativeLocation)
    // ComponentToWorld æ˜¯ FTransform, Translation åœ¨ FTransform+0x10
    {
        int32_t ctw = m_interface.getFieldOffsetInHierarchy("SceneComponent", "ComponentToWorld");
        if (ctw >= 0) {
            m_off.SceneComp_ComponentToWorld = ctw;
            m_off.SceneComp_Translation = ctw + 0x10; // FTransform.Translation offset
            LOG(LOG_LEVEL_INFO, "[InitOffsets] SceneComponent.ComponentToWorld+0x10 = 0x%X", m_off.SceneComp_Translation);
        } else {
            LOG(LOG_LEVEL_WARN, "[InitOffsets] ComponentToWorld æœªæ‰¾åˆ°, å›žé€€ 0x200");
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

    LOG(LOG_LEVEL_INFO, "[InitOffsets] è§£æžå®Œæˆ, isValid=%d", m_off.isValid());
    LOG(LOG_LEVEL_INFO, "[InitOffsets] World.GameState=0x%X GS.PlayerArray=0x%X PS.PlayerKey=0x%X",
        m_off.World_GameState, m_off.GS_PlayerArray, m_off.PS_PlayerKey);
    LOG(LOG_LEVEL_INFO, "[InitOffsets] Actor.RootComponent=0x%X Char.Health=0x%X Char.HealthMax=0x%X",
        m_off.Actor_RootComponent, m_off.Char_Health, m_off.Char_HealthMax);

    return m_off.isValid();
}

#undef RESOLVE_OFFSET
#undef RESOLVE_OFFSET_MULTI

// =====================================================================
//  å®‰å…¨å†…å­˜è¯»å–
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
//  BatchMemReader é™æ€æ¡¥æŽ¥
// =====================================================================
bool BatchMemReader::safeReadMemoryStatic(uintptr_t addr, void* out, size_t size) {
    return safeReadMemory(addr, out, size);
}

// =====================================================================
//  åç§»èŒƒå›´è®¡ç®— â€” ç¡®å®š PlayerState / Character æ‰¹é‡è¯»å–æ‰€éœ€çš„å­—èŠ‚æ•°
// =====================================================================
void MatchMonitor::computeBatchReadBounds() {
    auto maxOff = [](std::initializer_list<int32_t> offsets, size_t fieldSize) -> size_t {
        int32_t mx = 0;
        for (int32_t o : offsets) {
            if (o > mx) mx = o;
        }
        return (mx > 0) ? static_cast<size_t>(mx) + fieldSize : 0;
    };

    // PlayerState: åŒ…å«æ‰€æœ‰ PS_* åç§»ä¸­æœ€å¤§å€¼ + padding
    m_psReadSize = maxOff({
        m_off.PS_PlayerKey, m_off.PS_TeamID, m_off.PS_bAIPlayer,
        m_off.PS_LiveState, m_off.PS_PlayerHealth, m_off.PS_PlayerHealthMax,
        m_off.PS_Kills, m_off.PS_CharacterOwner,
        m_off.PS_SelfLocAndRot >= 0 ? m_off.PS_SelfLocAndRot + 12 : 0  // FVector3 = 12 bytes
    }, 8);  // 8 bytes for pointer fields

    // Character: åŒ…å«æ‰€æœ‰ Char_* åç§» + Actor_RootComponent
    m_charReadSize = maxOff({
        m_off.Char_Health, m_off.Char_HealthMax, m_off.Char_bDead,
        m_off.Char_PlayerKey, m_off.Char_TeamID,
        m_off.Actor_RootComponent, m_off.Actor_NetCullDistSq,
        m_off.Char_CurrentNetCullDistSq
    }, 8);

    // BatchMemReader::read() ä¼šè‡ªåŠ¨å°†è¶…å‡º kMaxBatchSize çš„éƒ¨åˆ†æˆªæ–­,
    // get() å¯¹æœªå‘½ä¸­ç¼“å†²çš„å­—æ®µè‡ªåŠ¨å›žé€€åˆ°å•ç‹¬è¯»å–, æ— éœ€åœ¨æ­¤ clamp
    LOG(LOG_LEVEL_INFO, "[BatchRead] PS æ‰¹é‡è¯»å–èŒƒå›´: %zu bytes (buf=%zu), Char æ‰¹é‡è¯»å–èŒƒå›´: %zu bytes (buf=%zu)",
        m_psReadSize, BatchMemReader::kMaxBatchSize, m_charReadSize, BatchMemReader::kMaxBatchSize);
}

// =====================================================================
//  å®‰å…¨å†…å­˜å†™å…¥ (ä¿¡å·ä¿æŠ¤, é˜²æ­¢å†™å…¥å·²é‡Šæ”¾å†…å­˜æ—¶å´©æºƒ)
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
//  FName è§£æž
// =====================================================================
std::string MatchMonitor::getNameByIndex(int index) {
    auto it = m_nameCache.find(index);
    if (it != m_nameCache.end()) return it->second;
    if (index < 0 || index >= m_numNames) return "";

    // é€šè¿‡ safeRead è®¿é—® GNames æ•°ç»„, é¿å…æ¸¸æˆé‡åˆ†é…æ—¶è£¸è§£å¼•ç”¨å´©æºƒ
    uintptr_t namesBase = m_gNames;
    int ci = index / ue4::NAMES_ELEMENTS_PER_CHUNK;
    int wi = index % ue4::NAMES_ELEMENTS_PER_CHUNK;

    // Chunks[ci] æ˜¯æŒ‡é’ˆæ•°ç»„, æ¯ä¸ªå…ƒç´  8 å­—èŠ‚
    uintptr_t chkPtr = safeReadPtr(namesBase + static_cast<uintptr_t>(ci) * 8);
    if (chkPtr == 0 || chkPtr < 0x10000) return "";

    // chk[wi] æ˜¯ FNameEntry* æ•°ç»„
    uintptr_t entryPtr = safeReadPtr(chkPtr + static_cast<uintptr_t>(wi) * 8);
    if (entryPtr == 0 || entryPtr < 0x10000) return "";

    // FNameEntry: +0x08 = Index (bit0=IsWide), +0x0C = AnsiName
    int32_t entryIndex = safeReadS32(entryPtr + 0x08);
    bool isWide = (entryIndex & 1) != 0;

    std::string name;
    if (!isWide) {
        // è¯»å– ANSI å­—ç¬¦ä¸² (entryPtr + 0x0C)
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
//  FString è¯»å– (UE4 Android: UTF-16LE -> UTF-8)
// =====================================================================
std::string MatchMonitor::readFString(uintptr_t addr) {
    uintptr_t dataPtr = safeReadPtr(addr);
    int32_t num = safeReadS32(addr + 8);
    if (dataPtr == 0 || num <= 0 || num > 256) return "";

    // ä¸€æ¬¡æ€§å®‰å…¨è¯»å–æ•´ä¸ª UTF-16 ç¼“å†²åŒº, é¿å…è£¸è§£å¼•ç”¨å´©æºƒ
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
//  å¯¹å±€çŠ¶æ€è¯»
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

    // åˆ¤æ–­æ˜¯å¦åœ¨å¯¹å±€: æŽ’é™¤å¤§åŽ…/UI åœ°å›¾
    ms.inMatch = ms.worldName.find("Editor_login") == std::string::npos
              && ms.worldName.find("UImap") == std::string::npos
              && ms.worldName.find("Lobby") == std::string::npos
              && ms.worldName != "None"
              && ms.worldName.find("invalid") == std::string::npos
              && gsPtr != 0;
    return ms;
}

// =====================================================================
//  Actor ä½ç½®è¯»å–
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

// ä»Ž FName æ•°ç»„åŒ¹é…éª¨éª¼ç´¢å¼•
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

// ä»Ž FMeshBoneInfo æ•°ç»„ (strideå­—èŠ‚, FNameåœ¨åç§»0) åŒ¹é…éª¨éª¼ç´¢å¼•
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

    // æ–¹æ³•1: SkeletalMesh+0x238 FReferenceSkeleton (å…¨èº«éª¨éª¼, stride=16)
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
        // stride=16 ä¸å¤Ÿ, è¯• stride=8 (çº¯ FName æ•°ç»„)
        BoneAssetCacheEntry trial;
        if (matchBoneNamesFromFNameArray(boneInfoData, boneInfoNum, trial) > entry.matchedCount)
            entry = trial;
    }

    // æ–¹æ³•2: Skeleton â†’ RefBoneNames
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
//  fillPlayerSkeleton â€” å¡«å……çŽ©å®¶éª¨éª¼ä¸–ç•Œåæ ‡
//
//  æµç¨‹: æ”¶é›†å€™é€‰ç»„ä»¶ â†’ é€‰æœ€å¤š transforms çš„ â†’ åŒ¹é…éª¨éª¼å â†’ åæ ‡å˜æ¢
// =====================================================================
bool MatchMonitor::fillPlayerSkeleton(uintptr_t characterPtr, ue4draw::DrawPlayerInfo& outPlayer) {
    static Clock::time_point s_lastSkeletonLogTime;

    // dump.cs é™æ€åç§» (åå°„å¤±è´¥æ—¶çš„å›žé€€)
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

    // ---- è¾…åŠ© ----
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

    // ---- æ”¶é›†å€™é€‰ç»„ä»¶ ----
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

    // 1. Character.Mesh + MasterPose é“¾
    const uintptr_t charMesh = safeReadPtr(characterPtr + oMesh);
    tryAdd(charMesh);
    uintptr_t masterRoot = followMasterPose(charMesh);
    if (masterRoot != charMesh) tryAdd(masterRoot);

    // 2. AvatarComponent â†’ MasterBoneComponent + meshComponentList + SkelPool
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

    // 3. FPPComp â†’ _AvatarComp â†’ MasterBone
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

    // ---- æŒ‰ transformCount é™åº ----
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.count > b.count; });

    // ---- æ”¶é›† SkeletalMesh èµ„äº§ ----
    std::vector<uintptr_t> assets;
    assets.reserve(candidates.size() + 2);
    auto addAsset = [&](uintptr_t ptr) {
        if (ptr >= 0x10000 && std::find(assets.begin(), assets.end(), ptr) == assets.end())
            assets.push_back(ptr);
    };
    for (const auto& c : candidates) addAsset(safeReadPtr(c.comp + oSkelMesh));
    addAsset(safeReadPtr(characterPtr + off(m_off.STBase_DefaultCharacterMesh, kFB_DefaultMesh)));
    addAsset(safeReadPtr(characterPtr + off(m_off.STBase_LastSkeletalMesh, kFB_LastSkelMesh)));

    // ---- åŒ¹é…: æ‰¾æœ€ä¼˜ (ç»„ä»¶ Ã— èµ„äº§) ----
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

    // ---- è¯»å–éª¨éª¼å˜æ¢ â†’ ä¸–ç•Œåæ ‡ ----
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
//  ç±»ç»§æ‰¿é“¾æ£€
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
//  è§‚æˆ˜ç±»åž‹æ£€
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
    uintptr_t psPtr = safeReadPtr(arrayData); // PlayerArray[0] = æœ¬åœ°çŽ©å®¶ PlayerState
    if (psPtr == 0) return 0;
    return safeReadPtr(psPtr + 0x98); // AActor::Owner = PlayerController
}

EObserverType MatchMonitor::detectObserverType() {
    uintptr_t pc = getLocalPlayerController();
    if (pc == 0) {
        LOG(LOG_LEVEL_INFO, "æ— æ³•èŽ·å–æœ¬åœ° PlayerController");
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
        LOG(LOG_LEVEL_ERROR, "[SetObserver] æ— æ³•èŽ·å– PlayerController");
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
            LOG(LOG_LEVEL_ERROR, "[SetObserver] æ— æ•ˆç±»åž‹: %d", static_cast<int>(type));
            return false;
    }
    LOG(LOG_LEVEL_INFO, "[SetObserver] å·²è®¾ç½®ä¸º EObserverType_%s (%d)",
        observerTypeName(type), static_cast<int>(type));

    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf), "[SetObserver] -> EObserverType_%s (%d)", observerTypeName(type), static_cast<int>(type));
    writeLog(logBuf);
    return true;
}

// =====================================================================
//  ç½‘ç»œå¯è§èŒƒå›´ä¿®æ”¹
// =====================================================================
void MatchMonitor::patchActorNetCull(uintptr_t actorPtr) {
    if (actorPtr == 0) return;
    // ä»…åœ¨ InProgress çŠ¶æ€å†™å…¥ NetCullDist, é¿å…åœ¨åŠ è½½/é£žæœº/è·³ä¼žé˜¶æ®µè§¦å‘ç½‘ç»œå¼‚å¸¸æ–­å¼€
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
//  GUObjectArray æ‰«ææ‰€Character
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

    // é€šè¿‡ safeRead è®¿é—® GUObjectArray, é¿å…æ¸¸æˆé‡åˆ†é…æ—¶è£¸è§£å¼•ç”¨å´©æºƒ
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
        // ChunkPtrs @ +0xC8, æ¯ä¸ªæŒ‡é’ˆ 8 å­—èŠ‚
        uintptr_t chunkBase = safeReadPtr(objArrayAddr + 0xC8 + static_cast<uintptr_t>(ci) * 8);
        // ChunkElementCounts @ +0xE8, æ¯ä¸ª int32 4 å­—èŠ‚
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

        // FUObjectItem å¤§å° = 24 å­—èŠ‚ (Object* @ +0x00)
        static constexpr size_t kFUObjectItemSize = 24;
        BatchMemReader charBatch;

        while (wi < chunkCount && processedItems < scanBudget) {
            // é€šè¿‡ safeReadPtr è¯»å– FUObjectItem.Object
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

            // ----- æ‰¹é‡è¯»å– Character (best-effort, get() è‡ªåŠ¨å›žé€€) -----
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
            // ä½ç½®: é€šè¿‡ RootComponent -> SceneComponent èŽ·å–
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
//  éåŽ† PlayerArray, æ›´æ–°åŒå‘é“¾è¡¨
// =====================================================================
int MatchMonitor::updatePlayerList(uintptr_t gameStatePtr) {
    uintptr_t arrayData = safeReadPtr(gameStatePtr + m_off.GS_PlayerArray);
    int32_t arrayNum = safeReadS32(gameStatePtr + m_off.GS_PlayerArray + 8);

    // è¯»å– GameState çš„å…¨å±€çŽ©å®¶è®¡æ•° (åç§»å¯èƒ½æœªè§£æž, å®‰å…¨æ£€æŸ¥)
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

    // ----- è¯»å–å…¨éƒ¨ PlayerState æŒ‡é’ˆ -----
    // åŽ»æŽ‰æ‰¹é‡è¯»å–: TArray æŒ‡é’ˆæ•°ç»„å¯èƒ½è·¨é¡µè¾¹ç•Œå¯¼è‡´æ•´å— memcpy å¤±è´¥,
    // è€Œé€ä¸ª safeReadPtr å¯ä»¥è·³è¿‡å•ä¸ªæ— æ•ˆæ¡ç›®è€Œä¸ä¸¢å¤±å…¨éƒ¨çŽ©å®¶
    std::vector<uintptr_t> psPtrs(static_cast<size_t>(arrayNum), 0);
    for (int i = 0; i < arrayNum; i++) {
        psPtrs[i] = safeReadPtr(arrayData + i * 8);
    }

    std::unordered_map<uint32_t, bool> seenKeys;
    int updated = 0;
    BatchMemReader psBatch;   // å¤ç”¨, é¿å…æ¯æ¬¡å¾ªçŽ¯é‡æ–°æž„é€ 
    BatchMemReader charBatch;

    for (int i = 0; i < arrayNum; i++) {
        const uintptr_t psPtr = psPtrs[i];
        if (psPtr == 0) continue;

        // ----- æ‰¹é‡è¯»å– PlayerState (best-effort, get() è‡ªåŠ¨å›žé€€) -----
        psBatch.read(psPtr, m_psReadSize);

        // ä»Žæœ¬åœ°ç¼“å†²æå–å­—æ®µ (å‘½ä¸­ç¼“å†²=é›¶å¼€é”€, æœªå‘½ä¸­=è‡ªåŠ¨å›žé€€åˆ°å•ç‹¬è¯»å–)
        const uint32_t playerKey = psBatch.getU32(m_off.PS_PlayerKey);
        if (playerKey == 0) continue;

        int32_t teamID = psBatch.getS32(m_off.PS_TeamID);
        bool isAI = psBatch.getU8(m_off.PS_bAIPlayer) != 0;
        uint8_t liveState = psBatch.getU8(m_off.PS_LiveState);
        float health = psBatch.getFloat(m_off.PS_PlayerHealth);
        float healthMax = psBatch.getFloat(m_off.PS_PlayerHealthMax);
        int32_t kills = psBatch.getS32(m_off.PS_Kills);
        // FString éœ€è¦è·ŸéšæŒ‡é’ˆ, ä»éœ€å•ç‹¬è¯»å–
        std::string playerName = readFString(psPtr + m_off.PS_PlayerName);

        // ä¼˜å…ˆé€šè¿‡ CharacterOwner -> RootComponent èŽ·å–ç²¾ç¡®ä½ç½®
        FVector3 loc;
        const uintptr_t charOwner = psBatch.getPtr(m_off.PS_CharacterOwner);
        if (charOwner != 0) {
            // ----- æ‰¹é‡è¯»å– Character (best-effort, get() è‡ªåŠ¨å›žé€€) -----
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
            // ä½ç½®: é€šè¿‡ RootComponent -> SceneComponent èŽ·å–
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
        // å›žé€€SelfLocAndRot
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

        // è®°å½•æœ¬åœ°çŽ©å®¶ key / TeamID
        if (i == 0) {
            if (teamID > 0) {
                m_myTeamID = teamID;
            }
            m_myPlayerKey = playerKey;
        }
    }

    // ç§»é™¤å·²é€€å‡ºçš„çŽ©å®¶
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

    // æ‰«æ GUObjectArray èŽ·å–é™„è¿‘çš„ Character (åŒ…æ‹¬æ•Œäºº)ï¼Œæ”¹ä¸ºè·¨å¤šæ¬¡è½®è¯¢çš„åˆ†ç‰‡æ‰«æã€‚
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
//  自瞄 — 计算最近敌人方向并写入 ControlRotation
// =====================================================================
void MatchMonitor::aimAtNearestEnemy() {
    if (!m_aimbotEnabled) return;
    if (m_off.Ctrl_ControlRotation < 0) return;

    const uintptr_t pc = getLocalPlayerController();
    if (pc == 0) return;

    // 读取当前相机位置作为射线起点
    const uintptr_t pcm = safeReadPtr(pc + 0x658);  // PlayerCameraManager
    if (pcm == 0) return;

    const float camX = safeReadFloat(pcm + 0x650 + 0x0);
    const float camY = safeReadFloat(pcm + 0x650 + 0x4);
    const float camZ = safeReadFloat(pcm + 0x650 + 0x8);
    if (!std::isfinite(camX) || !std::isfinite(camY) || !std::isfinite(camZ)) return;
    if (std::fabs(camX) < 1.0f && std::fabs(camY) < 1.0f && std::fabs(camZ) < 1.0f) return;

    // 找最近的存活敌人 (有有效骨骼数据)
    const int boneIdx = m_aimbotTargetBone;  // 4=head
    float bestDistSq = 1e18f;
    FVector3 bestTarget{};
    bool found = false;

    PlayerNode* cur = m_playerList.head();
    while (cur) {
        // 跳过自己
        if (m_myPlayerKey != 0 && cur->playerKey == m_myPlayerKey) {
            cur = cur->next;
            continue;
        }
        // 跳过队友
        if (m_myTeamID > 0 && cur->teamID == m_myTeamID) {
            cur = cur->next;
            continue;
        }
        // 跳过死亡
        if (cur->liveState != 0 || cur->health <= 0.0f) {
            cur = cur->next;
            continue;
        }
        // 需要有目标骨骼数据
        if (boneIdx >= 0 && boneIdx < static_cast<int>(TRACKED_BONE_COUNT)
            && (cur->cachedBoneMask & (1u << boneIdx)) != 0) {
            const float tx = cur->cachedBones[boneIdx].x;
            const float ty = cur->cachedBones[boneIdx].y;
            const float tz = cur->cachedBones[boneIdx].z;
            if (std::isfinite(tx) && std::isfinite(ty) && std::isfinite(tz)
                && (std::fabs(tx) > 1.0f || std::fabs(ty) > 1.0f)) {
                const float dx = tx - camX;
                const float dy = ty - camY;
                const float dz = tz - camZ;
                const float distSq = dx * dx + dy * dy + dz * dz;
                if (distSq < bestDistSq && distSq > 100.0f) {  // >10cm 避免自瞄
                    bestDistSq = distSq;
                    bestTarget = {tx, ty, tz};
                    found = true;
                }
            }
        } else {
            // 没有骨骼数据，用角色位置 + 高度偏移瞄准上半身
            if (std::isfinite(cur->pos.x) && std::isfinite(cur->pos.y) && std::isfinite(cur->pos.z)
                && (std::fabs(cur->pos.x) > 1.0f || std::fabs(cur->pos.y) > 1.0f)) {
                const float tx = cur->pos.x;
                const float ty = cur->pos.y;
                const float tz = cur->pos.z + 60.0f;  // 大约头部高度偏移
                const float dx = tx - camX;
                const float dy = ty - camY;
                const float dz = tz - camZ;
                const float distSq = dx * dx + dy * dy + dz * dz;
                if (distSq < bestDistSq && distSq > 100.0f) {
                    bestDistSq = distSq;
                    bestTarget = {tx, ty, tz};
                    found = true;
                }
            }
        }
        cur = cur->next;
    }

    if (!found) return;

    // 计算方向角
    const float dx = bestTarget.x - camX;
    const float dy = bestTarget.y - camY;
    const float dz = bestTarget.z - camZ;
    const float dist2D = std::sqrt(dx * dx + dy * dy);
    if (dist2D < 0.01f) return;

    // UE4 FRotator: Pitch=绕Y轴(仰角), Yaw=绕Z轴(水平), Roll=绕X轴
    // UE4 坐标系: X=前, Y=右, Z=上
    // atan2(Y, X) 给出 Yaw; atan2(Z, 水平距离) 给出 Pitch
    float aimYaw   = std::atan2(dy, dx) * (180.0f / 3.14159265f);
    float aimPitch = std::atan2(dz, dist2D) * (180.0f / 3.14159265f);

    if (!std::isfinite(aimYaw) || !std::isfinite(aimPitch)) return;

    // 写入 ControlRotation (FRotator: Pitch@+0, Yaw@+4, Roll@+8)
    const uintptr_t ctrlRotAddr = pc + static_cast<uintptr_t>(m_off.Ctrl_ControlRotation);
    writeMemFloat(ctrlRotAddr + 0, aimPitch);
    writeMemFloat(ctrlRotAddr + 4, aimYaw);
    // Roll 保持不变

    static Clock::time_point s_lastAimLog;
    if (shouldLogEvery(s_lastAimLog, std::chrono::milliseconds(2000))) {
        LOG(LOG_LEVEL_INFO, "[Aimbot] -> (%.1f, %.1f, %.1f) dist=%.0f pitch=%.2f yaw=%.2f",
            bestTarget.x, bestTarget.y, bestTarget.z,
            std::sqrt(bestDistSq), aimPitch, aimYaw);
    }
}

// =====================================================================
//  æ—¥å¿—å·¥å…·
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
    LOG(LOG_LEVEL_INFO, "æ—¥å¿—å·²ä¿å­˜: %s%s (%d lines)",
        m_logDir.c_str(), m_logFile.c_str(), m_logLineCount);
    m_logFp = nullptr;
    m_logLineCount = 0;
}

// =====================================================================
//  çŽ©å®¶æ•°æ®è½®è¯¢
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

    // å¼€å±€é™é»˜æœŸ: çŠ¶æ€æœªè¾¾åˆ° InProgress å‰ä¸è¿›è¡ŒçŽ©å®¶æ•°æ®è¯»å†™,
    // è®©æ¸¸æˆå®Œæˆèµ„æºåŠ è½½ã€é£žæœºèˆªçº¿ã€è·³ä¼žç­‰æµç¨‹
    m_currentMatchState = ms.state;
    const bool isInProgress = (ms.state == "InProgress");
    if (!isInProgress) {
        static Clock::time_point s_lastGraceLogTime;
        if (shouldLogEvery(s_lastGraceLogTime, std::chrono::milliseconds(3000))) {
            LOG(LOG_LEVEL_INFO, "[Grace] ç­‰å¾…å¯¹å±€è¿›å…¥ InProgress (å½“å‰: %s, å·²ç»è¿‡: %ds), è·³è¿‡çŽ©å®¶è¯»å–",
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

    // æŽ¨é€æ•°æ®åˆ°ç»˜åˆ¶å±‚
    ue4draw::DrawGameData drawData;
    drawData.inMatch = true;
    drawData.worldName = ms.worldName;
    drawData.matchState = ms.state;
    drawData.myTeamID = m_myTeamID;
    drawData.totalCount = m_playerList.size();
    drawData.players.reserve(static_cast<size_t>(m_playerList.size()));

    // èŽ·å–è‡ªå·±çš„ä½ç½® (PlayerArray[0])
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
            snprintf(logBuf, sizeof(logBuf), " â˜… T%d %.0f/%.0fHP (%.0f, %.0f, %.0f) %s",
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
            snprintf(logBuf, sizeof(logBuf), " â—‹ T%d %.0f/%.0fHP (%.0f, %.0f, %.0f) %s",
                     p->teamID, p->health, p->healthMax,
                     p->pos.x, p->pos.y, p->pos.z, p->playerName.c_str());
            writeLog(logBuf);
        }
        writeLog("");
    }

    ue4draw::SharedUE4Data::getInstance().pushData(drawData);
}

// =====================================================================
//  å¯¹å±€çŠ¶æ€è½®è¯¢å¾ª(åŽå°çº¿ç¨‹)
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
                    LOG(LOG_LEVEL_INFO, "â˜… ç¦»å¼€å¯¹å±€! å…±è¿½è¸ª %d åçŽ©å®¶", m_playerList.size());
                    writeSkeletonLogf("=== Skeleton Log End trackedPlayers=%d ===", m_playerList.size());
                    ue4draw::DrawGameData emptyData;
                    emptyData.inMatch = false;
                    ue4draw::SharedUE4Data::getInstance().pushData(emptyData);
                    closeLog();
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
            std::string status = m_isInMatch ? "â˜… å¯¹å±€ä¸­" : "â—‹ éžå¯¹å±€";
            LOG(LOG_LEVEL_INFO, "[%s] State=%s World=%s Players=%d",
                status.c_str(), lastKnownState.state.c_str(), lastKnownState.worldName.c_str(), m_playerList.size());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(MONITOR_IDLE_SLEEP_MS));
    }

    LOG(LOG_LEVEL_INFO, "ç›‘æŽ§çº¿ç¨‹é€€å‡º");
}

// =====================================================================
//  MatchMonitor æž„æžæž„/start/stop
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
        LOG(LOG_LEVEL_INFO, "ç›‘æŽ§å·²åœ¨è¿è¡Œ");
        return true;
    }

    // éªŒè¯ GNames (NumElements @ m_gNames + 0x1400)
    m_numNames = safeReadS32(m_gNames + 0x1400);
    if (m_numNames <= 0) {
        LOG(LOG_LEVEL_ERROR, "GNames æ— æ•ˆ, numNames=%d", m_numNames);
        return false;
    }
    LOG(LOG_LEVEL_INFO, "Base=%p GNames=%p numNames=%d GWorld=%p GUObjectArray=%p",
        (void*)m_moduleBase, (void*)m_gNames, m_numNames, (void*)m_gWorld, (void*)m_gUObjectArray);

    // éªŒè¯ entry[0] == "None"
    std::string entry0 = getNameByIndex(0);
    LOG(LOG_LEVEL_INFO, "Entry[0]='%s' %s", entry0.c_str(), (entry0 == "None") ? "OK" : "BAD");

    // é€šè¿‡ UE4Interface åŠ¨æ€è§£æžæ‰€æœ‰æ¸¸æˆåç§»
    if (!initOffsets()) {
        LOG(LOG_LEVEL_ERROR, "åç§»è§£æžå¤±è´¥, æ— æ³•å¯åŠ¨ç›‘æŽ§");
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
    LOG(LOG_LEVEL_INFO, "=== å¯¹å±€ç›‘æŽ§+çŽ©å®¶é‡‡é›†å·²å¯åŠ¨ ===");
    return true;
}

void MatchMonitor::stop() {
    if (!m_running) return;
    m_running = false;
    // ç­‰å¾…çº¿ç¨‹å®‰å…¨é€€å‡º (è½®è¯¢å‘¨æœŸ + ä½™é‡)
    std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS + 200));
    closeLog();
    LOG(LOG_LEVEL_INFO, "=== ç›‘æŽ§å·²åœæ­¢ ===");
}

} // namespace pubgmhd

OBFU_ATTRS_END
