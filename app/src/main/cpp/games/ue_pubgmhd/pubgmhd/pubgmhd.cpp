#include "pubgmhd.h"
#include "../engine/UE4Struct.h"
#include "../interface/interface.h"
#include "../draw/PubgmhdDraw.h"
#include "../../../core/log/log.h"
#include "../../../stack_spoof/stack_spoof.h"
#include <sys/system_properties.h>

extern "C" bool selfTestParasite();

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
#include <unordered_set>
#include <mutex>

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
    BoneAliasList{"pelvis", "hips", "bip001pelvis", "bip01pelvis", nullptr, nullptr, nullptr, nullptr},
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
static thread_local volatile sig_atomic_t s_forwardingSignal = 0;
static struct sigaction s_oldSigsegvAction{};
static struct sigaction s_oldSigbusAction{};
static std::mutex s_safeReadGuardMutex;

void safeReadSignalHandler(int sig, siginfo_t* info, void* ctx) {
    if (s_safeReadActive) {
        s_safeReadActive = 0;
        siglongjmp(s_safeReadJmpBuf, sig);
    }

    if (s_forwardingSignal) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    struct sigaction* old = (sig == SIGSEGV) ? &s_oldSigsegvAction : &s_oldSigbusAction;
    s_forwardingSignal = 1;
    if ((old->sa_flags & SA_SIGINFO) != 0 && old->sa_sigaction != nullptr) {
        old->sa_sigaction(sig, info, ctx);
        s_forwardingSignal = 0;
        return;
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
        s_forwardingSignal = 0;
        return;
    } else if (old->sa_handler == SIG_IGN) {
        s_forwardingSignal = 0;
        return;
    }
    s_forwardingSignal = 0;

    signal(sig, SIG_DFL);
    raise(sig);
}

bool isSafeReadHandlerInstalled(int sig) {
    struct sigaction current{};
    if (sigaction(sig, nullptr, &current) != 0) return false;
    return (current.sa_flags & SA_SIGINFO) != 0
        && current.sa_sigaction == safeReadSignalHandler;
}

void installSafeReadGuard() {
    std::lock_guard<std::mutex> lock(s_safeReadGuardMutex);
    struct sigaction sa{};
    sa.sa_sigaction = safeReadSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    if (!isSafeReadHandlerInstalled(SIGSEGV)) {
        sigaction(SIGSEGV, &sa, &s_oldSigsegvAction);
    }
    if (!isSafeReadHandlerInstalled(SIGBUS)) {
        sigaction(SIGBUS, &sa, &s_oldSigbusAction);
    }
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

bool looksLikeRemotePtr(uintptr_t value) {
    return value >= 0x10000 && value < 0x0000800000000000ULL;
}

bool readRemoteS32(uintptr_t addr, int32_t& out) {
    out = 0;
    return safeReadMemory(addr, &out, sizeof(out));
}

bool readRemotePtr(uintptr_t addr, uintptr_t& out) {
    out = 0;
    if (!safeReadMemory(addr, &out, sizeof(out))) return false;
    return looksLikeRemotePtr(out);
}

enum class UObjectArrayLayoutKind : uint8_t {
    Invalid = 0,
    ModernFlat,
    LegacyChunked,
};

struct UObjectArrayLayout {
    UObjectArrayLayoutKind kind = UObjectArrayLayoutKind::Invalid;
    uintptr_t items = 0;
    int32_t totalNum = 0;
    int32_t maxNum = 0;
    int32_t numChunks = 0;
};

const char* uObjectArrayLayoutName(UObjectArrayLayoutKind kind) {
    switch (kind) {
        case UObjectArrayLayoutKind::ModernFlat: return "ModernFlat";
        case UObjectArrayLayoutKind::LegacyChunked: return "LegacyChunked";
        default: return "Invalid";
    }
}

bool detectUObjectArrayLayout(uintptr_t arrayBase, UObjectArrayLayout& out) {
    out = {};
    if (!looksLikeRemotePtr(arrayBase)) return false;

    int32_t flatNum = 0;
    int32_t flatMax = 0;
    int32_t flatChunks = 0;
    uintptr_t flatItems = 0;
    if (readRemoteS32(arrayBase + 0xB8, flatNum)
        && readRemoteS32(arrayBase + 0xC0, flatMax)
        && readRemotePtr(arrayBase + 0xC8, flatItems)
        && readRemoteS32(arrayBase + 0xD0, flatChunks)
        && flatNum > 0 && flatNum <= flatMax && flatMax <= 5000000
        && flatChunks > 0 && flatChunks <= 4096) {
        out.kind = UObjectArrayLayoutKind::ModernFlat;
        out.items = flatItems;
        out.totalNum = flatNum;
        out.maxNum = flatMax;
        out.numChunks = flatChunks;
        return true;
    }

    int32_t numChunks = 0;
    int32_t totalNum = 0;
    uintptr_t firstChunk = 0;
    int32_t firstChunkCount = 0;
    if (readRemoteS32(arrayBase + 0xF8, numChunks)
        && readRemoteS32(arrayBase + 0x100, totalNum)
        && readRemotePtr(arrayBase + 0xC8, firstChunk)
        && readRemoteS32(arrayBase + 0xE8, firstChunkCount)
        && numChunks > 0 && numChunks <= 4096
        && totalNum > 0 && totalNum <= 5000000
        && firstChunkCount > 0) {
        out.kind = UObjectArrayLayoutKind::LegacyChunked;
        out.totalNum = totalNum;
        out.numChunks = numChunks;
        return true;
    }

    return false;
}

bool readUObjectItemAt(uintptr_t arrayBase, const UObjectArrayLayout& layout, int32_t index, ue4::FUObjectItem& outItem) {
    outItem = {};
    if (index < 0 || index >= layout.totalNum) return false;

    if (layout.kind == UObjectArrayLayoutKind::ModernFlat) {
        return safeReadMemory(layout.items + static_cast<uintptr_t>(index) * sizeof(ue4::FUObjectItem),
                              &outItem,
                              sizeof(outItem));
    }

    if (layout.kind == UObjectArrayLayoutKind::LegacyChunked) {
        int32_t remaining = index;
        for (int32_t chunkIndex = 0; chunkIndex < layout.numChunks; ++chunkIndex) {
            int32_t chunkCount = 0;
            uintptr_t chunkBase = 0;
            if (!readRemoteS32(arrayBase + 0xE8 + static_cast<uintptr_t>(chunkIndex) * sizeof(int32_t), chunkCount)
                || chunkCount <= 0) {
                continue;
            }
            if (remaining >= chunkCount) {
                remaining -= chunkCount;
                continue;
            }
            if (!readRemotePtr(arrayBase + 0xC8 + static_cast<uintptr_t>(chunkIndex) * sizeof(uintptr_t), chunkBase)) {
                return false;
            }
            return safeReadMemory(chunkBase + static_cast<uintptr_t>(remaining) * sizeof(ue4::FUObjectItem),
                                  &outItem,
                                  sizeof(outItem));
        }
    }

    return false;
}

uintptr_t readUObjectAt(uintptr_t arrayBase, const UObjectArrayLayout& layout, int32_t index) {
    ue4::FUObjectItem item{};
    if (!readUObjectItemAt(arrayBase, layout, index, item)) return 0;
    return reinterpret_cast<uintptr_t>(item.Object);
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
            return 8192;
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
            return 1000;
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

    UObjectArrayLayout layout{};
    if (!detectUObjectArrayLayout(guObjectArrayPtr, layout)
        || weakPtr.objectIndex >= layout.totalNum) {
        return 0;
    }

    ue4::FUObjectItem item{};
    if (!readUObjectItemAt(guObjectArrayPtr, layout, weakPtr.objectIndex, item)
        || item.SerialNumber != weakPtr.objectSerialNumber) {
        return 0;
    }

    return reinterpret_cast<uintptr_t>(item.Object);
}

bool MatchMonitor::initOffsets() {
    LOG(LOG_LEVEL_INFO, "[InitOffsets] 开始通过反射解析偏移...");

    // UWorld
    RESOLVE_OFFSET(m_off.World_GameState,         "World", "GameState");
    RESOLVE_OFFSET(m_off.World_AuthorityGameMode,  "World", "AuthorityGameMode");
    RESOLVE_OFFSET_MULTI(m_off.World_PersistentLevel,   "PersistentLevel",   "World");
    if (m_off.World_PersistentLevel < 0) m_off.World_PersistentLevel = 0xB0;
    RESOLVE_OFFSET_MULTI(m_off.World_ActiveLevelActors, "ActiveLevelActors", "World");
    if (m_off.World_ActiveLevelActors < 0) m_off.World_ActiveLevelActors = 0xAA0;
    RESOLVE_OFFSET_MULTI(m_off.World_Levels,            "Levels",           "World");
    if (m_off.World_Levels < 0) m_off.World_Levels = 0xAD8;

    // ULevel / LevelActorContainer — 当前关卡 Actor 列表, 用于训练场/大厅真实可见角色
    RESOLVE_OFFSET_MULTI(m_off.Level_ActorCluster,         "ActorCluster", "Level");
    if (m_off.Level_ActorCluster < 0) m_off.Level_ActorCluster = 0xE0;
    RESOLVE_OFFSET_MULTI(m_off.LevelActorContainer_Actors, "Actors",       "LevelActorContainer");
    if (m_off.LevelActorContainer_Actors < 0) m_off.LevelActorContainer_Actors = 0x28;

    // GameState / GameStateBase — MatchState 声明在 GameState 而非 GameStateBase
    // 注: 腾讯版字段实际类名为 STExtraGameStateBase (IDA + dump.cs 验证),
    //     bHasBegunPlay 已被 bReplicatedHasBegunPlay 取代 (GameStateBase+0x5F8)
    RESOLVE_OFFSET_MULTI(m_off.GS_bHasBegunPlay,   "bReplicatedHasBegunPlay", "GameStateBase", "GameState", "UAEGameState", "STExtraGameStateBase");
    RESOLVE_OFFSET_MULTI(m_off.GS_ElapsedTime,     "ElapsedTime",             "GameStateBase", "GameState", "UAEGameState", "STExtraGameStateBase");
    RESOLVE_OFFSET_MULTI(m_off.GS_PlayerArray,     "PlayerArray",             "GameStateBase", "GameState", "UAEGameState", "STExtraGameStateBase");
    // MatchState (FName) — 直接读字段, 不进入 libUE4.so 代码段, 避免 ACE 反作弊扫到调用栈
    RESOLVE_OFFSET_MULTI(m_off.GS_MatchState,      "MatchState",              "GameState", "GameStateBase", "UAEGameState", "STExtraGameStateBase");

    // STExtraGameStateBase — 计数字段 (PlayerNum=0xD3C, AlivePlayerNum=0x12AC 等)
    RESOLVE_OFFSET_MULTI(m_off.GS_PlayerNum,       "PlayerNum",          "STExtraGameStateBase", "UAEGameState", "GameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_TotalPlayerNum,  "TotalPlayerNum",     "STExtraGameStateBase", "UAEGameState", "GameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_GameType,        "GameType",           "STExtraGameStateBase", "UAEGameState", "GameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_AlivePlayerNum,  "AlivePlayerNum",     "STExtraGameStateBase", "UAEGameState");
    RESOLVE_OFFSET_MULTI(m_off.GS_AliveRealPlayerNum, "AliveRealPlayerNum", "STExtraGameStateBase", "UAEGameState");

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
    RESOLVE_OFFSET_MULTI(m_off.Actor_Owner,       "Owner",                 "Actor", "Pawn", "Character");
    RESOLVE_OFFSET_MULTI(m_off.Actor_NetCullDistSq, "NetCullDistanceSquared", "Actor", "Character", "Pawn");

    // Pawn — CharacterScan 扫到的 Pawn 可从这里回到 PlayerState 读取真实名字
    RESOLVE_OFFSET_MULTI(m_off.Pawn_PlayerState,  "PlayerState",           "Pawn", "Character", "UAECharacter", "STExtraCharacter");
    if (m_off.Pawn_PlayerState < 0) m_off.Pawn_PlayerState = 0x5F0;

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

    // 本地玩家链路 (反射 + dump.cs fallback)
    RESOLVE_OFFSET_MULTI(m_off.World_OwningGameInstance, "OwningGameInstance", "World");
    if (m_off.World_OwningGameInstance < 0) m_off.World_OwningGameInstance = 0xB08;
    RESOLVE_OFFSET_MULTI(m_off.GI_LocalPlayers,        "LocalPlayers",        "GameInstance");
    if (m_off.GI_LocalPlayers < 0) m_off.GI_LocalPlayers = 0x48;
    RESOLVE_OFFSET_MULTI(m_off.Player_PlayerController, "PlayerController",   "Player", "LocalPlayer");
    if (m_off.Player_PlayerController < 0) m_off.Player_PlayerController = 0x30;
    RESOLVE_OFFSET_MULTI(m_off.PC_AcknowledgedPawn,    "AcknowledgedPawn",    "PlayerController", "UAEPlayerController", "STExtraPlayerController");
    if (m_off.PC_AcknowledgedPawn < 0) m_off.PC_AcknowledgedPawn = 0x640;
    RESOLVE_OFFSET_MULTI(m_off.PC_PlayerState,         "PlayerState",         "Controller", "PlayerController", "UAEPlayerController");

    // Controller — ControlRotation (FRotator: Pitch, Yaw, Roll)
    RESOLVE_OFFSET_MULTI(m_off.Ctrl_ControlRotation,   "ControlRotation",     "Controller", "PlayerController", "UAEPlayerController", "STExtraPlayerController");
    RESOLVE_OFFSET_MULTI(m_off.STPC_LastFrameCacheControlRotation, "LastFrameCacheControlRotation", "STExtraPlayerController");
    if (m_off.STPC_LastFrameCacheControlRotation < 0) m_off.STPC_LastFrameCacheControlRotation = 0x5590;
    RESOLVE_OFFSET_MULTI(m_off.STPC_CachedViewControlRotation, "CachedViewControlRotation", "STExtraPlayerController");
    if (m_off.STPC_CachedViewControlRotation < 0) m_off.STPC_CachedViewControlRotation = 0x559C;
    RESOLVE_OFFSET_MULTI(m_off.STPC_CurrentActiveCameraCache, "CurrentActiveCameraCache", "STExtraPlayerController");
    if (m_off.STPC_CurrentActiveCameraCache < 0) m_off.STPC_CurrentActiveCameraCache = 0x3990;

    // PlayerController / PlayerCameraManager — 相机链
    RESOLVE_OFFSET_MULTI(m_off.PC_PlayerCameraManager, "PlayerCameraManager", "PlayerController", "UAEPlayerController", "STExtraPlayerController");
    RESOLVE_OFFSET_MULTI(m_off.PCM_PCOwner,             "PCOwner",             "PlayerCameraManager");
    if (m_off.PCM_PCOwner < 0) m_off.PCM_PCOwner = 0x5D0;
    RESOLVE_OFFSET_MULTI(m_off.PCM_CameraCache,        "CameraCache",         "PlayerCameraManager");
    RESOLVE_OFFSET_MULTI(m_off.PCM_DefaultFOV,         "DefaultFOV",          "PlayerCameraManager");

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
    RESOLVE_OFFSET_MULTI(m_off.STBase_STExtraPlayerState, "STExtraPlayerState", "STExtraBaseCharacter", "STExtraCharacter");
    if (m_off.STBase_STExtraPlayerState < 0) m_off.STBase_STExtraPlayerState = 0x3310;

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

    // Weapon — 当前武器 (弹道预测需要)
    // 腾讯版字段已重命名: CurWeapon -> CurEquipWeapon, BulletTrackComp -> CachedBulletTrackComponent
    // (IDA libUE4.so 字符串验证: STExtraShootWeapon+0x1F60 = CachedBulletTrackComponent)
    RESOLVE_OFFSET_MULTI(m_off.Char_CurWeapon,           "CurEquipWeapon",             "STExtraBaseCharacter", "STExtraCharacter", "UAECharacter");
    RESOLVE_OFFSET_MULTI(m_off.Weapon_BulletTrackComp,   "CachedBulletTrackComponent", "STExtraShootWeapon", "STExtraWeapon");

    // PickUpWrapperActor — 地面物资 (DefineID/Count/bHasBeenPickedUp)
    RESOLVE_OFFSET_MULTI(m_off.PickUp_DefineID,         "DefineID",          "PickUpWrapperActor");
    RESOLVE_OFFSET_MULTI(m_off.PickUp_Count,            "Count",             "PickUpWrapperActor");
    RESOLVE_OFFSET_MULTI(m_off.PickUp_bHasBeenPickedUp, "bHasBeenPickedUp",  "PickUpWrapperActor");
    RESOLVE_OFFSET_MULTI(m_off.PickUp_bIsInBox,         "bIsInBox",          "PickUpWrapperActor");
    if (m_off.PickUp_DefineID < 0)         m_off.PickUp_DefineID = 0x6F0;
    if (m_off.PickUp_Count < 0)            m_off.PickUp_Count = 0x708;
    if (m_off.PickUp_bHasBeenPickedUp < 0) m_off.PickUp_bHasBeenPickedUp = 0x70C;
    if (m_off.PickUp_bIsInBox < 0)         m_off.PickUp_bIsInBox = 0x70E;

    // STExtraVehicleBase — 载具
    RESOLVE_OFFSET_MULTI(m_off.Vehicle_VehicleType,        "VehicleType",        "STExtraVehicleBase");
    RESOLVE_OFFSET_MULTI(m_off.Vehicle_VehicleHealthState, "VehicleHealthState", "STExtraVehicleBase");
    RESOLVE_OFFSET_MULTI(m_off.Vehicle_TeamID,             "TeamID",             "STExtraVehicleBase");
    RESOLVE_OFFSET_MULTI(m_off.Vehicle_Fuel,               "CommonComponent_Fuel","STExtraVehicleBase");
    if (m_off.Vehicle_VehicleType < 0)        m_off.Vehicle_VehicleType = 0x7AE;
    if (m_off.Vehicle_VehicleHealthState < 0) m_off.Vehicle_VehicleHealthState = 0xB84;
    if (m_off.Vehicle_TeamID < 0)             m_off.Vehicle_TeamID = 0x1594;
    if (m_off.Vehicle_Fuel < 0)               m_off.Vehicle_Fuel = 0x3374;

    // PickUpListWrapperActor.PickUpDataList @ 0xD98 (TArray<PickUpItemData>, stride 0x38)
    RESOLVE_OFFSET_MULTI(m_off.PickUpList_DataList, "PickUpDataList", "PickUpListWrapperActor");
    if (m_off.PickUpList_DataList < 0) m_off.PickUpList_DataList = 0xD98;

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
    LOG(LOG_LEVEL_INFO, "[InitOffsets] Weapon: CurWeapon=0x%X BulletTrackComp=0x%X",
        m_off.Char_CurWeapon, m_off.Weapon_BulletTrackComp);
    LOG(LOG_LEVEL_INFO, "[InitOffsets] Camera: PC.PlayerCameraManager=0x%X PCM.CameraCache=0x%X PCM.DefaultFOV=0x%X",
        m_off.PC_PlayerCameraManager, m_off.PCM_CameraCache, m_off.PCM_DefaultFOV);

    return m_off.isValid();
}

#undef RESOLVE_OFFSET
#undef RESOLVE_OFFSET_MULTI

// =====================================================================
//  安全内存读取
// =====================================================================
//
// 防御性检查 (B): safeReadPtr 拿到的值如果不像有效用户态指针 (太小 / 太大 /
// 明显是 NameIndex / ObjectID / float bit pattern 等), 返回 0. 后续任何
// 链式 safeReadPtr(0) 都会被 < 0x10000 拦截, 不会再越界 memcpy.
// 0x6d8504... 之前的崩溃就是某处把 32-bit 字段 0x6cb52 当指针 memcpy(...,4) 抓的.
static inline bool looksLikeUserPtr(uintptr_t p) {
    // arm64 用户态地址通常 < 0x80_0000_0000 (不到 512GB), 且页对齐起步,
    // 至少要落在 mmap 范围, 拒绝 < 0x10000 的明显小整数.
    if (p < 0x10000) return false;
    if (p >= 0x0000800000000000ULL) return false; // 上半区是内核
    return true;
}

uintptr_t MatchMonitor::safeReadPtr(uintptr_t addr) {
    uintptr_t val = 0;
    if (!safeReadMemory(addr, &val, sizeof(val))) return 0;
    if (!looksLikeUserPtr(val)) return 0;
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
//  LiveCameraSnapshot — 实时相机刷新 (慢路径发布地址, 快路径每帧重读)
// =====================================================================
LiveCameraSnapshot& LiveCameraSnapshot::instance() {
    static LiveCameraSnapshot inst;
    return inst;
}

void LiveCameraSnapshot::publish(uintptr_t locAddr, uintptr_t rotAddr, uintptr_t fovAddr,
                                 uintptr_t rotOverrideAddr, float defaultFov) {
    m_locAddr.store(locAddr,         std::memory_order_relaxed);
    m_rotAddr.store(rotAddr,         std::memory_order_relaxed);
    m_fovAddr.store(fovAddr,         std::memory_order_relaxed);
    m_rotOverrideAddr.store(rotOverrideAddr, std::memory_order_relaxed);
    m_defaultFov.store(defaultFov,   std::memory_order_release);
}

void LiveCameraSnapshot::clear() {
    m_locAddr.store(0,        std::memory_order_relaxed);
    m_rotAddr.store(0,        std::memory_order_relaxed);
    m_fovAddr.store(0,        std::memory_order_relaxed);
    m_rotOverrideAddr.store(0, std::memory_order_release);
}

bool LiveCameraSnapshot::refresh(ue4draw::DrawGameData& data) const {
    const uintptr_t locAddr = m_locAddr.load(std::memory_order_acquire);
    if (locAddr < 0x10000) return false;
    const uintptr_t rotAddr = m_rotAddr.load(std::memory_order_relaxed);
    const uintptr_t fovAddr = m_fovAddr.load(std::memory_order_relaxed);
    const uintptr_t rotOverrideAddr = m_rotOverrideAddr.load(std::memory_order_relaxed);

    float locX = 0, locY = 0, locZ = 0;
    if (!safeReadMemory(locAddr,     &locX, sizeof(float))) return false;
    if (!safeReadMemory(locAddr + 4, &locY, sizeof(float))) return false;
    if (!safeReadMemory(locAddr + 8, &locZ, sizeof(float))) return false;
    if (!std::isfinite(locX) || !std::isfinite(locY) || !std::isfinite(locZ)) return false;

    const uintptr_t actualRotAddr = (rotOverrideAddr >= 0x10000) ? rotOverrideAddr : rotAddr;
    float pitch = 0, yaw = 0, roll = 0;
    if (actualRotAddr >= 0x10000) {
        safeReadMemory(actualRotAddr,     &pitch, sizeof(float));
        safeReadMemory(actualRotAddr + 4, &yaw,   sizeof(float));
        safeReadMemory(actualRotAddr + 8, &roll,  sizeof(float));
    }
    if (!std::isfinite(pitch) || !std::isfinite(yaw) || !std::isfinite(roll)) return false;

    float fov = 0;
    if (fovAddr >= 0x10000) safeReadMemory(fovAddr, &fov, sizeof(float));
    if (!std::isfinite(fov) || fov < 30.0f || fov > 170.0f) {
        fov = m_defaultFov.load(std::memory_order_relaxed);
    }

    data.camLocX = locX;
    data.camLocY = locY;
    data.camLocZ = locZ;
    data.camPitch = pitch;
    data.camYaw = yaw;
    data.camRoll = roll;
    data.camFOV = fov;
    return true;
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

// AGameStateBase::HasMatchStarted UFunction 原生 thunk 偏移 (libUE4.so 内).
//
// 来源: C:\Users\user\Desktop\bupg\dump.cs (~line 428625)
//   class GameStateBase : public Info
//     // Flags: Native|Net|Exec [Owner: GameStateBase]
//     void HasMatchStarted(); // [Offset: 0xD0EF3CC] // NumParms: 1
//
// 历史教训: 同名函数在 GameModeBase 上也存在 (Offset 0xD0DE220),
//   但客户端进程通常没有 GameMode 实例; 把 GameState 当 Context 传给
//   GameModeBase 版 thunk 会因虚表槽指向不同函数 -> SIGSEGV.
//   GameStateBase 版才与 findCurrentGameStateInstance() 命中的对象类型匹配.
//
// thunk 标准签名 (UE 源码 DECLARE_FUNCTION(execHasMatchStarted)):
//   void exec(UObject* Context, FFrame& Stack, void* Z_Param__Result)
//   函数体仅: P_FINISH; *(bool*)Result = ((AGameStateBase*)Context)->HasMatchStarted();
//   P_FINISH 展开为 Stack.Code += !!Stack.Code; 故 FFrame 全 0 即可安全通过.
//
// NumParms=1: 只有 1 个 UProperty (即 bool 返回值), 无输入参数.
// 调用约束: Context 必须能 static_cast 到 AGameStateBase* 后做虚函数调用,
//   即指向真实 GameStateBase 子类实例的对象指针 (findCurrentGameStateInstance
//   返回的 gsPtr 已经过 isSubclassOf("GameStateBase") 校验).
static constexpr uintptr_t kHasMatchStartedThunkOffset = 0xD0EF3CC;

namespace {

// stack_spoof::call_spoofed 只接受一参 (void*(*)(void*)), 用上下文结构桥接 thunk
// 三参 (Context, FFrame*, Result*) 调用. 出栈伪装窗口前后该结构在真栈上, 安全.
struct HasMatchStartedSpoofCtx {
    void  (*thunk)(void* /*Context*/, void* /*FFrame*/, void* /*Result*/);
    void*   context;
    // FFrame 在 tencent 改造的 UE 中内存布局未公开. execHasMatchStarted thunk
    // 体只触碰 Stack.Code (P_FINISH), 不做 Logf / Locals 访问, 故全 0 缓冲即可.
    // 256 字节 >= UE4 主流 FFrame size (~120B), 留足版本余量.
    alignas(8) uint8_t frameBuf[256];
    bool    result;
};

// 实际跑在 fake FP chain 下的小型蹦床: 先清空 FFrame 缓冲, 再调 UE 原生 thunk.
// 注意 SP 不切换, 局部变量 / 入参指针仍指向真栈, thunk 内对 Result 的写入正常.
void* hasMatchStartedTrampoline(void* p) {
    auto* c = static_cast<HasMatchStartedSpoofCtx*>(p);
    // 防御性清零, 防止上一次调用的脏字节让 thunk 误判 Stack.Code 非 null
    for (auto& b : c->frameBuf) b = 0;
    c->result = false;
    c->thunk(c->context, c->frameBuf, &c->result);
    return nullptr;
}

} // namespace

// 通过 GUObjectArray 全表扫定位真实 GameState 实例.
//
// 背景: Frida 验证证明腾讯 PUBG 的 World+0xAC0 不指向真 GameState (该位置常
//   是垃圾指针), 直接用其调 HasMatchStarted thunk 会读到非零脏字节产生假阳,
//   或直接 segfault. 唯一可靠路径是按 ClassPrivate 在对象表里找一个真实
//   GameStateBase 子类实例, 再调 thunk.
//
// 缓存策略: 命中后缓存对象指针 + 类指针白名单. 后续调用 O(1) 验证缓存对象
//   的 ClassPrivate 仍是已知 GameState 子类即可复用; 缓存失效再触发全表扫.
//   未命中时 2s 节流避免大厅期间空转.
uintptr_t MatchMonitor::findCurrentGameStateInstance() {
    // ---- 快路径: 缓存的 GS 仍存活 + 类未变 ----
    if (m_cachedGSPtr != 0) {
        uintptr_t cls = safeReadPtr(m_cachedGSPtr + kUObjectClassPrivateOffset);
        if (cls != 0 && cls >= 0x10000) {
            auto it = m_gsClassSet.find(cls);
            bool isGS = false;
            if (it != m_gsClassSet.end()) {
                isGS = it->second;
            } else {
                isGS = isSubclassOf(cls, "GameStateBase");
                m_gsClassSet[cls] = isGS;
            }
            if (isGS) return m_cachedGSPtr;
        }
        m_cachedGSPtr = 0;
    }

    // ---- 空扫节流: 上次没找到时, 2s 内不再重复扫 ----
    const uint64_t nowMs = nowMonotonicMs();
    if (m_lastEmptyGSScanMs != 0 && nowMs - m_lastEmptyGSScanMs < 2000) {
        return 0;
    }

    // ---- 慢路径: 全表扫 GUObjectArray ----
    const uintptr_t arr = m_gUObjectArray;
    UObjectArrayLayout layout{};
    if (!detectUObjectArrayLayout(arr, layout)) {
        m_lastEmptyGSScanMs = nowMs;
        return 0;
    }

    for (int32_t index = 0; index < layout.totalNum; ++index) {
        const uintptr_t obj = readUObjectAt(arr, layout, index);
        if (obj == 0 || obj < 0x10000) continue;
        const uintptr_t cls = safeReadPtr(obj + kUObjectClassPrivateOffset);
        if (cls == 0 || cls < 0x10000) continue;
        auto it = m_gsClassSet.find(cls);
        bool isGS;
        if (it != m_gsClassSet.end()) {
            isGS = it->second;
        } else {
            isGS = isSubclassOf(cls, "GameStateBase");
            m_gsClassSet[cls] = isGS;
        }
        if (!isGS) continue;
        // 排除 CDO (Default__XXX): NamePrivate 以 Default__ 开头
        const std::string objName = readObjName(obj);
        if (objName.compare(0, 9, "Default__") == 0) continue;
        m_cachedGSPtr = obj;
        m_lastEmptyGSScanMs = 0;
        LOG(LOG_LEVEL_INFO, "[MatchCall] GameState instance found @ %p name=%s class=%s layout=%s",
            (void*)obj, objName.c_str(), readObjName(cls).c_str(), uObjectArrayLayoutName(layout.kind));
        return obj;
    }
    m_lastEmptyGSScanMs = nowMs;
    return 0;
}

MatchState MatchMonitor::getMatchState() {
    MatchState ms;
    uintptr_t worldPtr = safeReadPtr(m_gWorld);
    if (worldPtr == 0 || worldPtr < 0x10000) {
        ms.state = "NO_WORLD";
        return ms;
    }
    ms.worldName = readObjName(worldPtr);

    // 唯一 GameState 来源: 扫 GUObjectArray, 不再读 World+0xAC0 (腾讯版指向垃圾).
    const uintptr_t gsPtr = findCurrentGameStateInstance();
    ms.gameStatePtr = gsPtr;

    if (gsPtr == 0) {
        ms.state = "NoGameState";
        ms.inMatch = false;
        // 周期诊断 (3s 一次): 帮助定位为何对局中仍找不到 GS 实例
        static uint64_t s_lastNoGSLogMs = 0;
        const uint64_t nowMs = nowMonotonicMs();
        if (nowMs - s_lastNoGSLogMs > 3000) {
            s_lastNoGSLogMs = nowMs;
            LOG(LOG_LEVEL_WARN, "[MatchRead] 未找到 GameState 实例 (world=%s, gsClassSet.size=%zu)",
                ms.worldName.c_str(), m_gsClassSet.size());
        }
        return ms;
    }

    if (m_off.GS_ElapsedTime >= 0) {
        const int32_t elapsedTime = safeReadS32(gsPtr + m_off.GS_ElapsedTime);
        if (elapsedTime >= 0 && elapsedTime < 7200) {
            ms.elapsedTimeSeconds = elapsedTime;
        }
    }

    // 唯一 inMatch 判定: 直接读 GameState.MatchState (FName) 字段.
    // 不调 HasMatchStarted thunk —— 进入 libUE4.so 代码段会被腾讯 ACE 反作弊
    // 扫到调用栈, 大约 30-50 秒后会被 SI_TKILL SIGBUS 杀掉 UE 主线程.
    // 直接读字段是纯读, ACE 难以区分.
    //
    // [2026-04-27] 按需求改为"函数调用"路径 (走下方 stack_spoof + thunk),
    // 这一段字段读判定整体注释保留作为回滚方案; 若 thunk 路径不稳, 把这段恢复
    // 并把下方 kEnableSpoofedHasMatchStartedProbe 改回 false 即可一键回退.
    // ----------------------------------------------------------------------
    // if (m_off.GS_MatchState >= 0) {
    //     const std::string mst = readFName(gsPtr + m_off.GS_MatchState);
    //     ms.state = mst;
    //     // 等价 UE 源码 AGameState::HasMatchStarted: MatchState != EnteringMap
    //     // 下游 (pollPlayers / patchActorNetCull) 按 state=="InProgress" 字面 gating
    //     ms.inMatch = (mst == "InProgress");
    // } else {
    //     ms.state = "MatchStateOffsetMissing";
    //     ms.inMatch = false;
    // }
    // ----------------------------------------------------------------------
    // state 仍保留 FName 字符串 (调试可读 + 下游 m_currentMatchState 仍用它做
    // "InProgress" 字面 gating). inMatch 改由下方 thunk 返回值决定.
    if (m_off.GS_MatchState >= 0) {
        ms.state = readFName(gsPtr + m_off.GS_MatchState);
    } else {
        ms.state = "MatchStateOffsetMissing";
    }
    ms.inMatch = false;  // 默认 false, 由下方 thunk 调用结果覆盖

    int32_t playerArrayNum = -1;
    if (m_off.GS_PlayerArray >= 0) {
        playerArrayNum = safeReadS32(gsPtr + m_off.GS_PlayerArray + 8);
    }
    ms.playerArrayNum = playerArrayNum;
    const bool hasStableWorld = !(ms.worldName.empty() || ms.worldName == "None");
    const bool hasLikelyMatchPlayers = (playerArrayNum < 0 || playerArrayNum >= 10);

    // === 栈伪装下调用原生 HasMatchStarted thunk (现作为 inMatch 权威来源) ===
    // 切换 x29 (FP) 到伪造 chain 后再 BLR 进 libUE4.so, 让 ACE 周期栈采样看到的
    // LR 链全部落在 libc.so / libart.so 白名单模块, 而非本注入 .so + libUE4.so 调用对.
    // 注意:
    //   - 现在调用的是 AGameStateBase 版 thunk (kHasMatchStartedThunkOffset =
    //     0xD0EF3CC), Context = gsPtr (findCurrentGameStateInstance 已校验为
    //     GameStateBase 子类实例), 虚表槽签名匹配, 不会再触发上次的 SIGSEGV.
    //   - 历史教训:
    //       1) 旧版本指向 AGameModeBase 版 thunk (0xD0DE220) + gsPtr,
    //          因虚表失配导致 SIGSEGV; 客户端通常没有 GameMode 实例.
    //       2) 即便切到 GameStateBase 版 thunk + 正确 Context, 仍可能崩, 怀疑
    //          腾讯改造版 thunk 在 P_FINISH 之外还会读 FFrame.Node/Object/
    //          PreviousFrame, 我们填的 256B 全 0 缓冲不满足.
    //   - 一键回退: 把 kEnableSpoofedHasMatchStartedProbe 改回 false 并取消上方
    //     字段读注释.
    //   - SIGSEGV/SIGBUS 熔断: 复用 installSafeReadGuard 装的信号屏障 +
    //     sigsetjmp, thunk 内部访问到非法内存时 longjmp 回来. 熔断粒度 = 该 GS
    //     对象指针 (s_disabledGsPtr): 只对崩过的对象禁用, 下一局新 GS 出现时
    //     自动恢复 thunk 调用.
    //   - LeavingMap / WaitingPostMatch 主动跳过 thunk: 关卡切换/结算窗口期 UE
    //     正在拆 GameState (World/Outer/AuthorityGameMode 链可能半 NULL),
    //     此时 AGameStateBase::HasMatchStarted 实现内部摸 World 几乎必崩.
    //     这两种状态下 inMatch 用 FName 字段读判定即可 (HasMatchStarted UE 语义
    //     是 MatchState != EnteringMap, LeavingMap 它也返回 true, 反而不准).
    constexpr bool kEnableSpoofedHasMatchStartedProbe = true;
    static std::atomic<uintptr_t> s_disabledGsPtr{0};
    bool spoofResolved = false;
    const bool inDestructiveWindow = (ms.state == "LeavingMap"
                                   || ms.state == "WaitingPostMatch");
    const bool gsBlacklisted = (s_disabledGsPtr.load(std::memory_order_acquire) == gsPtr);
    if (kEnableSpoofedHasMatchStartedProbe
        && !gsBlacklisted
        && !inDestructiveWindow
        && ms.state == "InProgress"
        && hasStableWorld
        && hasLikelyMatchPlayers
        && gsPtr != 0 && m_moduleBase != 0) {
        using ThunkFn = void(*)(void*, void*, void*);
        auto* thunk = reinterpret_cast<ThunkFn>(m_moduleBase + kHasMatchStartedThunkOffset);
        HasMatchStartedSpoofCtx ctx{};
        ctx.thunk   = thunk;
        ctx.context = reinterpret_cast<void*>(gsPtr);

        // 安装信号屏障 + sigsetjmp: 复用 safeReadMemory 那套 TLS jmp_buf, 信号
        // 处理器仅在 s_safeReadActive=1 时 longjmp; trampoline 内无嵌套 safeRead,
        // 复用安全.
        installSafeReadGuard();
        const int sigCaught = sigsetjmp(s_safeReadJmpBuf, 1);
        if (sigCaught == 0) {
            s_safeReadActive = 1;
            // 执行栈伪装调用: 进入前切换 FP 到 fake chain, 出来立刻还原, 不留痕迹.
            stack_spoof::call_spoofed(&hasMatchStartedTrampoline, &ctx);
            s_safeReadActive = 0;
            ms.inMatch = ctx.result;
            spoofResolved = true;
            static uint64_t s_lastSpoofLogMs = 0;
            const uint64_t spoofNowMs = nowMonotonicMs();
            if (spoofNowMs - s_lastSpoofLogMs > 3000) {
                s_lastSpoofLogMs = spoofNowMs;
                LOG(LOG_LEVEL_INFO,
                    "[MatchRead] HasMatchStarted(spoofed)=%d (FName=%s, gs=%p)",
                    ctx.result ? 1 : 0, ms.state.c_str(), (void*)gsPtr);
            }
        } else {
            // longjmp 回来: 把当前 gsPtr 拉黑, 下一局换了新 GS 自动恢复 thunk
            s_safeReadActive = 0;
            s_disabledGsPtr.store(gsPtr, std::memory_order_release);
            LOG(LOG_LEVEL_ERROR,
                "[MatchRead][CRASH] HasMatchStarted spoofed thunk faulted (sig=%d), "
                "blacklisting this GS and falling back to FName field read. "
                "thunk=%p gs=%p FName=%s",
                sigCaught,
                (void*)(m_moduleBase + kHasMatchStartedThunkOffset),
                (void*)gsPtr, ms.state.c_str());
        }
    }
    // 兜底: thunk 路径未生效 (开关关 / GS 拉黑 / 销毁窗口 / gsPtr 缺失) 时,
    // 用 FName 字段读还原 inMatch 判定, 与最初纯字段读语义一致.
    if (!spoofResolved) {
        ms.inMatch = (ms.state == "InProgress");
        // 大厅同样跑在 InProgress 状态 (avatar 展示场景), 表现为
        // world=None + PlayerArray<=1 (只有本地一个槽位).
        // 训练场 PlayerArray>=2 (小队), 真实对局 >=10, 均不会被误杀.
        if (ms.inMatch && !hasStableWorld
            && playerArrayNum >= 0 && playerArrayNum <= 1) {
            ms.inMatch = false;
        }
    }
    // 候选确认仅在 spoof 未跑 + worldName 不稳 + PlayerArray 完全空时使用,
    // 此时连大厅过滤都没法判定, 用 PC->AcknowledgedPawn 类名兜底.
    ms.needsPlayerConfirmation = (!spoofResolved
                               && ms.inMatch
                               && ms.state == "InProgress"
                               && !hasStableWorld
                               && playerArrayNum == 0);

    // === thunk 结果过滤 ===
    // worldName / PlayerArray.Num 只用于过滤 thunk 路径的大厅误命中.
    // 训练场 / 体验场景实测也会出现 InProgress + world=None + PlayerArray=0,
    // 因此字段读兜底路径不能再用这两个值强行判非对局.
    if (ms.inMatch && spoofResolved) {
        if (ms.worldName.empty() || ms.worldName == "None") {
            ms.inMatch = false;
        } else if (playerArrayNum >= 0) {
            if (playerArrayNum < 10) {
                ms.inMatch = false;
            }
        }
    }
    // 周期诊断 (3s 一次): 打印 inMatch + state 给排障使用
    static uint64_t s_lastStateLogMs = 0;
    static int s_lastInMatch = -1;
    const uint64_t nowMs2 = nowMonotonicMs();
    if (s_lastInMatch != (int)ms.inMatch || nowMs2 - s_lastStateLogMs > 3000) {
        s_lastStateLogMs = nowMs2;
        s_lastInMatch = ms.inMatch ? 1 : 0;
        LOG(LOG_LEVEL_INFO, "[MatchRead] gs=%p inMatch=%d candidate=%d state=%s world=%s playerArray=%d elapsed=%d",
            (void*)gsPtr, ms.inMatch ? 1 : 0,
            ms.needsPlayerConfirmation ? 1 : 0, ms.state.c_str(),
            ms.worldName.c_str(), ms.playerArrayNum, ms.elapsedTimeSeconds);
    }
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

    // 方法0: 当前 dump.cs 暴露了 SkeletalMesh.RefBoneNames, 优先走反射偏移。
    int32_t skMeshRefBoneNamesOff = -1;
    if (const auto* refNamesField = m_interface.findFieldInHierarchy("SkeletalMesh", "RefBoneNames")) {
        skMeshRefBoneNamesOff = refNamesField->offset;
    }
    if (skMeshRefBoneNamesOff < 0) skMeshRefBoneNamesOff = 0x3E8;
    ue4::TArray<ue4::FName> skMeshRefBoneNames{};
    if (safeReadMemory(skeletalMeshAssetPtr + skMeshRefBoneNamesOff, &skMeshRefBoneNames, sizeof(skMeshRefBoneNames))
        && isUsableRemoteArray(skMeshRefBoneNames, kMaxBoneNameCount)) {
        matchBoneNamesFromFNameArray(reinterpret_cast<uintptr_t>(skMeshRefBoneNames.Data),
                                     skMeshRefBoneNames.Num,
                                     entry);
        if (entry.matchedCount >= kMinRenderableBoneMatches) {
            m_boneAssetCache[skeletalMeshAssetPtr] = entry;
            outEntry = entry;
            return true;
        }
    }

    // 方法1: SkeletalMesh+0x220 FReferenceSkeleton.RawRefBoneInfo
    //   ※ 此 build (PUBGm HD) 该字段实际位于 0x220, dump.cs 显示的 0x238 是离线 padding 差异;
    //     运行时 [SkelScan] 多资产一致命中 0x220 (num=61~68, stride=16, FName@+0)。
    constexpr uintptr_t kRefBoneInfoOffset = 0x220;
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
    const int32_t refOff = (m_off.Skeleton_RefBoneNames >= 0) ? m_off.Skeleton_RefBoneNames : 0x2D0;
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

    // 当 matchedCount 很低时, dump 该资产前 25 个原始骨骼名, 用于扩展别名表.
    // 每个资产指针只 dump 一次 (静态 set 去重, 上限 32 条).
    if (entry.matchedCount < kMinRenderableBoneMatches) {
        static std::unordered_set<uintptr_t> s_dumpedAssets;
        static std::mutex s_dumpMutex;
        bool firstTime = false;
        {
            std::lock_guard<std::mutex> g(s_dumpMutex);
            if (s_dumpedAssets.size() < 32 && s_dumpedAssets.insert(skeletalMeshAssetPtr).second) {
                firstTime = true;
            }
        }
        if (firstTime) {
            // ---- 诊断: 把每个候选偏移的原始 (Data, Num) 都打印出来 ----
            // SkeletalMesh.RefBoneNames @0x3E8
            ue4::TArray<ue4::FName> rawSkMeshRBN{};
            safeReadMemory(skeletalMeshAssetPtr + (skMeshRefBoneNamesOff >= 0 ? skMeshRefBoneNamesOff : 0x3E8),
                           &rawSkMeshRBN, sizeof(rawSkMeshRBN));
            // SkeletalMesh→Skeleton @0x48 → Skeleton.RefBoneNames @0x2D0
            const uintptr_t skeletonDbg = safeReadPtr(skeletalMeshAssetPtr + skelOff);
            ue4::TArray<ue4::FName> rawSkelRBN{};
            if (skeletonDbg >= 0x10000)
                safeReadMemory(skeletonDbg + refOff, &rawSkelRBN, sizeof(rawSkelRBN));
            // FReferenceSkeleton.RawRefBoneInfo @0x220 (PUBGm HD 实测)
            const uintptr_t rawBIData = safeReadPtr(skeletalMeshAssetPtr + 0x220);
            const int32_t  rawBINum  = safeReadS32(skeletalMeshAssetPtr + 0x220 + 8);

            char dbg[384];
            snprintf(dbg, sizeof(dbg),
                "[BoneDump] asset=%p cls=%s  off3E8(num=%d data=%p max=%d)  skel=%p off2D0(num=%d data=%p max=%d)  off220(num=%d data=%p)",
                (void*)skeletalMeshAssetPtr, readObjName(safeReadPtr(skeletalMeshAssetPtr)).c_str(),
                rawSkMeshRBN.Num, rawSkMeshRBN.Data, rawSkMeshRBN.Max,
                (void*)skeletonDbg,
                rawSkelRBN.Num, rawSkelRBN.Data, rawSkelRBN.Max,
                rawBINum, (void*)rawBIData);
            writeSkeletonLog(dbg);

            // ---- 关键扫描: 找到该资产里哪个 8B 槽是真正的 USkeleton 指针 ----
            // 标准: 该指针 +0x2D0 上有 TArray<FName> RefBoneNames, Num 在 [40,300] 且 Data 是 heap.
            // 也尝试 +0x238 (FReferenceSkeleton.RawRefBoneInfo, stride 16) 的合理 Num.
            uint8_t buf[0x800];
            if (safeReadMemory(skeletalMeshAssetPtr, buf, sizeof(buf))) {
                std::string scanLine;
                int scanHits = 0;
                for (int off = 0x40; off + 8 <= (int)sizeof(buf); off += 8) {
                    uint64_t cand;
                    memcpy(&cand, buf + off, 8);
                    if (cand < 0x10000ULL || cand > 0x7fffffffffffULL) continue;
                    // 试 +0x2D0 RefBoneNames
                    ue4::TArray<ue4::FName> rbn{};
                    if (safeReadMemory(cand + 0x2D0, &rbn, sizeof(rbn))
                        && rbn.Num >= 40 && rbn.Num <= 300 && rbn.Max >= rbn.Num
                        && reinterpret_cast<uintptr_t>(rbn.Data) >= 0x10000) {
                        char tmp[80];
                        snprintf(tmp, sizeof(tmp), " off=0x%X->skel=%p num2D0=%d", off, (void*)cand, rbn.Num);
                        scanLine += tmp;
                        if (++scanHits >= 6) break;
                    }
                }
                if (!scanLine.empty()) {
                    char hdr[64];
                    snprintf(hdr, sizeof(hdr), "[SkelScan] asset=%p hits:", (void*)skeletalMeshAssetPtr);
                    writeSkeletonLog((std::string(hdr) + scanLine).c_str());
                }
                // 同时扫资产自身 +0x100..+0x500 上是否直接挂着合理的 RawRefBoneInfo (stride 16)
                std::string riLine;
                int riHits = 0;
                for (int off = 0x100; off + 16 <= (int)sizeof(buf); off += 8) {
                    uint64_t data; int32_t num;
                    memcpy(&data, buf + off, 8);
                    memcpy(&num, buf + off + 8, 4);
                    if (data < 0x10000ULL || data > 0x7fffffffffffULL) continue;
                    if (num < 40 || num > 300) continue;
                    // 验证 +0 处是合理 FName (ComparisonIndex 在 m_numNames 范围)
                    ue4::FName fn{};
                    if (!safeReadMemory(data, &fn, sizeof(fn))) continue;
                    if (fn.ComparisonIndex <= 0 || fn.ComparisonIndex >= m_numNames) continue;
                    char tmp[80];
                    snprintf(tmp, sizeof(tmp), " off=0x%X(num=%d data=%p fn0=%d)", off, num, (void*)data, fn.ComparisonIndex);
                    riLine += tmp;
                    if (++riHits >= 4) break;
                }
                if (!riLine.empty()) {
                    char hdr[64];
                    snprintf(hdr, sizeof(hdr), "[SkelScan] asset=%p arrayHits:", (void*)skeletalMeshAssetPtr);
                    writeSkeletonLog((std::string(hdr) + riLine).c_str());
                }
            }

            // 选取最大 transformsCount 的来源做 dump (优先 RefBoneNames)
            ue4::TArray<ue4::FName> dumpArr{};
            uintptr_t arrPtr = 0; int arrCount = 0;
            if (skMeshRefBoneNamesOff >= 0
                && safeReadMemory(skeletalMeshAssetPtr + skMeshRefBoneNamesOff, &dumpArr, sizeof(dumpArr))
                && isUsableRemoteArray(dumpArr, kMaxBoneNameCount)) {
                arrPtr = reinterpret_cast<uintptr_t>(dumpArr.Data);
                arrCount = dumpArr.Num;
            } else if (skeletonDbg >= 0x10000
                       && isUsableRemoteArray(rawSkelRBN, kMaxBoneNameCount)) {
                arrPtr = reinterpret_cast<uintptr_t>(rawSkelRBN.Data);
                arrCount = rawSkelRBN.Num;
            } else if (rawBIData >= 0x10000 && rawBINum > 0 && rawBINum < 1024) {
                // FMeshBoneInfo: FName 在 +0, stride 16  (仅在 Num 看起来合理时才尝试)
                char header[128];
                snprintf(header, sizeof(header), "[BoneDump] asset=%p (BoneInfo@0x220) names follow (n=%d):",
                         (void*)skeletalMeshAssetPtr, rawBINum);
                writeSkeletonLog(header);
                int dumpN = rawBINum > 25 ? 25 : rawBINum;
                std::string line = "  ";
                for (int i = 0; i < dumpN; ++i) {
                    ue4::FName bn{};
                    if (!safeReadMemory(rawBIData + static_cast<uintptr_t>(i) * 16, &bn, sizeof(bn))) break;
                    if (bn.ComparisonIndex < 0 || bn.ComparisonIndex >= m_numNames) continue;
                    line += getNameByIndex(bn.ComparisonIndex);
                    line += "|";
                    if (line.size() > 200) { writeSkeletonLog(line.c_str()); line = "  "; }
                }
                if (line.size() > 2) writeSkeletonLog(line.c_str());
            }
            if (arrPtr) {
                char header[128];
                snprintf(header, sizeof(header), "[BoneDump] asset=%p (RefBoneNames) names follow (n=%d):",
                         (void*)skeletalMeshAssetPtr, arrCount);
                writeSkeletonLog(header);
                int dumpN = arrCount > 25 ? 25 : arrCount;
                std::string line = "  ";
                for (int i = 0; i < dumpN; ++i) {
                    ue4::FName bn{};
                    if (!safeReadMemory(arrPtr + static_cast<uintptr_t>(i) * sizeof(ue4::FName), &bn, sizeof(bn))) break;
                    if (bn.ComparisonIndex < 0 || bn.ComparisonIndex >= m_numNames) continue;
                    line += getNameByIndex(bn.ComparisonIndex);
                    line += "|";
                    if (line.size() > 200) { writeSkeletonLog(line.c_str()); line = "  "; }
                }
                if (line.size() > 2) writeSkeletonLog(line.c_str());
            }
        }
    }

    return entry.matchedCount > 0;
}

// =====================================================================
//  fillPlayerSkeleton — 填充玩家骨骼世界坐标
//
//  流程: 收集候选组件 → 选最多 transforms 的 → 匹配骨骼名 → 坐标变换
// =====================================================================
bool MatchMonitor::fillPlayerSkeleton(uintptr_t characterPtr, ue4draw::DrawPlayerInfo& outPlayer) {
    static Clock::time_point s_lastSkeletonLogTime;

    // dump.cs 当前值仅作最后兜底; 正常路径使用 UE4Interface 运行时反射偏移。
    constexpr int32_t kFB_Char_Mesh         = 0x658;
    constexpr int32_t kFB_ComponentToWorld   = 0x1F0;
    constexpr int32_t kFB_SkeletalMesh       = 0x808;
    constexpr int32_t kFB_MasterPose         = 0x810;
    constexpr int32_t kFB_CachedTransforms   = 0xBE0;
    constexpr int32_t kFB_CachedBoneTrans    = 0xBD0;
    constexpr int32_t kFB_AvatarComp         = 0x3C88;
    constexpr int32_t kFB_FPPComp            = 0x4258;
    constexpr int32_t kFB_DefaultMesh        = 0x4730;
    constexpr int32_t kFB_LastSkelMesh       = 0x4890;
    constexpr int32_t kFB_MasterBone         = 0x300;
    constexpr int32_t kFB_MeshCompList       = 0x530;
    constexpr int32_t kFB_SkelPool           = 0xF30;
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

    const std::string characterClassName = readClassName(characterPtr);

    auto dynamicFieldOff = [&](const std::string& className, const char* fieldName) -> int32_t {
        if (className.empty() || className[0] == '<') return -1;
        if (const auto* field = m_interface.findFieldInHierarchy(className, fieldName)) {
            return field->offset;
        }
        return -1;
    };

    auto off = [](int32_t objectSpecific, int32_t reflected, int32_t fallback) -> int32_t {
        if (objectSpecific >= 0) return objectSpecific;
        if (reflected >= 0) return reflected;
        return fallback;
    };

    const int32_t oMesh      = off(dynamicFieldOff(characterClassName, "Mesh"), m_off.Char_Mesh, kFB_Char_Mesh);
    const int32_t oCtw       = off(dynamicFieldOff("SceneComponent", "ComponentToWorld"), m_off.SceneComp_ComponentToWorld, kFB_ComponentToWorld);
    const int32_t oSkelMesh  = off(dynamicFieldOff("SkeletalMeshComponent", "SkeletalMesh"), m_off.SkinnedMesh_SkeletalMesh, kFB_SkeletalMesh);
    const int32_t oMasterP   = off(dynamicFieldOff("SkeletalMeshComponent", "MasterPoseComponent"), m_off.SkinnedMesh_MasterPoseComponent, kFB_MasterPose);
    const int32_t oCached    = off(dynamicFieldOff("SkeletalMeshComponent", "CachedComponentSpaceTransforms"), m_off.SkeletalMeshComp_CachedComponentSpaceTransforms, kFB_CachedTransforms);
    const int32_t oCachedBone = off(dynamicFieldOff("SkeletalMeshComponent", "CachedBoneSpaceTransforms"), -1, kFB_CachedBoneTrans);
    const int32_t oAvatar    = off(dynamicFieldOff(characterClassName, "AvatarComponent"), m_off.STBase_AvatarComponent, kFB_AvatarComp);
    const int32_t oMasterB   = off(dynamicFieldOff("AvatarComponent", "MasterBoneComponent"), m_off.Avatar_MasterBoneComponent, kFB_MasterBone);

    // ---- 辅助 ----
    auto readTransformArray = [&](uintptr_t comp, ue4::TArray<RemoteTransform>& out) -> bool {
        if (comp < 0x10000) return false;
        if (safeReadMemory(comp + oCached, &out, sizeof(out)) && isUsableRemoteArray(out, kMaxBoneNameCount))
            return true;
        if (safeReadMemory(comp + oCachedBone, &out, sizeof(out)) && isUsableRemoteArray(out, kMaxBoneNameCount))
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
        const int32_t oMeshList = off(dynamicFieldOff("AvatarComponent", "meshComponentList"), m_off.Avatar_MeshComponentList, kFB_MeshCompList);
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
        const int32_t oPool = off(dynamicFieldOff("AvatarComponent", "SkeletalMeshCompPool"), m_off.Avatar_SkeletalMeshCompPool, kFB_SkelPool);
        ue4::TArray<uintptr_t> pool{};
        if (safeReadMemory(avatar + oPool, &pool, sizeof(pool)) && isUsableRemoteArray(pool, 64)) {
            uintptr_t pd = reinterpret_cast<uintptr_t>(pool.Data);
            for (int i = 0; i < pool.Num; ++i)
                tryAdd(safeReadPtr(pd + i * 8));
        }
    }

    // 3. FPPComp → _AvatarComp → MasterBone
    const uintptr_t fpp = safeReadPtr(characterPtr + off(dynamicFieldOff(characterClassName, "FPPComp"), m_off.STBase_FPPComp, kFB_FPPComp));
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

    // (4. 已移除暴力扫描: 每帧每玩家发 ~200 次 process_vm_readv 触发 CrashSight 反作弊检测.)

    if (candidates.empty()) {
        // per-class 节流 (不同 cls 独立计时, 避免 BP_TrainPlayerPawn_C 坠占诊断名额)
        static std::unordered_map<std::string, Clock::time_point> s_classFailLog;
        static std::mutex s_classFailMutex;
        bool emit = false;
        {
            std::lock_guard<std::mutex> g(s_classFailMutex);
            auto it = s_classFailLog.find(characterClassName);
            auto now = Clock::now();
            if (it == s_classFailLog.end() ||
                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() >= 5000) {
                s_classFailLog[characterClassName] = now;
                emit = true;
            }
        }
        if (emit) {
            char buf[256];
            snprintf(buf, sizeof(buf), "[SkeletonFail] key=%u stage=NoCandidates cls='%s' charMesh=%p avatar=%p oMesh=0x%x oAvatar=0x%x",
                outPlayer.playerKey, characterClassName.c_str(),
                (void*)charMesh, (void*)avatar, oMesh, oAvatar);
            LOG(LOG_LEVEL_INFO, "%s", buf);
            writeSkeletonLog(buf);
        }
        return false;
    }

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
    addAsset(safeReadPtr(characterPtr + off(dynamicFieldOff(characterClassName, "DefaultCharacterMesh"), m_off.STBase_DefaultCharacterMesh, kFB_DefaultMesh)));
    addAsset(safeReadPtr(characterPtr + off(dynamicFieldOff(characterClassName, "LastSkeletalMesh"), m_off.STBase_LastSkeletalMesh, kFB_LastSkelMesh)));

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
        // per-class 节流
        static std::unordered_map<std::string, Clock::time_point> s_classFailLog;
        static std::mutex s_classFailMutex;
        bool emit = false;
        {
            std::lock_guard<std::mutex> g(s_classFailMutex);
            auto it = s_classFailLog.find(characterClassName);
            auto now = Clock::now();
            if (it == s_classFailLog.end() ||
                std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count() >= 5000) {
                s_classFailLog[characterClassName] = now;
                emit = true;
            }
        }
        if (emit) {
            int topCount = candidates.empty() ? 0 : candidates.front().count;
            char buf[384];
            snprintf(buf, sizeof(buf),
                "[SkeletonFail] key=%u stage=NoMatch cls='%s' candidates=%zu topTransforms=%d assets=%zu bestMatched=%d bestCopied=%d oCached=0x%x oCachedBone=0x%x",
                outPlayer.playerKey, characterClassName.c_str(),
                candidates.size(), topCount, assets.size(),
                bestMatched, (int)bestCopied, oCached, oCachedBone);
            LOG(LOG_LEVEL_INFO, "%s", buf);
            writeSkeletonLog(buf);
        }
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

    // 主路径: World.OwningGameInstance -> LocalPlayers[0] -> PlayerController
    if (m_off.World_OwningGameInstance >= 0
        && m_off.GI_LocalPlayers >= 0
        && m_off.Player_PlayerController >= 0) {
        uintptr_t gi = safeReadPtr(worldPtr + m_off.World_OwningGameInstance);
        if (gi >= 0x10000) {
            uintptr_t lpData = safeReadPtr(gi + m_off.GI_LocalPlayers);
            int32_t   lpNum  = safeReadS32(gi + m_off.GI_LocalPlayers + 8);
            if (lpData >= 0x10000 && lpNum > 0) {
                uintptr_t lp = safeReadPtr(lpData);
                if (lp >= 0x10000) {
                    uintptr_t pc = safeReadPtr(lp + m_off.Player_PlayerController);
                    if (pc >= 0x10000) return pc;
                }
            }
        }
    }

    // 回退: PlayerArray[0].Owner (旧逻辑, 训练场不可靠)
    if (m_off.World_GameState < 0 || m_off.GS_PlayerArray < 0) return 0;
    uintptr_t gsPtr = safeReadPtr(worldPtr + m_off.World_GameState);
    if (gsPtr == 0 || gsPtr < 0x10000) return 0;
    uintptr_t arrayData = safeReadPtr(gsPtr + m_off.GS_PlayerArray);
    int32_t arrayNum = safeReadS32(gsPtr + m_off.GS_PlayerArray + 8);
    if (arrayData == 0 || arrayNum <= 0) return 0;
    uintptr_t psPtr = safeReadPtr(arrayData);
    if (psPtr == 0) return 0;
    const int32_t ownerOff = (m_off.Actor_Owner >= 0) ? m_off.Actor_Owner : 0x98;
    return safeReadPtr(psPtr + ownerOff);
}

uintptr_t MatchMonitor::findPlayerCameraManagerInstance() {
    auto looksLikeLivePCM = [&](uintptr_t pcmPtr) {
        if (pcmPtr < 0x10000) return false;
        const int32_t camCacheOff = (m_off.PCM_CameraCache >= 0) ? m_off.PCM_CameraCache : 0x640;
        const int32_t defaultFovOff = (m_off.PCM_DefaultFOV >= 0) ? m_off.PCM_DefaultFOV : 0x5E8;
        const uintptr_t pov = pcmPtr + static_cast<uintptr_t>(camCacheOff) + 0x10;
        const float locX = safeReadFloat(pov + 0x0);
        const float locY = safeReadFloat(pov + 0x4);
        const float locZ = safeReadFloat(pov + 0x8);
        const bool hasLoc = std::isfinite(locX) && std::isfinite(locY) && std::isfinite(locZ)
            && (std::fabs(locX) > 1.0f || std::fabs(locY) > 1.0f || std::fabs(locZ) > 1.0f);
        if (!hasLoc) return false;

        const float fovCandidates[] = {
            safeReadFloat(pov + 0x18),
            safeReadFloat(pov + 0x1C),
            safeReadFloat(pov + 0x30),
            safeReadFloat(pcmPtr + defaultFovOff),
        };
        for (float fov : fovCandidates) {
            if (std::isfinite(fov) && fov >= 30.0f && fov <= 170.0f) {
                return true;
            }
        }
        return false;
    };

    if (m_cachedPCMPtr >= 0x10000) {
        uintptr_t cachedClass = safeReadPtr(m_cachedPCMPtr + kUObjectClassPrivateOffset);
        if (cachedClass >= 0x10000) {
            auto cachedIt = m_pcmClassSet.find(cachedClass);
            bool cachedIsPCM = false;
            if (cachedIt != m_pcmClassSet.end()) {
                cachedIsPCM = cachedIt->second;
            } else {
                cachedIsPCM = isSubclassOf(cachedClass, "PlayerCameraManager");
                m_pcmClassSet[cachedClass] = cachedIsPCM;
            }
            if (cachedIsPCM && looksLikeLivePCM(m_cachedPCMPtr)) return m_cachedPCMPtr;
        }
        m_cachedPCMPtr = 0;
    }

    const uint64_t nowMs = nowMonotonicMs();
    if (m_lastPCMScanMs != 0 && nowMs >= m_lastPCMScanMs && nowMs - m_lastPCMScanMs < 2000) {
        return 0;
    }
    m_lastPCMScanMs = nowMs;

    UObjectArrayLayout layout{};
    if (!detectUObjectArrayLayout(m_gUObjectArray, layout)) return 0;

    for (int32_t index = 0; index < layout.totalNum; ++index) {
        const uintptr_t obj = readUObjectAt(m_gUObjectArray, layout, index);
        if (obj < 0x10000) continue;

        const uintptr_t cls = safeReadPtr(obj + kUObjectClassPrivateOffset);
        if (cls < 0x10000) continue;

        auto it = m_pcmClassSet.find(cls);
        bool isPCM = false;
        if (it != m_pcmClassSet.end()) {
            isPCM = it->second;
        } else {
            isPCM = isSubclassOf(cls, "PlayerCameraManager");
            m_pcmClassSet[cls] = isPCM;
        }
        if (!isPCM) continue;

        const std::string objName = readObjName(obj);
        if (objName.compare(0, 9, "Default__") == 0) continue;
        if (!looksLikeLivePCM(obj)) continue;

        m_cachedPCMPtr = obj;
        LOG(LOG_LEVEL_INFO, "[Camera] PlayerCameraManager fallback found pcm=%p name=%s class=%s layout=%s",
            (void*)obj, objName.c_str(), readObjName(cls).c_str(), uObjectArrayLayoutName(layout.kind));
        return obj;
    }

    static Clock::time_point s_lastPCMScanMissLog;
    if (shouldLogEvery(s_lastPCMScanMissLog, std::chrono::milliseconds(3000))) {
        LOG(LOG_LEVEL_WARN, "[Camera] PlayerCameraManager fallback scan miss total=%d layout=%s",
            layout.totalNum, uObjectArrayLayoutName(layout.kind));
    }
    return 0;
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
    if (m_memoryRestored.load(std::memory_order_acquire)) return false;  // 恢复后禁止写入
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
    // 已恢复状态下不再写入, 防止恢复后又被覆盖
    if (m_memoryRestored.load(std::memory_order_acquire)) return;
    // 候选状态扫描不允许写入; 只有状态机确认进入对局后才修改 NetCullDist.
    if (!m_isInMatch) return;
    // 仅在 InProgress 状态写入 NetCullDist, 避免在加载/飞机/跳伞阶段触发网络异常断开
    if (m_currentMatchState != "InProgress") return;
    const uint64_t nowMs = nowMonotonicMs();
    const uint64_t patchIntervalMs = getNetCullPatchIntervalMs(m_currentMatchElapsedSeconds);
    auto it = m_lastNetCullPatchMs.find(actorPtr);
    if (it != m_lastNetCullPatchMs.end() && nowMs >= it->second && nowMs - it->second < patchIntervalMs) {
        return;
    }
    m_lastNetCullPatchMs[actorPtr] = nowMs;

    // 生成随机化的高值 (避免固定特征值被反作弊扫描)
    // 正常值约 ~1e8～9e9, 我们用 4e9～8e9 (合理偏高但不离谱)
    const float randomCullDist = 4.0e9f + static_cast<float>(actorPtr % 4001) * 1.0e6f;

    // 首次修改前保存原始值 + playerKey 用于恢复时验证
    if (m_netCullOriginals.find(actorPtr) == m_netCullOriginals.end()) {
        NetCullOriginal orig;
        if (m_off.Actor_NetCullDistSq >= 0)
            orig.netCullDistSq = safeReadFloat(actorPtr + m_off.Actor_NetCullDistSq);
        if (m_off.Char_CurrentNetCullDistSq >= 0)
            orig.currentNetCullDistSq = safeReadFloat(actorPtr + m_off.Char_CurrentNetCullDistSq);
        if (m_off.Char_PlayerKey >= 0)
            orig.playerKey = safeReadU32(actorPtr + m_off.Char_PlayerKey);
        m_netCullOriginals[actorPtr] = orig;
    }

    if (m_off.Actor_NetCullDistSq >= 0)
        writeMemFloat(actorPtr + m_off.Actor_NetCullDistSq, randomCullDist);
    if (m_off.Char_CurrentNetCullDistSq >= 0)
        writeMemFloat(actorPtr + m_off.Char_CurrentNetCullDistSq, randomCullDist);
}

// =====================================================================
//  restoreAllModifiedMemory — 恢复所有修改过的游戏内存值
//
//  调用时机:
//    1. 玩家手动点击菜单 "恢复游戏数据" 按钮
//    2. 对局结束 (离开对局) 时自动调用
//
//  恢复内容:
//    - 所有 Actor 的 NetCullDistanceSquared → 原始值
//    - 停止 aimbot 写入 ControlRotation
//    - 清除 aimbot 锁定
// =====================================================================
void MatchMonitor::restoreAllModifiedMemory() {
    if (m_memoryRestored.load(std::memory_order_acquire)) return;  // 幂等: 只恢复一次
    m_memoryRestored.store(true, std::memory_order_release);

    LOG(LOG_LEVEL_INFO, "[Restore] 开始恢复 %zu 个 Actor 的 NetCullDist 原始值",
        m_netCullOriginals.size());

    int restored = 0;
    for (const auto& pair : m_netCullOriginals) {
        const uintptr_t actorPtr = pair.first;
        const NetCullOriginal& orig = pair.second;
        if (actorPtr < 0x10000) continue;

        // 验证地址未被复用: 检查 playerKey 是否一致
        if (orig.playerKey != 0 && m_off.Char_PlayerKey >= 0) {
            uint32_t curKey = safeReadU32(actorPtr + m_off.Char_PlayerKey);
            if (curKey != orig.playerKey) {
                continue;  // 地址已被复用给不同对象, 跳过
            }
        }

        bool ok = true;
        if (m_off.Actor_NetCullDistSq >= 0) {
            float cur = safeReadFloat(actorPtr + m_off.Actor_NetCullDistSq);
            // 检查值是否仍是我们写入的 (合理偏高值, >3e9)
            if (std::isfinite(cur) && cur > 3.0e9f) {
                ok &= writeMemFloat(actorPtr + m_off.Actor_NetCullDistSq, orig.netCullDistSq);
            }
        }
        if (m_off.Char_CurrentNetCullDistSq >= 0) {
            float cur = safeReadFloat(actorPtr + m_off.Char_CurrentNetCullDistSq);
            if (std::isfinite(cur) && cur > 3.0e9f) {
                ok &= writeMemFloat(actorPtr + m_off.Char_CurrentNetCullDistSq, orig.currentNetCullDistSq);
            }
        }
        if (ok) restored++;
    }

    LOG(LOG_LEVEL_INFO, "[Restore] NetCullDist 恢复完成: %d/%zu", restored, m_netCullOriginals.size());

    // 停止自瞄
    m_aimbotLockedKey = 0;

    // 通知 GUI 层
    ue4draw::SharedUE4Data::getInstance().setMemoryRestored(true);
    ue4draw::SharedUE4Data::getInstance().setAimbotEnabled(false);

    writeAimbotLog("[Restore] 所有内存已恢复, aimbot 已停止");
    LOG(LOG_LEVEL_INFO, "[Restore] 内存恢复完成, aimbot 已禁用");
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

    int newCharsFound = 0;
    int actorArrays = 0;
    int actorItems = 0;
    int charCandidates = 0;
    int skipSelf = 0;
    int skipExisting = 0;
    int skipHealth = 0;
    int skipPosition = 0;

    // 本地 Pawn (AcknowledgedPawn) 以便排除自己
    uintptr_t myPawn = 0;
    if (m_off.PC_AcknowledgedPawn >= 0) {
        uintptr_t pc = getLocalPlayerController();
        if (pc >= 0x10000) myPawn = safeReadPtr(pc + m_off.PC_AcknowledgedPawn);
    }

    int processedItems = 0;
    BatchMemReader charBatch;
    bool epochStartedThisCall = false;

    auto startCharacterScanEpoch = [&]() {
        ++m_characterScanEpoch;
        if (m_characterScanEpoch == 0) {
            m_characterScanEpoch = 1;
        }
        epochStartedThisCall = true;
    };

    auto acceptActor = [&](uintptr_t objPtr, const char* sourceName) -> bool {
        if (objPtr == 0 || objPtr < 0x10000) return false;
        if (myPawn != 0 && objPtr == myPawn) {
            skipSelf++;
            return false;
        }

        uintptr_t classPtr = safeReadPtr(objPtr + kUObjectClassPrivateOffset);
        if (classPtr == 0) return false;

        auto classIt = m_characterClassSet.find(classPtr);
        if (classIt != m_characterClassSet.end()) {
            if (!classIt->second) return false;
        } else {
            bool isChar = isSubclassOf(classPtr, "Character") || isSubclassOf(classPtr, "Pawn");
            // 排除载具 (STExtraVehicleBase 也继承自 Pawn) 与无人机, 否则自行车/摩托等会被
            // 误识别成训练人偶并画出 ESP 方框
            if (isChar && (isSubclassOf(classPtr, "STExtraVehicleBase")
                        || isSubclassOf(classPtr, "VehicleBase")
                        || isSubclassOf(classPtr, "AirDropBoxActor")
                        || isSubclassOf(classPtr, "PlayerTombBox"))) {
                isChar = false;
            }
            m_characterClassSet[classPtr] = isChar;
            if (!isChar) return false;
        }

        charCandidates++;
        charBatch.read(objPtr, m_charReadSize);

        uint32_t playerKey = (m_off.Char_PlayerKey >= 0) ? charBatch.getU32(m_off.Char_PlayerKey) : 0;
        if (playerKey == 0) {
            playerKey = 0x80000000u | static_cast<uint32_t>(objPtr & 0x7FFFFFFFu);
        }

        PlayerNode* existing = m_playerList.findByKey(playerKey);
        if (existing != nullptr && existing->source == PlayerSource::PlayerArray) {
            skipExisting++;
            return false;
        }

        FVector3 loc{};
        if (!getActorLocation(objPtr, loc) || !hasUsablePlayerPosition(loc)) {
            skipPosition++;
            return false;
        }

        const std::string className = readClassName(objPtr);
        const bool looksLikePlayerPawn = className.find("Player") != std::string::npos
            || className.find("Pawn") != std::string::npos
            || className.find("Character") != std::string::npos
            || className.find("Train") != std::string::npos;

        int32_t teamID = (m_off.Char_TeamID >= 0) ? charBatch.getS32(m_off.Char_TeamID) : -1;
        float health = (m_off.Char_Health >= 0) ? charBatch.getFloat(m_off.Char_Health) : 0.0f;
        float healthMax = (m_off.Char_HealthMax >= 0) ? charBatch.getFloat(m_off.Char_HealthMax) : 0.0f;
        bool bDead = (m_off.Char_bDead >= 0) ? ((charBatch.getU8(m_off.Char_bDead) & 1) != 0) : false;

        if (!std::isfinite(health) || health < 0.0f) health = 0.0f;
        if (!std::isfinite(healthMax) || healthMax <= 0.0f) {
            if (!looksLikePlayerPawn) {
                skipHealth++;
                return false;
            }
            health = health > 0.0f ? health : 100.0f;
            healthMax = 100.0f;
        }
        if (health <= 0.0f && looksLikePlayerPawn) {
            health = healthMax;
        }

        std::string playerName;
        if (m_off.Char_PlayerName >= 0) {
            playerName = readFString(objPtr + m_off.Char_PlayerName);
        }
        auto readPlayerStateName = [&](uintptr_t psPtr) -> std::string {
            if (psPtr < 0x10000 || m_off.PS_PlayerName < 0) return "";
            std::string name = readFString(psPtr + m_off.PS_PlayerName);
            if (name.empty() || name.size() > 64) return "";
            return name;
        };
        if ((playerName.empty() || playerName.size() > 64) && m_off.Pawn_PlayerState >= 0) {
            playerName = readPlayerStateName(safeReadPtr(objPtr + m_off.Pawn_PlayerState));
        }
        if ((playerName.empty() || playerName.size() > 64) && m_off.STBase_STExtraPlayerState >= 0) {
            playerName = readPlayerStateName(safeReadPtr(objPtr + m_off.STBase_STExtraPlayerState));
        }
        // 名称仍未解析: 不再直接把内部 className (如 "BP_FPS_Base_C")
        // 当玩家名展示, 改为根据 className 推断身份.
        bool inferredAI = false;
        if (playerName.empty() || playerName.size() > 64) {
            playerName.clear();
            const bool isTrainingDummy = !className.empty()
                && (className.find("Train") != std::string::npos
                    || className.find("FPS_Base") != std::string::npos
                    || className.find("Dummy") != std::string::npos
                    || className.find("Bot") != std::string::npos
                    || className.find("AI") != std::string::npos);
            if (isTrainingDummy) {
                playerName = "训练人偶";
                inferredAI = true;
            }
            // 其余 (className 缺失或不像 AI) 留空, 让 UI 显示 "Enemy" 而不是脏字符串.
            (void)sourceName;
        }

        patchActorNetCull(objPtr);

        PlayerNode data;
        data.teamID = (teamID > 0) ? teamID : -1;
        data.playerName = playerName;
        data.isAI = inferredAI;
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
        return true;
    };

    auto scanActorPointerArray = [&](uintptr_t arrayData, int32_t arrayNum, const char* sourceName) {
        if (arrayData < 0x10000 || arrayNum <= 0 || arrayNum > 30000) return;
        if (!epochStartedThisCall) {
            startCharacterScanEpoch();
        }
        actorArrays++;
        const int32_t limit = std::min<int32_t>(arrayNum, scanBudget);
        for (int32_t i = 0; i < limit; ++i) {
            uintptr_t actorPtr = safeReadPtr(arrayData + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
            actorItems++;
            acceptActor(actorPtr, sourceName);
        }
    };

    auto scanTArray = [&](uintptr_t base, int32_t offset, const char* sourceName) {
        if (base < 0x10000 || offset < 0) return;
        uintptr_t arrayData = safeReadPtr(base + offset);
        int32_t arrayNum = safeReadS32(base + offset + 8);
        scanActorPointerArray(arrayData, arrayNum, sourceName);
    };

    uintptr_t worldPtr = safeReadPtr(m_gWorld);
    uintptr_t activeActorsDataForLog = 0;
    int32_t activeActorsNumForLog = 0;
    uintptr_t persistentLevelForLog = 0;
    uintptr_t actorClusterForLog = 0;
    uintptr_t levelsDataForLog = 0;
    int32_t levelsNumForLog = 0;
    if (worldPtr >= 0x10000) {
        if (m_off.World_ActiveLevelActors >= 0) {
            activeActorsDataForLog = safeReadPtr(worldPtr + m_off.World_ActiveLevelActors);
            activeActorsNumForLog = safeReadS32(worldPtr + m_off.World_ActiveLevelActors + 8);
        }
        scanTArray(worldPtr, m_off.World_ActiveLevelActors, "World.ActiveLevelActors");

        uintptr_t persistentLevel = (m_off.World_PersistentLevel >= 0)
            ? safeReadPtr(worldPtr + m_off.World_PersistentLevel)
            : 0;
        persistentLevelForLog = persistentLevel;
        if (persistentLevel >= 0x10000 && m_off.Level_ActorCluster >= 0 && m_off.LevelActorContainer_Actors >= 0) {
            uintptr_t actorCluster = safeReadPtr(persistentLevel + m_off.Level_ActorCluster);
            actorClusterForLog = actorCluster;
            scanTArray(actorCluster, m_off.LevelActorContainer_Actors, "PersistentLevel.ActorCluster.Actors");
        }

        if (m_off.World_Levels >= 0 && m_off.Level_ActorCluster >= 0 && m_off.LevelActorContainer_Actors >= 0) {
            uintptr_t levelsData = safeReadPtr(worldPtr + m_off.World_Levels);
            int32_t levelsNum = safeReadS32(worldPtr + m_off.World_Levels + 8);
            levelsDataForLog = levelsData;
            levelsNumForLog = levelsNum;
            if (levelsData >= 0x10000 && levelsNum > 0 && levelsNum <= 256) {
                for (int32_t i = 0; i < levelsNum; ++i) {
                    uintptr_t levelPtr = safeReadPtr(levelsData + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
                    if (levelPtr < 0x10000) continue;
                    uintptr_t actorCluster = safeReadPtr(levelPtr + m_off.Level_ActorCluster);
                    scanTArray(actorCluster, m_off.LevelActorContainer_Actors, "World.Levels.ActorCluster.Actors");
                }
            }
        }
    }

    if (actorArrays > 0) {
        static Clock::time_point s_lastWorldActorScanLog;
        if (shouldLogEvery(s_lastWorldActorScanLog, std::chrono::milliseconds(1000))) {
            LOG(LOG_LEVEL_INFO,
                "[WorldActorScanProbe] arrays=%d actors=%d chars=%d added=%d skipSelf=%d skipExisting=%d skipHealth=%d skipPos=%d epoch=%u",
                actorArrays, actorItems, charCandidates, newCharsFound, skipSelf, skipExisting, skipHealth, skipPosition, m_characterScanEpoch);
        }
        m_lastCompletedCharacterScanEpoch = m_characterScanEpoch;
        return newCharsFound;
    }

    static Clock::time_point s_lastWorldActorScanMissLog;
    if (shouldLogEvery(s_lastWorldActorScanMissLog, std::chrono::milliseconds(1000))) {
        LOG(LOG_LEVEL_INFO,
            "[WorldActorScanMiss] world=%p activeData=%p activeNum=%d persistent=%p actorCluster=%p levelsData=%p levelsNum=%d epoch=%u",
            (void*)worldPtr, (void*)activeActorsDataForLog, activeActorsNumForLog,
            (void*)persistentLevelForLog, (void*)actorClusterForLog,
            (void*)levelsDataForLog, levelsNumForLog, m_characterScanEpoch);
    }

    // 通过 safeRead 访问 GUObjectArray, 避免游戏重分配时裸解引用崩溃; World Actor 数组不可用时才兜底。
    uintptr_t objArrayAddr = m_gUObjectArray;
    UObjectArrayLayout layout{};
    if (!detectUObjectArrayLayout(objArrayAddr, layout)) return 0;
    const int totalNum = layout.totalNum;

    if (m_characterScanItemIndex < 0 || m_characterScanItemIndex >= totalNum) {
        m_characterScanItemIndex = 0;
    }
    if (m_characterScanItemIndex == 0 || m_characterScanEpoch == 0) {
        startCharacterScanEpoch();
    }

    while (processedItems < scanBudget && m_characterScanItemIndex < totalNum) {
        uintptr_t objPtr = readUObjectAt(objArrayAddr, layout, m_characterScanItemIndex);
        processedItems++;
        m_characterScanItemIndex++;
        acceptActor(objPtr, "GUObjectArray");

        if (m_characterScanItemIndex >= totalNum) break;
    }

    if (m_characterScanItemIndex >= totalNum) {
        m_characterScanItemIndex = 0;
        m_lastCompletedCharacterScanEpoch = m_characterScanEpoch;
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

    }

    // 通过本地 PlayerController 锁定自己 (训练场 PlayerArray[0] 是占位, 不可信)
    if (m_off.PC_PlayerState >= 0 || m_off.PC_AcknowledgedPawn >= 0) {
        uintptr_t pc = getLocalPlayerController();
        if (pc >= 0x10000) {
            uintptr_t myPS = (m_off.PC_PlayerState >= 0) ? safeReadPtr(pc + m_off.PC_PlayerState) : 0;
            if (myPS >= 0x10000 && m_off.PS_PlayerKey >= 0) {
                uint32_t key = safeReadU32(myPS + m_off.PS_PlayerKey);
                if (key != 0) {
                    m_myPlayerKey = key;
                    if (m_off.PS_TeamID >= 0) {
                        int32_t tid = safeReadS32(myPS + m_off.PS_TeamID);
                        if (tid > 0) m_myTeamID = tid;
                    }
                }
            }
            // 始终缓存 AcknowledgedPawn —— 即便已通过 PlayerState 拿到 key,
            // 也用 pawn 指针做"绘制时排除自己"的兜底 (m_myPlayerKey 偶发=0 时仍能过滤)
            if (m_off.PC_AcknowledgedPawn >= 0) {
                uintptr_t myPawn = safeReadPtr(pc + m_off.PC_AcknowledgedPawn);
                if (myPawn >= 0x10000) {
                    m_myPawn = myPawn;
                    if (m_myPlayerKey == 0 && m_off.Char_PlayerKey >= 0) {
                        uint32_t key = safeReadU32(myPawn + m_off.Char_PlayerKey);
                        if (key != 0) {
                            m_myPlayerKey = key;
                            if (m_off.Char_TeamID >= 0) {
                                int32_t tid = safeReadS32(myPawn + m_off.Char_TeamID);
                                if (tid > 0) m_myTeamID = tid;
                            }
                        }
                    }
                }
            }
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

// =====================================================================
//  载具/物资名称映射 (基于 ESTExtraVehicleType 枚举 + EItemType 经验值)
// =====================================================================
static std::string lookupVehicleName(uint8_t vt) {
    // 索引来自 dump.cs ESTExtraVehicleType 枚举 (155=Deer, 156=Max), 见游戏 SDK
    static const char* kNames[] = {
        /*  0*/ "未知载具",
        /*  1*/ "摩托",        /*  2*/ "摩托",        /*  3*/ "三轮摩托",  /*  4*/ "三轮摩托",
        /*  5*/ "达契亚",      /*  6*/ "达契亚",      /*  7*/ "达契亚",    /*  8*/ "达契亚",
        /*  9*/ "吉普 UAZ",    /* 10*/ "吉普 UAZ",    /* 11*/ "吉普 UAZ",
        /* 12*/ "蹦蹦",        /* 13*/ "蹦蹦",        /* 14*/ "蹦蹦",
        /* 15*/ "汽艇 PG117",  /* 16*/ "水上摩托",
        /* 17*/ "面包车",      /* 18*/ "面包车",      /* 19*/ "面包车",
        /* 20*/ "皮卡",        /* 21*/ "皮卡",        /* 22*/ "皮卡",      /* 23*/ "皮卡",
        /* 24*/ "皮卡",        /* 25*/ "皮卡",        /* 26*/ "皮卡",      /* 27*/ "皮卡",
        /* 28*/ "皮卡",        /* 29*/ "皮卡",
        /* 30*/ "蹦蹦",        /* 31*/ "蹦蹦",        /* 32*/ "蹦蹦",
        /* 33*/ "Mirado",      /* 34*/ "Mirado",      /* 35*/ "Mirado",    /* 36*/ "Mirado",
        /* 37*/ "Mirado开",    /* 38*/ "Mirado开",    /* 39*/ "Mirado开",  /* 40*/ "Mirado开",
        /* 41*/ "UAZ04",
        /* 42*/ "Rony",        /* 43*/ "Rony",        /* 44*/ "Rony",
        /* 45*/ "电动滑板",    /* 46*/ "冲浪板",
        /* 47*/ "直升机 UH60",
        /* 48*/ "两栖车",
        /* 49*/ "TUK",
        /* 50*/ "雪板",
        /* 51*/ "无人机 UAV",
        /* 52*/ "电话亭车",
        /* 53*/ "无人战车",
        /* 54*/ "坦克",
        /* 55*/ "龙舟",
        /* 56*/ "特斯拉",
        /* 57*/ "坐骑",
        /* 58*/ "滑翔翼",
        /* 59*/ "战舰",
        /* 60*/ "运输车",
        /* 61*/ "Coupe RB",
        /* 62*/ "情人节气球",
        /* 63*/ "ATG摩托",
        /* 64*/ "激光帆船",
        /* 65*/ "空中出租",
        /* 66*/ "皮划艇",
        /* 67*/ "黑猫摩托",
        /* 68*/ "鸡蛋坦克",
        /* 69*/ "新年雪板",
        /* 70*/ "重力雪板",
        /* 71*/ "马",
        /* 72*/ "舞狮",
        /* 73*/ "龙车",
        /* 74*/ "叉车",
        /* 75*/ "科幻摩托",
        /* 76*/ "自行车",
        /* 77*/ "旅行车",
        /* 78*/ "FRC赛车",
        /* 79*/ "ATG赛车",
        /* 80*/ "风筝飞行器",
        /* 81*/ "漂移车",
        /* 82*/ "拼接列车",
        /* 83*/ "手推车",
        /* 84*/ "ATV",
        /* 85*/ "四座跑车",
        /* 86*/ "磁悬浮飞行器",
        /* 87*/ "战车",
        /* 88*/ "灯笼竞速",
        /* 89*/ "六翼",
        /* 90*/ "大脚车",
        /* 91*/ "拉达",
        /* 92*/ "雪地摩托",
        /* 93*/ "雪地摩托",
        /* 94*/ "武装达契亚",
        /* 95*/ "武装蹦蹦",
        /* 96*/ "武装皮卡",
        /* 97*/ "武装吉普",
        /* 98*/ "武装直升机",
        /* 99*/ "武装边三轮",
        /*100*/ "履带车",
        /*101*/ "FPV无人机",
        /*102*/ "炸弹无人机",
        /*103*/ "AH6",
        /*104*/ "UH60",
        /*105*/ "BF吉普",
        /*106*/ "皮卡艇",
        /*107*/ "进攻坦克",
        /*108*/ "防御坦克",
        /*109*/ "Mi28",
        /*110*/ "侦察无人机",
        /*111*/ "BF ATV",
        /*112*/ "BRDM",
        /*113*/ "月饼船",
        /*114*/ "月饼气球",
        /*115*/ "沙漠车",
        /*116*/ "气垫",
        /*117*/ "雪豹",
        /*118*/ "猛犸象",
        /*119*/ "基地车",
        /*120*/ "两栖坦克",
        /*121*/ "迷失移动",
        /*122*/ "突击车",
        /*123*/ "导弹",
        /*124*/ "挖掘机",
        /*125*/ "风筝",
        /*126*/ "门机",
        /*127*/ "巨口喷气",
        /*128*/ "Blanc",
        /*129*/ "Pico巴士",
        /*130*/ "AI车",
        /*131*/ "翻斗车",
        /*132*/ "BFV突击车",
        /*133*/ "BFV防空坦克",
        /*134*/ "RD挖掘机",
        /*135*/ "PELAI车",
        /*136*/ "锤头鲨",
        /*137*/ "哪吒莲花",
        /*138*/ "超级模式UAV",
        /*139*/ "雪橇",
        /*140*/ "气垫船",
        /*141*/ "冰马车",
        /*142*/ "BB车",
        /*143*/ "海上飞机",
        /*144*/ "雪球",
        /*145*/ "飞马",
        /*146*/ "EV3F4",
        /*147*/ "黑鹰运输",
        /*148*/ "滑翔伞",
        /*149*/ "沙漠艇",
        /*150*/ "骆驼车队",
        /*151*/ "冰马车",
        /*152*/ "企鹅",
        /*153*/ "九色鹿",
        /*154*/ "飞马",
        /*155*/ "六船",
        /*156*/ "鹿",
    };
    constexpr int kCount = sizeof(kNames) / sizeof(kNames[0]);
    if (vt < kCount && kNames[vt]) return kNames[vt];
    char buf[24]; snprintf(buf, sizeof(buf), "载具#%u", vt);
    return buf;
}

// =====================================================================
//  物资真实名称 — 调用 libUE4.so 内部配置表查名函数
//
//  IDA 验证 (ida-pro-mcp-2 / dump.cs):
//    ItemUtilsV2::GetItemNameV2 (BlueprintCallable thunk, NumParms=24)
//      -> sub_A3B7AC8(itemID)
//           -> sub_981FDB4(itemID, 0) + 0x8     (返回配置表行内 FString*)
//
//  我们直接走 sub_A3B7AC8 这条内部路径, 避开 BP thunk 的 FFrame 序列化:
//    - 标准 AAPCS 函数, 单参 int32 itemID, 返回值是 FString* (借引用,
//      指向引擎内部物品配置表行 +0x8 处, 不要 free).
//    - FString 布局 {void* Data; int32 Num; int32 Max}, Data 为 UTF-16LE.
//
//  反作弊对抗: 在 stack_spoof::call_spoofed 的伪造 FP chain 下发起调用,
//    周期栈采样看到的 LR 链落在 libc.so / libart.so 等白名单模块.
//    外层再套 sigsetjmp 熔断 (复用 safeReadMemory 装的信号屏障), 命中
//    SIGSEGV/SIGBUS 时把 itemID 拉黑, 单点失败不会拖垮整个扫描循环.
//
//  缓存策略: 配置表常量, itemID -> name 永久缓存 (空串表示已知失败).
// =====================================================================
//
// =====================================================================
//  ★ 游戏更新后如何重新定位 sub_A3B7AC8 / sub_981FDB4 / sub_98322E0 / unk_150ED768 ★
// =====================================================================
//
//  正常情况下完全不需要手动定位 — resolveItemNameFnAddress() 会运行时解析,
//  这里记录的是当 IDA 验证 / 兜底常量更新 / 链路被腾讯改动 时的复现步骤.
//
//  ─── 工具 ───────────────────────────────────────────────────────────
//   * IDA Pro (本仓库自带 mcp_ida-pro-mcp-2 桥接, 端口 13338)
//   * UE4Dumper 输出的 dump.cs (m_interface 的反射数据源)
//   * adb logcat 抓 [ItemNameNative] 日志
//
//  ─── Step 1: 找 thunk (ItemUtilsV2::execGetItemNameV2) ───────────────
//   1) 在 dump.cs 搜 `ItemUtilsV2` Class, 记下 GetItemNameV2 UFunction.
//      签名: static FString GetItemNameV2(int32 ItemID).
//   2) UFunction 对象 +0xB0 = Func, 即 thunk 入口 RVA.
//      也可在 IDA 字符串窗搜 "GetItemNameV2", 反查 xref 找到对应
//      FNativeFunctionRegistrar::RegisterFunction(...) 调用, 取第 2 个参数 (函数指针).
//
//  ─── Step 2: thunk 内识别真正的业务函数 sub_A3B7AC8 ─────────────────
//   BP Native thunk 的标准模板:
//     1. 拆 FFrame: 一系列 BL Stack.StepCompiledIn / FFrame::Step 等 helper
//        (用来从字节码栈取 ItemID 参数, 通常 1~3 个 BL).
//     2. BL <真正业务函数>          ← 我们要的 sub_A3B7AC8
//     3. 拷返回值 FString 到 Result.
//
//   ★ 区分手段 (本文件 resolveItemNameFnAddress 即采用第 3 种,自动化) ★
//     a) Stack helper 体积小, 第一条多为 ldr/cbz, 不会再做后续大调用链.
//     b) 真业务函数体积大, 通常以 stp x29,x30,[sp,#-...]! 起手, 内部多次 BL +
//        ADRP+LDR 访问全局表.
//     c) ★ 链路指纹 ★ — 真函数命中 "sub_A3B7AC8 → 1stBL → 1stBL → ADRP+ADD"
//        最终能解出一个落在 libUE4.so .bss 段的全局指针 (即 unk_150ED768).
//        helper 不可能产生这种链路, 因此可作判别条件.
//
//   人工验证: IDA 打开 thunk, F5 看伪代码. 真函数调用形如
//     `v3 = sub_A3B7AC8(itemID); *Result = *v3; ...; v3->Num=0;`
//
//  ─── Step 3: 沿 sub_A3B7AC8 找配置表缓存槽 unk_150ED768 ─────────────
//   sub_A3B7AC8 (~4 条指令的小桥) 第 1 条 BL → sub_981FDB4 (查表入口).
//   sub_981FDB4 内部第 1 条 BL → sub_98322E0 (LazyInit + 缓存 UClass*).
//   sub_98322E0 头部第 1 对 ADRP+ADD pair = unk_150ED768 (UClass* 缓存槽).
//   该槽内容 = 0 表示游戏自己还没碰过配置表; != 0 表示已经 FindObject 过.
//
//   验证 unk_150ED768 是否定位正确:
//     1) 在 IDA 看 sub_98322E0, 紧跟 ADRP+ADD 的代码必形如
//        `if (!*(_QWORD *)unk_150ED768) *(_QWORD *)unk_150ED768 = sub_AED16BC(...,"/Script/...");`
//     2) 跑游戏让其先自然加载一次 (开局之后), adb shell dd 直接读该地址应当
//        是一个落在 libUE4.so 范围内的非零指针 (UClass*).
//
//  ─── Step 4: 更新本文件的兜底常量 ───────────────────────────────────
//   两个 RVA 都是 "万一动态链路被改坏" 时使用的兜底硬编码:
//     kItemNameLookupInternalOffsetFallback = sub_A3B7AC8 RVA
//     kItemConfigUClassCacheRvaFallback     = unk_150ED768 RVA
//   日志会打印动态解析结果与兜底是否一致 ("一致" / "已漂移"). "已漂移" =
//   游戏更了, 此时把日志里的 RVA 抄进这两个常量即可保留兜底安全网.
//
//  ─── Step 5: 链路结构变了怎么办 ─────────────────────────────────────
//   如果腾讯把 sub_A3B7AC8/sub_981FDB4/sub_98322E0 之间的层级合并或拆分了,
//   resolveItemNameFnAddress 里的 validateChain lambda 会自动失败 (没有
//   候选能跑通链), 直接走兜底. 修复方法:
//     * 在 IDA 重新跟一次 thunk → ... → 缓存槽 的指令路径;
//     * 调整 validateChain 里 findFirstBLTarget 的层数 / 跳过的 BL 数量;
//     * 必要时改 findAdrpAddSlot 接受 ADRP+LDR (而非 ADRP+ADD) 模式 —
//       某些版本 LazyInit 直接把指针装载, 而非取 lea 地址.
//
//   只要 kItemConfigUClassCacheRvaFallback 仍然是真实槽, 即便链路解析失败,
//   tryGetItemNameNative 的门控仍能保护工作线程不撞 SIGSEGV (代价是无法享
//   受 ASLR 后偏移漂移自动跟随).
// =====================================================================

// libUE4.so 内 sub_A3B7AC8 的 RVA. ASLR 下运行时地址 = m_moduleBase + 该 RVA.
// 仅作动态解析失败时的兜底常量, 正常路径走 resolveItemNameFnAddress() 反射 + BL 解码.
// 更新方法见上方 "★ 游戏更新后如何重新定位 ★" 注释块 Step 1-4.
static constexpr uintptr_t kItemNameLookupInternalOffsetFallback = 0xA3B7AC8;

// 物资配置表 UClass 缓存指针的 RVA (libUE4.so 内 unk_150ED768) — 兜底常量,
// 正常路径走 resolveItemConfigCacheSlot() 沿调用链动态推算 (sub_A3B7AC8 →
// sub_981FDB4 → sub_98322E0 → ADRP+ADD), 与 dump.cs 版本完全解耦.
// 为何要门控该槽: sub_98322E0 在槽为零时会调 sub_AED16BC(...,"/",...) 即
// UE FindObject — 从我们的工作线程发起 UE 反射加载会 SIGSEGV.
// 更新方法见上方 "★ 游戏更新后如何重新定位 ★" 注释块 Step 3-4.
static constexpr uintptr_t kItemConfigUClassCacheRvaFallback = 0x150ED768;

namespace {

// stack_spoof::call_spoofed 只接受一参 (void*(*)(void*)),
// 用上下文结构桥接 sub_A3B7AC8 的 (int32) -> FString* 调用约定.
struct ItemNameNativeSpoofCtx {
    void* (*fn)(int32_t);  // sub_A3B7AC8 的真实函数指针 (m_moduleBase + RVA)
    int32_t itemID;        // 输入: ItemDefineID.TypeSpecificID
    void*   fstrPtr;       // 输出: 借引用的 FString* (指向配置表内, 勿释放)
};

// 在伪造 FP chain 下执行的小型蹦床, SP 不切换, 入参/返回值均落在真栈上.
void* itemNameNativeTrampoline(void* p) {
    auto* c = static_cast<ItemNameNativeSpoofCtx*>(p);
    c->fstrPtr = c->fn(c->itemID);
    return c->fstrPtr;
}

} // namespace

// ===== 物资名查表函数 / 配置表缓存槽 动态解析 =====
//
// 设计:
//   thunk(ItemUtilsV2::GetItemNameV2) 内部至少有 2 条 BL: 第 1 条通常是
//   Stack.StepCompiledIn 之类的参数解包 helper, 真正的 sub_A3B7AC8 在后面
//   (历史 IDA 验证为 thunk+0x68, 即 26 号 insn). 因此 "扫第一条 BL" 的旧
//   策略会误命中 helper, 调用即 SIGSEGV.
//
//   新策略 = scan + validate.
//   候选 = thunk 前 64 条指令里所有 BL 目标.
//   验证 = 该候选必须能跑通整条链:
//     候选 → 第 1 条 BL = sub_981FDB4
//          → 第 1 条 BL = sub_98322E0
//          → 第 1 对 ADRP+ADD = unk_150ED768 (slot)
//   且 slot 必须落在 m_moduleBase 加载范围内. 业务函数链路在该 thunk 内独
//   一无二, 几乎不可能与 Step/FrameNext 等 helper 冲突.
//
//   两个 resolver 共享 (fn, slot) 一次性缓存, 避免重复扫描.

namespace {
struct ItemNameResolved {
    uintptr_t fn   = 0;
    uintptr_t slot = 0;
};
constexpr uintptr_t kItemNameResolveSentinel = static_cast<uintptr_t>(-1);
} // namespace

uintptr_t MatchMonitor::resolveItemNameFnAddress() {
    static std::atomic<uintptr_t> s_addr{0};
    static std::atomic<uintptr_t> s_slot{0};
    constexpr uintptr_t kSentinelFailed = kItemNameResolveSentinel;

    uintptr_t cached = s_addr.load(std::memory_order_acquire);
    if (cached == kSentinelFailed) return 0;
    if (cached != 0) {
        // 让 resolveItemConfigCacheSlot 也能取到 slot
        m_itemConfigSlotCache.store(s_slot.load(std::memory_order_acquire),
                                    std::memory_order_release);
        return cached;
    }

    auto store = [&](uintptr_t fn, uintptr_t slot) -> uintptr_t {
        s_slot.store(slot, std::memory_order_release);
        s_addr.store(fn == 0 ? kSentinelFailed : fn, std::memory_order_release);
        m_itemConfigSlotCache.store(slot, std::memory_order_release);
        return fn;
    };
    auto fallback = [&]() -> uintptr_t {
        if (m_moduleBase == 0) return store(0, 0);
        const uintptr_t fn   = m_moduleBase + kItemNameLookupInternalOffsetFallback;
        const uintptr_t slot = m_moduleBase + kItemConfigUClassCacheRvaFallback;
        LOG(LOG_LEVEL_WARN,
            "[ItemNameNative] 反射/链式校验全部失败, 回退硬编码 fn RVA=0x%lX slot RVA=0x%lX",
            (unsigned long)kItemNameLookupInternalOffsetFallback,
            (unsigned long)kItemConfigUClassCacheRvaFallback);
        return store(fn, slot);
    };

    if (m_moduleBase == 0 || m_moduleSize == 0) return fallback();
    const uintptr_t modBegin = m_moduleBase;
    const uintptr_t modEnd   = m_moduleBase + m_moduleSize;
    auto inMod = [&](uintptr_t p) { return p >= modBegin && p < modEnd; };

    auto decodeBL = [](uintptr_t pc, uint32_t insn, uintptr_t& outTarget) -> bool {
        if ((insn & 0xFC000000u) != 0x94000000u) return false;
        int32_t imm26 = static_cast<int32_t>(insn & 0x03FFFFFFu);
        if (imm26 & 0x02000000) imm26 |= 0xFC000000;
        outTarget = pc + static_cast<uintptr_t>(static_cast<intptr_t>(imm26) * 4);
        return true;
    };
    auto findFirstBLTarget = [&](uintptr_t fnAddr, int maxScan,
                                 uintptr_t& outTarget) -> bool {
        std::vector<uint32_t> insns(maxScan, 0);
        if (!safeReadMemory(fnAddr, insns.data(), insns.size() * sizeof(uint32_t))) return false;
        for (int i = 0; i < maxScan; ++i) {
            uintptr_t t = 0;
            if (decodeBL(fnAddr + static_cast<uintptr_t>(i) * 4, insns[i], t)) {
                outTarget = t; return true;
            }
        }
        return false;
    };
    auto findAdrpAddSlot = [&](uintptr_t fnAddr, int maxScan,
                               uintptr_t& outSlot) -> bool {
        std::vector<uint32_t> insns(maxScan, 0);
        if (!safeReadMemory(fnAddr, insns.data(), insns.size() * sizeof(uint32_t))) return false;
        for (int i = 0; i + 1 < maxScan; ++i) {
            const uint32_t adrp = insns[i];
            if ((adrp & 0x9F000000u) != 0x90000000u) continue;
            const uint32_t add = insns[i + 1];
            if ((add & 0xFF800000u) != 0x91000000u) continue;
            const uint32_t adrpRd = adrp & 0x1Fu;
            const uint32_t addRn  = (add >> 5) & 0x1Fu;
            const uint32_t addRd  = add & 0x1Fu;
            if (adrpRd != addRn || adrpRd != addRd) continue;
            int32_t immlo = (adrp >> 29) & 0x3;
            int32_t immhi = (adrp >> 5) & 0x7FFFF;
            int32_t imm21 = (immhi << 2) | immlo;
            if (imm21 & 0x100000) imm21 |= 0xFFE00000;
            const uintptr_t pc = fnAddr + static_cast<uintptr_t>(i) * 4u;
            const uintptr_t adrpTarget = (pc & ~uintptr_t{0xFFF})
                + (static_cast<uintptr_t>(static_cast<intptr_t>(imm21)) << 12);
            uint32_t imm12 = (add >> 10) & 0xFFF;
            const uint32_t sh = (add >> 22) & 0x1;
            const uintptr_t addImm = static_cast<uintptr_t>(imm12) << (sh ? 12 : 0);
            outSlot = adrpTarget + addImm;
            return true;
        }
        return false;
    };
    // 链式校验: candidate 看起来像 sub_A3B7AC8 当且仅当
    // candidate→1stBL→1stBL 内能解出 ADRP+ADD slot 且 slot ∈ module.
    auto validateChain = [&](uintptr_t candidate, uintptr_t& outSlot) -> bool {
        if (!inMod(candidate)) return false;
        uintptr_t fn981 = 0;
        if (!findFirstBLTarget(candidate, 8, fn981) || !inMod(fn981)) return false;
        uintptr_t fn983 = 0;
        if (!findFirstBLTarget(fn981, 16, fn983) || !inMod(fn983)) return false;
        uintptr_t slot = 0;
        if (!findAdrpAddSlot(fn983, 32, slot)) return false;
        if (!inMod(slot)) return false;
        outSlot = slot;
        return true;
    };

    // ---- Step 1: 反射拿 ItemUtilsV2::GetItemNameV2 thunk ----
    const uintptr_t thunkAddr = m_interface.getFuncAddress("ItemUtilsV2", "GetItemNameV2");
    if (!inMod(thunkAddr)) {
        LOG(LOG_LEVEL_WARN,
            "[ItemNameNative] ItemUtilsV2::GetItemNameV2 反射地址越界: %p (module [%p, %p))",
            (void*)thunkAddr, (void*)m_moduleBase, (void*)(m_moduleBase + m_moduleSize));
        return fallback();
    }

    // ---- Step 2: 扫 thunk 前 64 条指令所有 BL, 逐个用链路验证 ----
    constexpr int kMaxScanInsns = 64;
    uint32_t insnBuf[kMaxScanInsns] = {};
    if (!safeReadMemory(thunkAddr, insnBuf, sizeof(insnBuf))) {
        LOG(LOG_LEVEL_WARN, "[ItemNameNative] thunk 字节读取失败 @ %p", (void*)thunkAddr);
        return fallback();
    }
    int candidatesScanned = 0;
    for (int i = 0; i < kMaxScanInsns; ++i) {
        uintptr_t cand = 0;
        if (!decodeBL(thunkAddr + static_cast<uintptr_t>(i) * 4u, insnBuf[i], cand)) continue;
        ++candidatesScanned;
        uintptr_t slot = 0;
        if (!validateChain(cand, slot)) continue;

        const uintptr_t fnRva = cand - m_moduleBase;
        const uintptr_t slotRva = slot - m_moduleBase;
        LOG(LOG_LEVEL_INFO,
            "[ItemNameNative] 动态解析成功 (扫描 %d 个 BL 候选): "
            "thunk=%p[+0x%X] -> sub_A3B7AC8=%p (RVA=0x%lX, 兜底 0x%lX, %s); "
            "unk_slot=%p (RVA=0x%lX, 兜底 0x%lX, %s)",
            candidatesScanned, (void*)thunkAddr, i * 4,
            (void*)cand, (unsigned long)fnRva,
            (unsigned long)kItemNameLookupInternalOffsetFallback,
            fnRva == kItemNameLookupInternalOffsetFallback ? "一致" : "已漂移",
            (void*)slot, (unsigned long)slotRva,
            (unsigned long)kItemConfigUClassCacheRvaFallback,
            slotRva == kItemConfigUClassCacheRvaFallback ? "一致" : "已漂移");
        return store(cand, slot);
    }

    LOG(LOG_LEVEL_WARN,
        "[ItemNameNative] thunk @ %p 内 %d 个 BL 候选均无法通过链路校验, 回退",
        (void*)thunkAddr, candidatesScanned);
    return fallback();
}

uintptr_t MatchMonitor::resolveItemConfigCacheSlot() {
    // 触发 (或读取已缓存) fn 解析, 同步把 slot 写入 m_itemConfigSlotCache.
    if (resolveItemNameFnAddress() == 0) return 0;
    return m_itemConfigSlotCache.load(std::memory_order_acquire);
}

bool MatchMonitor::tryGetItemNameNative(int32_t itemID, std::string& out) {
    if (itemID <= 0 || m_moduleBase == 0) return false;

    // 门控 1: 物资配置表 UClass 缓存必须已被游戏自己初始化过.
    // unk_150ED768 是 sub_98322E0 内 FindObject("/N") 的结果缓存. 非零 = 游戏
    // 线程已经至少跑过一次配置加载, 后续从工作线程调用纯属哈希查表, 安全.
    // 零 = 游戏还没碰过, 我们的工作线程贸然进 sub_AED16BC 就会 SIGSEGV.
    {
        const uintptr_t cacheSlot = resolveItemConfigCacheSlot();
        if (cacheSlot == 0) return false;
        const uintptr_t cachedClass = safeReadPtr(cacheSlot);
        if (cachedClass < 0x10000) {
            static std::atomic<uint64_t> s_lastWaitLogMs{0};
            const uint64_t now = nowMonotonicMs();
            if (now - s_lastWaitLogMs.load(std::memory_order_relaxed) > 5000) {
                s_lastWaitLogMs.store(now, std::memory_order_relaxed);
                LOG(LOG_LEVEL_INFO,
                    "[ItemNameNative] 等待游戏线程初始化物资配置表 (slot=%p val=%p), "
                    "暂时退化到硬编码表",
                    (void*)cacheSlot, (void*)cachedClass);
            }
            return false;
        }
    }

    // ---- 缓存 (永久, key=itemID, value="" 表示已知失败) ----
    static std::unordered_map<int32_t, std::string> s_cache;
    static std::mutex s_cacheMu;
    {
        std::lock_guard<std::mutex> g(s_cacheMu);
        auto it = s_cache.find(itemID);
        if (it != s_cache.end()) {
            if (it->second.empty()) return false;
            out = it->second;
            return true;
        }
    }

    const uintptr_t fnAddr = resolveItemNameFnAddress();
    if (fnAddr == 0) return false;

    using FnT = void* (*)(int32_t);
    auto fn = reinterpret_cast<FnT>(fnAddr);

    ItemNameNativeSpoofCtx ctx{};
    ctx.fn = fn;
    ctx.itemID = itemID;
    ctx.fstrPtr = nullptr;

    // 复用 safeReadMemory 的信号屏障 + TLS jmp_buf, 把 sub_A3B7AC8 调用本身
    // 也保护起来 (信号处理器仅在 s_safeReadActive=1 时 longjmp).
    installSafeReadGuard();
    void* fstrPtr = nullptr;
    const int sigCaught = sigsetjmp(s_safeReadJmpBuf, 1);
    if (sigCaught == 0) {
        s_safeReadActive = 1;
        // 进入栈伪装窗口: x29 切到 fake chain, BLR 跳进 libUE4.so;
        // 出来后 x29 还原, 不留痕迹.
        stack_spoof::call_spoofed(&itemNameNativeTrampoline, &ctx);
        s_safeReadActive = 0;
        fstrPtr = ctx.fstrPtr;
    } else {
        s_safeReadActive = 0;
        LOG(LOG_LEVEL_WARN,
            "[ItemNameNative] sub_A3B7AC8(%d) faulted sig=%d, blacklisting itemID",
            itemID, sigCaught);
        std::lock_guard<std::mutex> g(s_cacheMu);
        s_cache[itemID] = "";
        return false;
    }

    if (fstrPtr == nullptr || reinterpret_cast<uintptr_t>(fstrPtr) < 0x10000) {
        std::lock_guard<std::mutex> g(s_cacheMu);
        s_cache[itemID] = "";
        return false;
    }

    // FString {Data, Num, Max} -> UTF-8. readFString 内部全部 safeReadMemory,
    // 即便 Data 指针无效也只是返回 ""; 不会再次崩溃.
    std::string name = readFString(reinterpret_cast<uintptr_t>(fstrPtr));
    if (name.empty()) {
        std::lock_guard<std::mutex> g(s_cacheMu);
        s_cache[itemID] = "";
        return false;
    }

    {
        std::lock_guard<std::mutex> g(s_cacheMu);
        s_cache[itemID] = name;
    }
    static std::atomic<int> s_loggedCount{0};
    if (s_loggedCount.fetch_add(1, std::memory_order_relaxed) < 32) {
        LOG(LOG_LEVEL_INFO, "[ItemNameNative] itemID=%d -> %s", itemID, name.c_str());
    }
    out = std::move(name);
    return true;
}

// EItemType (PUBG 经验值): 1=Weapon 2=Attachment 3=Ammo 4=Med 5=Throwing
// 6=Armor 7=Helmet 8=Backpack 9=Cloth 10=Other
// TypeSpecificID 命名表 — 基于 PUBGm 社区已知的 ItemDefineID 配置.
// 找不到的会落到 "类别#ID" 兜底, 同时通过日志吐一次未知 ID 方便补全.
static std::string lookupItemName(int32_t typeId, int32_t subId) {
    // ---- 武器 (PUBGm 常见 ItemDefineID 对照) ----
    if (typeId == 1) {
        switch (subId) {
            // 步枪 / 突击枪
            case 1: case 101: return "AKM";
            case 2: case 102: return "M16A4";
            case 3: case 103: return "SCAR-L";
            case 4: case 104: return "M416";
            case 5: case 105: return "Groza";
            case 6: case 106: return "AUG";
            case 17: case 117: return "G36C";
            case 18: case 118: return "QBZ";
            case 19: case 119: return "Mk47";
            case 20: case 120: return "DP-28";
            // 精确射手枪 (DMR)
            case 8:  case 108: return "Mk14";
            case 12: case 112: return "SLR";
            case 13: case 113: return "Mini14";
            case 14: case 114: return "SKS";
            case 15: case 115: return "VSS";
            case 16: case 116: return "QBU";
            // 狙击枪
            case 9:  case 109: return "AWM";
            case 10: case 110: return "Kar98K";
            case 11: case 111: return "M24";
            // 轻机枪
            case 7:  case 107: return "M249";
            // 手枪
            case 21: case 201: return "P92";
            case 22: case 202: return "P1911";
            case 23: case 203: return "P18C";
            case 24: case 204: return "R1895";
            case 25: case 205: return "R45";
            // 霰弹枪
            case 30: case 301: return "S686";
            case 31: case 302: return "S1897";
            case 32: case 303: return "S12K";
            case 33: case 304: return "DBS";
            // 冲锋枪
            case 40: case 401: return "UZI";
            case 41: case 402: return "UMP45";
            case 42: case 403: return "Vector";
            case 43: case 404: return "Tommy";
            case 44: case 405: return "PP-19";
            case 45: case 406: return "MP5K";
            case 46: case 407: return "MP9";
            // 近战 / 特殊
            case 50: case 501: return "弩";
            case 51: case 502: return "平底锅";
            case 52: case 503: return "镰刀";
            case 53: case 504: return "砍刀";
            case 54: case 505: return "撬棍";
            default: break;
        }
    }
    // ---- 配件 ----
    if (typeId == 2) {
        switch (subId) {
            case 1: case 101: return "红点";
            case 2: case 102: return "全息";
            case 3: case 103: return "2倍镜";
            case 4: case 104: return "3倍镜";
            case 5: case 105: return "4倍镜";
            case 6: case 106: return "6倍镜";
            case 7: case 107: return "8倍镜";
            case 8: case 108: return "15倍镜";
            // 握把
            case 20: case 201: return "垂直握把";
            case 21: case 202: return "水平握把";
            case 22: case 203: return "拇指握把";
            case 23: case 204: return "轻型握把";
            case 24: case 205: return "半截握把";
            // 枪口
            case 30: case 301: return "补偿器";
            case 31: case 302: return "消音器";
            case 32: case 303: return "消焰器";
            // 弹匣
            case 40: case 401: return "扩容弹匣";
            case 41: case 402: return "快速弹匣";
            case 42: case 403: return "扩容快速弹匣";
            // 枪托
            case 50: case 501: return "战术枪托";
            // 子弹环
            case 60: case 601: return "子弹袋";
            default: break;
        }
    }
    // ---- 防具/箱子三级体系 ----
    if (typeId == 7) {  // 头盔
        switch (subId) {
            case 1: case 501: return "1级头盔";
            case 2: case 502: return "2级头盔";
            case 3: case 503: return "3级头盔";
            case 504:        return "3级特种头盔";
            default: break;
        }
    } else if (typeId == 6) {  // 防弹衣
        switch (subId) {
            case 1: case 701: return "1级防弹衣";
            case 2: case 702: return "2级防弹衣";
            case 3: case 703: return "3级防弹衣";
            case 704:        return "3级特种防弹衣";
            default: break;
        }
    } else if (typeId == 8) {  // 背包
        switch (subId) {
            case 1: case 801: return "1级背包";
            case 2: case 802: return "2级背包";
            case 3: case 803: return "3级背包";
            default: break;
        }
    } else if (typeId == 4) {  // 药品
        switch (subId) {
            case 1: return "绷带";
            case 2: return "急救包";
            case 3: return "医疗箱";
            case 10: return "急救包";
            case 11: return "医疗箱";
            case 12: return "绷带";
            case 13: return "止痛药";
            case 14: return "肾上腺素";
            case 20: return "能量饮料";
            case 21: return "能量饮料";
            default: break;
        }
    } else if (typeId == 5) {  // 投掷物
        switch (subId) {
            case 1: return "手雷";
            case 2: return "烟雾弹";
            case 3: return "燃烧弹";
            case 4: return "震爆弹";
            case 5: return "诱饵手雷";
            default: break;
        }
    } else if (typeId == 3) {  // 弹药
        switch (subId) {
            case 1: return "9mm";
            case 2: return ".45ACP";
            case 3: return "5.56mm";
            case 4: return "7.62mm";
            case 5: return "12号弹";
            case 6: return "300马格南";
            case 7: return "弩矢";
            default: break;
        }
    }

    // 未知 ID: 首次出现时打日志, 方便用户回传后扩展表
    static std::unordered_set<uint64_t> s_loggedUnknown;
    static std::mutex s_logMutex;
    {
        std::lock_guard<std::mutex> g(s_logMutex);
        uint64_t key = (uint64_t(uint32_t(typeId)) << 32) | uint32_t(subId);
        if (s_loggedUnknown.insert(key).second && s_loggedUnknown.size() < 256) {
            LOG(LOG_LEVEL_INFO, "[ItemNameMissing] type=%d subId=%d", typeId, subId);
        }
    }

    const char* category = nullptr;
    switch (typeId) {
        case 1: category = "枪";   break;
        case 2: category = "配件"; break;
        case 3: category = "弹药"; break;
        case 4: category = "药品"; break;
        case 5: category = "投掷"; break;
        case 6: category = "护甲"; break;
        case 7: category = "头盔"; break;
        case 8: category = "背包"; break;
        case 9: category = "服饰"; break;
        default: break;
    }
    char buf[40];
    if (category) snprintf(buf, sizeof(buf), "%s#%d", category, subId);
    else          snprintf(buf, sizeof(buf), "物#%d-%d", typeId, subId);
    return buf;
}

// =====================================================================
//  GUObjectArray 扫描场景物体: 物资 / 载具 / 空投 / 死亡箱
//
//  分片扫描: 每次 poll 处理 kScanBudget 条, 完成一轮后才整体替换前台缓存,
//  避免在主轮询线程一次性遍历百万级 GUObjectArray 引发渲染卡顿.
//  完整一轮结束 + kRefreshMs 冷却到期后才开启下一轮.
// =====================================================================
int MatchMonitor::scanWorldObjects(std::vector<ue4draw::DrawWorldObject>& outObjects) {
    constexpr uint64_t kRefreshMs   = 1500;  // 一轮结束后多久才允许重启 (反作弊压力 vs 实时性)
    constexpr int      kScanBudget  = 6000;  // 每次 poll 最多遍历多少 GUObjectArray 条目
    constexpr int      kMaxObjects  = 512;   // 输出上限 (含箱内 PickUp)

    const uint64_t nowMs = nowMonotonicMs();

    auto returnCached = [&]() {
        outObjects = m_cachedWorldObjects;
        return static_cast<int>(outObjects.size());
    };

    // 没有进行中的扫描 + 冷却未到 → 直接复用缓存
    if (!m_worldObjScanInProgress) {
        if (m_lastWorldObjScanMs != 0 && nowMs - m_lastWorldObjScanMs < kRefreshMs) {
            return returnCached();
        }
        // 启动新一轮分片扫描
        m_worldObjScanInProgress = true;
        m_worldObjScanIndex = 0;
        m_worldObjScanBuffer.clear();
        m_worldObjScanBuffer.reserve(m_cachedWorldObjects.size() + 16);
    }

    UObjectArrayLayout layout{};
    if (!detectUObjectArrayLayout(m_gUObjectArray, layout)) {
        m_worldObjScanInProgress = false;
        m_worldObjScanBuffer.clear();
        return returnCached();
    }
    const int totalNum = layout.totalNum;
    if (totalNum <= 0 || totalNum > 4'000'000) {
        m_worldObjScanInProgress = false;
        m_worldObjScanBuffer.clear();
        return returnCached();
    }

    auto classifyClass = [&](uintptr_t classPtr) -> uint8_t {
        if (classPtr < 0x10000) return 0;
        auto it = m_worldObjClassSet.find(classPtr);
        if (it != m_worldObjClassSet.end()) return it->second;
        uint8_t kind = 0;
        if (isSubclassOf(classPtr, "STExtraVehicleBase")) kind = 2;
        else if (isSubclassOf(classPtr, "AirDropBoxActor")) kind = 3;
        else if (isSubclassOf(classPtr, "PlayerTombBox")) kind = 4;
        // PickUpListWrapperActor 派生自 PickUpWrapperActor; 必须先判断 List 子类,
        // 否则会被基类匹配抢走, 错过 PickUpDataList 内嵌物品列表
        else if (isSubclassOf(classPtr, "PickUpListWrapperActor")) kind = 5;
        else if (isSubclassOf(classPtr, "PickUpWrapperActor")) kind = 1;
        m_worldObjClassSet[classPtr] = kind;
        return kind;
    };

    int processed = 0;
    int endIdx = m_worldObjScanIndex + kScanBudget;
    if (endIdx > totalNum) endIdx = totalNum;

    for (int idx = m_worldObjScanIndex;
         idx < endIdx && static_cast<int>(m_worldObjScanBuffer.size()) < kMaxObjects;
         ++idx) {
        processed++;
        uintptr_t objPtr = readUObjectAt(m_gUObjectArray, layout, idx);
        if (objPtr < 0x10000) continue;
        uintptr_t classPtr = safeReadPtr(objPtr + kUObjectClassPrivateOffset);
        if (classPtr < 0x10000) continue;
        const uint8_t kind = classifyClass(classPtr);
        if (kind == 0) continue;  // 缓存命中 → 单次哈希查表即可

        FVector3 loc{};
        if (!getActorLocation(objPtr, loc)) continue;
        if (!std::isfinite(loc.x) || !std::isfinite(loc.y) || !std::isfinite(loc.z)) continue;
        if (std::fabs(loc.x) < 1.0f && std::fabs(loc.y) < 1.0f && std::fabs(loc.z) < 1.0f) continue;
        if (std::fabs(loc.x) > 2.0e6f || std::fabs(loc.y) > 2.0e6f || std::fabs(loc.z) > 2.0e6f) continue;

        ue4draw::DrawWorldObject obj{};
        obj.posX = loc.x; obj.posY = loc.y; obj.posZ = loc.z;

        if (kind == 1) {
            if (m_off.PickUp_bHasBeenPickedUp >= 0
                && (safeReadU8(objPtr + m_off.PickUp_bHasBeenPickedUp) & 1) != 0) continue;
            // 不再跳过 bIsInBox 物品 - 透传 inBox 标记给 UI, 由菜单开关决定是否绘制
            bool isInBox = false;
            if (m_off.PickUp_bIsInBox >= 0) {
                isInBox = (safeReadU8(objPtr + m_off.PickUp_bIsInBox) & 1) != 0;
            }

            int32_t typeId = 0, subId = 0;
            if (m_off.PickUp_DefineID >= 0) {
                typeId = safeReadS32(objPtr + m_off.PickUp_DefineID + 0);
                subId  = safeReadS32(objPtr + m_off.PickUp_DefineID + 4);
            }
            obj.typeId = typeId;
            obj.subTypeId = subId;
            obj.inBox = isInBox;
            obj.kind = (typeId == 1) ? ue4draw::DrawWorldObjectKind::Weapon
                                      : ue4draw::DrawWorldObjectKind::Item;
            // 优先走 libUE4.so 内部配置表 (sub_A3B7AC8) 拿真实物资名,
            // 失败 (函数 fault / 返回空 FString / itemID 已被拉黑) 回退到硬编码表.
            if (!tryGetItemNameNative(subId, obj.label)) {
                obj.label = lookupItemName(typeId, subId);
            }
        } else if (kind == 2) {
            obj.kind = ue4draw::DrawWorldObjectKind::Vehicle;
            uint8_t vt = 0;
            if (m_off.Vehicle_VehicleType >= 0) {
                vt = safeReadU8(objPtr + m_off.Vehicle_VehicleType);
                obj.typeId = vt;
            }
            if (m_off.Vehicle_VehicleHealthState >= 0) {
                obj.healthState = safeReadU8(objPtr + m_off.Vehicle_VehicleHealthState);
            }
            if (obj.healthState >= 4) continue;
            if (m_off.Vehicle_Fuel >= 0) {
                float f = safeReadFloat(objPtr + m_off.Vehicle_Fuel);
                if (std::isfinite(f) && f >= 0.0f && f <= 1000.0f) obj.fuel = f;
            }
            obj.label = lookupVehicleName(vt);
        } else if (kind == 3) {
            obj.kind = ue4draw::DrawWorldObjectKind::Airdrop;
            obj.label = "空投";
        } else if (kind == 4) {
            obj.kind = ue4draw::DrawWorldObjectKind::DeathBox;
            obj.label = "死亡箱";
        } else if (kind == 5) {
            // PickUpListWrapperActor: 多物品箱
            obj.kind = ue4draw::DrawWorldObjectKind::DeathBox;
            obj.label = "物资堆";
        }

        // 三种容器 (Airdrop/DeathBox/PickUpListWrapper) 共用 PickUpDataList @ 0xD98
        // 之前只在 kind==5 内联处理, 但实战中 PickUpListWrapperActor 几乎不出现在 GUObjectArray,
        // 真正存在的是 AirDropBoxActor / PlayerTombBox; 它们派生自 PickUpListWrapperActor → 同样布局.
        if ((kind == 3 || kind == 4 || kind == 5) && m_off.PickUpList_DataList >= 0) {
            // 先 push 父项
            m_worldObjScanBuffer.push_back(obj);

            const uintptr_t listAddr = objPtr + m_off.PickUpList_DataList;
            const uintptr_t arrData  = safeReadPtr(listAddr);
            const int32_t   arrNum   = safeReadS32(listAddr + 8);
            if (arrData >= 0x10000 && arrNum > 0 && arrNum <= 64) {
                constexpr uintptr_t kStride = 0x38;
                for (int32_t i = 0; i < arrNum; ++i) {
                    const uintptr_t itemAddr = arrData + static_cast<uintptr_t>(i) * kStride;
                    int32_t typeId = safeReadS32(itemAddr + 0);
                    int32_t subId  = safeReadS32(itemAddr + 4);
                    int32_t count  = safeReadS32(itemAddr + 0x18);
                    if (typeId <= 0 || typeId > 0x1000) continue;
                    if (count <= 0 || count > 100000) count = 1;

                    // 强制日志: 每个新出现的 (type,subId) 都打一次, 便于扩展物品名表
                    static std::unordered_set<uint64_t> s_seenIds;
                    static std::mutex s_seenMu;
                    {
                        std::lock_guard<std::mutex> g(s_seenMu);
                        uint64_t k = (uint64_t(uint32_t(typeId)) << 32) | uint32_t(subId);
                        if (s_seenIds.insert(k).second && s_seenIds.size() < 500) {
                            LOG(LOG_LEVEL_INFO, "[BoxItem] type=%d subId=%d count=%d kind=%d",
                                typeId, subId, count, (int)kind);
                        }
                    }

                    ue4draw::DrawWorldObject child{};
                    child.posX = loc.x; child.posY = loc.y; child.posZ = loc.z;
                    child.typeId = typeId;
                    child.subTypeId = subId;
                    child.inBox = true;
                    child.kind = (typeId == 1) ? ue4draw::DrawWorldObjectKind::Weapon
                                                : ue4draw::DrawWorldObjectKind::Item;
                    char nameBuf[64];
                    std::string nm;
                    if (!tryGetItemNameNative(subId, nm)) {
                        nm = lookupItemName(typeId, subId);
                    }
                    if (count > 1) snprintf(nameBuf, sizeof(nameBuf), "%s x%d", nm.c_str(), count);
                    else           snprintf(nameBuf, sizeof(nameBuf), "%s", nm.c_str());
                    child.label = nameBuf;
                    m_worldObjScanBuffer.push_back(std::move(child));
                    if (static_cast<int>(m_worldObjScanBuffer.size()) >= kMaxObjects) break;
                }
            }
            continue;  // 父项已 push
        }

        m_worldObjScanBuffer.push_back(std::move(obj));
    }

    m_worldObjScanIndex = endIdx;

    const bool reachedEnd = (m_worldObjScanIndex >= totalNum)
        || (static_cast<int>(m_worldObjScanBuffer.size()) >= kMaxObjects);

    if (reachedEnd) {
        // 一轮完成 → 整体替换前台缓存
        m_cachedWorldObjects = std::move(m_worldObjScanBuffer);
        m_worldObjScanBuffer.clear();
        m_worldObjScanInProgress = false;
        m_worldObjScanIndex = 0;
        m_lastWorldObjScanMs = nowMs;

        static Clock::time_point s_lastWorldObjLog;
        if (shouldLogEvery(s_lastWorldObjLog, std::chrono::milliseconds(2000))) {
            int items = 0, weapons = 0, vehicles = 0, airdrops = 0, deathboxes = 0;
            for (const auto& o : m_cachedWorldObjects) {
                switch (o.kind) {
                    case ue4draw::DrawWorldObjectKind::Item:     items++; break;
                    case ue4draw::DrawWorldObjectKind::Weapon:   weapons++; break;
                    case ue4draw::DrawWorldObjectKind::Vehicle:  vehicles++; break;
                    case ue4draw::DrawWorldObjectKind::Airdrop:  airdrops++; break;
                    case ue4draw::DrawWorldObjectKind::DeathBox: deathboxes++; break;
                }
            }
            LOG(LOG_LEVEL_INFO, "[WorldObj] total=%zu items=%d weapons=%d vehicles=%d airdrops=%d deathboxes=%d classes=%zu processed=%d",
                m_cachedWorldObjects.size(), items, weapons, vehicles, airdrops, deathboxes,
                m_worldObjClassSet.size(), processed);
        }
    }

    return returnCached();
}

void MatchMonitor::refreshTrackedPlayersFast() {
    // 优化: 每个玩家原本要发起 6 次 safeRead (RootComp + pos×3 + health + healthMax + bDead),
    // 每次 safeRead 都会经过 sigsetjmp (含 rt_sigprocmask) 和函数调用开销.
    // 改用 BatchMemReader 后:
    //   - Character 结构: 1 次批量读 (m_charReadSize 字节) -> 提取 RootComp/Health/HealthMax/bDead
    //   - SceneComponent 结构: 1 次批量读 (Translation+12) -> 提取 FVector3
    // 单玩家 6 次 safeRead -> 2 次, 20 玩家 120 -> 40 次, 反作弊采样压力同步下降.
    BatchMemReader charBatch;
    BatchMemReader rootBatch;
    const int32_t sceneTransOff = m_off.SceneComp_Translation;
    const size_t  rootBatchSize = (sceneTransOff >= 0)
        ? static_cast<size_t>(sceneTransOff) + sizeof(FVector3)
        : 0;

    PlayerNode* cur = m_playerList.head();
    while (cur) {
        if (cur->characterPtr >= 0x10000 && m_charReadSize > 0) {
            charBatch.read(cur->characterPtr, m_charReadSize);

            uintptr_t rootComp = 0;
            if (m_off.Actor_RootComponent >= 0) {
                const uintptr_t raw = charBatch.getPtr(m_off.Actor_RootComponent);
                if (raw >= 0x10000) rootComp = raw;
            }

            if (rootComp != 0 && rootBatchSize > 0) {
                rootBatch.read(rootComp, rootBatchSize);
                const FVector3 loc = rootBatch.getVec3(sceneTransOff);
                if (hasUsablePlayerPosition(loc)) {
                    cur->pos = loc;
                }
            }

            if (m_off.Char_Health >= 0) {
                const float charHealth = charBatch.getFloat(m_off.Char_Health);
                if (std::isfinite(charHealth) && charHealth >= 0.0f) {
                    cur->health = charHealth;
                }
            }
            if (m_off.Char_HealthMax >= 0) {
                const float charHealthMax = charBatch.getFloat(m_off.Char_HealthMax);
                if (std::isfinite(charHealthMax) && charHealthMax > 0.0f) {
                    cur->healthMax = charHealthMax;
                }
            }
            if (m_off.Char_bDead >= 0) {
                const bool bDead = (charBatch.getU8(m_off.Char_bDead) & 1) != 0;
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

    const int32_t pcmOff = (m_off.PC_PlayerCameraManager >= 0) ? m_off.PC_PlayerCameraManager : 0x660;
    uintptr_t pcm = (pc >= 0x10000) ? safeReadPtr(pc + pcmOff) : 0;
    if (pcm < 0x10000) {
        pcm = findPlayerCameraManagerInstance();
    }
    if (pcm == 0) {
        static Clock::time_point s_lastNoPCMLog;
        if (shouldLogEvery(s_lastNoPCMLog, std::chrono::milliseconds(2000))) {
            LOG(LOG_LEVEL_WARN, "[Camera] no PlayerCameraManager pc=%p pcmOff=0x%X", (void*)pc, pcmOff);
        }
        return;
    }

    uintptr_t cameraPC = pc;
    if (cameraPC < 0x10000 && m_off.PCM_PCOwner >= 0) {
        cameraPC = safeReadPtr(pcm + m_off.PCM_PCOwner);
    }

    const int32_t camCacheOff = (m_off.PCM_CameraCache >= 0) ? m_off.PCM_CameraCache : 0x640;
    const int32_t defaultFovOff = (m_off.PCM_DefaultFOV >= 0) ? m_off.PCM_DefaultFOV : 0x5E8;

    auto hasFiniteCameraPose = [](float locX,
                                  float locY,
                                  float locZ,
                                  float pitch,
                                  float yaw,
                                  float roll) {
        // UE world bounds: PUBG 地图最大约 8x8km, 任何 |loc| > 2e6 cm (20 km) 都是
        // 候选 ViewInfo 偏移读错 (野指针/未初始化内存被解释为 float) 产生的垃圾值.
        // 必须 reject, 否则会以最优 baseScore 抢占真正有效的 CameraCache 候选.
        constexpr float kMaxWorldCoord = 2.0e6f;
        return std::isfinite(locX) && std::isfinite(locY) && std::isfinite(locZ)
            && std::fabs(locX) < kMaxWorldCoord
            && std::fabs(locY) < kMaxWorldCoord
            && std::fabs(locZ) < kMaxWorldCoord
            && (std::fabs(locX) > 1.0f || std::fabs(locY) > 1.0f || std::fabs(locZ) > 1.0f)
            && std::isfinite(pitch) && std::isfinite(yaw) && std::isfinite(roll);
    };
    auto hasValidFov = [](float fov) {
        return std::isfinite(fov) && fov >= 30.0f && fov <= 170.0f;
    };
    auto normalizeAngle = [](float value) {
        return std::isfinite(value) ? std::remainder(value, 360.0f) : 0.0f;
    };
    auto angleDelta = [&](float lhs, float rhs) {
        return std::fabs(normalizeAngle(lhs - rhs));
    };
    auto angleMagnitude = [&](float pitch, float yaw, float roll) {
        return std::fabs(normalizeAngle(pitch)) + std::fabs(normalizeAngle(yaw)) + std::fabs(normalizeAngle(roll));
    };
    auto hasMeaningfulRotation = [&](float pitch, float yaw, float roll) {
        return std::isfinite(pitch) && std::isfinite(yaw) && std::isfinite(roll)
            && angleMagnitude(pitch, yaw, roll) > 1.0f;
    };

    float controlPitch = 0.0f;
    float controlYaw = 0.0f;
    float controlRoll = 0.0f;
    bool hasControlRotation = false;
    if (cameraPC >= 0x10000 && m_off.Ctrl_ControlRotation >= 0) {
        controlPitch = safeReadFloat(cameraPC + m_off.Ctrl_ControlRotation);
        controlYaw = safeReadFloat(cameraPC + m_off.Ctrl_ControlRotation + 4);
        controlRoll = safeReadFloat(cameraPC + m_off.Ctrl_ControlRotation + 8);
        hasControlRotation = std::isfinite(controlPitch) && std::isfinite(controlYaw) && std::isfinite(controlRoll);
    }

    struct RotationCandidate {
        const char* label = "";
        uintptr_t addr = 0;
        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
        float score = 1000000.0f;
        bool valid = false;
    };
    auto makeRotationCandidate = [&](uintptr_t addr, const char* label, float baseScore) {
        RotationCandidate candidate{};
        candidate.label = label;
        candidate.addr = addr;
        if (addr < 0x10000) return candidate;
        candidate.pitch = safeReadFloat(addr);
        candidate.yaw = safeReadFloat(addr + 4);
        candidate.roll = safeReadFloat(addr + 8);
        candidate.valid = hasMeaningfulRotation(candidate.pitch, candidate.yaw, candidate.roll);
        if (!candidate.valid) return candidate;
        candidate.score = baseScore;
        if (hasControlRotation && hasMeaningfulRotation(controlPitch, controlYaw, controlRoll)) {
            candidate.score += angleDelta(candidate.pitch, controlPitch) * 0.5f;
            candidate.score += angleDelta(candidate.yaw, controlYaw) * 0.75f;
            candidate.score += angleDelta(candidate.roll, controlRoll) * 0.1f;
        }
        return candidate;
    };

    struct CameraCandidate {
        const char* label = "";
        uintptr_t viewInfoPtr = 0;
        int32_t rotOff = 0;
        int32_t fovOff = 0;
        float locX = 0.0f;
        float locY = 0.0f;
        float locZ = 0.0f;
        float pitch = 0.0f;
        float yaw = 0.0f;
        float roll = 0.0f;
        float fov = 0.0f;
        float score = 1000000.0f;
        bool valid = false;
    };

    const float defaultFov = safeReadFloat(pcm + defaultFovOff);
    auto makeCandidate = [&](uintptr_t viewInfoPtr,
                             int32_t rotOff,
                             int32_t fovOff,
                             const char* label,
                             float baseScore) {
        CameraCandidate candidate{};
        candidate.label = label;
        candidate.viewInfoPtr = viewInfoPtr;
        candidate.rotOff = rotOff;
        candidate.fovOff = fovOff;
        if (viewInfoPtr < 0x10000) return candidate;
        candidate.locX = safeReadFloat(viewInfoPtr + 0x0);
        candidate.locY = safeReadFloat(viewInfoPtr + 0x4);
        candidate.locZ = safeReadFloat(viewInfoPtr + 0x8);
        candidate.pitch = safeReadFloat(viewInfoPtr + rotOff);
        candidate.yaw = safeReadFloat(viewInfoPtr + rotOff + 4);
        candidate.roll = safeReadFloat(viewInfoPtr + rotOff + 8);
        candidate.fov = safeReadFloat(viewInfoPtr + fovOff);
        if (!hasValidFov(candidate.fov) && hasValidFov(defaultFov)) {
            candidate.fov = defaultFov;
            baseScore += 15.0f;
        }

        candidate.valid = hasFiniteCameraPose(candidate.locX, candidate.locY, candidate.locZ,
                                              candidate.pitch, candidate.yaw, candidate.roll)
            && hasValidFov(candidate.fov);
        if (!candidate.valid) return candidate;

        candidate.score = baseScore;
        if (hasControlRotation) {
            candidate.score += angleDelta(candidate.pitch, controlPitch) * 1.5f;
            candidate.score += angleDelta(candidate.yaw, controlYaw) * 2.0f;
            candidate.score += angleDelta(candidate.roll, controlRoll) * 0.25f;
        } else {
            candidate.score += std::fabs(normalizeAngle(candidate.pitch)) * 0.05f;
            candidate.score += std::fabs(normalizeAngle(candidate.roll)) * 0.05f;
        }
        return candidate;
    };

    float camLocX = 0.0f;
    float camLocY = 0.0f;
    float camLocZ = 0.0f;
    float camPitch = 0.0f;
    float camYaw = 0.0f;
    float camRoll = 0.0f;
    float camFov = 0.0f;

    CameraCandidate best{};
    const uintptr_t cameraCachePOV = pcm + static_cast<uintptr_t>(camCacheOff) + 0x10;
    const uintptr_t cachedViewPOV = pcm + 0x2120;
    std::vector<CameraCandidate> candidates;
    candidates.reserve(18);

    uintptr_t activeCameraCache = 0;
    if (cameraPC >= 0x10000 && m_off.STPC_CurrentActiveCameraCache >= 0) {
        activeCameraCache = safeReadPtr(cameraPC + static_cast<uintptr_t>(m_off.STPC_CurrentActiveCameraCache));
        if (activeCameraCache >= 0x10000) {
            candidates.push_back(makeCandidate(activeCameraCache + 0x10, 0x0C, 0x18, "STPC.CurrentActiveCameraCache+10.compact", -30.0f));
            candidates.push_back(makeCandidate(activeCameraCache + 0x10, 0x10, 0x1C, "STPC.CurrentActiveCameraCache+10.aligned", -25.0f));
            candidates.push_back(makeCandidate(activeCameraCache + 0x10, 0x18, 0x30, "STPC.CurrentActiveCameraCache+10.legacyWide", -20.0f));
            candidates.push_back(makeCandidate(activeCameraCache + 0x30, 0x0C, 0x18, "STPC.CurrentActiveCameraCache+30.compact", -15.0f));
            candidates.push_back(makeCandidate(activeCameraCache + 0x30, 0x10, 0x1C, "STPC.CurrentActiveCameraCache+30.aligned", -10.0f));
            candidates.push_back(makeCandidate(activeCameraCache + 0x30, 0x18, 0x30, "STPC.CurrentActiveCameraCache+30.legacyWide", -5.0f));
        }
    }

    candidates.push_back(makeCandidate(cachedViewPOV, 0x0C, 0x18, "CachedViewPOV.compact", 0.0f));
    candidates.push_back(makeCandidate(cachedViewPOV, 0x10, 0x1C, "CachedViewPOV.aligned", 5.0f));
    candidates.push_back(makeCandidate(cachedViewPOV, 0x18, 0x30, "CachedViewPOV.legacyWide", 10.0f));
    candidates.push_back(makeCandidate(cameraCachePOV, 0x0C, 0x18, "CameraCache.compact", 20.0f));
    candidates.push_back(makeCandidate(cameraCachePOV, 0x10, 0x1C, "CameraCache.aligned", 25.0f));
    candidates.push_back(makeCandidate(cameraCachePOV, 0x18, 0x30, "CameraCache.legacyWide", 30.0f));

    for (const auto& candidate : candidates) {
        if (candidate.valid && (!best.valid || candidate.score < best.score)) {
            best = candidate;
        }
    }

    if (best.valid) {
        camLocX = best.locX;
        camLocY = best.locY;
        camLocZ = best.locZ;
        camPitch = best.pitch;
        camYaw = best.yaw;
        camRoll = best.roll;
        camFov = best.fov;

        RotationCandidate bestRotation{};
        if (cameraPC >= 0x10000) {
            const RotationCandidate rotationCandidates[] = {
            makeRotationCandidate((m_off.STPC_CachedViewControlRotation >= 0) ? cameraPC + static_cast<uintptr_t>(m_off.STPC_CachedViewControlRotation) : 0,
                                      "STPC.CachedViewControlRotation", 0.0f),
            makeRotationCandidate((m_off.STPC_LastFrameCacheControlRotation >= 0) ? cameraPC + static_cast<uintptr_t>(m_off.STPC_LastFrameCacheControlRotation) : 0,
                                      "STPC.LastFrameCacheControlRotation", 5.0f),
            makeRotationCandidate((m_off.Ctrl_ControlRotation >= 0) ? cameraPC + static_cast<uintptr_t>(m_off.Ctrl_ControlRotation) : 0,
                                      "Controller.ControlRotation", 15.0f),
            };
            for (const auto& rotationCandidate : rotationCandidates) {
                if (rotationCandidate.valid && (!bestRotation.valid || rotationCandidate.score < bestRotation.score)) {
                    bestRotation = rotationCandidate;
                }
            }
        }

        // ⚠️ 重要: 只在相机旋转 "真的无效" (全零/NaN) 时才用控制器旋转覆盖.
        // TPS 第三人称视角下, PlayerCameraManager.CameraCache 的 rotation 与
        // PlayerController.ControlRotation 之间会差几度 (spring-arm 平滑/瞄准点偏移),
        // 这是游戏正常行为. 如果用 ControlRotation 覆盖, 会让 *旋转源* 与 *位置源*
        // (仍是 PCM CameraCache.POV.Location) 不匹配, 导致 ESP 在屏幕上随相机旋转方向
        // 系统性漂移. 必须保持 rot/loc 来自同一个 ViewInfo.
        bool usedRotationOverride = false;
        const bool cameraRotationLooksEmpty = !hasMeaningfulRotation(camPitch, camYaw, camRoll);
        if (bestRotation.valid && cameraRotationLooksEmpty) {
            camPitch = bestRotation.pitch;
            camYaw = bestRotation.yaw;
            camRoll = bestRotation.roll;
            usedRotationOverride = true;
        }

        // 把选中的源地址 publish 给渲染线程, 让 ESP 在 90~120Hz 重绘时
        // 不再依赖 16ms 旧的 DrawGameData 相机快照, 滑屏时 ESP 框零滞后.
        if (best.viewInfoPtr >= 0x10000) {
            const uintptr_t locAddr = best.viewInfoPtr;
            const uintptr_t rotAddr = best.viewInfoPtr + static_cast<uintptr_t>(best.rotOff);
            const uintptr_t fovAddr = best.viewInfoPtr + static_cast<uintptr_t>(best.fovOff);
            const uintptr_t rotOverrideAddr = (usedRotationOverride && bestRotation.addr >= 0x10000)
                                                ? bestRotation.addr : 0;
            LiveCameraSnapshot::instance().publish(locAddr, rotAddr, fovAddr,
                                                   rotOverrideAddr,
                                                   hasValidFov(defaultFov) ? defaultFov : 90.0f);
        }

        static Clock::time_point s_lastCameraLayoutLog;
        if (shouldLogEvery(s_lastCameraLayoutLog, std::chrono::milliseconds(2000))) {
            LOG(LOG_LEVEL_INFO,
                "[Camera] selected=%s score=%.1f pc=%p ownerPC=%p activeCam=%p loc=(%.0f,%.0f,%.0f) rot=(%.1f,%.1f,%.1f) fov=%.1f ctrl=(%.1f,%.1f,%.1f) hasCtrl=%d rotOverride=%s%s",
                best.label, best.score, (void*)pc, (void*)cameraPC, (void*)activeCameraCache, camLocX, camLocY, camLocZ, camPitch, camYaw, camRoll, camFov,
                controlPitch, controlYaw, controlRoll, hasControlRotation ? 1 : 0,
                usedRotationOverride ? "1:" : "0",
                usedRotationOverride ? bestRotation.label : "");
        }
    }

    if (hasFiniteCameraPose(camLocX, camLocY, camLocZ, camPitch, camYaw, camRoll)) {
        drawData.camLocX = camLocX;
        drawData.camLocY = camLocY;
        drawData.camLocZ = camLocZ;
        drawData.camPitch = camPitch;
        drawData.camYaw = camYaw;
        drawData.camRoll = camRoll;
        drawData.camFOV = hasValidFov(camFov) ? camFov : 90.0f;
    } else {
        LiveCameraSnapshot::instance().clear();
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
// 缓入缓出插值: smoothstep(t) = 3t² - 2t³, 比线性更像人类 (#17)
float smoothStepT(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float lerpAngle(float from, float to, float t) {
    float diff = normalizeAngle180(to - from);
    return from + diff * smoothStepT(t);
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
//  getTargetVelocity — 读取目标角色的当前移动速度向量
//
//  通过 Character → CharacterMovement → CMC.Velocity 获取
//  用于弹道预测中的目标移动前置量计算
// =====================================================================
FVector3 MatchMonitor::getTargetVelocity(uintptr_t characterPtr) {
    FVector3 vel{};
    if (characterPtr < 0x10000) return vel;

    const int32_t offCMC = (m_off.Char_CharacterMovement >= 0) ? m_off.Char_CharacterMovement : 0x658;
    const uintptr_t cmcPtr = safeReadPtr(characterPtr + offCMC);
    if (cmcPtr < 0x10000) return vel;

    const int32_t offVel = (m_off.CMC_Velocity >= 0) ? m_off.CMC_Velocity : 0x13C;
    vel.x = safeReadFloat(cmcPtr + offVel);
    vel.y = safeReadFloat(cmcPtr + offVel + 4);
    vel.z = safeReadFloat(cmcPtr + offVel + 8);

    // 过滤异常速度
    if (!std::isfinite(vel.x) || !std::isfinite(vel.y) || !std::isfinite(vel.z)) {
        return FVector3{};
    }
    // 速度过大说明数据异常 (>200 m/s = 20000 cm/s 不合理)
    const float speedSq = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
    if (speedSq > 20000.0f * 20000.0f) {
        return FVector3{};
    }

    return vel;
}

// =====================================================================
//  getLocalWeaponBulletParams — 获取本地玩家当前武器的弹速/重力参数
//
//  读取路径: Character → CurWeapon → 反射查找弹速字段
//  缓存策略: 以武器指针为 key, 同一把武器只查找一次反射
//  回退: 如果无法读取, 使用 PUBG Mobile 常见默认值 (750 m/s, 重力1.0)
// =====================================================================
WeaponBulletParams MatchMonitor::getLocalWeaponBulletParams() {
    const uint64_t nowMs = nowMonotonicMs();
    // 每 500ms 重新读取武器指针 (防止换枪后失效)
    if (m_cachedBulletParams.valid && nowMs - m_lastBulletParamReadMs < 500) {
        return m_cachedBulletParams;
    }

    WeaponBulletParams params;

    // 获取本地玩家角色
    PlayerNode* myNode = (m_myPlayerKey != 0) ? m_playerList.findByKey(m_myPlayerKey) : m_playerList.head();
    if (!myNode || myNode->characterPtr < 0x10000) {
        return params; // 返回默认值
    }

    // 尝试读取 CurWeapon
    uintptr_t weaponPtr = 0;
    if (m_off.Char_CurWeapon >= 0) {
        weaponPtr = safeReadPtr(myNode->characterPtr + m_off.Char_CurWeapon);
    }

    if (weaponPtr >= 0x10000) {
        // 检查武器指针缓存
        auto cacheIt = m_weaponParamsCache.find(weaponPtr);
        if (cacheIt != m_weaponParamsCache.end()) {
            params = cacheIt->second;
            m_cachedBulletParams = params;
            m_lastBulletParamReadMs = nowMs;
            return params;
        }

        // 通过反射查找弹速/重力字段
        // 候选名根据 dump.cs 实际存在性筛选 (移除全部死候选):
        //   弹速:   BulletFireSpeed (ShootWeaponEntity), BulletSpeed (CG36MusicGameBulletActor)
        //   重力:   GravityScale, ProjectileGravityScale, UniversalProjectileGravityScale,
        //           LaunchGravityScale, BulletGravityModifier
        //   无重力: MaxNoGravityRange (子弹基类)
        const std::string weaponClass = readClassName(weaponPtr);
        if (!weaponClass.empty() && weaponClass[0] != '<') {
            // 查找弹速
            for (const char* fieldName : {"BulletFireSpeed", "BulletSpeed",
                                          "BulletFireSpeedModifier"}) {
                const auto* fi = m_interface.findFieldInHierarchy(weaponClass, fieldName);
                if (fi && fi->offset > 0) {
                    float speed = safeReadFloat(weaponPtr + fi->offset);
                    if (std::isfinite(speed) && speed > 1000.0f && speed < 200000.0f) {
                        params.bulletSpeed = speed;
                        params.valid = true;
                        LOG(LOG_LEVEL_INFO, "[Ballistic] Found %s.%s = %.0f cm/s",
                            weaponClass.c_str(), fieldName, speed);
                        break;
                    }
                }
            }

            // 查找重力缩放
            for (const char* fieldName : {"UniversalProjectileGravityScale", "ProjectileGravityScale",
                                          "LaunchGravityScale", "BulletGravityModifier", "GravityScale"}) {
                const auto* fi = m_interface.findFieldInHierarchy(weaponClass, fieldName);
                if (fi && fi->offset > 0) {
                    float grav = safeReadFloat(weaponPtr + fi->offset);
                    if (std::isfinite(grav) && grav >= 0.0f && grav <= 10.0f) {
                        params.gravityScale = grav;
                        LOG(LOG_LEVEL_INFO, "[Ballistic] Found %s.%s = %.2f",
                            weaponClass.c_str(), fieldName, grav);
                        break;
                    }
                }
            }

            // 查找无重力范围
            for (const char* fieldName : {"MaxNoGravityRange"}) {
                const auto* fi = m_interface.findFieldInHierarchy(weaponClass, fieldName);
                if (fi && fi->offset > 0) {
                    float range = safeReadFloat(weaponPtr + fi->offset);
                    if (std::isfinite(range) && range >= 0.0f && range < 100000.0f) {
                        params.maxNoGravityRange = range;
                        break;
                    }
                }
            }
        }

        // 限制武器缓存大小
        if (m_weaponParamsCache.size() > 64) {
            m_weaponParamsCache.clear();
        }
        m_weaponParamsCache[weaponPtr] = params;
    }

    m_cachedBulletParams = params;
    m_lastBulletParamReadMs = nowMs;

    static Clock::time_point s_lastBulletLog;
    if (shouldLogEvery(s_lastBulletLog, std::chrono::milliseconds(5000))) {
        writeAimbotLog("[Ballistic] bulletSpeed=%.0f gravScale=%.2f launchGrav=%.2f noGravRange=%.0f valid=%d",
            params.bulletSpeed, params.gravityScale, params.launchGravityScale,
            params.maxNoGravityRange, params.valid);
    }

    return params;
}

// =====================================================================
//  predictBallisticAimPoint — 弹道物理模拟, 计算预测瞄准点
//
//  输入:
//    shooterPos     — 射手位置 (相机/枪口, UE4 坐标 cm)
//    targetPos      — 目标位置 (骨骼/角色位置)
//    targetVelocity — 目标当前速度向量 (cm/s)
//    params         — 武器子弹参数 (弹速, 重力, 无重力范围)
//
//  物理模型:
//    1. 子弹以 bulletSpeed 发射, 飞行时间 t = distance / bulletSpeed
//    2. 目标移动前置: predictedPos = targetPos + targetVelocity × t
//    3. 重力下坠补偿: 在无重力范围外, drop = ½ × g × t²
//       g = 980 cm/s² × gravityScale × launchGravityScale
//    4. 迭代求解: 预测位置改变距离 → 重新计算飞行时间 → 收敛
//
//  返回: 补偿后的瞄准点 (需瞄准此点以命中移动目标)
// =====================================================================
FVector3 MatchMonitor::predictBallisticAimPoint(
    const FVector3& shooterPos,
    const FVector3& targetPos,
    const FVector3& targetVelocity,
    const WeaponBulletParams& params)
{
    constexpr float kUE4Gravity = 980.0f;    // UE4 默认重力加速度 cm/s²
    constexpr int kMaxIterations = 8;        // 增加迭代次数, 远距离需要更多步骤收敛
    constexpr float kConvergenceThreshold = 0.00005f; // 收敛精度提升: 0.05ms

    const float bulletSpeed = (params.bulletSpeed > 100.0f) ? params.bulletSpeed : 75000.0f;
    const float gravity = kUE4Gravity * params.gravityScale * params.launchGravityScale;
    const float noGravRange = (params.maxNoGravityRange > 0.0f) ? params.maxNoGravityRange : 0.0f;

    // 检查目标速度是否有意义 (静止目标且无重力 → 无需预测)
    const float targetSpeedSq = targetVelocity.x * targetVelocity.x
                               + targetVelocity.y * targetVelocity.y
                               + targetVelocity.z * targetVelocity.z;
    const bool targetMoving = (targetSpeedSq > 1.0f);

    if (!targetMoving && gravity < 0.01f) {
        return targetPos; // 静止目标 + 无重力 → 直接瞄准
    }

    // 使用水平距离计算飞行时间 (更物理正确: 子弹水平速度近似恒定)
    FVector3 predicted = targetPos;
    float lastFlightTime = 0.0f;

    for (int iter = 0; iter < kMaxIterations; ++iter) {
        const float dx = predicted.x - shooterPos.x;
        const float dy = predicted.y - shooterPos.y;
        const float dz = predicted.z - shooterPos.z;

        // 水平距离用于计算飞行时间 (重力仅影响z轴, 不影响水平速度)
        const float horizDistSq = dx * dx + dy * dy;
        const float totalDistSq = horizDistSq + dz * dz;
        if (totalDistSq < 1.0f) break;

        const float horizDist = std::sqrt(horizDistSq);
        // 飞行时间基于3D距离 (近距离)/水平距离 (远距离) 混合
        // 远距离时水平距离更准确, 因为重力使实际弹道弯曲
        const float totalDist = std::sqrt(totalDistSq);
        const float flightTime = (horizDist > 100.0f) ? (horizDist / bulletSpeed) : (totalDist / bulletSpeed);

        // 收敛检查
        if (iter > 0 && std::fabs(flightTime - lastFlightTime) < kConvergenceThreshold) break;
        lastFlightTime = flightTime;

        // 目标移动前置: 假设目标在子弹飞行时间内保持当前速度方向和大小
        predicted.x = targetPos.x + targetVelocity.x * flightTime;
        predicted.y = targetPos.y + targetVelocity.y * flightTime;
        // Z轴: 目标移动 + 目标自身重力 (跳跃/下落中的敌人)
        predicted.z = targetPos.z + targetVelocity.z * flightTime;

        // 重力下坠补偿: 子弹在飞行过程中受重力作用下坠
        if (gravity > 0.01f) {
            float gravTime;
            if (noGravRange > 0.0f && horizDist > 1.0f) {
                if (horizDist > noGravRange) {
                    gravTime = (horizDist - noGravRange) / bulletSpeed;
                } else {
                    gravTime = 0.0f;
                }
            } else {
                gravTime = flightTime;
            }
            // 标准抛物线: drop = ½gt²
            const float drop = 0.5f * gravity * gravTime * gravTime;
            predicted.z += drop;
        }
    }

    // 安全检查: 预测结果必须有限且合理
    if (!std::isfinite(predicted.x) || !std::isfinite(predicted.y) || !std::isfinite(predicted.z)) {
        return targetPos;
    }
    // 预测偏移不应超过 100 米 (10000 cm), 否则数据可能异常
    const float offsetSq = (predicted.x - targetPos.x) * (predicted.x - targetPos.x)
                          + (predicted.y - targetPos.y) * (predicted.y - targetPos.y)
                          + (predicted.z - targetPos.z) * (predicted.z - targetPos.z);
    if (offsetSq > 10000.0f * 10000.0f) {
        return targetPos;
    }

    return predicted;
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
    if (m_memoryRestored.load(std::memory_order_acquire)) return;  // 恢复后禁止写入
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

    // 限频: 25~40ms 随机间隔写入, 仿人类输入节奏 (#17)
    const uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count();
    const uint64_t aimInterval = 25 + (nowMs % 16);  // 25~40ms 随机报动
    if (nowMs - m_lastAimbotWriteMs < aimInterval) return;

    const uintptr_t pc = getLocalPlayerController();
    if (pc == 0) return;

    // 读取当前 ControlRotation (用于平滑插值)
    const uintptr_t ctrlRotAddr = pc + static_cast<uintptr_t>(m_off.Ctrl_ControlRotation);
    const float curPitch = safeReadFloat(ctrlRotAddr + 0);
    const float curYaw   = safeReadFloat(ctrlRotAddr + 4);
    if (!std::isfinite(curPitch) || !std::isfinite(curYaw)) return;

    // 读取当前相机位置作为射线起点
    const int32_t pcmOff2 = (m_off.PC_PlayerCameraManager >= 0) ? m_off.PC_PlayerCameraManager : 0x660;  // dump.cs PlayerController.PlayerCameraManager
    const uintptr_t pcm = safeReadPtr(pc + pcmOff2);
    if (pcm == 0) return;

    const int32_t camCacheOff = (m_off.PCM_CameraCache >= 0) ? m_off.PCM_CameraCache : 0x640;  // dump.cs PlayerCameraManager.CameraCache
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

    const int boneIdx = m_aimbotTargetBone;  // 3=neck
    // 骨骼回退链: 脖子 → 头部 → 脊椎上段 → 位置偏移
    constexpr int kBoneFallback[] = {3, 4, 2};  // Neck, Head, SpineUpper
    constexpr int kBoneFallbackCount = 3;

    // ---- 辅助: 从 PlayerNode 获取瞄准坐标 (带骨骼回退链) ----
    auto getTargetPos = [&](const PlayerNode* node, FVector3& outTgt) -> bool {
        // 优先用目标骨骼, 不可用则沿回退链尝试
        if ((nowMs - node->cachedBoneTimestampMs) < kMaxBoneAgeMs && node->cachedBoneMask != 0) {
            // 首选: 用户设置的目标骨骼
            if (boneIdx >= 0 && boneIdx < static_cast<int>(TRACKED_BONE_COUNT)
                && (node->cachedBoneMask & (1u << boneIdx)) != 0) {
                const auto& b = node->cachedBones[boneIdx];
                if (std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z)
                    && (std::fabs(b.x) > 1.0f || std::fabs(b.y) > 1.0f)) {
                    outTgt = {b.x, b.y, b.z};
                    return true;
                }
            }
            // 回退链: Neck → Head → SpineUpper
            for (int fi = 0; fi < kBoneFallbackCount; ++fi) {
                int bi = kBoneFallback[fi];
                if (bi == boneIdx) continue; // 已经试过
                if (bi < 0 || bi >= static_cast<int>(TRACKED_BONE_COUNT)) continue;
                if ((node->cachedBoneMask & (1u << bi)) == 0) continue;
                const auto& b = node->cachedBones[bi];
                if (std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.z)
                    && (std::fabs(b.x) > 1.0f || std::fabs(b.y) > 1.0f)) {
                    outTgt = {b.x, b.y, b.z};
                    return true;
                }
            }
        }
        // 最终回退: 角色位置 + 脖子高度偏移 (~145cm 人物, 脖子约 130cm)
        if (std::isfinite(node->pos.x) && std::isfinite(node->pos.y) && std::isfinite(node->pos.z)
            && (std::fabs(node->pos.x) > 1.0f || std::fabs(node->pos.y) > 1.0f)) {
            outTgt = {node->pos.x, node->pos.y, node->pos.z + 50.0f};
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
            if (getTargetPos(locked, tgt)) {
                // 弹道物理预测: 补偿子弹飞行时间内的重力下坠和目标移动
                FVector3 targetVel = getTargetVelocity(locked->characterPtr);
                WeaponBulletParams wp = getLocalWeaponBulletParams();
                FVector3 predicted = predictBallisticAimPoint({camX, camY, camZ}, tgt, targetVel, wp);
                if (calcAim(predicted, distSq, aimPitch, aimYaw)) {
                    // 平滑插值
                    float finalPitch = lerpAngle(curPitch, aimPitch, smoothT);
                    float finalYaw   = lerpAngle(curYaw,   aimYaw,   smoothT);
                    writeMemFloat(ctrlRotAddr + 0, finalPitch);
                    writeMemFloat(ctrlRotAddr + 4, finalYaw);
                    m_lastAimbotWriteMs = nowMs;

                    static Clock::time_point s_lastLockLog;
                    if (shouldLogEvery(s_lastLockLog, std::chrono::milliseconds(2000))) {
                        writeAimbotLog("[Aimbot] Tracking key=%u hp=%.0f dist=%.0fm ballistic=(%.1f,%.1f,%.1f)",
                            m_aimbotLockedKey, locked->health, std::sqrt(distSq) / 100.0f,
                            predicted.x - tgt.x, predicted.y - tgt.y, predicted.z - tgt.z);
                    }
                    return;
                }
            }
        }
        // 锁定失效
        writeAimbotLog("[Aimbot] Lost lock key=%u, re-selecting", m_aimbotLockedKey);
        m_aimbotLockedKey = 0;
    }

    // ---- 选择新目标: 准心最近的敌人 (FOV 角度最小优先, 距离加权) ----
    // 评分 = 准心角度偏移 + 距离惩罚, 越小越优
    float bestScore = 1e18f;
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
            // 准心偏移角度 (度)
            const float dPitch = normalizeAngle180(aimPitch - curPitch);
            const float dYaw   = normalizeAngle180(aimYaw - curYaw);
            const float angleDeg = std::sqrt(dPitch * dPitch + dYaw * dYaw);
            // 距离权重: 每100米增加1度等效偏移
            const float distMeters = std::sqrt(distSq) / 100.0f;
            const float score = angleDeg + distMeters * 0.01f;
            if (score < bestScore) {
                bestScore = score;
                bestNode = cur;
                bestTarget = tgt;
            }
        }
        cur = cur->next;
    }

    if (!bestNode) return;

    m_aimbotLockedKey = bestNode->playerKey;
    float lockDistSq = 0.0f, lockPitch = 0.0f, lockYaw = 0.0f;
    calcAim(bestTarget, lockDistSq, lockPitch, lockYaw);
    writeAimbotLog("[Aimbot] New lock: key=%u name=%s hp=%.0f dist=%.0fm score=%.1f",
        bestNode->playerKey, bestNode->playerName.c_str(), bestNode->health,
        std::sqrt(lockDistSq) / 100.0f, bestScore);

    // 首次锁定也用平滑 (不瞬移), 应用弹道物理预测
    FVector3 targetVel = getTargetVelocity(bestNode->characterPtr);
    WeaponBulletParams wp = getLocalWeaponBulletParams();
    FVector3 predictedTarget = predictBallisticAimPoint({camX, camY, camZ}, bestTarget, targetVel, wp);
    float aimPitch = 0.0f, aimYaw = 0.0f, distSq = 0.0f;
    if (calcAim(predictedTarget, distSq, aimPitch, aimYaw)) {
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

    // 进入 InProgress 后再额外延迟 10 秒, 等游戏自身完成 SkeletalMesh / Avatar / 物资生成,
    // 避免我们和游戏自身初始化抢占 process_vm_readv 流量, 导致 CrashSight 检测崩溃.
    if (m_matchEnterTickMs != 0) {
        const uint64_t elapsedMs = nowMonotonicMs() - m_matchEnterTickMs;
        constexpr uint64_t kPostMatchDelayMs = 10000;
        if (elapsedMs < kPostMatchDelayMs) {
            static Clock::time_point s_lastDelayLogTime;
            if (shouldLogEvery(s_lastDelayLogTime, std::chrono::milliseconds(2000))) {
                LOG(LOG_LEVEL_INFO, "[Grace] InProgress 后延迟 10s 等待游戏初始化 (剩余 %llu ms)",
                    static_cast<unsigned long long>(kPostMatchDelayMs - elapsedMs));
            }
            return;
        }
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

    bool ranSlowPath = false;
    if (shouldRunSlowPath) {
        const int count = updatePlayerList(ms.gameStatePtr);
        if (count <= 0 && m_playerList.size() <= 0) {
            return;
        }
        ranSlowPath = true;
    } else {
        refreshTrackedPlayersFast();
    }

    if (ranSlowPath && m_playerList.size() > 0) {
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

    // 场景物资 / 载具 / 空投扫描 (内部 500ms 节流)
    scanWorldObjects(drawData.worldObjects);

    // 自瞄: 在刷新位置和相机数据后执行 (恢复模式下跳过)
    if (!m_memoryRestored.load(std::memory_order_acquire)) {
        // 检查 GUI 恢复请求
        if (ue4draw::SharedUE4Data::getInstance().isRestoreRequested()) {
            ue4draw::SharedUE4Data::getInstance().clearRestoreRequest();
            restoreAllModifiedMemory();
        } else {
            aimAtNearestEnemy();
        }
    }

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

        // 自己 (本地玩家): 用 playerKey + pawn 双重判定, 任一命中即视为自己,
        // 永远不绘制. 之前仅靠 m_myPlayerKey,偶发为 0 时第三人称视角下
        // 自己角色会被错画成红色敌人方框.
        const bool isSelf = (m_myPlayerKey != 0 && cur->playerKey == m_myPlayerKey)
                         || (m_myPawn != 0 && cur->characterPtr == m_myPawn);
        if (isSelf) {
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
        // 节流: 每个玩家每 SKELETON_RESOLVE_INTERVAL_MS 毫秒最多尝试一次, 大幅降低
        // process_vm_readv 调用总量, 避免反作弊侦测.
        const uint64_t skelNowMs = nowMonotonicMs();
        bool freshBones = false;
        if (skelNowMs - cur->lastSkeletonAttemptMs >= SKELETON_RESOLVE_INTERVAL_MS) {
            cur->lastSkeletonAttemptMs = skelNowMs;
            freshBones = fillPlayerSkeleton(cur->characterPtr, dp);
        }
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

    ue4draw::SharedUE4Data::getInstance().pushData(std::move(drawData));
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

    while (m_running.load(std::memory_order_acquire)) {
        const auto now = Clock::now();
        bool stateChanged = false;

        if (lastStatePollTime.time_since_epoch().count() == 0
            || now - lastStatePollTime >= std::chrono::milliseconds(POLL_INTERVAL_MS)) {
            lastStatePollTime = now;

            // GNames: NumElements @ m_gNames + 0x1400
            m_numNames = safeReadS32(m_gNames + 0x1400);

            MatchState ms = getMatchState();
            lastKnownState = ms;

            if (!m_isInMatch && ms.needsPlayerConfirmation) {
                ms.inMatch = false;
                static uint64_t s_lastCandidateLogMs = 0;
                const uint64_t candidateNowMs = nowMonotonicMs();

                std::string pawnClass;
                uintptr_t localPawn = 0;
                if (m_off.PC_AcknowledgedPawn >= 0) {
                    const uintptr_t pc = getLocalPlayerController();
                    if (pc >= 0x10000) {
                        localPawn = safeReadPtr(pc + m_off.PC_AcknowledgedPawn);
                        if (localPawn >= 0x10000) {
                            pawnClass = readClassName(localPawn);
                        }
                    }
                }

                // 放宽 pawn 类名匹配: PUBG 的本地 Pawn 类名经常是
                // STExtraXXX_C / BP_xxx_C 等不含 Player/Pawn/Character 关键字,
                // 之前的关键字白名单会让 "已开局" 延迟到第一次成功匹配为止.
                // 现在只要本地 PlayerController 已经 ack 了一个非 Lobby pawn,
                // 就视为已开局.
                const bool isLobbyPawn = pawnClass.find("Lobby") != std::string::npos;
                const bool isGamePawn = !isLobbyPawn && localPawn >= 0x10000;
                if (isGamePawn) {
                    ms.inMatch = true;
                    LOG(LOG_LEVEL_INFO,
                        "[MatchCandidate] confirmed by local pawn=%p class=%s state=%s world=%s playerArray=%d",
                        (void*)localPawn, pawnClass.c_str(), ms.state.c_str(), ms.worldName.c_str(), ms.playerArrayNum);
                } else if (candidateNowMs - s_lastCandidateLogMs > 3000) {
                    s_lastCandidateLogMs = candidateNowMs;
                    LOG(LOG_LEVEL_INFO,
                        "[MatchCandidate] waiting local pawn=%p class=%s state=%s world=%s playerArray=%d",
                        (void*)localPawn, pawnClass.c_str(), ms.state.c_str(), ms.worldName.c_str(), ms.playerArrayNum);
                }
                lastKnownState = ms;
            }

            bool wasInMatch = m_isInMatch;
            stateChanged = (ms.state != m_lastMatchState) || (ms.inMatch != wasInMatch);
            if (stateChanged) {
                m_isInMatch = ms.inMatch;

                if (m_isInMatch && !wasInMatch) {
                    LOG(LOG_LEVEL_INFO, "Entered match! State=%s World=%s", ms.state.c_str(), ms.worldName.c_str());
                    ue4draw::SharedUE4Data::getInstance().setInMatch(true);
                    // 立即推送空帧标记 inMatch=true, 否则 GUI 端 getData() 取到的
                    // 双缓冲快照是上一局结束时 push 的 inMatch=false (warmup 60s
                    // 内 pollPlayers 不跑, 不会再 pushData), 导致菜单 "已进入对局"
                    // 提示要等 60s 才出现.
                    {
                        ue4draw::DrawGameData enterData;
                        enterData.inMatch = true;
                        ue4draw::SharedUE4Data::getInstance().pushData(enterData);
                    }
                    m_playerList.clear();
                    m_lastNetCullPatchMs.clear();
                    m_netCullOriginals.clear();
                    m_characterClassSet.clear();
                    m_boneAssetCache.clear();
                    m_weaponParamsCache.clear();
                    m_cachedBulletParams = WeaponBulletParams{};
                    m_lastBulletParamReadMs = 0;
                    m_memoryRestored.store(false, std::memory_order_release);
                    ue4draw::SharedUE4Data::getInstance().setMemoryRestored(false);
                    ue4draw::SharedUE4Data::getInstance().clearRestoreRequest();
                    ue4draw::SharedUE4Data::getInstance().setAimbotEnabled(false);
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
                    // 对局结束: 先恢复所有修改的内存, 再清理状态
                    if (!m_memoryRestored.load(std::memory_order_acquire)) {
                        restoreAllModifiedMemory();
                    }
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
                    m_netCullOriginals.clear();
                    m_characterClassSet.clear();
                    m_boneAssetCache.clear();
                    m_weaponParamsCache.clear();
                    m_cachedBulletParams = WeaponBulletParams{};
                    m_lastBulletParamReadMs = 0;
                    m_memoryRestored.store(false, std::memory_order_release);
                    ue4draw::SharedUE4Data::getInstance().setMemoryRestored(false);
                    ue4draw::SharedUE4Data::getInstance().clearRestoreRequest();
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
            // 进入对局后等待 60 秒再开始遍历, 避免 ACE 反作弊在加载期检测到扫描行为
            constexpr uint64_t kPostEnterWarmupMs = 60000;
            if (m_matchEnterTickMs != 0
                && (nowMonotonicMs() - m_matchEnterTickMs) < kPostEnterWarmupMs) {
                static uint64_t s_lastWarmupLogMs = 0;
                const uint64_t nowLogMs = nowMonotonicMs();
                if (nowLogMs - s_lastWarmupLogMs > 2000) {
                    s_lastWarmupLogMs = nowLogMs;
                    const uint64_t leftMs = kPostEnterWarmupMs - (nowLogMs - m_matchEnterTickMs);
                    LOG(LOG_LEVEL_INFO, "[Warmup] entered match, waiting %llums before traversal",
                        (unsigned long long)leftMs);
                }
                // warmup 期间也要心跳推送, 否则 GUI 端 stale 检查 (>5s) 会把
                // inMatch 强制覆写回 false, 让 "对局中" 标签在开局 ~5s 后消失.
                ue4draw::DrawGameData warmupData;
                warmupData.inMatch = true;
                ue4draw::SharedUE4Data::getInstance().pushData(warmupData);
            } else {
                pollPlayers();
            }
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

    // 防御性 kill switch (A): 通过 system property 一键禁用 MatchMonitor.
    //   adb shell setprop debug.pubgmhd.nohook 1   后重启 app -> 不启动 hook.
    //   adb shell setprop debug.pubgmhd.nohook 0   恢复.
    // 用于现场二分: 关掉后若仍崩 = 与 hook 无关; 若不崩 = hook 是肇事者.
    {
        char propVal[8] = {0};
        __system_property_get("debug.pubgmhd.nohook", propVal);
        if (propVal[0] == '1') {
            LOG(LOG_LEVEL_WARN, "[KillSwitch] debug.pubgmhd.nohook=1, 跳过 MatchMonitor::start");
            return false;
        }
    }

    // 验证 GNames (NumElements @ m_gNames + 0x1400)
    m_numNames = safeReadS32(m_gNames + 0x1400);
    if (m_numNames <= 0) {
        LOG(LOG_LEVEL_ERROR, "GNames 无效, numNames=%d", m_numNames);
        return false;
    }
    LOG(LOG_LEVEL_INFO, "Base=%p GNames=%p numNames=%d GWorld=%p GUObjectArray=%p",
        (void*)m_moduleBase, (void*)m_gNames, m_numNames, (void*)m_gWorld, (void*)m_gUObjectArray);

    // ── D 方案 (寄生执行) 自检 — 已停用 ──
    // 用户要求停掉 parasite 接入. 如需重新启用, 取消下方注释即可.
    // {
    //     bool ok = ::selfTestParasite();
    //     LOG(LOG_LEVEL_INFO, "[Parasite] selfTest=%d", (int)ok);
    // }

    // 验证 entry[0] == "None"
    std::string entry0 = getNameByIndex(0);
    LOG(LOG_LEVEL_INFO, "Entry[0]='%s' %s", entry0.c_str(), (entry0 == "None") ? "OK" : "BAD");

    // 通过 UE4Interface 动态解析所有游戏偏移
    if (!initOffsets()) {
        LOG(LOG_LEVEL_ERROR, "偏移解析失败, 无法启动监控");
        return false;
    }
    computeBatchReadBounds();

    // 预热 stack_spoof 返回地址池: 扫 libc/libart/libandroid_runtime/libutils 的
    // r-x 段, 收集 BL/BLR 之后的合法返回地址, 供 getMatchState 中的伪造 FP chain
    // 复用. 提前在这里调用避免首次 spoof 时被扫描成本拖慢 poll 周期.
    if (!stack_spoof::init()) {
        LOG(LOG_LEVEL_WARN, "[StackSpoof] 返回地址池初始化失败, call_spoofed 将退化为直调");
    } else {
        LOG(LOG_LEVEL_INFO, "[StackSpoof] 返回地址池就绪");
    }

    m_running.store(true, std::memory_order_release);
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

    m_pollThread = std::thread(&MatchMonitor::pollMatchStateLoop, this);
    m_pollThread.detach();  // detach: MatchMonitor 在当前架构下永不被 delete
    LOG(LOG_LEVEL_INFO, "=== 对局监控+玩家采集已启动 ===");
    return true;
}

void MatchMonitor::stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_running.store(false, std::memory_order_release);
    // 线程已 detach, 等待其自行退出 (轮询周期 + 余量)
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
