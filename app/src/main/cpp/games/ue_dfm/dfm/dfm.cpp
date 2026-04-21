/**
 * @file    dfm.cpp
 * @brief   三角洲行动 (DFM) 对局状态监控 + 玩家/物资采集 实现
 *          从 loot_scan.js (Frida) 转写, 遵循 pubgmhd.cpp 代码规范
 */
#include "dfm.h"
#include "../../../core/log/log.h"
#include "../engine/UE5DfmStruct.h"
#include "../interface/interface.h"
#include "dfm_item_registry.h"

// IM_COL32 兼容宏 (避免引入 imgui.h)
#ifndef IM_COL32
#define IM_COL32(R,G,B,A) (((uint32_t)(A)<<24)|((uint32_t)(B)<<16)|((uint32_t)(G)<<8)|((uint32_t)(R)))
#endif

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <cctype>
#include <cmath>
#include <chrono>
#include <atomic>
#include <algorithm>

#define TAG "[DFM]"

using namespace ue5dfm;

namespace dfm {

namespace {

constexpr int kMinRenderableBoneCount = 21;
constexpr int kMaxRenderableBoneCount = 256;
constexpr int kMaxMasterPoseDepth = 4;
constexpr int kMinTrackedBoneMatches = 4;
constexpr uintptr_t kReplicatedMovementLocationOffset = 0x18;
constexpr uintptr_t kEncHandlerEncryptedFlagOffset = 0x0E;
constexpr uintptr_t kSceneComponentGetSocketLocationVtableOffset = 0x468;

// SceneComponent.ComponentToWorld 解密 trampoline (通过 K2_GetActorLocation 反编译定位):
//   K2_GetActorLocation @ 0xEA2679C 内核心三步:
//     X8 = actor + 0x180; X0 = atomic_load(X8)        // RootComponent
//     BL  sub_D982DB0(rootComp)                       // 返回解密后的 FTransform*
//     读取返回值 +0x10/+0x14/+0x18 = X/Y/Z
//   sub_D982DB0 @ 0xD982DB0 (3 条指令的 vthunk):
//     ADD X0, X0, #0x210                              // sceneComp + 0x210 = &ComponentToWorld
//     LDR X16, =0xB400006Fxxxxxxxx                    // 字面量池中堆地址 (运行时被反作弊改写)
//     BR  X16                                         // 跳到堆里的解密 stub
// 后果:
//   * SceneComponent.ComponentToWorld (sceneComp+0x210) 在内存里以加密/混淆形式存储。
//   * 直接读字节得到的 X/Y/Z 对本地 Pawn / 简单 AI 是"刚被引擎写回"的明文 (因为引擎
//     物理 Tick 也走同一条 hook 路径), 但对远端 replicated Pawn 几十秒才更新一次,
//     读到的字节会陈旧到与真实位置完全错位。
//   * 想拿到当前真实坐标必须调 sub_D982DB0(sceneComp), 它会让堆 stub 现场解密并返回
//     一个明文 FTransform 指针。
constexpr uintptr_t kSceneComponentDecryptCTWThunkOffset = 0xD982DB0;
using DecryptComponentToWorldFn = const void* (*)(uintptr_t sceneComp);

constexpr size_t kTrackedBoneHeadSlot = 0;
constexpr size_t kTrackedBonePelvisSlot = 4;
constexpr size_t kTrackedBoneRightFootSlot = 13;
constexpr size_t kTrackedBoneLeftFootSlot = 16;
constexpr FName kInvalidTrackedBoneName{-1, 0};

using BoneAliasList = std::array<const char*, 8>;

const std::array<BoneAliasList, PlayerInfo::BONE_COUNT> kTrackedBoneAliases = {{
    BoneAliasList{"head", "head01", "head02", "bip001head", "bip01head", nullptr, nullptr, nullptr},
    BoneAliasList{"neck01", "neck", "neck02", "bip001neck", "bip01neck", nullptr, nullptr, nullptr},
    BoneAliasList{"spine03", "spine3", "spine02", "spine2", "chest", "spine03jnt", "bip001spine2", "bip01spine2"},
    BoneAliasList{"spine01", "spine1", "spine", "bip001spine", "bip01spine", nullptr, nullptr, nullptr},
    // pelvis: Mixamo "Hips" 才是真正的骨盆; "root" 是世界原点(在脚下), 必须排除
    BoneAliasList{"pelvis", "hips", "bip001pelvis", "bip01pelvis", "hip", nullptr, nullptr, nullptr},
    BoneAliasList{"upperarmr", "rupperarm", "rightarm", "rightupperarm", "clavicler", "bip001rupperarm", "bip01rupperarm", nullptr},
    BoneAliasList{"lowerarmr", "rlowerarm", "rightforearm", "rightlowerarm", "forearmr", "bip001rforearm", "bip01rforearm", nullptr},
    BoneAliasList{"handr", "rhand", "righthand", "bip001rhand", "bip01rhand", nullptr, nullptr, nullptr},
    BoneAliasList{"upperarml", "lupperarm", "leftarm", "leftupperarm", "claviclel", "bip001lupperarm", "bip01lupperarm", nullptr},
    BoneAliasList{"lowerarml", "llowerarm", "leftforearm", "leftlowerarm", "forearml", "bip001lforearm", "bip01lforearm", nullptr},
    BoneAliasList{"handl", "lhand", "lefthand", "bip001lhand", "bip01lhand", nullptr, nullptr, nullptr},
    BoneAliasList{"thighr", "rthigh", "rightupleg", "rightthigh", "bip001rthigh", "bip01rthigh", nullptr, nullptr},
    BoneAliasList{"calfr", "rcalf", "rightleg", "rightlowerleg", "bip001rcalf", "bip01rcalf", nullptr, nullptr},
    BoneAliasList{"footr", "rfoot", "rightfoot", "bip001rfoot", "bip01rfoot", nullptr, nullptr, nullptr},
    BoneAliasList{"thighl", "lthigh", "leftupleg", "leftthigh", "bip001lthigh", "bip01lthigh", nullptr, nullptr},
    BoneAliasList{"calfl", "lcalf", "leftleg", "leftlowerleg", "bip001lcalf", "bip01lcalf", nullptr, nullptr},
    BoneAliasList{"footl", "lfoot", "leftfoot", "bip001lfoot", "bip01lfoot", nullptr, nullptr, nullptr},
}};

struct RemoteWeakObjectPtr {
    int32_t objectIndex = -1;
    int32_t objectSerialNumber = 0;
};
static_assert(sizeof(RemoteWeakObjectPtr) == 0x8, "RemoteWeakObjectPtr size mismatch");

template<typename T>
bool isUsableRemoteArray(const TArray<T>& array, int maxNum) {
    const uintptr_t dataPtr = reinterpret_cast<uintptr_t>(array.Data);
    return dataPtr > 0x10000 && array.Num > 0 && array.Num <= maxNum && array.Max >= array.Num;
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

FVector3 rotateVector(const FTransform& rotation, const FVector3& value) {
    const FVector3 quatVector{rotation.RotationX, rotation.RotationY, rotation.RotationZ};
    const FVector3 uv = crossProduct(quatVector, value);
    const FVector3 uuv = crossProduct(quatVector, uv);
    return {
        value.x + ((uv.x * rotation.RotationW) + uuv.x) * 2.0f,
        value.y + ((uv.y * rotation.RotationW) + uuv.y) * 2.0f,
        value.z + ((uv.z * rotation.RotationW) + uuv.z) * 2.0f,
    };
}

FVector3 transformPosition(const FTransform& transform, const FVector3& localPosition) {
    FVector3 safeScale{transform.Scale3DX, transform.Scale3DY, transform.Scale3DZ};
    if (!std::isfinite(safeScale.x) || std::fabs(safeScale.x) < 0.0001f) safeScale.x = 1.0f;
    if (!std::isfinite(safeScale.y) || std::fabs(safeScale.y) < 0.0001f) safeScale.y = 1.0f;
    if (!std::isfinite(safeScale.z) || std::fabs(safeScale.z) < 0.0001f) safeScale.z = 1.0f;

    const FVector3 scaled = scaleVector(localPosition, safeScale);
    const FVector3 rotated = rotateVector(transform, scaled);
    return {
        rotated.x + transform.TranslationX,
        rotated.y + transform.TranslationY,
        rotated.z + transform.TranslationZ,
    };
}

bool hasUsableWorldPoint(const FVector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
        && std::fabs(value.x) < 1.0e8f && std::fabs(value.y) < 1.0e8f && std::fabs(value.z) < 1.0e8f;
}

bool hasUsableActorPoint(const FVector3& value) {
    return hasUsableWorldPoint(value)
        && (std::fabs(value.x) > 1.0f || std::fabs(value.y) > 1.0f || std::fabs(value.z) > 1.0f);
}

constexpr float kRootCompensationZBias = 958.0f;
constexpr float kRootCompensationXYQuantize = 10.0f;
constexpr size_t kMaxCachedActorLocations = 4096;
constexpr size_t kMaxRootCompensationOwners = 2048;
constexpr auto kRootCompensationHoldWindow = std::chrono::milliseconds(1500);

uint64_t makeQuantizedXYKey(float x, float y) {
    const int32_t qx = static_cast<int32_t>(std::lround(x / kRootCompensationXYQuantize));
    const int32_t qy = static_cast<int32_t>(std::lround(y / kRootCompensationXYQuantize));
    return (static_cast<uint64_t>(static_cast<uint32_t>(qx)) << 32)
        | static_cast<uint32_t>(qy);
}

float distanceMeters(const FVector3& lhs, const FVector3& rhs) {
    const double dx = static_cast<double>(lhs.x) - static_cast<double>(rhs.x);
    const double dy = static_cast<double>(lhs.y) - static_cast<double>(rhs.y);
    const double dz = static_cast<double>(lhs.z) - static_cast<double>(rhs.z);
    return static_cast<float>(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)) / 100.0);
}

bool deriveFootPositionFromBones(const PlayerInfo& player, FVector3& outPos) {
    if (!player.bonesValid) return false;

    const FVector3& pelvis = player.bones[4];
    const FVector3& head = player.bones[0];
    const FVector3& rFoot = player.bones[13];
    const FVector3& lFoot = player.bones[16];

    const bool pelvisValid = hasUsableActorPoint(pelvis);
    const bool headValid = hasUsableActorPoint(head);
    const bool rFootValid = hasUsableActorPoint(rFoot);
    const bool lFootValid = hasUsableActorPoint(lFoot);

    if (rFootValid && lFootValid) {
        outPos.x = (rFoot.x + lFoot.x) * 0.5f;
        outPos.y = (rFoot.y + lFoot.y) * 0.5f;
        outPos.z = std::min(rFoot.z, lFoot.z);
        return true;
    }
    if (rFootValid) {
        outPos = rFoot;
        return true;
    }
    if (lFootValid) {
        outPos = lFoot;
        return true;
    }
    if (pelvisValid) {
        outPos.x = pelvis.x;
        outPos.y = pelvis.y;
        outPos.z = pelvis.z - 90.0f;
        return true;
    }
    if (headValid) {
        outPos.x = head.x;
        outPos.y = head.y;
        outPos.z = head.z - 160.0f;
        return true;
    }
    return false;
}

bool isLikelyMeshComponentClass(const std::string& className) {
    return className.find("SkeletalMeshComponent") != std::string::npos
        || className.find("SkinnedMeshComponent") != std::string::npos
        || className.find("MeshComponentBudgeted") != std::string::npos;
}

std::string normalizeBoneName(const std::string& value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char ch : value) {
        if (!std::isalnum(ch)) continue;
        normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
    return normalized;
}

bool endsWithText(const std::string& value, const char* suffix) {
    if (suffix == nullptr) return false;
    const size_t suffixLength = std::strlen(suffix);
    return value.size() >= suffixLength
        && value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
}

bool matchesTrackedBoneName(size_t slot, const std::string& normalizedName) {
    for (const char* alias : kTrackedBoneAliases[slot]) {
        if (alias == nullptr) break;
        if (normalizedName == alias
            || endsWithText(normalizedName, alias)
            || (std::strlen(alias) >= 6 && normalizedName.find(alias) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

} // namespace

// =====================================================================
//  安全内存读取 (sigsetjmp/siglongjmp 防崩溃)
// =====================================================================

static thread_local sigjmp_buf s_safeReadJmpBuf;
static thread_local volatile sig_atomic_t s_safeReadActive = 0;
static struct sigaction s_oldSigsegvAction;
static struct sigaction s_oldSigbusAction;
static std::once_flag s_safeReadGuardOnce;

static void safeReadSignalHandler(int sig, siginfo_t* info, void* ctx) {
    if (s_safeReadActive) {
        s_safeReadActive = 0;
        siglongjmp(s_safeReadJmpBuf, sig);
    }
    struct sigaction* old = (sig == SIGSEGV) ? &s_oldSigsegvAction : &s_oldSigbusAction;
    if (old->sa_flags & SA_SIGINFO) old->sa_sigaction(sig, info, ctx);
    else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) old->sa_handler(sig);
    else { signal(sig, SIG_DFL); raise(sig); }
}

static void installSafeReadGuard() {
    std::call_once(s_safeReadGuardOnce, []() {
        struct sigaction sa{};
        sa.sa_sigaction = safeReadSignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &s_oldSigsegvAction);
        sigaction(SIGBUS,  &sa, &s_oldSigbusAction);
    });
}

bool DfmMatchMonitor::safeReadMemory(uintptr_t addr, void* out, size_t size) {
    if (addr == 0 || addr < 0x10000) return false;
    installSafeReadGuard();
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) { s_safeReadActive = 0; return false; }
    s_safeReadActive = 1;
    memcpy(out, reinterpret_cast<const void*>(addr), size);
    s_safeReadActive = 0;
    return true;
}

uintptr_t DfmMatchMonitor::safeReadPtr(uintptr_t addr) {
    uintptr_t v = 0; safeReadMemory(addr, &v, sizeof(v)); return v;
}
int32_t DfmMatchMonitor::safeReadS32(uintptr_t addr) {
    int32_t v = 0; safeReadMemory(addr, &v, sizeof(v)); return v;
}
uint32_t DfmMatchMonitor::safeReadU32(uintptr_t addr) {
    uint32_t v = 0; safeReadMemory(addr, &v, sizeof(v)); return v;
}
uint8_t DfmMatchMonitor::safeReadU8(uintptr_t addr) {
    uint8_t v = 0; safeReadMemory(addr, &v, sizeof(v)); return v;
}
float DfmMatchMonitor::safeReadFloat(uintptr_t addr) {
    float v = 0; safeReadMemory(addr, &v, sizeof(v)); return v;
}
double DfmMatchMonitor::safeReadDouble(uintptr_t addr) {
    double v = 0; safeReadMemory(addr, &v, sizeof(v)); return v;
}

static inline bool ok(uintptr_t p) { return p != 0 && p > 0x10000; }

// =====================================================================
//  NamePool 解码 (NOT+XOR 混淆, 9-case mask)
// =====================================================================

uint8_t DfmMatchMonitor::amask(int l) {
    switch (l % 9) {
        case 0: return ((l & 0x1f) + l) & 0x80;
        case 1: return ((l ^ 0xdf) + l) & 0x80;
        case 2: return ((l | 0xcf) + l) & 0x80;
        case 3: return (33 * l) & 0x80;
        case 4: return (l + (l >> 2)) & 0x80;
        case 5: return (3 * l + 5) & 0x80;
        case 6: return (((l << 2) | 5) + l) & 0x80;
        case 7: return (((l >> 4) | 7) + l) & 0x80;
        case 8: return ((l ^ 0x0c) + l) & 0x80;
        default: return ((l ^ 0x40) + l) & 0x80;
    }
}

std::string DfmMatchMonitor::resolveName(uint32_t id) const {
    if (id == 0) return "None";
    uintptr_t pool = m_moduleBase + m_offNamePool;
    uint32_t bi = id >> FNameEntryAllocator::OffsetBits;
    uint32_t bo = (id & 0x3FFFF) << 1;

    uintptr_t bp = safeReadPtr(pool + offsetof(FNameEntryAllocator, Blocks) + static_cast<uintptr_t>(bi) * 8);
    if (!ok(bp)) return "?";

    uint16_t hdr = 0;
    safeReadMemory(bp + bo, &hdr, sizeof(hdr));
    if (hdr == 0) return "?";

    int length = hdr >> 6;
    if (length <= 0 || length > 1024) return "?";

    uintptr_t raw = bp + bo + 2;
    uint8_t mk = amask(length);

    std::string out;
    out.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; i++) {
        uint8_t b = 0;
        safeReadMemory(raw + static_cast<uintptr_t>(i), &b, 1);
        out += static_cast<char>((0xFF ^ b ^ mk) & 0xFF);
    }
    return out;
}

std::string DfmMatchMonitor::readFName(uintptr_t addr) const {
    uint32_t c = safeReadU32(addr);
    uint32_t n = safeReadU32(addr + offsetof(FName, Number));
    std::string s = resolveName(c);
    if (n > 0) { s += "_"; s += std::to_string(n - 1); }
    return s;
}

std::string DfmMatchMonitor::readObjName(uintptr_t objPtr) const {
    return readFName(objPtr + offsetof(UObjectBase, NamePrivate));
}

std::string DfmMatchMonitor::readClassName(uintptr_t objPtr) const {
    uintptr_t cls = safeReadPtr(objPtr + offsetof(UObjectBase, ClassPrivate));
    return ok(cls) ? readObjName(cls) : "?";
}

// =====================================================================
//  FString 读取 (UTF-16LE → UTF-8, 支持中文)
// =====================================================================

std::string DfmMatchMonitor::readFString(uintptr_t addr) {
    uintptr_t dataPtr = safeReadPtr(addr);
    if (!ok(dataPtr)) return "";
    int32_t num = safeReadS32(addr + offsetof(FString, Num));
    if (num <= 0 || num > 512) return "";

    // 读取 UTF-16 数据
    std::vector<uint16_t> wbuf(static_cast<size_t>(num));
    if (!safeReadMemory(dataPtr, wbuf.data(), static_cast<size_t>(num) * 2)) return "";

    // UTF-16LE → UTF-8 完整转换 (支持中文/日文/韩文等)
    std::string result;
    result.reserve(static_cast<size_t>(num) * 3);
    for (int i = 0; i < num - 1; i++) {
        uint16_t ch = wbuf[static_cast<size_t>(i)];
        if (ch == 0) break;
        if (ch < 0x80) {
            result += static_cast<char>(ch);
        } else if (ch < 0x800) {
            result += static_cast<char>(0xC0 | (ch >> 6));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        } else if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < num - 1) {
            // UTF-16 surrogate pair → UTF-8 (4 bytes)
            uint16_t lo = wbuf[static_cast<size_t>(i + 1)];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                uint32_t cp = 0x10000 + ((static_cast<uint32_t>(ch - 0xD800) << 10) | (lo - 0xDC00));
                result += static_cast<char>(0xF0 | (cp >> 18));
                result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (cp & 0x3F));
                i++; // skip low surrogate
            }
        } else {
            result += static_cast<char>(0xE0 | (ch >> 12));
            result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (ch & 0x3F));
        }
    }
    return result;
}

// [修复] 新增: FText 读取 — 从 InteractorBase.InteractorName 获取本地化物品名
// FText 读取 (UE5 FText 内部: FTextData* → SourceString/DisplayString)
// FText Size=0x18: 通常 +0x0 是 FTextData* (SharedReferenceCount 之后是 FString)
// 简化策略: 直接将 FText 地址当 FString 尝试读取, 回退到 FTextData* → FString
std::string DfmMatchMonitor::readFText(uintptr_t addr) const {
    if (!ok(addr)) return "";

    // 策略1: FText 内部可能直接包含 FString (某些 UE5 版本)
    std::string direct = readFString(addr);
    if (!direct.empty() && direct != "None" && direct.length() > 1) return direct;

    // 策略2: FText → FTextData* (第一个指针) → 内部 FString
    uintptr_t textData = safeReadPtr(addr);
    if (ok(textData)) {
        // FTextData 内部: 通常 +0x28 或 +0x30 位置有 DisplayString (FString)
        static const uint32_t kStrCandidates[] = {0x28, 0x30, 0x38, 0x40, 0x48};
        for (uint32_t off : kStrCandidates) {
            std::string s = readFString(textData + off);
            if (!s.empty() && s != "None" && s.length() > 1) return s;
        }
    }
    return "";
}

// =====================================================================
//  坐标读取 (来自 IDA K2_GetActorLocation 反编译)
//  优先级: RootComponent.CTW → ReplicatedMovement.Location(plain) → CameraViewLoc → Root.CTW+958 → RelativeLocation
// =====================================================================

bool DfmMatchMonitor::tryGetCachedActorLocation(uintptr_t actorPtr, FVector3& outLoc) const {
    const auto cached = m_lastKnownActorPositions.find(actorPtr);
    if (cached == m_lastKnownActorPositions.end() || !hasUsableActorPoint(cached->second)) {
        return false;
    }
    outLoc = cached->second;
    return true;
}

void DfmMatchMonitor::rememberActorLocation(uintptr_t actorPtr, const FVector3& location) const {
    if (!ok(actorPtr) || !hasUsableActorPoint(location)) {
        return;
    }
    if (m_lastKnownActorPositions.size() >= kMaxCachedActorLocations) {
        m_lastKnownActorPositions.clear();
    }
    m_lastKnownActorPositions[actorPtr] = location;
}

DfmMatchMonitor::ReplicatedMovementSnapshot
DfmMatchMonitor::sampleReplicatedMovement(uintptr_t actorPtr) const {
    ReplicatedMovementSnapshot snapshot{};
    if (!ok(actorPtr) || m_off.Actor_ReplicatedMovement <= 0) {
        return snapshot;
    }

    // bReplicateMovement 是位字段, sdk_dump 标注 bit5 (Flags 含 EReplicate 位)
    // 旧代码读整字节比较 != 0, 0x90 上还堆着 bHidden / bReplicates / bAlwaysRelevant 等多个 bit,
    // 只要任一 bit 被置就误判为开启 — 修正为只检查 bit5.
    if (m_off.Actor_bReplicateMovement <= 0) {
        snapshot.movementEnabled = true;
    } else {
        const uint8_t bits = safeReadU8(actorPtr + static_cast<uintptr_t>(m_off.Actor_bReplicateMovement));
        snapshot.movementEnabled = (bits & 0x20) != 0; // bit5 = bReplicateMovement
    }

    const uintptr_t repMoveBase = actorPtr + static_cast<uintptr_t>(m_off.Actor_ReplicatedMovement);
    snapshot.location.x = safeReadFloat(repMoveBase + kReplicatedMovementLocationOffset + 0x00);
    snapshot.location.y = safeReadFloat(repMoveBase + kReplicatedMovementLocationOffset + 0x04);
    snapshot.location.z = safeReadFloat(repMoveBase + kReplicatedMovementLocationOffset + 0x08);
    // EncVector.EncHandler @ +0xC: int16 Index@+0, int8 bEncrypted@+2, bitfield@+3
    snapshot.encByte = safeReadU8(repMoveBase + kReplicatedMovementLocationOffset + kEncHandlerEncryptedFlagOffset);
    snapshot.hasUsablePlainLocation = snapshot.movementEnabled
        && snapshot.encByte == 0
        && hasUsableActorPoint(snapshot.location);
    return snapshot;
}

bool DfmMatchMonitor::shouldAcceptRootCompensation(uintptr_t actorPtr, float x, float y) const {
    if (!ok(actorPtr) || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const uint64_t key = makeQuantizedXYKey(x, y);

    auto slot = m_rootCompensationOwners.find(key);
    if (slot != m_rootCompensationOwners.end()) {
        if ((now - slot->second.seenAt) <= kRootCompensationHoldWindow
            && slot->second.actorPtr != 0
            && slot->second.actorPtr != actorPtr) {
            return false;
        }
        slot->second.actorPtr = actorPtr;
        slot->second.seenAt = now;
        return true;
    }

    if (m_rootCompensationOwners.size() >= kMaxRootCompensationOwners) {
        for (auto it = m_rootCompensationOwners.begin(); it != m_rootCompensationOwners.end();) {
            if ((now - it->second.seenAt) > kRootCompensationHoldWindow) {
                it = m_rootCompensationOwners.erase(it);
                continue;
            }
            ++it;
        }
        if (m_rootCompensationOwners.size() >= kMaxRootCompensationOwners) {
            m_rootCompensationOwners.clear();
        }
    }

    m_rootCompensationOwners[key] = {actorPtr, now};
    return true;
}

bool DfmMatchMonitor::getActorLocation(uintptr_t actorPtr, FVector3& outLoc) const {
    const FVector3 previousLoc = outLoc;
    outLoc = {};
    if (!ok(actorPtr)) {
        if (tryGetCachedActorLocation(actorPtr, outLoc)) return true;
        if (hasUsableActorPoint(previousLoc)) { outLoc = previousLoc; return true; }
        return false;
    }
    uintptr_t root = safeReadPtr(actorPtr + m_off.Actor_RootComponent);

    // ── 方法 -1: RootComponent.ComponentToWorld 通过解密 trampoline ──
    // SceneComponent+0x210 的 FTransform 在内存里加密存储, 直接读字节对远端
    // replicated Pawn 几十秒才更新一次会严重错位; 调 sub_D982DB0 让游戏 stub 现场
    // 解密一次, 拿到的才是当前真实坐标。本地 Pawn / AI 也走这条路, 反正游戏自己
    // 就是这么读的, 不会更慢更不准。
    if (ok(root)) {
        FVector3 decryptedLoc{};
        if (decryptSceneComponentLocation(root, decryptedLoc)) {
            outLoc = decryptedLoc;
            rememberActorLocation(actorPtr, outLoc);
            static int s_cntD = 0;
            if (s_cntD++ < 30) {
                LOG(LOG_LEVEL_INFO, TAG " [getLoc] M-1 decrypt(root) actor=%p root=%p pos=(%.1f,%.1f,%.1f)",
                    (void*)actorPtr, (void*)root, outLoc.x, outLoc.y, outLoc.z);
            }
            return true;
        }
    }

    auto isUsableLocation = [](float x, float y, float z) {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z)
            && std::fabs(x) < 1e8f && std::fabs(y) < 1e8f && std::fabs(z) < 1e8f
            && (std::fabs(x) > 1.0f || std::fabs(y) > 1.0f || std::fabs(z) > 1.0f);
    };

    auto readCtwTranslation = [&](uintptr_t comp, float& tx, float& ty, float& tz,
                                  float& qx, float& qy, float& qz, float& qw) {
        const uintptr_t ctw = comp + static_cast<uintptr_t>(m_off.Scene_ComponentToWorld);
        qx = safeReadFloat(ctw + 0x00);
        qy = safeReadFloat(ctw + 0x04);
        qz = safeReadFloat(ctw + 0x08);
        qw = safeReadFloat(ctw + 0x0C);
        tx = safeReadFloat(ctw + offsetof(FTransform, TranslationX));
        ty = safeReadFloat(ctw + offsetof(FTransform, TranslationY));
        tz = safeReadFloat(ctw + offsetof(FTransform, TranslationZ));
    };

    // ── 方法 0: SkeletalMeshComponent.ComponentToWorld (优先, 渲染管线维护) ──
    // Mesh.CTW 由引擎渲染线程在客户端 LOD 内时持续刷新, 是 "可见敌人精确位置"
    // 的唯一权威源。LOD 外 actor 的 CTW 会归零 / 冻结, 此时 hasValidPos 在 ESP
    // 端会过滤掉 (宁可漏画, 也不画一个错位框)。
    // 不要把 RepMovement 放前面: RepMovement 对远端 actor 给的是服务器几十秒前
    // 最后一次同步的 spawn 区位置, 数值合法但完全错位, 会让 ESP 在地上画一堆
    // 没有玩家的框。
    if (m_off.Char_Mesh > 0) {
        const uintptr_t meshComp = resolveObjectField(actorPtr + static_cast<uintptr_t>(m_off.Char_Mesh));
        if (ok(meshComp)) {
            float mx, my, mz, mqx, mqy, mqz, mqw;
            readCtwTranslation(meshComp, mx, my, mz, mqx, mqy, mqz, mqw);
            const bool quatValid = !(std::fabs(mqx) < 0.001f && std::fabs(mqy) < 0.001f
                                  && std::fabs(mqz) < 0.001f && std::fabs(mqw) < 0.001f);
            if (isUsableLocation(mx, my, mz) && quatValid) {
                outLoc.x = mx; outLoc.y = my; outLoc.z = mz;
                rememberActorLocation(actorPtr, outLoc);
                static int s_cnt0 = 0;
                if (s_cnt0++ < 30) {
                    LOG(LOG_LEVEL_INFO, TAG " [getLoc] M0 mesh.CTW actor=%p pos=(%.1f,%.1f,%.1f)",
                        (void*)actorPtr, mx, my, mz);
                }
                return true;
            }
        }
    }

    if (!ok(root)) {
        if (tryGetCachedActorLocation(actorPtr, outLoc)) return true;
        if (hasUsableActorPoint(previousLoc)) { outLoc = previousLoc; rememberActorLocation(actorPtr, outLoc); return true; }
        return false;
    }

    // ── 方法 1: RootComponent.ComponentToWorld (四元数有效时使用) ──
    float qx, qy, qz, qw, tx, ty, tz;
    readCtwTranslation(root, tx, ty, tz, qx, qy, qz, qw);
    bool posValid = isUsableLocation(tx, ty, tz);
    bool quatValid = !(std::fabs(qx) < 0.001f && std::fabs(qy) < 0.001f
        && std::fabs(qz) < 0.001f && std::fabs(qw) < 0.001f);

    if (posValid && quatValid) {
        outLoc.x = tx; outLoc.y = ty; outLoc.z = tz;
        rememberActorLocation(actorPtr, outLoc);
        static int s_cnt1 = 0;
        if (s_cnt1++ < 30) {
            LOG(LOG_LEVEL_INFO, TAG " [getLoc] M2 root.CTW actor=%p root=%p pos=(%.1f,%.1f,%.1f)",
                (void*)actorPtr, (void*)root, tx, ty, tz);
        }
        return true;
    }

    // ── 方法 3: CameraViewLoc (明文 Vector, 但语义为视角点 — 仅作粗略 fallback) ──
    if (m_off.Char_CameraViewLoc > 0) {
        float cvx = safeReadFloat(actorPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc));
        float cvy = safeReadFloat(actorPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc) + 4);
        float cvz = safeReadFloat(actorPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc) + 8);
        if (isUsableLocation(cvx, cvy, cvz) && (std::fabs(cvx) > 1.0f || std::fabs(cvy) > 1.0f)) {
            outLoc.x = cvx; outLoc.y = cvy; outLoc.z = cvz;
            rememberActorLocation(actorPtr, outLoc);
            static int s_cnt3 = 0;
            if (s_cnt3++ < 30) {
                LOG(LOG_LEVEL_WARN, TAG " [getLoc] M3 cameraView actor=%p pos=(%.1f,%.1f,%.1f)",
                    (void*)actorPtr, cvx, cvy, cvz);
            }
            return true;
        }
    }

    // ── 方法 4: RelativeLocation (EncVector, 仅当未加密 + 无父附加 = 等价世界坐标) ──
    if (ok(root)) {
        const uintptr_t relBase = root + static_cast<uintptr_t>(m_off.Scene_RelativeLocation);
        const uint8_t encByte = safeReadU8(relBase + kEncHandlerEncryptedFlagOffset);
        const uintptr_t attachParent = m_off.Scene_AttachParent > 0
            ? safeReadPtr(root + static_cast<uintptr_t>(m_off.Scene_AttachParent)) : 0;
        if (encByte == 0 && attachParent == 0) {
            float x = safeReadFloat(relBase + 0x00);
            float y = safeReadFloat(relBase + 0x04);
            float z = safeReadFloat(relBase + 0x08);
            if (isUsableLocation(x, y, z)) {
                outLoc.x = x; outLoc.y = y; outLoc.z = z;
                rememberActorLocation(actorPtr, outLoc);
                static int s_cnt4 = 0;
                if (s_cnt4++ < 30) {
                    LOG(LOG_LEVEL_WARN, TAG " [getLoc] M4 relLoc actor=%p pos=(%.1f,%.1f,%.1f)",
                        (void*)actorPtr, x, y, z);
                }
                return true;
            }
        } else {
            static int s_cntSkip = 0;
            if (s_cntSkip++ < 10) {
                LOG(LOG_LEVEL_WARN, TAG " [getLoc] M4 skip actor=%p enc=%u attachParent=%p",
                    (void*)actorPtr, (unsigned)encByte, (void*)attachParent);
            }
        }
    }

    if (tryGetCachedActorLocation(actorPtr, outLoc)) {
        static int s_cntCache = 0;
        if (s_cntCache++ < 10) {
            LOG(LOG_LEVEL_WARN, TAG " [getLoc] cached actor=%p pos=(%.1f,%.1f,%.1f)",
                (void*)actorPtr, outLoc.x, outLoc.y, outLoc.z);
        }
        return true;
    }
    if (hasUsableActorPoint(previousLoc)) {
        outLoc = previousLoc;
        rememberActorLocation(actorPtr, outLoc);
        return true;
    }
    static int s_cntFail = 0;
    if (s_cntFail++ < 30) {
        LOG(LOG_LEVEL_ERROR, TAG " [getLoc] FAIL actor=%p root=%p (no usable source)",
            (void*)actorPtr, (void*)root);
    }
    return false;
}

// =====================================================================
//  角色血量读取
// =====================================================================

HealthInfo DfmMatchMonitor::getCharacterHealth(uintptr_t actorPtr) const {
    HealthInfo info;
    uintptr_t healthComp = safeReadPtr(actorPtr + m_off.Char_HealthComp);
    if (!ok(healthComp)) return info;

    info.maxHp = safeReadFloat(healthComp + m_off.HC_HealthMax);

    uintptr_t healthSet = safeReadPtr(healthComp + m_off.HC_HealthSet);
    if (ok(healthSet)) {
        info.hp     = safeReadFloat(healthSet + m_off.HS_HealthCur);
        float maxFromSet = safeReadFloat(healthSet + m_off.HS_HealthMax);
        if (maxFromSet > 0) info.maxHp = maxFromSet;
        info.armor  = safeReadFloat(healthSet + m_off.HS_ArmorCur);
        info.helmet = safeReadFloat(healthSet + m_off.HS_HelmetCur);
    }
    return info;
}

// =====================================================================
//  玩家名/队伍/武器
// =====================================================================

std::string DfmMatchMonitor::getPlayerName(uintptr_t actorPtr) const {
    uintptr_t ps = safeReadPtr(actorPtr + m_off.Pawn_PlayerState);
    if (!ok(ps)) return "";
    std::string name = readFString(ps + m_off.PS_PlayerName);
    if (name.empty() || name == "None" || name == "DefaultName") {
        name = readFString(ps + m_off.PS_PlayerName2);
    }
    return name;
}

int32_t DfmMatchMonitor::getTeamId(uintptr_t actorPtr) const {
    uintptr_t ps = safeReadPtr(actorPtr + m_off.Pawn_PlayerState);
    return ok(ps) ? safeReadS32(ps + m_off.PS_TeamID) : -1;
}

std::string DfmMatchMonitor::getWeaponName(uintptr_t actorPtr) const {
    uintptr_t wp = safeReadPtr(actorPtr + m_off.Char_CurWeapon);
    if (!ok(wp)) return "";
    std::string wn = readClassName(wp);
    // 简化: 去掉常见前后缀
    auto replace = [](std::string& s, const char* from, const char* to) {
        size_t pos = s.find(from);
        if (pos != std::string::npos) s.replace(pos, strlen(from), to);
    };
    replace(wn, "BP_", ""); replace(wn, "_C", "");
    replace(wn, "Weapon_", ""); replace(wn, "WP_", "");
    return wn;
}

// =====================================================================
//  类名分类 (与 loot_scan.js 关键词一致)
// =====================================================================

bool DfmMatchMonitor::isPickupClass(const std::string& cn) {
    static const char* keywords[] = {
        "InventoryPickup", "PickupBase", "DroppedItem",
        "GroundItem", "WeaponPickup", "AmmoPickup"
    };
    static const char* excludes[] = {
        "_Container", "_OpenBox", "_DeadBody", "RandomObj",
        "_EggGolden", "_WeaponModule", "_JailBreak"
    };
    bool found = false;
    for (auto kw : keywords) { if (cn.find(kw) != std::string::npos) { found = true; break; } }
    if (!found) return false;
    for (auto ex : excludes) { if (cn.find(ex) != std::string::npos) return false; }
    return true;
}

bool DfmMatchMonitor::isCharacterClass(const std::string& cn) {
    static const char* keywords[] = {
        "GPCharacter", "DFMCharacter", "DFMAICharacter",
        "Character_BP", "PlayerCharacter", "SoldierCharacter"
    };
    for (auto kw : keywords) { if (cn.find(kw) != std::string::npos) return true; }
    return false;
}

bool DfmMatchMonitor::isAICharacter(const std::string& cn) {
    static const char* keywords[] = {
        "_AI_", "AICharacter", "_AI_DT", "_AI_RPG",
        "_AI_Crocodile", "Boss", "Shielder"
    };
    for (auto kw : keywords) { if (cn.find(kw) != std::string::npos) return true; }
    return false;
}

bool DfmMatchMonitor::isContainerClass(const std::string& cn) {
    static const char* keywords[] = {
        "SingleItemContainer", "InteractorContainer_",
        "InventoryPickup_Container", "InventoryPickup_OpenBox",
        "Container_Collector", "ContainerTruck",
        "Container_SafeBox", "DrillingSafe",
        "ForceSafeBox", "Inventory_DeadBody"
    };
    for (auto kw : keywords) { if (cn.find(kw) != std::string::npos) return true; }
    return false;
}

// =====================================================================
//  物品名映射 (静态分类回退)
// =====================================================================

std::string DfmMatchMonitor::getItemDisplayName(int32_t itemId) const {
    if (itemId <= 0) return "未知物品";
    // ItemID 格式: MMSSXXXXXXX (MM=大类, SS=子类, XXXXXXX=序号)
    std::string s = std::to_string(itemId);
    while (s.size() < 11) s = "0" + s;
    int main = std::stoi(s.substr(0, 2));
    int sub  = std::stoi(s.substr(2, 2));
    int key  = main * 100 + sub;

    // 子类映射 (常见)
    static const std::unordered_map<int, const char*> subTypes = {
        {1001,"突击步枪"},{1002,"冲锋枪"},{1003,"精确步枪"},{1004,"狙击步枪"},
        {1005,"轻机枪"},{1006,"霰弹枪"},{1007,"手枪"},{1101,"护甲"},{1105,"头盔"},
        {1108,"背包"},{1301,"枪口"},{1302,"握把"},{1303,"瞄准镜"},{1304,"弹匣"},
        {1401,"急救包"},{1405,"注射器"},{1406,"能量饮料"},
        {2101,"手雷"},{2102,"闪光弹"},{2103,"烟雾弹"},
        {3701,"步枪弹"},{3702,"冲锋弹"},{3703,"狙击弹"},{3704,"霰弹"},{3705,"手枪弹"},
    };
    auto it = subTypes.find(key);
    if (it != subTypes.end()) return it->second;

    static const std::unordered_map<int, const char*> mainTypes = {
        {10,"武器"},{11,"装备"},{13,"配件"},{14,"药品"},{15,"收集品"},
        {16,"杂物"},{17,"箱子"},{21,"投掷物"},{37,"弹药"},{39,"载具"},
    };
    auto mt = mainTypes.find(main);
    return mt != mainTypes.end() ? std::string("[") + mt->second + "]" + std::to_string(itemId)
                                 : std::to_string(itemId);
}

// =====================================================================
//  类名 → 可读名翻译
//  规则: 剥离 BP_/Inventory_/_C 等前后缀, 然后查询固定翻译表
//  (来自 sdk_dump.cs 中常见 InventoryPickup 子类 + DFM SDK CDO 类名)
// =====================================================================
std::string DfmMatchMonitor::translateItemClassName(const std::string& className) {
    if (className.empty() || className == "?" || className == "None") return "";

    // 1. 提取核心 token: 剥离常见前后缀
    auto stripPrefix = [](std::string s, const char* p) {
        size_t pl = strlen(p);
        if (s.size() >= pl && s.compare(0, pl, p) == 0) s.erase(0, pl);
        return s;
    };
    auto stripSuffix = [](std::string s, const char* p) {
        size_t pl = strlen(p);
        if (s.size() >= pl && s.compare(s.size() - pl, pl, p) == 0) s.erase(s.size() - pl);
        return s;
    };

    std::string core = className;
    // 反复剥离前缀
    for (const char* p : {"BP_", "Default__", "InventoryPickup_", "Inventory_", "Pickup_",
                          "Item_", "Wpn_", "Weapon_", "DFM_"}) {
        core = stripPrefix(core, p);
    }
    // 反复剥离后缀
    for (const char* p : {"_C", "_BP", "_Default", "_Server", "_Mobile"}) {
        core = stripSuffix(core, p);
    }
    if (core.empty()) return "";

    // 2. 翻译表: 经典 PUBG/DFM 物品名 → 中文 (大小写不敏感匹配)
    static const std::unordered_map<std::string, const char*> kMap = {
        // ── 武器 ──
        {"AKM",        "AKM"},          {"AK74",       "AK-74"},        {"AK15",       "AK-15"},
        {"M4A1",       "M4A1"},         {"M16A4",      "M16A4"},        {"HK416",      "HK416"},
        {"SCAR",       "SCAR"},         {"SCARH",      "SCAR-H"},       {"AUG",        "AUG"},
        {"G36",        "G36"},          {"M249",       "M249"},         {"PKM",        "PKM"},
        {"VECTOR",     "Vector"},       {"MP5",        "MP5"},          {"MP7",        "MP7"},
        {"UMP",        "UMP-45"},       {"UMP9",       "UMP-9"},        {"P90",        "P90"},
        {"AWM",        "AWM"},          {"M24",        "M24"},          {"K98",        "Kar98k"},
        {"Mosin",      "莫辛纳甘"},     {"R93",         "R93"},          {"SVD",         "SVD"},
        {"M870",       "M870"},         {"S686",       "S686"},         {"S12K",       "S12K"},
        {"M9",         "M9"},           {"P92",        "P92"},          {"P1911",      "P1911"},
        {"Glock",      "Glock"},        {"Deagle",     "沙漠之鹰"},     {"Revolver",   "左轮"},
        // ── 投掷物 ──
        {"Grenade",    "手雷"},         {"Frag",       "破片手雷"},     {"Flash",      "闪光弹"},
        {"Smoke",      "烟雾弹"},       {"Molotov",    "燃烧瓶"},       {"Stun",       "震撼弹"},
        // ── 药品 ──
        {"FirstAidKit","急救包"},       {"Bandage",    "绷带"},         {"MedKit",     "医疗包"},
        {"Painkiller", "止痛药"},       {"Adrenaline", "肾上腺素"},     {"EnergyDrink","能量饮料"},
        {"Syringe",    "注射器"},       {"Drink",      "饮料"},         {"Water",      "水"},
        // ── 装备 ──
        {"Vest",       "护甲"},         {"Armor",      "护甲"},         {"Helmet",     "头盔"},
        {"Backpack",   "背包"},         {"Pack",       "背包"},
        // ── 配件 ──
        {"Scope",      "瞄准镜"},       {"Suppressor", "消音器"},       {"Compensator","枪口补偿"},
        {"Grip",       "握把"},         {"Foregrip",   "前握把"},       {"Mag",        "弹匣"},
        {"Magazine",   "弹匣"},         {"Stock",      "枪托"},         {"Laser",      "激光"},
        {"Sight",      "瞄具"},
        // ── 弹药 ──
        {"556",        "5.56mm"},       {"762",        "7.62mm"},       {"545",        "5.45mm"},
        {"9mm",        "9mm"},          {"45ACP",      ".45 ACP"},      {"12g",        "12号弹"},
        {"Shell",      "霰弹"},         {"Ammo",       "弹药"},
        // ── 物资箱/特殊 ──
        {"DeadBody",   "尸体"},         {"Container",  "物资箱"},       {"OpenBox",    "开启箱"},
        {"SafeBox",    "保险箱"},       {"GoldenNest", "金蛋"},
    };

    // 大小写不敏感查找
    std::string lcCore;
    lcCore.reserve(core.size());
    for (char c : core) lcCore += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const auto& kv : kMap) {
        std::string k = kv.first;
        std::string lcK; lcK.reserve(k.size());
        for (char c : k) lcK += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lcCore == lcK) return kv.second;
    }
    // 子串匹配 (e.g. core="Wpn_AKM_Variant" 含 "AKM")
    for (const auto& kv : kMap) {
        std::string k = kv.first;
        std::string lcK; lcK.reserve(k.size());
        for (char c : k) lcK += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lcK.size() >= 3 && lcCore.find(lcK) != std::string::npos) return kv.second;
    }
    // 没找到, 返回剥离后的核心名 (英文也比纯数字 ID 强)
    return core;
}

// =====================================================================
//  综合物品显示名解析 — 4 级回退
// =====================================================================
std::string DfmMatchMonitor::resolveItemDisplay(uintptr_t pickupActor,
                                                const std::string& rawId,
                                                int32_t numId) const {
    // 0. 注册表优先 — 之前帧已成功解出过的本地化名直接命中, 极快, 还能跨场景沿用。
    if (!rawId.empty()) {
        std::string cached = ItemRegistry::instance().lookup(rawId);
        if (!cached.empty()) return cached;
    }

    if (!ok(pickupActor)) {
        if (numId > 0) return getItemDisplayName(numId);
        return rawId.empty() ? std::string("?") : rawId;
    }

    // 1. InteractorName FText (优先, 本地化)
    std::string fromText = readFText(pickupActor + m_off.Interactor_Name);
    if (!fromText.empty()) {
        // 命中 → 写入注册表持久化, 下次直接走第 0 步快路径
        std::string cn = readClassName(pickupActor);
        ItemRegistry::instance().record(rawId, fromText, cn);
        return fromText;
    }

    // 2. InventoryType UClass* → 类名 → 翻译
    if (m_off.Pickup_InvType > 0) {
        uintptr_t invClass = safeReadPtr(pickupActor + m_off.Pickup_InvType);
        if (ok(invClass)) {
            std::string clsName = readObjName(invClass);
            std::string translated = translateItemClassName(clsName);
            if (!translated.empty()) {
                // className 翻译只有规则命中才足够稳定, 也写入注册表。
                ItemRegistry::instance().record(rawId, translated, clsName);
                return translated;
            }
        }
    }

    // 3. 数字 ID → 大类映射
    if (numId > 0) {
        std::string fromId = getItemDisplayName(numId);
        if (!fromId.empty()) return fromId;
    }

    // 4. 兜底: 原始 FName
    return rawId.empty() ? std::string("?") : rawId;
}

// =====================================================================
//  对局状态检测
// =====================================================================

MatchState DfmMatchMonitor::getMatchState() const {
    MatchState ms;
    uintptr_t gworldAddr = m_moduleBase + m_offGWorld;
    uintptr_t gworld = safeReadPtr(gworldAddr);
    if (!ok(gworld)) {
        static std::atomic<uint64_t> lastTickMs{0};
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t prev = lastTickMs.load(std::memory_order_relaxed);
        if (nowMs - prev > 5000) {
            lastTickMs.store(nowMs, std::memory_order_relaxed);
            LOG(LOG_LEVEL_ERROR, TAG " [matchState] GWorld 无效: gworldAddr=%p val=%p base=%p offGWorld=0x%X",
                (void*)gworldAddr, (void*)gworld, (void*)m_moduleBase, m_offGWorld);
        }
        return ms;
    }

    ms.worldName = readObjName(gworld);
    ms.gameStatePtr = safeReadPtr(gworld + m_off.World_GameState);
    if (!ok(ms.gameStatePtr)) {
        static std::atomic<uint64_t> lastTickMs{0};
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t prev = lastTickMs.load(std::memory_order_relaxed);
        if (nowMs - prev > 5000) {
            lastTickMs.store(nowMs, std::memory_order_relaxed);
            LOG(LOG_LEVEL_ERROR, TAG " [matchState] GameState 无效: gworld=%p +0x%X=%p world='%s'",
                (void*)gworld, m_off.World_GameState, (void*)ms.gameStatePtr, ms.worldName.c_str());
        }
        return ms;
    }

    std::string gsClassName = readClassName(ms.gameStatePtr);

    // DFM 使用安全屋+对局分离架构: GWorld 始终指向安全屋 (如 Iris_Entry)
    // 进入对局时 GWorld 不切换, 而是通过 sublevel streaming 加载战斗场景
    // 因此不能用 worldName 判定大厅, 只能通过 GameState 类名来区分

    // 安全屋 GameState 判定 (仅排除安全屋 GS, 不排除世界名)
    if (gsClassName.find("SafeHouse") != std::string::npos ||
        gsClassName.find("Lobby") != std::string::npos ||
        gsClassName.find("Entry") != std::string::npos) {
        // 限频: 每 5 秒最多打一条, 避免每帧爆刷把 trace 文件撑到 GB 级
        static std::atomic<uint64_t> lastTickMs{0};
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t prev = lastTickMs.load(std::memory_order_relaxed);
        if (nowMs - prev > 5000) {
            lastTickMs.store(nowMs, std::memory_order_relaxed);
            LOG(LOG_LEVEL_INFO, TAG " [matchState] 安全屋GS: gsClass='%s' world='%s'",
                gsClassName.c_str(), ms.worldName.c_str());
        }
        ms.inMatch = false; return ms;
    }

    // 读取 MatchState FName (动态偏移优先, 回退到多个候选偏移)
    ms.state = readFName(ms.gameStatePtr + m_off.GS_MatchState);
    if (ms.state != "InProgress" && ms.state != "WaitingToStart" &&
        ms.state != "WaitingPostMatch" && ms.state != "LeavingMap") {
        LOG(LOG_LEVEL_INFO, TAG " [matchState] MatchState FName 未命中已知值: '%s' (GS+0x%X)",
            ms.state.c_str(), m_off.GS_MatchState);
    }

    ms.elapsedTimeSeconds = safeReadS32(ms.gameStatePtr + m_off.GS_ElapsedTime);
    uint8_t hasBegun = safeReadU8(ms.gameStatePtr + m_off.GS_bHasBegunPlay);
    int32_t playerCount = safeReadS32(ms.gameStatePtr + m_off.GS_PlayerArray + offsetof(TArray<void*>, Num));

    // 详细诊断日志
    LOG(LOG_LEVEL_INFO, TAG " [matchState] world='%s' gsClass='%s' state='%s' elapsed=%d hasBegun=%d players=%d",
        ms.worldName.c_str(), gsClassName.c_str(), ms.state.c_str(),
        ms.elapsedTimeSeconds, (int)hasBegun, playerCount);

    // 战斗类 GameState (DFM 特有: GameState_PVPVE, GameState_Arena, GameState_Raid, DFMGameState 等)
    bool isBattle = gsClassName.find("PVPVE") != std::string::npos ||
                    gsClassName.find("PVP") != std::string::npos ||
                    gsClassName.find("Battle") != std::string::npos ||
                    gsClassName.find("Mission") != std::string::npos ||
                    gsClassName.find("Arena") != std::string::npos ||
                    gsClassName.find("Raid") != std::string::npos ||
                    gsClassName.find("DFMGameState") != std::string::npos ||
                    gsClassName.find("GPGameState") != std::string::npos;

    if (ms.state == "InProgress") { ms.inMatch = true; return ms; }
    if (isBattle && ms.elapsedTimeSeconds > 0 && (hasBegun & 1) && playerCount > 1) {
        ms.inMatch = true;
        return ms;
    }
    // 回退: 非大厅 + 战斗 GS + 有玩家 → 视为对局 (MatchState FName 可能在其他偏移)
    if (isBattle && playerCount > 0) {
        LOG(LOG_LEVEL_INFO, TAG " [matchState] 回退判定: isBattle=1 players=%d", playerCount);
        ms.inMatch = true;
        return ms;
    }

    LOG(LOG_LEVEL_INFO, TAG " [matchState] 未判定为对局: isBattle=%d", isBattle ? 1 : 0);
    return ms;
}

// =====================================================================
//  Actor 遍历 (PersistentLevel + SubLevels + StreamingLevels)
// =====================================================================

std::vector<uintptr_t> DfmMatchMonitor::getAllActors() const {
    std::vector<uintptr_t> actors;
    std::unordered_map<uintptr_t, bool> seen;
    actors.reserve(4096);

    auto addFromLevel = [&](uintptr_t lv) {
        if (!ok(lv)) return;
        // 方式1: Level+0x98 → TArray<Actor*>
        uintptr_t rawPtr = safeReadPtr(lv + m_off.Level_Actors);
        int32_t rawCount = safeReadS32(lv + m_off.Level_Actors + offsetof(TArray<void*>, Num));
        if (ok(rawPtr) && rawCount > 0) {
            for (int i = 0; i < std::min(rawCount, 50000); i++) {
                uintptr_t a = safeReadPtr(rawPtr + static_cast<uintptr_t>(i) * 8);
                if (ok(a) && !seen[a]) { seen[a] = true; actors.push_back(a); }
            }
            return;
        }
        // 方式2: LevelActorContainer
        uintptr_t ac = safeReadPtr(lv + m_off.Level_ActorCluster);
        if (!ok(ac)) return;
        uintptr_t p = safeReadPtr(ac + m_off.Container_Actors);
        int32_t c = safeReadS32(ac + m_off.Container_Actors + offsetof(TArray<void*>, Num));
        if (!ok(p) || c <= 0) return;
        for (int i = 0; i < std::min(c, 50000); i++) {
            uintptr_t a = safeReadPtr(p + static_cast<uintptr_t>(i) * 8);
            if (ok(a) && !seen[a]) { seen[a] = true; actors.push_back(a); }
        }
    };

    uintptr_t gworld = safeReadPtr(m_moduleBase + m_offGWorld);
    if (!ok(gworld)) return actors;

    // ── NetDriver.OpenChannels (优先, 让真人 pawn 先占 dedup key) ──
    // 服务器按相关性 (relevancy) 复制到客户端的真人/AI actor 都通过 UActorChannel
    // 暴露在 NetConnection.OpenChannels[]。这是 DFM 这种 100v100 大场战斗里
    // 唯一覆盖"近距可见但 PersistentLevel 不收录"敌人的列表。
    // 必须放在 PersistentLevel 之前: PersistentLevel 收录的是 placeholder pawn,
    // 它们和真 pawn 共享同一个 PlayerState, 后续 scanActors 用 PS 做 dedup,
    // 谁先进 charSeen 谁赢; 我们要的是让真 pawn 赢。
    auto addFromConnection = [&](uintptr_t conn) {
        if (!ok(conn)) return;
        uintptr_t chArr = safeReadPtr(conn + m_off.NC_OpenChannels);
        int32_t chNum  = safeReadS32(conn + m_off.NC_OpenChannels + offsetof(TArray<void*>, Num));
        if (!ok(chArr) || chNum <= 0) return;
        for (int i = 0; i < std::min(chNum, 8000); ++i) {
            uintptr_t ch = safeReadPtr(chArr + static_cast<uintptr_t>(i) * 8);
            if (!ok(ch)) continue;
            uintptr_t a = safeReadPtr(ch + m_off.AChan_Actor);
            if (!ok(a)) continue;             // ControlChannel/VoiceChannel.Actor==0
            if (seen.count(a)) continue;
            seen[a] = true;
            actors.push_back(a);
        }
    };
    uintptr_t netDriver = safeReadPtr(gworld + m_off.World_NetDriver);
    int beforeNet = static_cast<int>(actors.size());
    if (ok(netDriver)) {
        addFromConnection(safeReadPtr(netDriver + m_off.Net_ServerConnection));
        uintptr_t ccArr = safeReadPtr(netDriver + m_off.Net_ClientConnections);
        int32_t ccNum  = safeReadS32(netDriver + m_off.Net_ClientConnections + offsetof(TArray<void*>, Num));
        if (ok(ccArr) && ccNum > 0) {
            for (int i = 0; i < std::min(ccNum, 32); ++i) {
                addFromConnection(safeReadPtr(ccArr + static_cast<uintptr_t>(i) * 8));
            }
        }
    }
    int netActorsAdded = static_cast<int>(actors.size()) - beforeNet;

    // PersistentLevel
    addFromLevel(safeReadPtr(gworld + m_off.World_PersistentLevel));

    // SubLevels (World+0x158)
    uintptr_t levelsPtr = safeReadPtr(gworld + m_off.World_Levels);
    int32_t levelsCount = safeReadS32(gworld + m_off.World_Levels + offsetof(TArray<void*>, Num));
    if (ok(levelsPtr) && levelsCount > 0) {
        for (int i = 0; i < std::min(levelsCount, 200); i++) {
            addFromLevel(safeReadPtr(levelsPtr + static_cast<uintptr_t>(i) * 8));
        }
    }

    // StreamingLevels (World+0x90)
    uintptr_t slPtr = safeReadPtr(gworld + m_off.World_StreamingLevels);
    int32_t slCount = safeReadS32(gworld + m_off.World_StreamingLevels + offsetof(TArray<void*>, Num));
    if (ok(slPtr) && slCount > 0) {
        for (int i = 0; i < std::min(slCount, 500); i++) {
            uintptr_t sl = safeReadPtr(slPtr + static_cast<uintptr_t>(i) * 8);
            if (ok(sl)) addFromLevel(safeReadPtr(sl + m_off.Streaming_LoadedLevel));
        }
    }

    {
        // 限频 3s 日志
        static std::atomic<uint64_t> lastNetLogMs{0};
        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        uint64_t prev = lastNetLogMs.load(std::memory_order_relaxed);
        if (nowMs - prev > 3000) {
            lastNetLogMs.store(nowMs, std::memory_order_relaxed);
            LOG(LOG_LEVEL_INFO,
                "[netDriver] gworld=0x%lx nd=0x%lx netActors=%d totalActors=%zu",
                (unsigned long)gworld, (unsigned long)netDriver,
                netActorsAdded, actors.size());
        }
    }

    return actors;
}

// =====================================================================
//  Actor 扫描分类 (物资/角色/箱子)
// =====================================================================

void DfmMatchMonitor::scanActors(const std::vector<uintptr_t>& actors, DrawDfmData& outData) {
    std::unordered_map<uintptr_t, bool> charSeen;

    for (uintptr_t actor : actors) {
        std::string cn = readClassName(actor);

        // ── 物资 ──
        if (isPickupClass(cn)) {
            std::string rawId = readFName(actor + m_off.Pickup_InvIdName);
            if (rawId.empty() || rawId == "None" || rawId == "?") continue;

            int32_t numId = 0;
            try { numId = std::stoi(rawId); } catch (...) {}

            LootItem item;
            // 4 级回退: InteractorName(FText) → InventoryType(UClass) → 数字大类 → rawFName
            item.itemName = resolveItemDisplay(actor, rawId, numId);
            item.className = cn;
            item.itemId = numId;
            item.stackCount = safeReadS32(actor + m_off.Pickup_StackCount);
            getActorLocation(actor, item.pos);
            outData.lootItems.push_back(std::move(item));
        }

        // ── 角色 ──
        if (isCharacterClass(cn)) {
            uintptr_t psPtr = safeReadPtr(actor + m_off.Pawn_PlayerState);
            uintptr_t dedupKey = ok(psPtr) ? psPtr : actor;
            if (charSeen[dedupKey]) continue;
            charSeen[dedupKey] = true;

            bool isAI = isAICharacter(cn);
            std::string playerName = getPlayerName(actor);
            if (!isAI && playerName.empty() && !ok(psPtr)) continue; // 幽灵/残影

            PlayerInfo pi{};
            pi.playerName = playerName.empty() ? (isAI ? "[AI]" : "(无名)") : playerName;
            pi.className = cn;
            pi.isAI = isAI;
            pi.teamId = isAI ? -1 : getTeamId(actor);
            pi.weapon = getWeaponName(actor);
            pi.characterPtr = actor;

            HealthInfo hi = getCharacterHealth(actor);
            pi.hp = hi.hp; pi.maxHp = hi.maxHp;
            pi.armor = hi.armor; pi.helmet = hi.helmet;

            // ── 坐标采集 ──
            // AI / 自机: getActorLocation 可信, 先拿 actor pos 再补骨骼(这样骨骼不合理会被过滤)。
            // 真人敌人: M0~M4 都在腾讯加密路径上, getActorLocation 返回的 pos 可能是错位。
            // 为避免 fillPlayerBones 用错位 pos 误判、拒掉所有骨骼(>16m 阈值),
            //   这里采取 "骨骼优先" 顺序: pos 先置空 → fillPlayerBones 不受根点约束 →
            //   deriveFootPositionFromBones 拿到脚底世界坐标, 这才是渲染管线使用的明文值.
            //   骨骼不可用时才退回 getActorLocation.
            FVector3 bonePos{};
            FVector3 socketPos{};
            bool posResolved = false;
            if (!pi.isAI) {
                pi.pos = {}; // 取消根点约束, 允许骨骼任意坐标被接受
                if (getCharacterSocketLocation(actor, socketPos)) {
                    pi.pos = socketPos;
                    rememberActorLocation(actor, pi.pos);
                    posResolved = true;
                }
                fillPlayerBones(pi);
                if (!posResolved && deriveFootPositionFromBones(pi, bonePos)) {
                    pi.pos = bonePos;
                    rememberActorLocation(actor, pi.pos);
                    posResolved = true;
                }
            }
            if (!posResolved) {
                getActorLocation(actor, pi.pos);
                fillPlayerBones(pi);
                // AI 仍允许骨骼复核 (原逻辑): pos 不可用时补上
                const bool posUsable = hasUsableActorPoint(pi.pos);
                if (((!pi.isAI) || !posUsable) && deriveFootPositionFromBones(pi, bonePos)) {
                    pi.pos = bonePos;
                    rememberActorLocation(actor, pi.pos);
                }
            }
            {
                static int s_cntFix = 0;
                if (s_cntFix++ < 30) {
                    LOG(LOG_LEVEL_INFO,
                        "[posFixBone] %s isAI=%d boneFirst=%d bonesValid=%d final=(%.0f,%.0f,%.0f)",
                        pi.playerName.c_str(), pi.isAI ? 1 : 0,
                        posResolved ? 1 : 0, pi.bonesValid ? 1 : 0,
                        pi.pos.x, pi.pos.y, pi.pos.z);
                }
            }

            // ── 坐标诊断 ──
            {
                static int diagCount = 0;
                if (diagCount < 30) {
                    uintptr_t root = safeReadPtr(actor + m_off.Actor_RootComponent);
                    uintptr_t pawnPrivate = ok(psPtr) ? safeReadPtr(psPtr + m_off.PS_PawnPrivate) : 0;
                    float ctw_x = 0, ctw_y = 0, ctw_z = 0;
                    float mesh_x = 0, mesh_y = 0, mesh_z = 0;
                    uint8_t encByte = 0;
                    if (ok(root)) {
                        ctw_x = safeReadFloat(root + m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationX));
                        ctw_y = safeReadFloat(root + m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationY));
                        ctw_z = safeReadFloat(root + m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationZ));
                        encByte = safeReadU8(root + m_off.Scene_RelativeLocation + 0xE);
                    }
                    uintptr_t meshComp = m_off.Char_Mesh > 0
                        ? resolveObjectField(actor + static_cast<uintptr_t>(m_off.Char_Mesh)) : 0;
                    if (ok(meshComp)) {
                        mesh_x = safeReadFloat(meshComp + m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationX));
                        mesh_y = safeReadFloat(meshComp + m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationY));
                        mesh_z = safeReadFloat(meshComp + m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationZ));
                    }
                    LOG(LOG_LEVEL_WARN,
                        "[posDiag] %s isAI=%d actor=%p rootPtr=%p ps=%p pawnPrivate=%p rootCTW=(%.0f,%.0f,%.0f) meshCTW=(%.0f,%.0f,%.0f) enc=%d final=(%.0f,%.0f,%.0f) bones=%d name=%s",
                        cn.c_str(), isAI?1:0,
                        (void*)actor,
                        (void*)root,
                        (void*)psPtr,
                        (void*)pawnPrivate,
                        ctw_x, ctw_y, ctw_z,
                        mesh_x, mesh_y, mesh_z,
                        (int)encByte,
                        pi.pos.x, pi.pos.y, pi.pos.z,
                        pi.bonesValid?1:0,
                        pi.playerName.c_str());
                    diagCount++;
                }
            }

            outData.players.push_back(std::move(pi));
        }

        // ── 物资箱 ──
        if (isContainerClass(cn)) {
            ContainerInfo ci = readContainerInfo(actor, cn);
            ci.items = readContainerItems(actor, cn);
            ci.actorPtr = actor;  // 供 GUI 后续发起远程开箱
            outData.containers.push_back(std::move(ci));
        }
    }
}

// =====================================================================
//  PlayerArray 回退 (Actor 遍历未找到角色时)
// =====================================================================

void DfmMatchMonitor::scanFromPlayerArray(DrawDfmData& outData) const {
    uintptr_t gworld = safeReadPtr(m_moduleBase + m_offGWorld);
    if (!ok(gworld)) return;
    uintptr_t gs = safeReadPtr(gworld + m_off.World_GameState);
    if (!ok(gs)) return;

    uintptr_t paPtr = safeReadPtr(gs + m_off.GS_PlayerArray);
    int32_t paCount = safeReadS32(gs + m_off.GS_PlayerArray + offsetof(TArray<void*>, Num));
    if (!ok(paPtr) || paCount <= 0) return;

    for (int i = 0; i < std::min(paCount, 200); i++) {
        uintptr_t ps = safeReadPtr(paPtr + static_cast<uintptr_t>(i) * 8);
        if (!ok(ps)) continue;

        uintptr_t pawn = safeReadPtr(ps + m_off.PS_PawnPrivate);
        if (!ok(pawn)) continue;

        PlayerInfo pi{};
        pi.playerName = readFString(ps + m_off.PS_PlayerName);
        if (pi.playerName.empty()) pi.playerName = readFString(ps + m_off.PS_PlayerName2);
        if (pi.playerName.empty()) pi.playerName = "(无名)";
        pi.className = readClassName(pawn);
        pi.teamId = safeReadS32(ps + m_off.PS_TeamID);
        pi.weapon = getWeaponName(pawn);
        pi.characterPtr = pawn;

        HealthInfo hi = getCharacterHealth(pawn);
        pi.hp = hi.hp; pi.maxHp = hi.maxHp;
        pi.armor = hi.armor; pi.helmet = hi.helmet;

        // 骨骼优先链 (与 scanActors 一致): 真人敌人先读骨骼避免被错位 pos 拒掉
        FVector3 bonePos{};
        FVector3 socketPos{};
        bool posResolved = false;
        if (!pi.isAI) {
            pi.pos = {};
            if (getCharacterSocketLocation(pawn, socketPos)) {
                pi.pos = socketPos;
                rememberActorLocation(pawn, pi.pos);
                posResolved = true;
            }
            fillPlayerBones(pi);
            if (!posResolved && deriveFootPositionFromBones(pi, bonePos)) {
                pi.pos = bonePos;
                rememberActorLocation(pawn, pi.pos);
                posResolved = true;
            }
        }
        if (!posResolved) {
            getActorLocation(pawn, pi.pos);
            fillPlayerBones(pi);
            const bool posUsable = hasUsableActorPoint(pi.pos);
            if (((!pi.isAI) || !posUsable) && deriveFootPositionFromBones(pi, bonePos)) {
                pi.pos = bonePos;
                rememberActorLocation(pawn, pi.pos);
            }
        }

        outData.players.push_back(std::move(pi));
    }
}

// =====================================================================
//  物资箱读取
// =====================================================================

ContainerInfo DfmMatchMonitor::readContainerInfo(uintptr_t actorPtr, const std::string& cn) const {
    ContainerInfo ci;
    ci.className = cn;
    getActorLocation(actorPtr, ci.pos);

    if (cn.find("SingleItemContainer") != std::string::npos) {
        ci.opened = (safeReadU8(actorPtr + m_off.SIC_FirstOpened) & 1) != 0;
        ci.finished = (safeReadU8(actorPtr + m_off.SIC_Finished) & 1) != 0;
        ci.boxType = "单物品箱";
    } else if (cn.find("Inventory_DeadBody") != std::string::npos) {
        ci.boxType = "尸体箱";
    } else {
        uint8_t boxType = safeReadU8(actorPtr + m_off.Cont_PickupBoxType);
        // [修复] 使用 ExtraRepInfo.bFirstOpen (0x1C30) 判断是否被打开过
        // 原代码只用 bIsEmpty 判断, 拿完物资后仍显示"未打开"
        ci.opened = (safeReadU8(actorPtr + m_off.Cont_ExtraRepInfo) & 1) != 0;
        // bIsEmpty (0x2110) — 是否已被拿空 (独立于打开状态)
        ci.finished = (safeReadU8(actorPtr + m_off.Cont_IsEmpty) & 1) != 0;
        // 如果没有 bFirstOpen 但为空, 也标记为已打开
        if (!ci.opened && ci.finished) ci.opened = true;
        static const char* boxTypes[] = {"默认","武器箱","护甲箱","杂物箱","据点箱"};
        ci.boxType = (boxType < 5) ? boxTypes[boxType] : "未知";
    }
    return ci;
}

std::vector<ContainerItem> DfmMatchMonitor::readContainerItems(uintptr_t actorPtr, const std::string& cn) const {
    std::vector<ContainerItem> items;

    // InventoryPickup_Container 及其子类: RepItemArray
    bool isRepItem = (cn.find("InventoryPickup_Container") != std::string::npos ||
                      cn.find("InteractorContainer_") != std::string::npos ||
                      cn.find("ContainerTruck") != std::string::npos ||
                      cn.find("Container_SafeBox") != std::string::npos ||
                      cn.find("Inventory_DeadBody") != std::string::npos) &&
                     cn.find("SingleItemContainer") == std::string::npos;

    if (isRepItem) {
        uintptr_t itemsPtr = safeReadPtr(actorPtr + m_off.Cont_RepItemArray + m_off.Cont_ItemsOffset);
        int32_t itemsCount = safeReadS32(actorPtr + m_off.Cont_RepItemArray + m_off.Cont_ItemsOffset + offsetof(TArray<void*>, Num));
        if (ok(itemsPtr) && itemsCount > 0) {
            for (int i = 0; i < std::min(itemsCount, 50); i++) {
                uintptr_t item = itemsPtr + static_cast<uintptr_t>(i) * m_off.ItemInfoSize;
                uint32_t cat = safeReadU32(item + offsetof(InventoryItemInfo, ItemCategory));
                uint32_t seq = safeReadU32(item + offsetof(InventoryItemInfo, ItemSequence));
                int32_t count = safeReadS32(item + offsetof(InventoryItemInfo, ItemCount));
                float dur = safeReadFloat(item + offsetof(InventoryItemInfo, ItemDurability));
                float durMax = safeReadFloat(item + offsetof(InventoryItemInfo, ItemDurabilityMax));

                int32_t itemIdNum = static_cast<int32_t>(cat * 10000 + seq);
                ContainerItem ci;
                // 容器内 item 拿不到 actor 指针, 不能读 InteractorName。
                // 改为: 注册表 (按 numId 反查, 之前帧地面 PickupBase 命中过的同 ID 直接复用)
                //       → translateItemClassName(itemId 字符串无法用) 跳过
                //       → getItemDisplayName 数字大类映射兜底
                std::string fromReg = ItemRegistry::instance().lookupById(itemIdNum);
                ci.name = !fromReg.empty() ? fromReg : getItemDisplayName(itemIdNum);
                ci.itemId = itemIdNum;
                ci.count = count;
                ci.durability = dur;
                ci.durabilityMax = durMax;
                items.push_back(std::move(ci));
            }
        }
    }

    // SingleItemContainer: CachedPickups
    if (cn.find("SingleItemContainer") != std::string::npos) {
        uintptr_t puPtr = safeReadPtr(actorPtr + m_off.SIC_CachedPickups);
        int32_t puCount = safeReadS32(actorPtr + m_off.SIC_CachedPickups + offsetof(TArray<void*>, Num));
        if (ok(puPtr) && puCount > 0) {
            for (int i = 0; i < std::min(puCount, 50); i++) {
                uintptr_t pickup = safeReadPtr(puPtr + static_cast<uintptr_t>(i) * 8);
                if (!ok(pickup)) continue;
                std::string idName = readFName(pickup + m_off.Pickup_InvIdName);
                int32_t stackCount = safeReadS32(pickup + m_off.Pickup_StackCount);

                ContainerItem ci;
                int32_t numId = 0;
                try { numId = std::stoi(idName); } catch (...) {}
                // 4 级回退同 ground loot
                ci.name = resolveItemDisplay(pickup, idName, numId);
                ci.count = stackCount;
                ci.itemId = numId;
                items.push_back(std::move(ci));
            }
        }
    }

    return items;
}

// =====================================================================
//  本地 PlayerController 查找 (带缓存 + 多条路径)
// =====================================================================

uintptr_t DfmMatchMonitor::findLocalPlayerController() const {
    // 每次都重新查找 (不缓存), 因为 DFM 切换场景时 PC 会变
    uintptr_t gworld = safeReadPtr(m_moduleBase + m_offGWorld);
    if (!ok(gworld)) return 0;

    // ── 路径 1: OwningGameInstance → LocalPlayers[0] → PlayerController ──
    uintptr_t gi = safeReadPtr(gworld + m_off.World_OwningGameInstance);
    if (ok(gi)) {
        uintptr_t lpArr = safeReadPtr(gi + m_off.GI_LocalPlayers);
        int32_t lpCnt = safeReadS32(gi + m_off.GI_LocalPlayers + 8);
        if (ok(lpArr) && lpCnt >= 1 && lpCnt <= 4) {
            uintptr_t lp0 = safeReadPtr(lpArr);
            if (ok(lp0)) {
                uintptr_t pc = safeReadPtr(lp0 + m_off.LP_PlayerController);
                if (ok(pc)) {
                    uintptr_t pcm = safeReadPtr(pc + m_off.PC_PlayerCameraManager);
                    if (ok(pcm)) {
                        return pc;
                    }
                }
            }
        }
    }

    // ── 路径 2: PlayerArray[i].Owner (旧方法, 回退) ──
    uintptr_t gs = safeReadPtr(gworld + m_off.World_GameState);
    if (ok(gs)) {
        uintptr_t paPtr = safeReadPtr(gs + m_off.GS_PlayerArray);
        int32_t paCount = safeReadS32(gs + m_off.GS_PlayerArray + offsetof(TArray<void*>, Num));
        if (ok(paPtr) && paCount > 0) {
            for (int i = 0; i < std::min(paCount, 10); i++) {
                uintptr_t ps = safeReadPtr(paPtr + static_cast<uintptr_t>(i) * 8);
                if (!ok(ps)) continue;
                uintptr_t owner = safeReadPtr(ps + m_off.Actor_Owner);
                if (!ok(owner)) continue;
                uintptr_t pcm = safeReadPtr(owner + m_off.PC_PlayerCameraManager);
                if (ok(pcm)) {
                    LOG(LOG_LEVEL_INFO, TAG " [camera] PC via PA[%d].Owner: PC=%p PCM=%p",
                        i, (void*)owner, (void*)pcm);
                    return owner;
                }
            }
        }
    }

    LOG(LOG_LEVEL_INFO, TAG " [camera] PlayerController 未找到");
    return 0;
}

// =====================================================================
//  相机数据读取 (PlayerController → PCM → CameraCachePrivate → POV)
// =====================================================================

void DfmMatchMonitor::fillCameraData(DrawDfmData& outData) const {
    uintptr_t pc = findLocalPlayerController();
    bool gotRotation = false;
    bool gotLocation = false;
    bool gotFov = false;

    if (ok(pc)) {
        // ControlRotation (FRotator = 3 floats) — 最可靠的旋转源
        float crPitch = safeReadFloat(pc + m_off.Ctrl_ControlRotation);
        float crYaw   = safeReadFloat(pc + m_off.Ctrl_ControlRotation + offsetof(FRotator, Yaw));
        float crRoll  = safeReadFloat(pc + m_off.Ctrl_ControlRotation + offsetof(FRotator, Roll));
        if (std::isfinite(crPitch) && std::isfinite(crYaw) && std::isfinite(crRoll)
            && std::fabs(crPitch) < 360.0f && std::fabs(crYaw) < 360.0f) {
            outData.camPitch = crPitch;
            outData.camYaw   = crYaw;
            outData.camRoll  = crRoll;
            gotRotation = true;
        }

        // 回退: TargetViewRotation (PC+0x41C) — 游戏复制的视角旋转
        if (!gotRotation) {
            float tvPitch = safeReadFloat(pc + m_off.PC_TargetViewRotation);
            float tvYaw   = safeReadFloat(pc + m_off.PC_TargetViewRotation + offsetof(FRotator, Yaw));
            float tvRoll  = safeReadFloat(pc + m_off.PC_TargetViewRotation + offsetof(FRotator, Roll));
            if (std::isfinite(tvPitch) && std::isfinite(tvYaw)
                && std::fabs(tvPitch) < 360.0f && std::fabs(tvYaw) < 360.0f) {
                outData.camPitch = tvPitch;
                outData.camYaw   = tvYaw;
                outData.camRoll  = tvRoll;
                gotRotation = true;
            }
        }

        uintptr_t pcm = safeReadPtr(pc + m_off.PC_PlayerCameraManager);
        if (ok(pcm)) {
            // DefaultFOV
            float defaultFov = safeReadFloat(pcm + m_off.PCM_DefaultFOV);
            if (defaultFov >= 30.0f && defaultFov <= 170.0f) {
                outData.camFOV = defaultFov;
                gotFov = true;
            }

            // 尝试从多个相机缓存源读取 Rotation 和 FOV
            // SDK dump 确认的 5 个缓存位置 (POV 内部布局相同):
            //   CameraCache(0x3E0), LastFrameCache(0xDB0),
            //   ViewTarget(0x1780), CameraCachePrivate(0x2B60),
            //   LastFrameCachePriv(0x3530)
            const int32_t kCacheSources[] = {
                m_off.PCM_CameraCache,
                m_off.PCM_LastFrameCache,
                m_off.PCM_ViewTarget,
                m_off.PCM_CameraCachePrivate,
                m_off.PCM_LastFrameCachePriv,
            };
            for (int32_t srcOff : kCacheSources) {
                uintptr_t src = pcm + srcOff;
                float cPitch = safeReadFloat(src + m_off.CamCache_RotPitch);
                float cYaw   = safeReadFloat(src + m_off.CamCache_RotYaw);
                float cFov   = safeReadFloat(src + m_off.CamCache_FOV);

                bool rotValid = std::isfinite(cPitch) && std::isfinite(cYaw)
                    && (std::fabs(cPitch) > 0.001f || std::fabs(cYaw) > 0.001f)
                    && std::fabs(cPitch) < 360.0f && std::fabs(cYaw) < 360.0f;
                bool fovValid = cFov >= 30.0f && cFov <= 170.0f;

                if (rotValid && !gotRotation) {
                    float cRoll = safeReadFloat(src + m_off.CamCache_RotRoll);
                    outData.camPitch = cPitch;
                    outData.camYaw   = cYaw;
                    outData.camRoll  = cRoll;
                    gotRotation = true;
                }
                if (fovValid && !gotFov) {
                    outData.camFOV = cFov;
                    gotFov = true;
                }
                if (gotRotation && gotFov) break;
            }
        }
    }

    // Location: 使用 myPos + 眼高 (EncVector 加密无法直接读取)
    if (!gotLocation && (std::fabs(outData.myPos.x) > 1.0f || std::fabs(outData.myPos.y) > 1.0f)) {
        outData.camLocX = outData.myPos.x;
        outData.camLocY = outData.myPos.y;
        outData.camLocZ = outData.myPos.z + 160.0f;
        gotLocation = true;
    }

    if (!gotFov) outData.camFOV = 90.0f;

    static auto lastCamLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCamLog).count() >= 5) {
        lastCamLog = now;
        // 直接诊断: 打印所有 5 个缓存源的 FOV 值
        uintptr_t diagPC = findLocalPlayerController();
        uintptr_t diagPCM = ok(diagPC) ? safeReadPtr(diagPC + m_off.PC_PlayerCameraManager) : 0;
        float diagDefFov = ok(diagPCM) ? safeReadFloat(diagPCM + m_off.PCM_DefaultFOV) : -1;
        // 5 个缓存源: CameraCache, LastFrame, ViewTarget, Private, LastFramePriv
        float f1 = ok(diagPCM) ? safeReadFloat(diagPCM + m_off.PCM_CameraCache + m_off.CamCache_FOV) : -1;
        float f2 = ok(diagPCM) ? safeReadFloat(diagPCM + m_off.PCM_LastFrameCache + m_off.CamCache_FOV) : -1;
        float f3 = ok(diagPCM) ? safeReadFloat(diagPCM + m_off.PCM_ViewTarget + m_off.ViewTarget_FOV) : -1;
        float f4 = ok(diagPCM) ? safeReadFloat(diagPCM + m_off.PCM_CameraCachePrivate + m_off.CamCache_FOV) : -1;
        float f5 = ok(diagPCM) ? safeReadFloat(diagPCM + m_off.PCM_LastFrameCachePriv + m_off.CamCache_FOV) : -1;
        LOG(LOG_LEVEL_INFO,
            "[cam] loc=%d rot=%d fov=%d PCM=%p defFov=%.1f fovs=[%.1f,%.1f,%.1f,%.1f,%.1f]",
            gotLocation?1:0, gotRotation?1:0, gotFov?1:0,
            (void*)diagPCM, diagDefFov, f1, f2, f3, f4, f5);
    }
}

// =====================================================================
//  本地玩家位置
// =====================================================================

FVector3 DfmMatchMonitor::getMyPosition() const {
    FVector3 pos;

    uintptr_t pc = findLocalPlayerController();
    if (ok(pc)) {
        // 方法1: PlayerController.AcknowledgedPawn (最直接, SDK 0x3F0)
        uintptr_t ackPawn = safeReadPtr(pc + m_off.PC_AcknowledgedPawn);
        if (ok(ackPawn)) {
            getActorLocation(ackPawn, pos);
            if (std::fabs(pos.x) > 1.0f || std::fabs(pos.y) > 1.0f) return pos;
        }

        // 方法2: Controller.Pawn (SDK 0x3A0)
        uintptr_t pawn = safeReadPtr(pc + m_off.Ctrl_Pawn);
        if (ok(pawn) && pawn != ackPawn) {
            getActorLocation(pawn, pos);
            if (std::fabs(pos.x) > 1.0f || std::fabs(pos.y) > 1.0f) return pos;
        }

        // 方法3: Controller.PlayerState → PawnPrivate
        uintptr_t ps = safeReadPtr(pc + m_off.Ctrl_PlayerState);
        if (ok(ps)) {
            uintptr_t pawnFromPS = safeReadPtr(ps + m_off.PS_PawnPrivate);
            if (ok(pawnFromPS)) {
                getActorLocation(pawnFromPS, pos);
                if (std::fabs(pos.x) > 1.0f || std::fabs(pos.y) > 1.0f) return pos;
            }
        }
    }

    // 方法2: 回退 - 遍历 PlayerArray, 找到与相机位置最近的玩家
    uintptr_t gworld = safeReadPtr(m_moduleBase + m_offGWorld);
    if (!ok(gworld)) return pos;

    uintptr_t gs = safeReadPtr(gworld + m_off.World_GameState);
    if (!ok(gs)) return pos;

    uintptr_t paPtr = safeReadPtr(gs + m_off.GS_PlayerArray);
    int32_t paCount = safeReadS32(gs + m_off.GS_PlayerArray + offsetof(TArray<void*>, Num));
    if (!ok(paPtr) || paCount <= 0) return pos;

    // 尝试每个 PlayerState 的 PawnPrivate
    for (int i = 0; i < std::min(paCount, 20); i++) {
        uintptr_t ps = safeReadPtr(paPtr + static_cast<uintptr_t>(i) * 8);
        if (!ok(ps)) continue;

        // 检查这个 PlayerState 的 Owner 是否是 PlayerController (本地玩家的标志)
        uintptr_t owner = safeReadPtr(ps + m_off.Actor_Owner);
        if (ok(owner) && ok(pc) && owner == pc) {
            uintptr_t pawn = safeReadPtr(ps + m_off.PS_PawnPrivate);
            if (ok(pawn)) {
                getActorLocation(pawn, pos);
                if (std::fabs(pos.x) > 1.0f || std::fabs(pos.y) > 1.0f) {
                    return pos;
                }
            }
        }
    }

    // 方法3: 最终回退 - 用 PlayerArray[0]
    uintptr_t ps0 = safeReadPtr(paPtr);
    if (ok(ps0)) {
        uintptr_t pawn0 = safeReadPtr(ps0 + m_off.PS_PawnPrivate);
        if (ok(pawn0)) getActorLocation(pawn0, pos);
    }
    return pos;
}

// =====================================================================
//  SharedDfmData
// =====================================================================

void SharedDfmData::pushData(const DrawDfmData& data) {
    std::lock_guard<std::mutex> lock(m_mutex);
    int backIdx = 1 - m_frontIdx;
    m_buffers[backIdx] = data;
    m_frontIdx = backIdx;
    m_inMatch.store(data.inMatch, std::memory_order_release);
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    m_lastPushMs.store(nowMs, std::memory_order_release);
}

void SharedDfmData::getData(DrawDfmData& outData) {
    std::lock_guard<std::mutex> lock(m_mutex);
    outData = m_buffers[m_frontIdx];
}

int64_t SharedDfmData::getMsSinceLastPush() const {
    int64_t last = m_lastPushMs.load(std::memory_order_acquire);
    if (last < 0) return -1;
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return nowMs - last;
}

void SharedDfmData::enqueueOpenBox(uintptr_t actorPtr) {
    if (!actorPtr) return;
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    // 去重: 短时间内同一 box 多次按钮按下只发一次
    for (auto p : m_openBoxQueue) if (p == actorPtr) return;
    if (m_openBoxQueue.size() >= 32) return;  // 防溢出
    m_openBoxQueue.push_back(actorPtr);
}

std::vector<uintptr_t> SharedDfmData::drainOpenBoxQueue() {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    std::vector<uintptr_t> out;
    out.swap(m_openBoxQueue);
    return out;
}

// =====================================================================
//  DfmMatchMonitor 构造/析构
// =====================================================================

DfmMatchMonitor::DfmMatchMonitor(uintptr_t moduleBase, uintptr_t moduleSize,
                                 uint32_t offNamePool, uint32_t offGUObjectArrayNum,
                                 uint32_t offGUObjectArrayChunks, uint32_t offGWorld,
                                 ue5dfminf::UE5DfmInterface* interface)
    : m_interface(interface), m_moduleBase(moduleBase), m_moduleSize(moduleSize)
    , m_offNamePool(offNamePool), m_offGUObjectArrayNum(offGUObjectArrayNum)
    , m_offGUObjectArrayChunks(offGUObjectArrayChunks), m_offGWorld(offGWorld)
{
    // 初始化物资名注册表 — 加载已积累的 idName→display 映射, 后续运行时累积更新。
    // 路径选 app 私有 external 目录, 无需运行时权限。包名/项目结构变了改这里即可。
    ItemRegistry::instance().init(
        "/storage/emulated/0/Android/data/com.example.dobbyproject/files");
}

DfmMatchMonitor::~DfmMatchMonitor() { stop(); }

// =====================================================================
//  偏移初始化 — 通过 UE5DfmInterface 动态查询覆盖默认值
// =====================================================================

bool DfmMatchMonitor::initOffsets() {
    if (!m_interface) {
        LOG(LOG_LEVEL_INFO, TAG " 无 Interface, 使用 IDA 默认偏移");
        return true;
    }

    LOG(LOG_LEVEL_INFO, TAG " 开始动态解析偏移...");

    // 辅助宏: 查询成功则覆盖默认值
    #define TRY_RESOLVE(cls, field, target) do { \
        int32_t v = m_interface->getFieldOffsetInHierarchy(cls, field); \
        if (v >= 0) { target = v; \
            LOG(LOG_LEVEL_INFO, TAG " [offset] %s.%s = 0x%X", cls, field, v); \
        } \
    } while(0)

    #define TRY_RESOLVE_MULTI(field, target, ...) do { \
        const char* classes[] = { __VA_ARGS__ }; \
        std::string owner; \
        bool _resolved = false; \
        for (const char* cls : classes) { \
            const auto* info = m_interface->findFieldInHierarchy(cls, field, &owner); \
            if (info) { \
                target = info->offset; \
                LOG(LOG_LEVEL_INFO, TAG " [offset] %s.%s = 0x%X (via %s)", cls, field, info->offset, owner.c_str()); \
                _resolved = true; \
                break; \
            } \
        } \
        if (!_resolved) { \
            LOG(LOG_LEVEL_WARN, TAG " [offset] reflect MISS '%s' — keep default 0x%X", field, target); \
        } \
    } while(0)

    // Actor
    TRY_RESOLVE("Actor", "bReplicateMovement", m_off.Actor_bReplicateMovement);
    TRY_RESOLVE("Actor", "ReplicatedMovement", m_off.Actor_ReplicatedMovement);
    TRY_RESOLVE("Actor", "RootComponent", m_off.Actor_RootComponent);

    // SceneComponent
    TRY_RESOLVE("SceneComponent", "AttachParent", m_off.Scene_AttachParent);
    TRY_RESOLVE("SceneComponent", "RelativeLocation", m_off.Scene_RelativeLocation);
    TRY_RESOLVE("SceneComponent", "ComponentToWorld", m_off.Scene_ComponentToWorld);

    // 骨骼系统 (sdk_dump.cs + 既有 IDA 分析)
    TRY_RESOLVE_MULTI("Mesh", m_off.Char_Mesh,
        "Character", "CHARACTER", "CharacterBase", "GPCharacterBase", "GPCharacter");
    TRY_RESOLVE_MULTI("FPPMesh", m_off.Char_FPPMesh,
        "CharacterBase", "GPCharacter", "GPCharacterBase", "Character");
    TRY_RESOLVE_MULTI("AvatarComponent", m_off.STBase_AvatarComponent,
        "STExtraBaseCharacter", "STExtraCharacter", "CharacterBase", "Character");
    TRY_RESOLVE_MULTI("FPPComp", m_off.STBase_FPPComp,
        "STExtraBaseCharacter", "STExtraCharacter", "CharacterBase", "Character");
    TRY_RESOLVE_MULTI("MasterBoneComponent", m_off.Avatar_MasterBoneComponent,
        "AvatarComponent");
    TRY_RESOLVE_MULTI("meshComponentList", m_off.Avatar_MeshComponentList,
        "AvatarComponent");
    TRY_RESOLVE_MULTI("AvatarEntityList", m_off.Avatar_AvatarEntityList,
        "AvatarComponent");
    TRY_RESOLVE_MULTI("DefaultAvatarSubSystemList", m_off.Avatar_DefaultAvatarSubSystemList,
        "AvatarComponent");
    TRY_RESOLVE_MULTI("LocalSubSystemList", m_off.Avatar_LocalSubSystemList,
        "AvatarComponent");
    TRY_RESOLVE_MULTI("LocalActiveSubSystemList", m_off.Avatar_LocalActiveSubSystemList,
        "AvatarComponent");
    TRY_RESOLVE_MULTI("SkeletalMeshCompPool", m_off.Avatar_SkeletalMeshCompPool,
        "AvatarComponent");
    TRY_RESOLVE_MULTI("SkeletonMappingComp", m_off.AvatarFuncBranch_SkeletonMappingComp,
        "AvatarFuncBranch_NewFPP");
    TRY_RESOLVE_MULTI("SkeletalMesh", m_off.SkinnedMesh_SkeletalMesh,
        "SkinnedMeshComponent", "SkeletalMeshComponent");
    TRY_RESOLVE_MULTI("MasterPoseComponent", m_off.SkinnedMesh_MasterPoseComponent,
        "SkinnedMeshComponent", "SkeletalMeshComponent");
    TRY_RESOLVE_MULTI("CachedComponentSpaceTransforms", m_off.Skel_CachedCompSpace,
        "SkeletalMeshComponent", "SkinnedMeshComponent");
    TRY_RESOLVE_MULTI("CachedBoneSpaceTransforms", m_off.Skel_BoneSpaceTransforms,
        "SkeletalMeshComponent", "SkinnedMeshComponent");

    // Pawn
    TRY_RESOLVE("Pawn", "PlayerState", m_off.Pawn_PlayerState);

    // PlayerState
    TRY_RESOLVE("PlayerState", "PlayerName", m_off.PS_PlayerName);
    TRY_RESOLVE("PlayerState", "PawnPrivate", m_off.PS_PawnPrivate);

    // GameStateBase
    TRY_RESOLVE("GameStateBase", "PlayerArray", m_off.GS_PlayerArray);
    TRY_RESOLVE("GameStateBase", "bHasBegunPlay", m_off.GS_bHasBegunPlay);
    TRY_RESOLVE("GameStateBase", "ReplicatedWorldTimeSeconds", m_off.GS_ElapsedTime);
    TRY_RESOLVE("GameStateBase", "MatchState", m_off.GS_MatchState);
    TRY_RESOLVE("GameState", "MatchState", m_off.GS_MatchState);  // 回退: GameState 子类

    // World
    TRY_RESOLVE("World", "PersistentLevel", m_off.World_PersistentLevel);
    TRY_RESOLVE("World", "GameState", m_off.World_GameState);
    TRY_RESOLVE("World", "Levels", m_off.World_Levels);
    TRY_RESOLVE("World", "StreamingLevelsToConsider", m_off.World_StreamingLevels);

    // Level (UE5 的 Actors 可能叫 OwningWorld/Actors)
    TRY_RESOLVE("Level", "Actors", m_off.Level_Actors);

    // DFM 游戏自定义类 (如果也在反射中)
    TRY_RESOLVE("GPPlayerState", "TeamID", m_off.PS_TeamID);
    // sdk_dump: GPCharacterBase.HealthComp (字段名不是 GPHealthDataComponent)
    TRY_RESOLVE("GPCharacterBase", "HealthComp", m_off.Char_HealthComp);
    TRY_RESOLVE("GPCharacterBase", "CacheCurWeapon", m_off.Char_CurWeapon);

    // GPHealthDataComponent (sdk_dump 验证)
    TRY_RESOLVE("GPHealthDataComponent", "HealthMAX", m_off.HC_HealthMax);
    TRY_RESOLVE("GPHealthDataComponent", "HealthSet", m_off.HC_HealthSet);

    // PickupBase / Container (sdk_dump 验证)
    TRY_RESOLVE("PickupBase", "InventoryIdName", m_off.Pickup_InvIdName);
    TRY_RESOLVE("PickupBase", "InventoryType",   m_off.Pickup_InvType);
    TRY_RESOLVE("PickupBase", "StackCount",      m_off.Pickup_StackCount);
    TRY_RESOLVE("InventoryPickup_Container", "PickupBoxType", m_off.Cont_PickupBoxType);
    TRY_RESOLVE("InventoryPickup_Container", "ExtraRepInfo",  m_off.Cont_ExtraRepInfo);
    TRY_RESOLVE("InventoryPickup_Container", "RepItemArray",  m_off.Cont_RepItemArray);
    TRY_RESOLVE("InventoryPickup_Container", "bIsEmpty",      m_off.Cont_IsEmpty);

    // Interactor_SingleItemContainer (sdk_dump 验证)
    TRY_RESOLVE("Interactor_SingleItemContainer", "boxId",         m_off.SIC_BoxId);
    TRY_RESOLVE("Interactor_SingleItemContainer", "CachedPickups", m_off.SIC_CachedPickups);

    // InteractorBase (物品/箱子显示名)
    TRY_RESOLVE("InteractorBase", "InteractorName", m_off.Interactor_Name);

    // Actor (通用)
    TRY_RESOLVE("Actor", "Owner", m_off.Actor_Owner);

    // 相机系统 (反射可查)
    TRY_RESOLVE("Controller", "Pawn",              m_off.Ctrl_Pawn);
    TRY_RESOLVE("Controller", "PlayerState",       m_off.Ctrl_PlayerState);
    TRY_RESOLVE("Controller", "ControlRotation",   m_off.Ctrl_ControlRotation);
    TRY_RESOLVE("PlayerController", "AcknowledgedPawn", m_off.PC_AcknowledgedPawn);
    TRY_RESOLVE("PlayerController", "PlayerCameraManager", m_off.PC_PlayerCameraManager);
    TRY_RESOLVE("PlayerCameraManager", "DefaultFOV", m_off.PCM_DefaultFOV);
    TRY_RESOLVE("PlayerCameraManager", "CameraCache", m_off.PCM_CameraCache);
    TRY_RESOLVE("PlayerCameraManager", "LastFrameCameraCache", m_off.PCM_LastFrameCache);
    TRY_RESOLVE("PlayerCameraManager", "ViewTarget", m_off.PCM_ViewTarget);
    TRY_RESOLVE("PlayerCameraManager", "CameraCachePrivate", m_off.PCM_CameraCachePrivate);
    TRY_RESOLVE("PlayerCameraManager", "LastFrameCameraCachePrivate", m_off.PCM_LastFrameCachePriv);

    // 本地 PlayerController 查找链
    TRY_RESOLVE("World", "OwningGameInstance", m_off.World_OwningGameInstance);
    TRY_RESOLVE("GameInstance", "LocalPlayers", m_off.GI_LocalPlayers);
    TRY_RESOLVE("Player", "PlayerController", m_off.LP_PlayerController);
    TRY_RESOLVE("LocalPlayer", "PlayerController", m_off.LP_PlayerController);

    #undef TRY_RESOLVE_MULTI
    #undef TRY_RESOLVE

    LOG(LOG_LEVEL_INFO, TAG " 偏移解析完成: RepMove=0x%X RootComp=0x%X PS=0x%X GS_PA=0x%X PCM=0x%X CamCache=0x%X",
        m_off.Actor_ReplicatedMovement, m_off.Actor_RootComponent, m_off.Pawn_PlayerState, m_off.GS_PlayerArray,
        m_off.PC_PlayerCameraManager, m_off.PCM_CameraCachePrivate);
    return m_off.isValid();
}

bool DfmMatchMonitor::start() {
    if (m_running.load()) return false;
    if (m_moduleBase == 0) {
        LOG(LOG_LEVEL_ERROR, TAG " moduleBase 为空");
        return false;
    }

    // 动态解析偏移 (Interface 可选, 无则使用默认值)
    initOffsets();

    m_running.store(true, std::memory_order_release);
    m_pollThread = std::thread(&DfmMatchMonitor::pollLoop, this);
    LOG(LOG_LEVEL_INFO, TAG " 监控线程已启动");
    return true;
}

void DfmMatchMonitor::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_pollThread.joinable()) m_pollThread.join();
}

// =====================================================================
//  骨骼系统 — 基于 sdk_dump.cs / 既有 IDA 导出结果的读取链
// =====================================================================
//
// 运行时候选链:
//   Character.Mesh / CharacterBase.FPPMesh
//       -> SkinnedMeshComponent.MasterPoseComponent
//       -> SkeletalMeshComponent.CachedComponentSpaceTransforms
//       -> SceneComponent.ComponentToWorld
//
// 说明:
//   1. sdk_dump.cs 导出为 Character.Mesh=0x3D0, CharacterBase.FPPMesh=0xA80,
//      SkinnedMeshComponent.MasterPoseComponent=0x710,
//      SkeletalMeshComponent.CachedComponentSpaceTransforms=0x9D8。
//   2. 实际运行中 MasterPose 字段有时表现为直接 UObject*，有时更像
//      objectIndex+serial 的弱引用对，因此解析逻辑同时兼容两种存储。
//   3. CachedComponentSpaceTransforms 里的 Translation 已经是组件空间骨骼原点，
//      只需要再套一次 ComponentToWorld 的旋转/缩放/平移即可得到世界坐标。
//
// 固定索引只作为最后回退；正常路径优先从 SkeletalMesh+0x238 的
// FReferenceSkeleton.RawRefBoneInfo 按骨骼名动态映射到 17 个绘制槽位。
static const int kFallbackBoneMap[PlayerInfo::BONE_COUNT] = {
    14,  // [0] Head
    13,  // [1] Neck
    4,   // [2] Chest (spine_03)
    2,   // [3] Belly (spine_01)
    1,   // [4] Pelvis
    9,   // [5] R Shoulder (clavicle_r)
    11,  // [6] R Elbow (lowerarm_r)
    12,  // [7] R Hand
    5,   // [8] L Shoulder (clavicle_l)
    7,   // [9] L Elbow (lowerarm_l)
    8,   // [10] L Hand
    18,  // [11] R Thigh (thigh_r)
    19,  // [12] R Knee (calf_r)
    20,  // [13] R Foot (foot_r)
    15,  // [14] L Thigh (thigh_l)
    16,  // [15] L Knee (calf_l)
    17,  // [16] L Foot (foot_l)
};

int DfmMatchMonitor::matchBoneNamesFromFNameArray(uintptr_t dataPtr,
                                                  int count,
                                                  BoneAssetCacheEntry& entry) const {
    entry.trackedBoneIndices.fill(-1);
    entry.trackedBoneNames.fill(kInvalidTrackedBoneName);
    entry.trackedBoneNameValid.fill(0);
    entry.matchedCount = 0;
    if (dataPtr < 0x10000 || count <= 0 || count > kMaxRenderableBoneCount) return 0;

    // 一次性诊断: 全量dump 骨骼名字 + 索引, 让人看清哪个才是真头/脚骨。
    // 同一个 dataPtr 只 dump 一次, 避免刷屏。
    bool dumpThis = false;
    {
        static std::mutex sMu;
        static std::unordered_map<uintptr_t, bool> sDumped;
        std::lock_guard<std::mutex> lk(sMu);
        if (sDumped.find(dataPtr) == sDumped.end() && sDumped.size() < 8) {
            sDumped[dataPtr] = true;
            dumpThis = true;
        }
    }
    std::string dumpBuf;
    if (dumpThis) dumpBuf.reserve(2048);

    for (int index = 0; index < count; ++index) {
        const uintptr_t nameAddr = dataPtr + static_cast<uintptr_t>(index) * sizeof(FName);
        FName rawBoneName = kInvalidTrackedBoneName;
        const bool haveRawBoneName = safeReadMemory(nameAddr, &rawBoneName, sizeof(rawBoneName));
        const std::string rawName = readFName(nameAddr);
        const std::string normalizedName = normalizeBoneName(rawName);
        if (dumpThis && !rawName.empty()) {
            char tb[80];
            std::snprintf(tb, sizeof(tb), "[%d]%s ", index, rawName.c_str());
            dumpBuf += tb;
        }
        if (normalizedName.empty()) continue;

        for (size_t slot = 0; slot < PlayerInfo::BONE_COUNT; ++slot) {
            if (entry.trackedBoneIndices[slot] >= 0) continue;
            if (matchesTrackedBoneName(slot, normalizedName)) {
                entry.trackedBoneIndices[slot] = index;
                if (haveRawBoneName) {
                    entry.trackedBoneNames[slot] = rawBoneName;
                    entry.trackedBoneNameValid[slot] = 1;
                }
                entry.matchedCount++;
                break;
            }
        }
        // 不提前 break: 即使全部 slot 匹配完, 仍要 dump 完名字表 (仅dumpThis)
        if (!dumpThis && entry.matchedCount == PlayerInfo::BONE_COUNT) break;
    }
    if (dumpThis) {
        // 分 chunk 输出, logcat 单行上限 ~4KB, 超过会被截断
        size_t off = 0;
        int chunk = 0;
        while (off < dumpBuf.size()) {
            size_t take = std::min<size_t>(dumpBuf.size() - off, 1500);
            LOG(LOG_LEVEL_WARN, TAG " [bones-dump] dataPtr=%p count=%d chunk=%d: %.*s",
                (void*)dataPtr, count, chunk, (int)take, dumpBuf.c_str() + off);
            off += take;
            ++chunk;
        }
        LOG(LOG_LEVEL_WARN, TAG " [bones-dump] dataPtr=%p matched=%d slots: head[0]=%d neck[1]=%d chest[2]=%d pelvis[4]=%d rhand[7]=%d rfoot[13]=%d",
            (void*)dataPtr, entry.matchedCount,
            entry.trackedBoneIndices[0], entry.trackedBoneIndices[1],
            entry.trackedBoneIndices[2], entry.trackedBoneIndices[4],
            entry.trackedBoneIndices[7], entry.trackedBoneIndices[13]);
    }
    return entry.matchedCount;
}

int DfmMatchMonitor::matchBoneNamesFromBoneInfoArray(uintptr_t dataPtr,
                                                     int count,
                                                     int stride,
                                                     BoneAssetCacheEntry& entry) const {
    entry.trackedBoneIndices.fill(-1);
    entry.trackedBoneNames.fill(kInvalidTrackedBoneName);
    entry.trackedBoneNameValid.fill(0);
    entry.matchedCount = 0;
    if (dataPtr < 0x10000 || count <= 0 || count > kMaxRenderableBoneCount || stride < 8) {
        return 0;
    }

    bool dumpThis = false;
    {
        static std::mutex sMu;
        static std::unordered_map<uintptr_t, bool> sDumped;
        std::lock_guard<std::mutex> lk(sMu);
        if (sDumped.find(dataPtr) == sDumped.end() && sDumped.size() < 8) {
            sDumped[dataPtr] = true;
            dumpThis = true;
        }
    }
    std::string dumpBuf;
    if (dumpThis) dumpBuf.reserve(2048);

    for (int index = 0; index < count; ++index) {
        const uintptr_t nameAddr = dataPtr + static_cast<uintptr_t>(index) * static_cast<uintptr_t>(stride);
        FName rawBoneName = kInvalidTrackedBoneName;
        const bool haveRawBoneName = safeReadMemory(nameAddr, &rawBoneName, sizeof(rawBoneName));
        const std::string rawName = readFName(nameAddr);
        const std::string normalizedName = normalizeBoneName(rawName);
        if (dumpThis && !rawName.empty()) {
            char tb[80];
            std::snprintf(tb, sizeof(tb), "[%d]%s ", index, rawName.c_str());
            dumpBuf += tb;
        }
        if (normalizedName.empty()) continue;

        for (size_t slot = 0; slot < PlayerInfo::BONE_COUNT; ++slot) {
            if (entry.trackedBoneIndices[slot] >= 0) continue;
            if (matchesTrackedBoneName(slot, normalizedName)) {
                entry.trackedBoneIndices[slot] = index;
                if (haveRawBoneName) {
                    entry.trackedBoneNames[slot] = rawBoneName;
                    entry.trackedBoneNameValid[slot] = 1;
                }
                entry.matchedCount++;
                break;
            }
        }
        if (!dumpThis && entry.matchedCount == PlayerInfo::BONE_COUNT) break;
    }
    if (dumpThis) {
        size_t off = 0;
        int chunk = 0;
        while (off < dumpBuf.size()) {
            size_t take = std::min<size_t>(dumpBuf.size() - off, 1500);
            LOG(LOG_LEVEL_WARN, TAG " [bones-dump-bi] dataPtr=%p count=%d stride=%d chunk=%d: %.*s",
                (void*)dataPtr, count, stride, chunk, (int)take, dumpBuf.c_str() + off);
            off += take;
            ++chunk;
        }
        LOG(LOG_LEVEL_WARN, TAG " [bones-dump-bi] dataPtr=%p matched=%d slots: head[0]=%d neck[1]=%d chest[2]=%d pelvis[4]=%d rhand[7]=%d rfoot[13]=%d",
            (void*)dataPtr, entry.matchedCount,
            entry.trackedBoneIndices[0], entry.trackedBoneIndices[1],
            entry.trackedBoneIndices[2], entry.trackedBoneIndices[4],
            entry.trackedBoneIndices[7], entry.trackedBoneIndices[13]);
    }
    return entry.matchedCount;
}

uintptr_t DfmMatchMonitor::getSkeletalMeshAsset(uintptr_t meshComp) const {
    if (!ok(meshComp) || m_off.SkinnedMesh_SkeletalMesh <= 0) return 0;
    return resolveObjectField(meshComp + static_cast<uintptr_t>(m_off.SkinnedMesh_SkeletalMesh));
}

bool DfmMatchMonitor::resolveTrackedBoneIndices(uintptr_t skeletalMeshAssetPtr,
                                                BoneAssetCacheEntry& outEntry) const {
    outEntry.trackedBoneIndices.fill(-1);
    outEntry.trackedBoneNames.fill(kInvalidTrackedBoneName);
    outEntry.trackedBoneNameValid.fill(0);
    outEntry.matchedCount = 0;
    if (!ok(skeletalMeshAssetPtr)) return false;

    const auto cached = m_boneAssetCache.find(skeletalMeshAssetPtr);
    if (cached != m_boneAssetCache.end()) {
        outEntry = cached->second;
        return outEntry.matchedCount >= kMinTrackedBoneMatches;
    }

    if (m_boneAssetCache.size() > 256) {
        m_boneAssetCache.clear();
    }

    BoneAssetCacheEntry entry;
    entry.trackedBoneIndices.fill(-1);

    // FReferenceSkeleton 在 USkeletalMesh 内嵌, 但不是 UPROPERTY (sdk dump 不导出).
    // IDA 已确认: sizeof(FReferenceSkeleton)=0x108, sizeof(FMeshBoneInfo)=12 (FName+int32 ParentIndex).
    // RefSkeleton 第一个字段就是 TArray<FMeshBoneInfo> RawRefBoneInfo.
    // 这里扫描 asset[0x60..0x400] 找一个 TArray header (Data ptr + Num + Max),
    // 其 Data 指向的前几条记录用 stride=12 解析能匹配到至少 5 个已知骨骼名.
    uintptr_t bestData = 0;
    int32_t   bestNum  = 0;
    int       bestStride = 12;
    int       bestMatched = 0;
    BoneAssetCacheEntry bestEntry;
    {
        std::string dumpScan;
        for (uintptr_t off = 0x60; off <= 0x400; off += 8) {
            const uintptr_t dataPtr = safeReadPtr(skeletalMeshAssetPtr + off);
            const int32_t   num     = safeReadS32(skeletalMeshAssetPtr + off + 8);
            const int32_t   maxv    = safeReadS32(skeletalMeshAssetPtr + off + 12);
            if (dataPtr < 0x10000 || num < 8 || num > kMaxRenderableBoneCount) continue;
            if (maxv < num || maxv > num * 4) continue;
            // try strides 12, 16, 8
            for (int trialStride : {12, 16, 8}) {
                BoneAssetCacheEntry trial;
                int matched = matchBoneNamesFromBoneInfoArray(dataPtr, num, trialStride, trial);
                if (matched > bestMatched) {
                    bestMatched = matched;
                    bestData    = dataPtr;
                    bestNum     = num;
                    bestStride  = trialStride;
                    bestEntry   = trial;
                }
            }
            if (bestMatched >= 5) {
                // 同时 dump 命中的偏移
                dumpScan += "@0x" + [&]{ char b[8]; snprintf(b,sizeof(b),"%lx",(unsigned long)off); return std::string(b);}()
                         + " num=" + std::to_string(num)
                         + " stride=" + std::to_string(bestStride)
                         + " matched=" + std::to_string(bestMatched) + " ";
            }
        }
        // 一次性诊断: 输出扫描胜出的偏移 + 前 16 根骨名
        std::string names;
        if (bestData >= 0x10000 && bestNum > 0) {
            for (int i = 0; i < std::min<int32_t>(bestNum, 16); ++i) {
                std::string n = readFName(bestData + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(bestStride));
                names += "[" + std::to_string(i) + "]='" + n + "' ";
            }
        }
        LOG(LOG_LEVEL_WARN, TAG " [bones-scan] mesh=%p winner=%p num=%d stride=%d matched=%d hits=[%s] names=%s",
            (void*)skeletalMeshAssetPtr, (void*)bestData, bestNum, bestStride, bestMatched,
            dumpScan.c_str(), names.c_str());

        // 二次诊断: 17 个 slot 各自命中的 bone idx + 名字, 用于核实 head 别名是否走对
        if (bestMatched > 0 && bestData >= 0x10000) {
            std::string slotMap;
            static const char* kSlotLabels[PlayerInfo::BONE_COUNT] = {
                "head","neck","chest","spine","pelvis",
                "Rshld","Rfarm","Rhand","Lshld","Lfarm","Lhand",
                "Rthigh","Rcalf","Rfoot","Lthigh","Lcalf","Lfoot"
            };
            for (int slot = 0; slot < PlayerInfo::BONE_COUNT; ++slot) {
                int idx = bestEntry.trackedBoneIndices[slot];
                std::string bn = (idx >= 0 && idx < bestNum)
                    ? readFName(bestData + static_cast<uintptr_t>(idx) * static_cast<uintptr_t>(bestStride))
                    : std::string("-");
                slotMap += std::string(kSlotLabels[slot]) + "[" + std::to_string(slot) + "]=" + std::to_string(idx) + ":" + bn + " ";
            }
            LOG(LOG_LEVEL_WARN, TAG " [bones-slotmap] mesh=%p %s", (void*)skeletalMeshAssetPtr, slotMap.c_str());
        }
    }
    if (bestMatched > 0) {
        entry = bestEntry;
    }

    if (entry.matchedCount < kMinTrackedBoneMatches) {
        constexpr uintptr_t kSkeletonOffset = 0x48;
        constexpr uintptr_t kRefBoneNamesOffset = 0x280;
        const uintptr_t skeletonPtr = resolveObjectField(skeletalMeshAssetPtr + kSkeletonOffset);
        if (ok(skeletonPtr)) {
            TArray<FName> refBoneNames{};
            if (safeReadMemory(skeletonPtr + kRefBoneNamesOffset, &refBoneNames, sizeof(refBoneNames))
                && isUsableRemoteArray(refBoneNames, kMaxRenderableBoneCount)) {
                BoneAssetCacheEntry trial;
                if (matchBoneNamesFromFNameArray(reinterpret_cast<uintptr_t>(refBoneNames.Data),
                                                 refBoneNames.Num,
                                                 trial) > entry.matchedCount) {
                    entry = trial;
                }
            }
        }
    }

    m_boneAssetCache[skeletalMeshAssetPtr] = entry;
    outEntry = entry;
    return entry.matchedCount >= kMinTrackedBoneMatches;
}

int32_t DfmMatchMonitor::getCachedTransformCount(uintptr_t meshComp) const {
    if (!ok(meshComp) || m_off.Skel_CachedCompSpace <= 0) return 0;

    TArray<FTransform> transforms{};
    if (!safeReadMemory(meshComp + static_cast<uintptr_t>(m_off.Skel_CachedCompSpace),
                        &transforms,
                        sizeof(transforms))) {
        return 0;
    }
    return isUsableRemoteArray(transforms, kMaxRenderableBoneCount) ? transforms.Num : 0;
}

int32_t DfmMatchMonitor::collectSparseMapValues(uintptr_t mapBase,
                                               std::vector<uintptr_t>& outValues,
                                               int32_t maxEntries) const {
    if (!ok(mapBase) || maxEntries <= 0) return 0;

    const uintptr_t entries = safeReadPtr(mapBase);
    const int32_t maxIdx = safeReadS32(mapBase + 0x28);
    if (!ok(entries) || maxIdx <= 0 || maxIdx > maxEntries) {
        return 0;
    }

    const uintptr_t secFlags = safeReadPtr(mapBase + 0x20);
    int32_t activeCount = 0;
    for (int32_t wordIndex = 0; wordIndex < (maxIdx + 31) / 32; ++wordIndex) {
        uint32_t flags = 0;
        if (wordIndex < 4) {
            safeReadMemory(mapBase + 0x10 + wordIndex * 4, &flags, sizeof(flags));
        } else if (ok(secFlags)) {
            safeReadMemory(secFlags + wordIndex * 4, &flags, sizeof(flags));
        }

        for (int bit = 0; bit < 32 && flags; ++bit, flags >>= 1) {
            if ((flags & 1u) == 0) continue;
            const int32_t idx = wordIndex * 32 + bit;
            if (idx >= maxIdx) break;

            ++activeCount;
            const uintptr_t valueAddr = entries + static_cast<uintptr_t>(idx) * 24 + 8;
            const uintptr_t value = resolveObjectField(valueAddr);
            if (ok(value)) {
                outValues.push_back(value);
            }
        }
    }
    return activeCount;
}

int32_t DfmMatchMonitor::collectObjectArrayValues(uintptr_t arrayAddr,
                                                  std::vector<uintptr_t>& outValues,
                                                  int32_t maxEntries) const {
    if (!ok(arrayAddr) || maxEntries <= 0) return 0;

    TArray<uintptr_t> array{};
    if (!safeReadMemory(arrayAddr, &array, sizeof(array))
        || !isUsableRemoteArray(array, maxEntries)) {
        return 0;
    }

    const uintptr_t data = reinterpret_cast<uintptr_t>(array.Data);
    int32_t activeCount = 0;
    for (int32_t i = 0; i < array.Num; ++i) {
        ++activeCount;
        const uintptr_t value = resolveObjectField(data + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
        if (ok(value)) {
            outValues.push_back(value);
        }
    }
    return activeCount;
}

uintptr_t DfmMatchMonitor::scanObjectForMeshComponent(uintptr_t objectPtr, int scanBytes) const {
    if (!ok(objectPtr) || scanBytes < 8) return 0;

    uintptr_t bestComp = 0;
    int32_t bestCount = 0;
    auto considerMesh = [&](uintptr_t meshComp) {
        if (!ok(meshComp)) return;
        const std::string className = readClassName(meshComp);
        if (!isLikelyMeshComponentClass(className)) return;

        int32_t transformCount = getCachedTransformCount(meshComp);
        if (transformCount > bestCount) {
            bestComp = meshComp;
            bestCount = transformCount;
        }

        const uintptr_t masterComp = followMasterPoseChain(meshComp);
        const int32_t masterCount = getCachedTransformCount(masterComp);
        if (masterCount > bestCount) {
            bestComp = masterComp;
            bestCount = masterCount;
        }
    };

    for (int offset = 0; offset <= scanBytes - 8; offset += 8) {
        const uintptr_t candidate = resolveObjectField(objectPtr + static_cast<uintptr_t>(offset));
        if (!ok(candidate) || candidate == objectPtr) continue;

        const std::string className = readClassName(candidate);
        if (isLikelyMeshComponentClass(className)) {
            considerMesh(candidate);
            continue;
        }

        if (m_off.AvatarFuncBranch_SkeletonMappingComp > 0
            && className.find("AvatarFuncBranch") != std::string::npos) {
            const uintptr_t skeletonMappingComp = resolveObjectField(
                candidate + static_cast<uintptr_t>(m_off.AvatarFuncBranch_SkeletonMappingComp));
            considerMesh(skeletonMappingComp);
        }
    }

    return bestComp;
}

uintptr_t DfmMatchMonitor::resolveObjectField(uintptr_t fieldAddr) const {
    auto looksLikeUObject = [&](uintptr_t objPtr) -> bool {
        if (!ok(objPtr)) return false;
        uintptr_t classPtr = safeReadPtr(objPtr + offsetof(UObjectBase, ClassPrivate));
        return ok(classPtr);
    };

    uintptr_t directPtr = safeReadPtr(fieldAddr);
    if (looksLikeUObject(directPtr)) {
        return directPtr;
    }

    RemoteWeakObjectPtr weakPtr{};
    if (!safeReadMemory(fieldAddr, &weakPtr, sizeof(weakPtr))
        || weakPtr.objectIndex < 0
        || weakPtr.objectSerialNumber <= 0) {
        return 0;
    }

    const uint32_t totalObjects = safeReadU32(m_moduleBase + m_offGUObjectArrayNum);
    const uintptr_t chunkTable = safeReadPtr(m_moduleBase + m_offGUObjectArrayChunks);
    if (!ok(chunkTable) || weakPtr.objectIndex >= static_cast<int32_t>(totalObjects)) {
        return 0;
    }

    const uint32_t objectIndex = static_cast<uint32_t>(weakPtr.objectIndex);
    const uintptr_t chunkBase = safeReadPtr(
        chunkTable + static_cast<uintptr_t>(objectIndex >> 16) * sizeof(uintptr_t));
    if (!ok(chunkBase)) {
        return 0;
    }

    FUObjectItem item{};
    const uintptr_t itemAddr = chunkBase
        + static_cast<uintptr_t>(objectIndex & 0xFFFF) * sizeof(FUObjectItem);
    if (!safeReadMemory(itemAddr, &item, sizeof(item))
        || item.SerialNumber != weakPtr.objectSerialNumber) {
        return 0;
    }

    const uintptr_t resolved = reinterpret_cast<uintptr_t>(item.Object);
    return looksLikeUObject(resolved) ? resolved : 0;
}

uintptr_t DfmMatchMonitor::followMasterPoseChain(uintptr_t meshComp) const {
    if (!ok(meshComp) || m_off.SkinnedMesh_MasterPoseComponent <= 0) {
        return meshComp;
    }

    uintptr_t bestComp = meshComp;
    int32_t bestCount = getCachedTransformCount(meshComp);
    uintptr_t current = meshComp;
    for (int depth = 0; depth < kMaxMasterPoseDepth; ++depth) {
        uintptr_t next = resolveObjectField(
            current + static_cast<uintptr_t>(m_off.SkinnedMesh_MasterPoseComponent));
        if (!ok(next) || next == current) {
            break;
        }

        int32_t nextCount = getCachedTransformCount(next);
        if (nextCount > bestCount) {
            bestComp = next;
            bestCount = nextCount;
        }
        current = next;
    }
    return bestComp;
}

bool DfmMatchMonitor::decryptSceneComponentToWorld(uintptr_t sceneComp,
                                                   FTransform& outTransform) const {
    if (!ok(sceneComp) || m_moduleBase == 0) return false;

    // 通过 sub_D982DB0 trampoline 拿到当前帧解密后的 ComponentToWorld 指针。
    // 该 trampoline 已被反作弊改写, 真正实现位于 RWX 堆里; 但 trampoline 自身
    // (ADD X0,#0x210; LDR X16,=imm; BR X16) 在游戏 .text 内偏移固定, 我们 BL 它即可,
    // 字面量池由游戏自己维护并在每次启动时重新指向当前的解密 stub。
    const uintptr_t vtable = safeReadPtr(sceneComp);
    if (!ok(vtable)) return false;

    auto fn = reinterpret_cast<DecryptComponentToWorldFn>(
        m_moduleBase + kSceneComponentDecryptCTWThunkOffset);
    const void* result = fn(sceneComp);
    if (!result) return false;

    // 解密后的 FTransform 由 stub 内部 buffer 持有, 立即拷贝出来。
    // 直接 memcpy: stub 返回的指针在 mmap 出来的合法堆段, 内核可读, 无需 safeReadMemory。
    std::memcpy(&outTransform, result, sizeof(outTransform));
    return std::isfinite(outTransform.TranslationX)
        && std::isfinite(outTransform.TranslationY)
        && std::isfinite(outTransform.TranslationZ);
}

bool DfmMatchMonitor::decryptSceneComponentLocation(uintptr_t sceneComp,
                                                   FVector3& outLoc) const {
    FTransform t{};
    if (!decryptSceneComponentToWorld(sceneComp, t)) return false;
    outLoc = {t.TranslationX, t.TranslationY, t.TranslationZ};
    return hasUsableActorPoint(outLoc);
}

bool DfmMatchMonitor::getSocketLocationFromMesh(uintptr_t meshComp,
                                                const FName& socketName,
                                                FVector3& outLoc) const {
    outLoc = {};
    if (!ok(meshComp) || socketName.ComparisonIndex <= 0) {
        return false;
    }

    const uintptr_t vtable = safeReadPtr(meshComp);
    if (!ok(vtable)) {
        return false;
    }

    const uintptr_t fnAddr = safeReadPtr(vtable + kSceneComponentGetSocketLocationVtableOffset);
    if (!ok(fnAddr)) {
        return false;
    }
    if (m_moduleBase != 0 && m_moduleSize != 0
        && (fnAddr < m_moduleBase || fnAddr >= (m_moduleBase + m_moduleSize))) {
        return false;
    }

    using GetSocketLocationFn = FVector3 (*)(void*, FName);
    const auto fn = reinterpret_cast<GetSocketLocationFn>(fnAddr);
    const FVector3 value = fn(reinterpret_cast<void*>(meshComp), socketName);
    if (!hasUsableActorPoint(value)) {
        return false;
    }

    outLoc = value;
    return true;
}

bool DfmMatchMonitor::getCharacterSocketLocation(uintptr_t characterPtr, FVector3& outLoc) const {
    outLoc = {};
    if (!ok(characterPtr)) {
        return false;
    }

    const uintptr_t meshComp = resolveBestBoneMeshComponent(characterPtr);
    if (!ok(meshComp)) {
        return false;
    }

    BoneAssetCacheEntry boneEntry;
    resolveTrackedBoneIndices(getSkeletalMeshAsset(meshComp), boneEntry);

    auto trySocketSlot = [&](size_t slot, FVector3& socketPos) -> bool {
        if (slot >= PlayerInfo::BONE_COUNT || boneEntry.trackedBoneIndices[slot] < 0
            || boneEntry.trackedBoneNameValid[slot] == 0) {
            return false;
        }
        return getSocketLocationFromMesh(meshComp, boneEntry.trackedBoneNames[slot], socketPos);
    };

    FVector3 rightFoot{};
    FVector3 leftFoot{};
    const bool haveRightFoot = trySocketSlot(kTrackedBoneRightFootSlot, rightFoot);
    const bool haveLeftFoot = trySocketSlot(kTrackedBoneLeftFootSlot, leftFoot);
    if (haveRightFoot && haveLeftFoot) {
        outLoc.x = (rightFoot.x + leftFoot.x) * 0.5f;
        outLoc.y = (rightFoot.y + leftFoot.y) * 0.5f;
        outLoc.z = std::min(rightFoot.z, leftFoot.z);
        return true;
    }
    if (haveRightFoot) {
        outLoc = rightFoot;
        return true;
    }
    if (haveLeftFoot) {
        outLoc = leftFoot;
        return true;
    }

    FVector3 pelvis{};
    if (trySocketSlot(kTrackedBonePelvisSlot, pelvis)) {
        outLoc.x = pelvis.x;
        outLoc.y = pelvis.y;
        outLoc.z = pelvis.z - 90.0f;
        return hasUsableActorPoint(outLoc);
    }

    FVector3 head{};
    if (trySocketSlot(kTrackedBoneHeadSlot, head)) {
        outLoc.x = head.x;
        outLoc.y = head.y;
        outLoc.z = head.z - 160.0f;
        return hasUsableActorPoint(outLoc);
    }

    return false;
}

uintptr_t DfmMatchMonitor::resolveBestBoneMeshComponent(uintptr_t characterPtr) const {
    struct Candidate {
        uintptr_t meshComp = 0;
        int32_t transformCount = 0;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(16);

    Candidate best{};
    auto consider = [&](uintptr_t meshComp) {
        if (!ok(meshComp)) return;
        for (const auto& existing : candidates) {
            if (existing.meshComp == meshComp) return;
        }
        int32_t transformCount = getCachedTransformCount(meshComp);
        candidates.push_back({meshComp, transformCount});
        if (transformCount > best.transformCount) {
            best.meshComp = meshComp;
            best.transformCount = transformCount;
        }
    };

    auto readMeshField = [&](int32_t offset) -> uintptr_t {
        return (offset > 0)
            ? resolveObjectField(characterPtr + static_cast<uintptr_t>(offset))
            : 0;
    };

    const uintptr_t mesh3p = readMeshField(m_off.Char_Mesh);
    consider(mesh3p);
    consider(followMasterPoseChain(mesh3p));

    const uintptr_t meshFpp = readMeshField(m_off.Char_FPPMesh);
    consider(meshFpp);
    consider(followMasterPoseChain(meshFpp));

    auto scanAvatarComponent = [&](uintptr_t avatarComp) {
        if (!ok(avatarComp)) return;

        const uintptr_t masterBone = resolveObjectField(
            avatarComp + static_cast<uintptr_t>(m_off.Avatar_MasterBoneComponent));
        consider(masterBone);
        consider(followMasterPoseChain(masterBone));

        std::vector<uintptr_t> meshListValues;
        meshListValues.reserve(16);
        collectSparseMapValues(
            avatarComp + static_cast<uintptr_t>(m_off.Avatar_MeshComponentList),
            meshListValues,
            256);
        for (uintptr_t meshComp : meshListValues) {
            consider(meshComp);
            consider(followMasterPoseChain(meshComp));
        }

        TArray<uintptr_t> pool{};
        if (safeReadMemory(avatarComp + static_cast<uintptr_t>(m_off.Avatar_SkeletalMeshCompPool),
                           &pool,
                           sizeof(pool))
            && isUsableRemoteArray(pool, 64)) {
            const uintptr_t poolData = reinterpret_cast<uintptr_t>(pool.Data);
            for (int i = 0; i < pool.Num; ++i) {
                uintptr_t meshComp = resolveObjectField(
                    poolData + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
                consider(meshComp);
                consider(followMasterPoseChain(meshComp));
            }
        }

        if (best.transformCount < kMinRenderableBoneCount && m_off.Avatar_AvatarEntityList > 0) {
            std::vector<uintptr_t> entityValues;
            entityValues.reserve(16);
            collectSparseMapValues(
                avatarComp + static_cast<uintptr_t>(m_off.Avatar_AvatarEntityList),
                entityValues,
                128);
            for (uintptr_t entity : entityValues) {
                consider(scanObjectForMeshComponent(entity, 0x200));
            }
        }

        if (best.transformCount < kMinRenderableBoneCount) {
            std::vector<uintptr_t> subsystemValues;
            subsystemValues.reserve(24);
            collectObjectArrayValues(
                avatarComp + static_cast<uintptr_t>(m_off.Avatar_DefaultAvatarSubSystemList),
                subsystemValues,
                64);
            collectObjectArrayValues(
                avatarComp + static_cast<uintptr_t>(m_off.Avatar_LocalSubSystemList),
                subsystemValues,
                64);
            collectObjectArrayValues(
                avatarComp + static_cast<uintptr_t>(m_off.Avatar_LocalActiveSubSystemList),
                subsystemValues,
                64);
            for (uintptr_t subsystem : subsystemValues) {
                consider(scanObjectForMeshComponent(subsystem, 0x200));
            }
        }

        if (best.transformCount < kMinRenderableBoneCount) {
            consider(scanObjectForMeshComponent(avatarComp, 0x1000));
        }
    };

    const uintptr_t avatarComp = readMeshField(m_off.STBase_AvatarComponent);
    scanAvatarComponent(avatarComp);

    const uintptr_t fppComp = readMeshField(m_off.STBase_FPPComp);
    if (ok(fppComp)) {
        int32_t avatarFieldOffset = m_off.FPPComp_AvatarComp;
        if (m_interface) {
            const std::string fppClass = readClassName(fppComp);
            if (!fppClass.empty() && fppClass[0] != '<') {
                for (const char* fieldName : {"_AvatarComp", "AvatarComp", "AvatarComponent"}) {
                    std::string owner;
                    const auto* info = m_interface->findFieldInHierarchy(fppClass, fieldName, &owner);
                    if (info) {
                        avatarFieldOffset = info->offset;
                        break;
                    }
                }
            }
        }

        const uintptr_t fppAvatar = resolveObjectField(
            fppComp + static_cast<uintptr_t>(avatarFieldOffset));
        if (ok(fppAvatar) && fppAvatar != avatarComp) {
            scanAvatarComponent(fppAvatar);
        }

        if (best.transformCount < kMinRenderableBoneCount) {
            consider(scanObjectForMeshComponent(fppComp, 0x400));
            if (ok(fppAvatar)) {
                consider(scanObjectForMeshComponent(fppAvatar, 0x1000));
            }
        }
    }

    return best.transformCount >= kMinRenderableBoneCount ? best.meshComp : 0;
}

bool DfmMatchMonitor::fillPlayerBones(PlayerInfo& player) const {
    for (auto& bone : player.bones) bone = {};
    player.bonesValid = false;
    if (!ok(player.characterPtr)) return false;

    const uintptr_t meshComp = resolveBestBoneMeshComponent(player.characterPtr);
    if (!ok(meshComp)) return false;

    // 反作弊会随版本搬动 CachedComponentSpaceTransforms 偏移, 而误读到旁边的
    // CachedBoneSpaceTransforms (父空间局部 transform) 会让所有骨骼坍缩到一条线;
    // 误读到无效区域则会得到天文数字 Z spread 反而骗过宽松筛选。
    // 自动探测: 在 0x9C8 ~ 0xA08 区间内, 选满足以下三条的最优偏移
    //   ① Z 轴展开在 60 ~ 500 cm (合理人形/含挂件)
    //   ② 大多数四元数模长 ∈ [0.5, 1.5] (有效 FQuat 都是单位四元数)
    //   ③ Translation 全部有限且 |xyz| < 5e3 cm
    // 命中后按 meshComp 缓存, 避免每帧重新枚举。
    static const int32_t kCandidateOffsets[] = {
        0x9E8, 0x9F8, 0xA08, 0x9D8, 0x9C8,           // 原候选 (CachedComponentSpaceTransforms 周边)
        0xA18, 0xA28, 0xA38, 0xA48,                  // 向后扩展 16 字节步进
        0x9B8, 0x9A8, 0x998,                         // 向前扩展
        0xAA8, 0xAB8, 0xAC8,                         // ComponentSpaceTransformsArray 在某些版本位置
        0xC30, 0xC40, 0xC50,                         // BoneSpaceTransforms 之外的另一个布局
    };
    constexpr float kMinHumanHeightCm = 60.f;   // 蹲伏/儿童 mesh 下限
    constexpr float kMaxHumanHeightCm = 500.f;  // 含巨型 mesh / 挂件
    constexpr float kMaxTranslationCm = 5000.f; // 单根骨骼 Translation 绝对值上限

    auto tryReadArray = [&](int32_t off, TArray<FTransform>& out) -> bool {
        if (!safeReadMemory(meshComp + static_cast<uintptr_t>(off),
                            &out, sizeof(out))) return false;
        return isUsableRemoteArray(out, kMaxRenderableBoneCount)
            && out.Num >= kMinRenderableBoneCount;
    };

    auto evaluateSpread = [&](const TArray<FTransform>& arr) -> float {
        // 采样最多 32 根骨骼, 校验四元数 + 平移合理性, 再算 Z 极差。
        const int sample = std::min<int>(arr.Num, 32);
        std::array<FTransform, 32> buf{};
        if (!safeReadMemory(reinterpret_cast<uintptr_t>(arr.Data),
                            buf.data(), sizeof(FTransform) * sample)) return -1.f;
        float zMin = 1e30f, zMax = -1e30f;
        int valid = 0;
        for (int i = 0; i < sample; ++i) {
            const FTransform& t = buf[i];
            // ① 平移分量必须有限且在合理量级
            if (!std::isfinite(t.TranslationX) || !std::isfinite(t.TranslationY)
                || !std::isfinite(t.TranslationZ)) return -1.f;
            if (std::fabs(t.TranslationX) > kMaxTranslationCm
                || std::fabs(t.TranslationY) > kMaxTranslationCm
                || std::fabs(t.TranslationZ) > kMaxTranslationCm) return -1.f;
            // ② 四元数模长应 ≈ 1 (允许 ±0.5 容忍 SIMD 精度)
            float qLen2 = t.RotationX*t.RotationX + t.RotationY*t.RotationY
                        + t.RotationZ*t.RotationZ + t.RotationW*t.RotationW;
            if (!std::isfinite(qLen2) || qLen2 < 0.25f || qLen2 > 2.25f) return -1.f;
            zMin = std::min(zMin, t.TranslationZ);
            zMax = std::max(zMax, t.TranslationZ);
            ++valid;
        }
        if (valid < 12) return -1.f;
        const float spread = zMax - zMin;
        if (spread < kMinHumanHeightCm || spread > kMaxHumanHeightCm) return -1.f;
        return spread;
    };

    TArray<FTransform> boneArray{};
    int32_t chosenOffset = 0;
    float bestSpread = -1.f;

    // 按 meshComp 缓存上次有效偏移 (不同玩家不同 mesh 互不干扰)
    {
        std::lock_guard<std::mutex> lk(m_compSpaceOffsetMu);
        auto it = m_compSpaceOffsetCache.find(meshComp);
        if (it != m_compSpaceOffsetCache.end()) {
            TArray<FTransform> tmp{};
            if (tryReadArray(it->second, tmp)) {
                float sp = evaluateSpread(tmp);
                if (sp > 0.f) {
                    boneArray = tmp;
                    chosenOffset = it->second;
                    bestSpread = sp;
                }
            }
        }
    }

    if (chosenOffset == 0) {
        // 一次性诊断: 打出所有候选的实际 (Zmin, Zmax, count) 让用户判断哪个才是
        // 真正的标准 component-space (foot 应该 ≈ 0)。每 5s 输出一次, 防刷屏。
        bool wantProbe = false;
        {
            static auto sLastProbe = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (now - sLastProbe > std::chrono::seconds(5)) {
                sLastProbe = now;
                wantProbe = true;
            }
        }

        struct CandSnap {
            int32_t off;
            int     num;
            float   zMin, zMax, spread;
            float   bone0Z, bone1Z, bone2Z;
            bool    valid;
        };
        std::vector<CandSnap> probes;

        for (int32_t off : kCandidateOffsets) {
            TArray<FTransform> tmp{};
            if (!tryReadArray(off, tmp)) {
                if (wantProbe) probes.push_back({off, 0, 0, 0, 0, 0, 0, 0, false});
                continue;
            }
            float sp = evaluateSpread(tmp);
            if (wantProbe) {
                // 抓前 32 根再算一遍 zMin/zMax (无校验, 纯展示)
                int n = std::min<int>(tmp.Num, 32);
                std::array<FTransform, 32> rawBuf{};
                CandSnap cs{off, tmp.Num, 1e30f, -1e30f, sp, 0.f, 0.f, 0.f, false};
                cs.spread = sp;
                if (safeReadMemory(reinterpret_cast<uintptr_t>(tmp.Data),
                                   rawBuf.data(), sizeof(FTransform) * n)) {
                    for (int i = 0; i < n; ++i) {
                        float z = rawBuf[i].TranslationZ;
                        if (std::isfinite(z)) {
                            cs.zMin = std::min(cs.zMin, z);
                            cs.zMax = std::max(cs.zMax, z);
                        }
                    }
                    cs.bone0Z = rawBuf[0].TranslationZ;
                    cs.bone1Z = rawBuf[1].TranslationZ;
                    cs.bone2Z = rawBuf[2].TranslationZ;
                    cs.valid = true;
                }
                probes.push_back(cs);
            }
            if (sp > bestSpread) { bestSpread = sp; boneArray = tmp; chosenOffset = off; }
        }

        if (wantProbe) {
            std::string buf;
            char tmp[160];
            for (const auto& c : probes) {
                if (!c.valid) {
                    std::snprintf(tmp, sizeof(tmp), "[0x%X readFail] ", c.off);
                } else {
                    std::snprintf(tmp, sizeof(tmp),
                        "[0x%X n=%d z=[%.0f..%.0f]=%.0f sp_eval=%.0f b0z=%.0f b1z=%.0f b2z=%.0f] ",
                        c.off, c.num, c.zMin, c.zMax, c.zMax - c.zMin, c.spread,
                        c.bone0Z, c.bone1Z, c.bone2Z);
                }
                buf += tmp;
            }
            LOG(LOG_LEVEL_WARN, TAG " [bones-probe] mesh=%p picked=0x%X cands: %s",
                (void*)meshComp, chosenOffset, buf.c_str());
        }

        if (chosenOffset == 0) {
            // 所有候选要么读失败要么校验不过 (BoneSpace 局部偏移 / 垃圾内存)
            static auto lastWarn = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (now - lastWarn > std::chrono::seconds(5)) {
                lastWarn = now;
                LOG(LOG_LEVEL_WARN, TAG " [bones] no valid CompSpace offset for mesh %p", (void*)meshComp);
            }
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(m_compSpaceOffsetMu);
            m_compSpaceOffsetCache[meshComp] = chosenOffset;
        }
        LOG(LOG_LEVEL_INFO, TAG " [bones] mesh %p picked CompSpace offset 0x%X (Z spread %.1f cm, %d bones)",
            (void*)meshComp, chosenOffset, bestSpread, boneArray.Num);
    }

    // SkeletalMeshComponent.ComponentToWorld 与 SceneComponent.ComponentToWorld
    // 是同一个加密字段; 直接读 meshComp+0x210 对远端 replicated 角色会拿到陈旧/
    // 错位的矩阵, 把所有局部空间骨骼变换乘上去后整副骨架就会平移到错误位置 ──
    // 这就是 ESP 框 "目标一动就跟不上" 的真正物理来源。先尝试通过解密 trampoline
    // 拿到当前帧的明文 CTW; 失败才退回直接内存读。
    FTransform componentToWorld{};
    bool ctwDecrypted = decryptSceneComponentToWorld(meshComp, componentToWorld);
    if (!ctwDecrypted) {
        if (!safeReadMemory(meshComp + static_cast<uintptr_t>(m_off.Scene_ComponentToWorld),
                            &componentToWorld,
                            sizeof(componentToWorld))) {
            return false;
        }
    }

    // ── mesh 平移锚定 (修复版) ──
    //   旧版无条件 `mesh.T = root.T - (0,0,88)`, 假设 UE 默认 capsule half-height=88,
    //   但 DFM IntCharacter 的实际半高未必是 88, 导致整副骨架被强制下移到脚下。
    //   症状: ESP 框出现在角色脚跟附近, 框高很短只覆盖小腿。
    //
    //   修复策略 (按可信度优先):
    //     A. mesh.CTW 解密成功且 mesh.T 与 root.T 距离合理 (drift < 200cm)
    //        → 直接用 mesh.T (UE 引擎已经计算好的当前帧明文, 含正确的 RelativeLocation 偏移)
    //     B. mesh.CTW 解密成功但 drift 太大 (滞后/LOD 冻结)
    //        → 用 root.T 校正 XY, Z 用 mesh.T 自己的相对值 (root.Z + (mesh.Z - mesh.Z_at_anchor)),
    //          这里简化为只校正 XY, Z 保留 mesh 自己的值
    //     C. mesh.CTW 解密失败
    //        → 退回 root.T (Z 不偏移; 由 bone localPos.Z 决定垂直分布)
    //   保留 mesh 自身的 Rotation/Scale (动画/朝向)。
    {
        const uintptr_t rootComp = safeReadPtr(player.characterPtr + m_off.Actor_RootComponent);
        FTransform rootCtw{};
        const bool rootDec = ok(rootComp) && decryptSceneComponentToWorld(rootComp, rootCtw);
        const bool rootTValid = rootDec
            && std::isfinite(rootCtw.TranslationX) && std::isfinite(rootCtw.TranslationY) && std::isfinite(rootCtw.TranslationZ)
            && (std::fabs(rootCtw.TranslationX) > 1.f || std::fabs(rootCtw.TranslationY) > 1.f || std::fabs(rootCtw.TranslationZ) > 1.f)
            && std::fabs(rootCtw.TranslationX) < 1e7f && std::fabs(rootCtw.TranslationY) < 1e7f && std::fabs(rootCtw.TranslationZ) < 1e7f;
        const bool meshTValid = ctwDecrypted
            && std::isfinite(componentToWorld.TranslationX)
            && std::isfinite(componentToWorld.TranslationY)
            && std::isfinite(componentToWorld.TranslationZ);

        if (rootTValid && meshTValid) {
            const float dx = componentToWorld.TranslationX - rootCtw.TranslationX;
            const float dy = componentToWorld.TranslationY - rootCtw.TranslationY;
            const float dz_xy = std::sqrt(dx*dx + dy*dy);  // 仅 XY 偏差
            // 路径 A: mesh 与 root 同步 → 信任 mesh.T (含正确 RelativeLocation.Z)
            // 路径 B: mesh 滞后 (XY 偏差 > 200cm) → 用 root.T 校正 XY, 保留 Z
            //         这避免假设具体的 capsule_half_height 值
            if (dz_xy > 200.f) {
                componentToWorld.TranslationX = rootCtw.TranslationX;
                componentToWorld.TranslationY = rootCtw.TranslationY;
                static auto s_lastAnchorLog = std::chrono::steady_clock::time_point{};
                auto now = std::chrono::steady_clock::now();
                if (now - s_lastAnchorLog > std::chrono::seconds(2)) {
                    s_lastAnchorLog = now;
                    LOG(LOG_LEVEL_WARN, TAG " [bones-anchor] %s mesh-XY-drift=%.0fcm > 200cm, snap to root.XY",
                        player.playerName.c_str(), dz_xy);
                }
            }
        } else if (rootTValid && !meshTValid) {
            // 路径 C: mesh 解密失败, 用 root 校正 XY, Z 保留原值
            componentToWorld.TranslationX = rootCtw.TranslationX;
            componentToWorld.TranslationY = rootCtw.TranslationY;
            // Z 不变: 让骨骼 localPos.Z 自己决定垂直分布
            static auto s_lastNoMeshLog = std::chrono::steady_clock::time_point{};
            auto now = std::chrono::steady_clock::now();
            if (now - s_lastNoMeshLog > std::chrono::seconds(5)) {
                s_lastNoMeshLog = now;
                LOG(LOG_LEVEL_WARN, TAG " [bones-anchor] %s meshDecFail, using root.XY",
                    player.playerName.c_str());
            }
        }
    }

    // 诊断: 周期性比较 mesh.CTW 与 root.CTW (capsule), 看是否旋转一致
    {
        static auto s_lastCtwLog = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastCtwLog > std::chrono::seconds(3)) {
            s_lastCtwLog = now;
            FTransform rootCtw{};
            uintptr_t root = safeReadPtr(player.characterPtr + m_off.Actor_RootComponent);
            bool rootDec = ok(root) && decryptSceneComponentToWorld(root, rootCtw);
            float qLen = std::sqrt(componentToWorld.RotationX*componentToWorld.RotationX
                + componentToWorld.RotationY*componentToWorld.RotationY
                + componentToWorld.RotationZ*componentToWorld.RotationZ
                + componentToWorld.RotationW*componentToWorld.RotationW);
            LOG(LOG_LEVEL_WARN, TAG " [bones-ctw] %s mesh=%p meshDec=%d "
                "meshQ=(%.3f,%.3f,%.3f,%.3f)|%.3f meshT=(%.0f,%.0f,%.0f) "
                "rootDec=%d rootQ=(%.3f,%.3f,%.3f,%.3f) rootT=(%.0f,%.0f,%.0f)",
                player.playerName.c_str(), (void*)meshComp, ctwDecrypted?1:0,
                componentToWorld.RotationX, componentToWorld.RotationY,
                componentToWorld.RotationZ, componentToWorld.RotationW, qLen,
                componentToWorld.TranslationX, componentToWorld.TranslationY,
                componentToWorld.TranslationZ,
                rootDec?1:0,
                rootCtw.RotationX, rootCtw.RotationY, rootCtw.RotationZ, rootCtw.RotationW,
                rootCtw.TranslationX, rootCtw.TranslationY, rootCtw.TranslationZ);
        }
    }

    std::array<FTransform, kMaxRenderableBoneCount> localTransforms{};
    const uintptr_t boneData = reinterpret_cast<uintptr_t>(boneArray.Data);
    if (!safeReadMemory(boneData,
                        localTransforms.data(),
                        static_cast<size_t>(boneArray.Num) * sizeof(FTransform))) {
        return false;
    }

    BoneAssetCacheEntry boneEntry;
    const uintptr_t skeletalMeshAsset = getSkeletalMeshAsset(meshComp);
    const bool haveTrackedBoneMap = resolveTrackedBoneIndices(skeletalMeshAsset, boneEntry);

    const bool haveRootPos = hasUsableActorPoint(player.pos)
        && (std::fabs(player.pos.x) > 100.0f || std::fabs(player.pos.y) > 100.0f);
    const float maxAllowedDelta = player.isAI ? 500.0f : 1600.0f;
    bool anyValid = false;
    int validBoneCount = 0;
    for (int slot = 0; slot < PlayerInfo::BONE_COUNT; ++slot) {
        const int boneIndex = haveTrackedBoneMap
            ? boneEntry.trackedBoneIndices[slot]
            : kFallbackBoneMap[slot];
        if (boneIndex < 0 || boneIndex >= boneArray.Num) continue;

        const FTransform& boneTransform = localTransforms[boneIndex];
        const FVector3 localPos{
            boneTransform.TranslationX,
            boneTransform.TranslationY,
            boneTransform.TranslationZ,
        };
        const FVector3 worldPos = transformPosition(componentToWorld, localPos);
        if (!hasUsableWorldPoint(worldPos)) continue;

        if (haveRootPos) {
            const float dx = worldPos.x - player.pos.x;
            const float dy = worldPos.y - player.pos.y;
            const float dz = worldPos.z - player.pos.z;
            if (dx * dx + dy * dy + dz * dz > maxAllowedDelta * maxAllowedDelta) continue;
        }

        player.bones[slot] = worldPos;
        anyValid = true;
        validBoneCount++;
    }

    player.bonesValid = anyValid && validBoneCount >= kMinTrackedBoneMatches;

    // 诊断: 周期性打印 17 个 slot 的世界坐标 spread + 是否走 fallback 映射
    {
        static auto s_lastDiag = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (now - s_lastDiag > std::chrono::seconds(3) && player.bonesValid) {
            s_lastDiag = now;
            float xMin=1e30f,xMax=-1e30f,yMin=1e30f,yMax=-1e30f,zMin=1e30f,zMax=-1e30f;
            for (int i = 0; i < PlayerInfo::BONE_COUNT; ++i) {
                const auto& b = player.bones[i];
                if (!hasUsableWorldPoint(b) || (b.x==0&&b.y==0&&b.z==0)) continue;
                xMin=std::min(xMin,b.x);xMax=std::max(xMax,b.x);
                yMin=std::min(yMin,b.y);yMax=std::max(yMax,b.y);
                zMin=std::min(zMin,b.z);zMax=std::max(zMax,b.z);
            }
            // 也打印 head/pelvis/foot 三个关键 slot 的 trackedBoneIndex
            int idxHead = haveTrackedBoneMap ? boneEntry.trackedBoneIndices[0]  : kFallbackBoneMap[0];
            int idxPelv = haveTrackedBoneMap ? boneEntry.trackedBoneIndices[4]  : kFallbackBoneMap[4];
            int idxRFoot= haveTrackedBoneMap ? boneEntry.trackedBoneIndices[13] : kFallbackBoneMap[13];
            const auto& bH = player.bones[0];
            const auto& bP = player.bones[4];
            const auto& bRF = player.bones[13];
            const auto& bLF = player.bones[16];
            LOG(LOG_LEVEL_WARN, TAG " [bones-world] %s validBones=%d "
                "Xspread=%.0f Yspread=%.0f Zspread=%.0f trackedMap=%d "
                "head[0]=%d pelvis[4]=%d rfoot[13]=%d arr=%d "
                "headZ=%.0f pelvZ=%.0f rfootZ=%.0f lfootZ=%.0f meshT=(%.0f,%.0f,%.0f)",
                player.playerName.c_str(), validBoneCount,
                xMax-xMin, yMax-yMin, zMax-zMin,
                haveTrackedBoneMap?1:0, idxHead, idxPelv, idxRFoot, boneArray.Num,
                bH.z, bP.z, bRF.z, bLF.z,
                componentToWorld.TranslationX, componentToWorld.TranslationY, componentToWorld.TranslationZ);
        }
    }

    return player.bonesValid;
}

// =====================================================================
//  玩家位置快速更新

void DfmMatchMonitor::updatePlayerPositions(DrawDfmData& data) const {
    // 周期性位置诊断 (每 10 秒)
    static auto lastPosDiag = std::chrono::steady_clock::now();
    auto nowDiag = std::chrono::steady_clock::now();
    bool doDiag = std::chrono::duration_cast<std::chrono::seconds>(nowDiag - lastPosDiag).count() >= 10;
    if (doDiag) lastPosDiag = nowDiag;

    for (auto& p : data.players) {
        if (p.characterPtr == 0) continue;
        bool posResolved = false;

        // ── 真人玩家: 骨骼优先 (与 scanActors 保持一致) ──
        // 不要先调 getCharacterSocketLocation: 该函数走 SceneComponent::GetSocketLocation
        // 虚函数 (vtable+0x468)。这条 vthunk 在 DFM 上被反作弊 hook (典型特征:
        // ADD X0, X0, #0x210; LDR X16, =0xB400006Fxxxxxxxx; BR X16, 目标是 scudo
        // 堆地址)。对静止远端 actor 该 hook 返回的值还能凑合, 一旦目标移动就返回
        // 锁住 / 错位的坐标。这个错位坐标进 p.pos 后又被 fillPlayerBones 当成
        // 16m 距离过滤的锚点, 把所有真实骨骼全过滤掉, 于是 deriveFootPositionFromBones
        // 也救不回来, p.pos 永远卡在错位值。
        // 这里改成 "先 fillPlayerBones (无锚点约束) → deriveFootPositionFromBones",
        // 直接用 CachedComponentSpaceTransforms × ComponentToWorld 自己算脚底世界
        // 坐标, 完全绕过被 hook 的 vfunc, 与渲染管线读到的同一份矩阵保持一致。
        if (!p.isAI) {
            p.pos = {};
            fillPlayerBones(p);
            FVector3 bonePos{};
            if (deriveFootPositionFromBones(p, bonePos)) {
                p.pos = bonePos;
                rememberActorLocation(p.characterPtr, p.pos);
                posResolved = true;
            }
        }

        if (!posResolved) {
            // AI / 骨骼不可用: 走 mesh.CTW 直接内存读 (绕过 vfunc, 也避开 hook)
            getActorLocation(p.characterPtr, p.pos);
        }

        // 诊断: 每 10 秒采样最多 3 个玩家的详细坐标
        if (doDiag && p.hp > 0) {
            uintptr_t psPtr = safeReadPtr(p.characterPtr + m_off.Pawn_PlayerState);
            uintptr_t root = safeReadPtr(p.characterPtr + m_off.Actor_RootComponent);
            if (ok(root)) {
                uintptr_t pawnPrivate = ok(psPtr) ? safeReadPtr(psPtr + m_off.PS_PawnPrivate) : 0;
                const ReplicatedMovementSnapshot repMove = sampleReplicatedMovement(p.characterPtr);
                uintptr_t ctwBase = root + m_off.Scene_ComponentToWorld;
                // 完整 FTransform: quat(XYZW) + translation(XYZ)
                float qx = safeReadFloat(ctwBase + 0x00);
                float qy = safeReadFloat(ctwBase + 0x04);
                float qz = safeReadFloat(ctwBase + 0x08);
                float qw = safeReadFloat(ctwBase + 0x0C);
                float tx = safeReadFloat(ctwBase + 0x10);
                float ty = safeReadFloat(ctwBase + 0x14);
                float tz = safeReadFloat(ctwBase + 0x18);

                // RootComponent 类名诊断
                std::string rootClassName = readClassName(root);

                // CameraViewLoc + ReplicatedMovement.Location 诊断
                float cvx = safeReadFloat(p.characterPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc));
                float cvy = safeReadFloat(p.characterPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc) + 4);
                float cvz = safeReadFloat(p.characterPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc) + 8);

                // Actor.ReplicatedMovement 内的 Location
                // RepMovement 结构体: LinearVelocity(0xC) + AngularVelocity(0xC) + EncVector Location(0x10)
                // EncVector: {float X +0x00, float Y +0x04, float Z +0x08, EncHandler +0x0C}
                LOG(LOG_LEVEL_WARN,
                    "[posUpdate] %s isAI=%d actor=%p rootPtr=%p ps=%p pawnPrivate=%p rootClass=%s quat=(%.3f,%.3f,%.3f,%.3f) rootT=(%.0f,%.0f,%.0f) "
                    "repMovLoc=(%.0f,%.0f,%.0f) rep=%d enc=%u camView=(%.0f,%.0f,%.0f) final=(%.0f,%.0f,%.0f)",
                    p.playerName.c_str(), p.isAI?1:0,
                    (void*)p.characterPtr,
                    (void*)root,
                    (void*)psPtr,
                    (void*)pawnPrivate,
                    rootClassName.c_str(),
                    qx, qy, qz, qw,
                    tx, ty, tz,
                    repMove.location.x, repMove.location.y, repMove.location.z,
                    repMove.movementEnabled ? 1 : 0,
                    static_cast<unsigned>(repMove.encByte),
                    cvx, cvy, cvz,
                    p.pos.x, p.pos.y, p.pos.z);
            }
            static int diagSamples = 0;
            if (++diagSamples >= 3) { doDiag = false; diagSamples = 0; }
        }

        // 更新血量
        HealthInfo hi = getCharacterHealth(p.characterPtr);
        p.hp = hi.hp; p.maxHp = hi.maxHp;
        p.armor = hi.armor; p.helmet = hi.helmet;

        // 更新武器
        p.weapon = getWeaponName(p.characterPtr);

        // 更新骨骼 (非真人 / 骨骼之前没填上时才需要再算一次)
        if (p.isAI || !p.bonesValid) {
            fillPlayerBones(p);
        }

        // ── AI / 真人(若上方骨骼链失败): 用骨骼脚底覆盖 p.pos ──
        {
            FVector3 bonePos{};
            const bool posUsable = hasUsableActorPoint(p.pos);
            if (((!p.isAI) || !posUsable) && deriveFootPositionFromBones(p, bonePos)) {
                p.pos = bonePos;
                rememberActorLocation(p.characterPtr, p.pos);
            }
        }
    }

    // 重新统计存活
    data.aliveCount = 0;
    for (const auto& p : data.players) {
        if (p.hp > 0) data.aliveCount++;
    }
}

// =====================================================================
//  击杀/伤害变化检测
// =====================================================================

void DfmMatchMonitor::detectChanges(const DrawDfmData& prev, DrawDfmData& curr) {
    // 构建旧数据的名字→血量映射
    std::unordered_map<std::string, float> prevHP;
    for (const auto& p : prev.players) {
        if (!p.playerName.empty() && !p.isAI) {
            prevHP[p.playerName] = p.hp;
        }
    }

    for (const auto& p : curr.players) {
        if (p.isAI || p.playerName.empty()) continue;

        auto it = prevHP.find(p.playerName);
        if (it == prevHP.end()) {
            // 新玩家出现
            Notification n;
            n.text = "+" + p.playerName;
            n.color = IM_COL32(100, 200, 255, 255);
            n.timeLeft = 3.0f;
            curr.notifications.push_back(std::move(n));
        } else {
            float oldHp = it->second;
            if (p.hp <= 0 && oldHp > 0) {
                // 被击杀
                Notification n;
                n.text = p.playerName + " KILLED";
                n.color = IM_COL32(255, 50, 50, 255);
                n.timeLeft = 4.0f;
                curr.notifications.push_back(std::move(n));
            } else if (p.hp < oldHp && p.hp > 0) {
                // 受伤
                Notification n;
                char buf[128];
                snprintf(buf, sizeof(buf), "%s -%.0fHP", p.playerName.c_str(), oldHp - p.hp);
                n.text = buf;
                n.color = IM_COL32(255, 200, 50, 255);
                n.timeLeft = 2.5f;
                curr.notifications.push_back(std::move(n));
            }
        }
    }
}

// =====================================================================
//  轮询主循环
// =====================================================================

void DfmMatchMonitor::pollLoop() {
    LOG(LOG_LEVEL_INFO, TAG " pollLoop 启动");

    auto lastScanTime = std::chrono::steady_clock::now();
    auto lastPlayerUpdate = std::chrono::steady_clock::now();
    bool wasInMatch = false;
    DrawDfmData persistentData;
    DrawDfmData prevData;  // 上一帧数据 (用于变化检测)

    while (m_running.load(std::memory_order_acquire)) {
        MatchState ms = getMatchState();

        if (ms.inMatch && !wasInMatch) {
            LOG(LOG_LEVEL_INFO, TAG " 检测到对局开始: %s (%s)", ms.state.c_str(), ms.worldName.c_str());
            persistentData = DrawDfmData{};
            prevData = DrawDfmData{};
            m_lastKnownActorPositions.clear();
            m_rootCompensationOwners.clear();
        } else if (!ms.inMatch && wasInMatch) {
            LOG(LOG_LEVEL_INFO, TAG " 对局结束");
            persistentData = DrawDfmData{};
            prevData = DrawDfmData{};
            m_lastKnownActorPositions.clear();
            m_rootCompensationOwners.clear();
            SharedDfmData::getInstance().pushData(persistentData);
        }
        wasInMatch = ms.inMatch;

        if (ms.inMatch) {
            auto now = std::chrono::steady_clock::now();
            auto scanElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastScanTime).count();
            auto playerElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPlayerUpdate).count();

            // ── 完整 Actor 扫描 (每 SCAN_INTERVAL_MS) ──
            if (scanElapsed >= SCAN_INTERVAL_MS) {
                lastScanTime = now;
                lastPlayerUpdate = now;

                prevData = persistentData;  // 保存旧数据用于变化检测

                persistentData.players.clear();
                persistentData.lootItems.clear();
                persistentData.containers.clear();
                persistentData.notifications.clear();
                persistentData.inMatch = true;
                persistentData.worldName = ms.worldName;
                persistentData.matchState = ms.state;

                auto actors = getAllActors();
                scanActors(actors, persistentData);

                if (persistentData.players.empty()) {
                    scanFromPlayerArray(persistentData);
                }

                // 同步存活物资箱白名单 — 供 GUI 远程开箱按钮校验脏指针
                {
                    std::lock_guard<std::mutex> lock(m_aliveContainersMu);
                    m_aliveContainers.clear();
                    m_aliveContainers.reserve(persistentData.containers.size());
                    for (const auto& c : persistentData.containers) {
                        if (c.actorPtr) m_aliveContainers.emplace(c.actorPtr, c.className);
                    }
                }

                // [placeholder 去重] DFM 远端 actor 大部分时间共享同一个 placeholder
                // RootComponent (RelativeLocation/RepMovement/Mesh CTW 都返回相同的
                // spawn 点坐标), 多个不同 actor 解出 byte-identical 位置 → 视为占位
                // 并清空其 pos, 避免 ESP 框堆在地图固定点。
                // 自己 (characterPtr == ackPawn / ctrlPawn) 不参与去重统计。
                {
                    uintptr_t pcPtr = findLocalPlayerController();
                    uintptr_t ackPawn = ok(pcPtr) ? safeReadPtr(pcPtr + m_off.PC_AcknowledgedPawn) : 0;
                    uintptr_t ctrlPawn = ok(pcPtr) ? safeReadPtr(pcPtr + m_off.Ctrl_Pawn) : 0;

                    // 统计每个位置出现次数 (按厘米取整, 同一 cm 视为同位置)
                    struct Key { int x, y, z; bool operator==(const Key& o) const { return x==o.x&&y==o.y&&z==o.z; } };
                    struct KeyHash { size_t operator()(const Key& k) const noexcept {
                        return (static_cast<size_t>(k.x) * 73856093u) ^
                               (static_cast<size_t>(k.y) * 19349663u) ^
                               (static_cast<size_t>(k.z) * 83492791u); } };
                    std::unordered_map<Key, int, KeyHash> posCount;
                    for (const auto& p : persistentData.players) {
                        if (p.characterPtr == ackPawn || p.characterPtr == ctrlPawn) continue;
                        if (!hasUsableActorPoint(p.pos)) continue;
                        Key k{ static_cast<int>(p.pos.x), static_cast<int>(p.pos.y), static_cast<int>(p.pos.z) };
                        posCount[k]++;
                    }

                    int dropped = 0;
                    for (auto& p : persistentData.players) {
                        if (p.characterPtr == ackPawn || p.characterPtr == ctrlPawn) continue;
                        if (!hasUsableActorPoint(p.pos)) continue;
                        Key k{ static_cast<int>(p.pos.x), static_cast<int>(p.pos.y), static_cast<int>(p.pos.z) };
                        auto it = posCount.find(k);
                        if (it != posCount.end() && it->second >= 2) {
                            // 共享坐标 = placeholder, 清空位置 (ESP 不绘制), 但保留条目供
                            // 玩家列表显示名字/血量
                            p.pos = {};
                            p.bonesValid = false;
                            for (auto& b : p.bones) b = {};
                            ++dropped;
                        }
                    }
                    if (dropped > 0) {
                        static std::atomic<uint64_t> lastDropLogMs{0};
                        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
                        uint64_t prev = lastDropLogMs.load(std::memory_order_relaxed);
                        if (nowMs - prev > 2000) {
                            lastDropLogMs.store(nowMs, std::memory_order_relaxed);
                            LOG(LOG_LEVEL_INFO, TAG " [placeholder] 丢弃 %d 个共享坐标 actor (placeholder)", dropped);
                        }
                    }
                }

                persistentData.totalCount = static_cast<int32_t>(persistentData.players.size());
                persistentData.aliveCount = 0;
                for (const auto& p : persistentData.players) {
                    if (p.hp > 0) persistentData.aliveCount++;
                }

                // myTeamId 检测
                if (persistentData.myTeamId < 0 &&
                    (std::fabs(persistentData.myPos.x) > 1.0f || std::fabs(persistentData.myPos.y) > 1.0f)) {
                    for (const auto& p : persistentData.players) {
                        if (!p.isAI && p.teamId >= 0) {
                            float dx = p.pos.x - persistentData.myPos.x;
                            float dy = p.pos.y - persistentData.myPos.y;
                            float dz = p.pos.z - persistentData.myPos.z;
                            float dist = std::sqrt(dx*dx + dy*dy + dz*dz) / 100.0f;
                            if (dist < 5.0f) {
                                persistentData.myTeamId = p.teamId;
                                LOG(LOG_LEVEL_INFO, TAG " 检测到本地队伍 ID=%d", p.teamId);
                                break;
                            }
                        }
                    }
                }

                // 击杀/伤害变化检测
                detectChanges(prevData, persistentData);

                LOG(LOG_LEVEL_INFO, TAG " 扫描: 玩家=%d 物资=%d 箱子=%d actors=%zu",
                    static_cast<int>(persistentData.players.size()),
                    static_cast<int>(persistentData.lootItems.size()),
                    static_cast<int>(persistentData.containers.size()),
                    actors.size());
            }
            // ── 玩家位置快速更新 (每 PLAYER_UPDATE_INTERVAL_MS) ──
            else if (playerElapsed >= PLAYER_UPDATE_INTERVAL_MS && !persistentData.players.empty()) {
                lastPlayerUpdate = now;
                DrawDfmData snapshot = persistentData;  // 保存旧状态
                updatePlayerPositions(persistentData);
                // 快速更新也检测变化 (主要检测击杀)
                detectChanges(snapshot, persistentData);
            }

            // ── 每次循环都更新: 本地位置 + 相机数据 (~250ms) ──
            persistentData.inMatch = true;
            persistentData.myPos = getMyPosition();
            fillCameraData(persistentData);

            // 每 5 秒输出一次 myPos 诊断
            {
                static auto lastMyPosLog = std::chrono::steady_clock::now();
                auto nowLog = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(nowLog - lastMyPosLog).count() >= 5) {
                    lastMyPosLog = nowLog;
                    uintptr_t pc = findLocalPlayerController();
                    uintptr_t ackPawn = ok(pc) ? safeReadPtr(pc + m_off.PC_AcknowledgedPawn) : 0;
                    uintptr_t ctrlPawn = ok(pc) ? safeReadPtr(pc + m_off.Ctrl_Pawn) : 0;
                    LOG(LOG_LEVEL_INFO,
                        "myPos=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f) fov=%.0f yaw=%.1f team=%d "
                        "PC=%p ackPawn=%p ctrlPawn=%p",
                        persistentData.myPos.x, persistentData.myPos.y, persistentData.myPos.z,
                        persistentData.camLocX, persistentData.camLocY, persistentData.camLocZ,
                        persistentData.camFOV, persistentData.camYaw, persistentData.myTeamId,
                        (void*)pc, (void*)ackPawn, (void*)ctrlPawn);

                    struct TrackLogEntry {
                        const PlayerInfo* player = nullptr;
                        float distMeters = -1.0f;
                    };

                    std::vector<TrackLogEntry> selfEntries;
                    std::vector<TrackLogEntry> enemyEntries;
                    std::vector<TrackLogEntry> aiEntries;
                    selfEntries.reserve(2);
                    enemyEntries.reserve(persistentData.players.size());
                    aiEntries.reserve(persistentData.players.size());

                    for (const auto& p : persistentData.players) {
                        if (p.hp <= 0.0f || p.characterPtr == 0) continue;

                        const float distMeters = (hasUsableActorPoint(persistentData.myPos) && hasUsableActorPoint(p.pos))
                            ? distanceMeters(persistentData.myPos, p.pos)
                            : -1.0f;
                        const bool sameTeam = persistentData.myTeamId >= 0
                            && p.teamId >= 0
                            && p.teamId == persistentData.myTeamId;
                        const bool isLocalPawn = p.characterPtr == ackPawn || p.characterPtr == ctrlPawn;

                        if (isLocalPawn || (sameTeam && distMeters >= 0.0f && distMeters < 5.0f)) {
                            selfEntries.push_back({&p, distMeters});
                            continue;
                        }
                        if (p.isAI) {
                            aiEntries.push_back({&p, distMeters});
                            continue;
                        }
                        if (!sameTeam || persistentData.myTeamId < 0) {
                            enemyEntries.push_back({&p, distMeters});
                        }
                    }

                    const auto sortEntries = [](std::vector<TrackLogEntry>& entries) {
                        std::sort(entries.begin(), entries.end(), [](const TrackLogEntry& lhs, const TrackLogEntry& rhs) {
                            const float lhsDist = lhs.distMeters >= 0.0f ? lhs.distMeters : 1.0e9f;
                            const float rhsDist = rhs.distMeters >= 0.0f ? rhs.distMeters : 1.0e9f;
                            return lhsDist < rhsDist;
                        });
                    };
                    sortEntries(selfEntries);
                    sortEntries(enemyEntries);
                    sortEntries(aiEntries);

                    LOG(LOG_LEVEL_INFO,
                        "[track][summary] players=%zu self=%zu enemy=%zu ai=%zu myTeam=%d myPos=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f)",
                        persistentData.players.size(),
                        selfEntries.size(),
                        enemyEntries.size(),
                        aiEntries.size(),
                        persistentData.myTeamId,
                        persistentData.myPos.x, persistentData.myPos.y, persistentData.myPos.z,
                        persistentData.camLocX, persistentData.camLocY, persistentData.camLocZ);

                    const auto dumpTrackEntry = [&](const char* category, const TrackLogEntry& entry) {
                        const PlayerInfo& p = *entry.player;
                        uintptr_t psPtr = safeReadPtr(p.characterPtr + m_off.Pawn_PlayerState);
                        uintptr_t pawnPrivate = ok(psPtr) ? safeReadPtr(psPtr + m_off.PS_PawnPrivate) : 0;
                        uintptr_t root = safeReadPtr(p.characterPtr + m_off.Actor_RootComponent);
                        float rootX = 0.0f;
                        float rootY = 0.0f;
                        float rootZ = 0.0f;
                        if (ok(root)) {
                            rootX = safeReadFloat(root + m_off.Scene_ComponentToWorld + 0x10);
                            rootY = safeReadFloat(root + m_off.Scene_ComponentToWorld + 0x14);
                            rootZ = safeReadFloat(root + m_off.Scene_ComponentToWorld + 0x18);
                        }

                        const float cvx = safeReadFloat(p.characterPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc));
                        const float cvy = safeReadFloat(p.characterPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc) + 4);
                        const float cvz = safeReadFloat(p.characterPtr + static_cast<uintptr_t>(m_off.Char_CameraViewLoc) + 8);
                        const ReplicatedMovementSnapshot repMove = sampleReplicatedMovement(p.characterPtr);

                        FVector3 boneFoot{};
                        const bool boneFootValid = deriveFootPositionFromBones(p, boneFoot);
                        const int sameTeamFlag = (persistentData.myTeamId >= 0 && p.teamId >= 0)
                            ? (p.teamId == persistentData.myTeamId ? 1 : 0)
                            : -1;

                        LOG(LOG_LEVEL_INFO,
                            "[track][%s] name='%s' class='%s' isAI=%d team=%d sameTeam=%d hp=%.0f/%.0f armor=%.0f helmet=%.0f dist=%.1f pos=(%.0f,%.0f,%.0f) root=(%.0f,%.0f,%.0f) repMov=(%.0f,%.0f,%.0f) rep=%d enc=%u camView=(%.0f,%.0f,%.0f) boneFootValid=%d boneFoot=(%.0f,%.0f,%.0f) bones=%d weapon='%s' actor=%p rootPtr=%p ps=%p pawnPrivate=%p",
                            category,
                            p.playerName.c_str(),
                            p.className.c_str(),
                            p.isAI ? 1 : 0,
                            p.teamId,
                            sameTeamFlag,
                            p.hp,
                            p.maxHp,
                            p.armor,
                            p.helmet,
                            entry.distMeters,
                            p.pos.x,
                            p.pos.y,
                            p.pos.z,
                            rootX,
                            rootY,
                            rootZ,
                            repMove.location.x,
                            repMove.location.y,
                            repMove.location.z,
                            repMove.movementEnabled ? 1 : 0,
                            static_cast<unsigned>(repMove.encByte),
                            cvx,
                            cvy,
                            cvz,
                            boneFootValid ? 1 : 0,
                            boneFoot.x,
                            boneFoot.y,
                            boneFoot.z,
                            p.bonesValid ? 1 : 0,
                            p.weapon.c_str(),
                                (void*)p.characterPtr,
                                (void*)root,
                                (void*)psPtr,
                                (void*)pawnPrivate);
                    };

                    if (!selfEntries.empty()) {
                        dumpTrackEntry("selfPawn", selfEntries.front());
                    }
                    const size_t kMaxEnemyLogs = 6;
                    for (size_t i = 0; i < std::min(enemyEntries.size(), kMaxEnemyLogs); ++i) {
                        dumpTrackEntry("enemy", enemyEntries[i]);
                    }
                    const size_t kMaxAiLogs = 6;
                    for (size_t i = 0; i < std::min(aiEntries.size(), kMaxAiLogs); ++i) {
                        dumpTrackEntry("ai", aiEntries[i]);
                    }

                    // 骨骼诊断: 打印第一个有效玩家的第三人称/FPP/MasterPose 候选状态
                    for (const auto& p : persistentData.players) {
                        if (p.hp <= 0 || p.characterPtr == 0) continue;
                        uintptr_t mesh3p = resolveObjectField(
                            p.characterPtr + static_cast<uintptr_t>(m_off.Char_Mesh));
                        int32_t bc3p = getCachedTransformCount(mesh3p);
                        uintptr_t meshFpp = resolveObjectField(
                            p.characterPtr + static_cast<uintptr_t>(m_off.Char_FPPMesh));
                        int32_t bcFpp = getCachedTransformCount(meshFpp);
                        uintptr_t fppComp = resolveObjectField(
                            p.characterPtr + static_cast<uintptr_t>(m_off.STBase_FPPComp));
                        uintptr_t avatarComp = resolveObjectField(
                            p.characterPtr + static_cast<uintptr_t>(m_off.STBase_AvatarComponent));
                        uintptr_t fppAvatar = ok(fppComp) && m_off.FPPComp_AvatarComp > 0
                            ? resolveObjectField(
                                fppComp + static_cast<uintptr_t>(m_off.FPPComp_AvatarComp))
                            : 0;
                        uintptr_t masterBone = ok(avatarComp)
                            ? resolveObjectField(
                                avatarComp + static_cast<uintptr_t>(m_off.Avatar_MasterBoneComponent))
                            : 0;
                        int32_t bcMasterBone = getCachedTransformCount(masterBone);
                        uintptr_t master3p = ok(mesh3p) ? followMasterPoseChain(mesh3p) : 0;
                        int32_t bcMaster = getCachedTransformCount(master3p);
                        uintptr_t avatarDeep = ok(avatarComp) ? scanObjectForMeshComponent(avatarComp, 0x1000) : 0;
                        int32_t bcAvatarDeep = getCachedTransformCount(avatarDeep);
                        std::vector<uintptr_t> meshListValues;
                        std::vector<uintptr_t> entityListValues;
                        int32_t meshListActive = 0;
                        int32_t entityListActive = 0;
                        int32_t defaultSubActive = 0;
                        int32_t localSubActive = 0;
                        int32_t activeSubActive = 0;
                        int32_t meshListBest = 0;
                        int32_t entityListBest = 0;
                        int32_t subsystemBest = 0;
                        if (ok(avatarComp)) {
                            meshListValues.reserve(16);
                            entityListValues.reserve(16);
                            std::vector<uintptr_t> subsystemValues;
                            subsystemValues.reserve(24);
                            meshListActive = collectSparseMapValues(
                                avatarComp + static_cast<uintptr_t>(m_off.Avatar_MeshComponentList),
                                meshListValues,
                                256);
                            entityListActive = collectSparseMapValues(
                                avatarComp + static_cast<uintptr_t>(m_off.Avatar_AvatarEntityList),
                                entityListValues,
                                128);
                            for (uintptr_t meshComp : meshListValues) {
                                meshListBest = std::max(meshListBest, getCachedTransformCount(meshComp));
                                meshListBest = std::max(meshListBest,
                                    getCachedTransformCount(followMasterPoseChain(meshComp)));
                            }
                            for (uintptr_t entity : entityListValues) {
                                const uintptr_t entityMesh = scanObjectForMeshComponent(entity, 0x200);
                                entityListBest = std::max(entityListBest,
                                    getCachedTransformCount(entityMesh));
                            }
                            defaultSubActive = collectObjectArrayValues(
                                avatarComp + static_cast<uintptr_t>(m_off.Avatar_DefaultAvatarSubSystemList),
                                subsystemValues,
                                64);
                            localSubActive = collectObjectArrayValues(
                                avatarComp + static_cast<uintptr_t>(m_off.Avatar_LocalSubSystemList),
                                subsystemValues,
                                64);
                            activeSubActive = collectObjectArrayValues(
                                avatarComp + static_cast<uintptr_t>(m_off.Avatar_LocalActiveSubSystemList),
                                subsystemValues,
                                64);
                            for (uintptr_t subsystem : subsystemValues) {
                                const uintptr_t subsystemMesh = scanObjectForMeshComponent(subsystem, 0x200);
                                subsystemBest = std::max(subsystemBest,
                                    getCachedTransformCount(subsystemMesh));
                            }
                        }
                        TArray<uintptr_t> pool{};
                        int32_t poolNum = 0;
                        if (ok(avatarComp)
                            && safeReadMemory(avatarComp + static_cast<uintptr_t>(m_off.Avatar_SkeletalMeshCompPool),
                                              &pool,
                                              sizeof(pool))
                            && isUsableRemoteArray(pool, 64)) {
                            poolNum = pool.Num;
                        }
                        uintptr_t fppBranch = ok(fppComp) ? scanObjectForMeshComponent(fppComp, 0x400) : 0;
                        int32_t bcFppBranch = getCachedTransformCount(fppBranch);
                        uintptr_t bestMesh = resolveBestBoneMeshComponent(p.characterPtr);
                        int32_t bcBest = getCachedTransformCount(bestMesh);
                        const uintptr_t bestAsset = getSkeletalMeshAsset(bestMesh);
                        BoneAssetCacheEntry bestEntry;
                        const int boneMatched = resolveTrackedBoneIndices(bestAsset, bestEntry)
                            ? bestEntry.matchedCount
                            : 0;
                        if (bcBest <= 0 && boneMatched <= 0 && !p.bonesValid) {
                            continue;
                        }

                        LOG(LOG_LEVEL_INFO,
                            "[bone] '%s' mesh3p=%p(bc=%d) fpp=%p(bc=%d) fppComp=%p fppAvatar=%p fppBranch=%p(bc=%d) avatar=%p avatarDeep=%p(bc=%d) masterBone=%p(bc=%d) master=%p(bc=%d) meshList=%d/%zu(best=%d) entityList=%d/%zu(best=%d) subSys=%d/%d/%d(best=%d) pool=%d best=%p(bc=%d asset=%p matched=%d) valid=%d",
                            p.playerName.c_str(), (void*)mesh3p, bc3p,
                            (void*)meshFpp, bcFpp,
                            (void*)fppComp, (void*)fppAvatar,
                            (void*)fppBranch, bcFppBranch,
                            (void*)avatarComp,
                            (void*)avatarDeep, bcAvatarDeep,
                            (void*)masterBone, bcMasterBone,
                            (void*)master3p, bcMaster,
                            meshListActive, meshListValues.size(), meshListBest,
                            entityListActive, entityListValues.size(), entityListBest,
                            defaultSubActive, localSubActive, activeSubActive, subsystemBest,
                            poolNum,
                            (void*)bestMesh, bcBest, (void*)bestAsset, boneMatched,
                            p.bonesValid ? 1 : 0);
                        break;  // 只打印第一个
                    }
                }
            }

            SharedDfmData::getInstance().pushData(persistentData);
        }

        // 处理 GUI 入队的远程开箱请求 (在游戏线程的 polling tick 内执行)
        if (ms.inMatch) {
            processOpenBoxRequests();
        }

        // 大厅期心跳: 即使不在对局, 也每秒推一帧空数据, 防止 GUI 看门狗 (uestart.cpp
        // kStaleExitMs=10s) 把渲染线程清屏退出, 导致菜单消失
        if (!ms.inMatch) {
            int64_t sinceLastPush = SharedDfmData::getInstance().getMsSinceLastPush();
            if (sinceLastPush < 0 || sinceLastPush >= 1000) {
                SharedDfmData::getInstance().pushData(persistentData);
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }

    LOG(LOG_LEVEL_INFO, TAG " pollLoop 退出");
}

// =====================================================================
//  远程开箱 (普通杂物/枪/甲/尸体箱)
// =====================================================================
//
//  IDA 验证 (libUE4.so):
//   * AInventoryPickup_OpenBox::OpenThisBox  exec thunk @ 0x1335C8E4
//     - thunk 调 box->vtable[?]( box, PC )  →  OpenThisBoxInner_Impl
//     - thunk 上层路径: 客户端调 ProcessEvent → UE 检测 FUNC_NetServer
//       → CallRemoteFunction → NetDriver 包发服务端 → 服务端执行 thunk
//   * 服务端 OpenThisBoxInner_Impl @ 0x1225CEF4 仅校验:
//        if (box.PickupBoxType == 3 && !box.bAlreadyOpened)
//        → SetOpenState(box,1); 触发 OnRep
//     **没有 距离/视线/朝向 校验**, 因此可远程触发。
//   * 密码保险箱: ServerVerifySafeBoxCode @ 0x13259A0C 服务端验密码,
//     本通道无法绕过, 已在类名过滤里跳过。
//
//  调用方式: 通过 UObject::ProcessEvent(UFunction*, &params)。
//
//  ProcessEvent 定位 (IDA 反编译相邻 wrapper sub_1335B7FC 等):
//     wrapper(this, arg) {
//        UFunction* f = FindFunctionChecked(this, &cachedFName);
//        return (*(vtable + 552))(this, f, &params);   // ← ProcessEvent
//     }
//   → ProcessEvent 在 UObject vtable 的 byte offset **552** (slot 69).
//   → 每个 UObject 实例 *(uintptr_t*)box 就是 vtable, 读 vtable[552/8] 拿到地址。
//
//  UFunction* OpenThisBox 通过 m_interface 在启动时枚举 UClass.Children
//  得到, 缓存在 m_openThisBoxUFunction; 避免每次开箱都做反射。

// UE5 UObject vtable 中 ProcessEvent 的字节偏移. 由 wrapper 反编译验证:
//   sub_1335B7FC ... ret (*(vtable+552))(this, f, params)
constexpr uintptr_t kProcessEventVtableByteOffset = 552;

// ★★★ 远程开箱实际 RPC 发送总开关 ★★★
// false = 安全模式: GUI 按钮入队、Monitor 走完所有校验/日志, 但不真正调
//         box.vtable[69](ProcessEvent), 不发任何包。物资透视/内容显示
//         完全可用, 程序也不会崩溃。
// true  = 实战模式: 真正向服务端发 RPC。仅在以下条件全部满足时再开启:
//         1) 已确认 ProcessEvent 在 vtable byte offset 552 不变
//         2) 已通过 hook 确保调用发生在 UE 游戏主线程
//         3) 接受可能崩溃 / 封号风险
// UE 的 ProcessEvent 内部多处假设 IsInGameThread(), 在 std::thread 上直接
// 调几乎一定会触发 ensure/check 崩溃 — 这是上次 "程序崩溃" 的根因。
constexpr bool kRemoteOpenActuallyFireRPC = false;

bool DfmMatchMonitor::isContainerActorAlive(uintptr_t actorPtr) const {
    if (!ok(actorPtr)) return false;
    std::lock_guard<std::mutex> lock(m_aliveContainersMu);
    return m_aliveContainers.find(actorPtr) != m_aliveContainers.end();
}

uintptr_t DfmMatchMonitor::findFunctionByName(uintptr_t obj, const FName& fname) const {
    (void)obj;
    (void)fname;
    // 旧的 FindFunctionByName 路径不再使用; 通过 m_interface 在启动时一次性
    // 解析 UFunction* 并缓存在 m_openThisBoxUFunction (见 sendServerOpenPickupBox)。
    return 0;
}

bool DfmMatchMonitor::callUFunction(uintptr_t obj, uintptr_t func, void* params) const {
    if (!ok(obj) || !ok(func)) return false;
    // ProcessEvent 通过 obj 的 vtable 第 (552/8 = 69) 个槽调用; 这是 UE5 UObject
    // 基类的 ProcessEvent 虚函数槽, 所有 UObject 子类共享同一槽位 (vtable 前段)。
    uintptr_t vtable = safeReadPtr(obj);
    if (!ok(vtable)) return false;
    uintptr_t pe = safeReadPtr(vtable + kProcessEventVtableByteOffset);
    if (!ok(pe)) return false;
    // 校验 pe 在模块代码段内 (防止 vtable 被篡改导致跳到野指针)
    if (m_moduleBase != 0 && m_moduleSize != 0
        && (pe < m_moduleBase || pe >= (m_moduleBase + m_moduleSize))) {
        LOG(LOG_LEVEL_WARN, TAG " [openbox] ProcessEvent 地址 %p 超出模块范围", (void*)pe);
        return false;
    }

    // ★ 默认安全模式: 不真正调 ProcessEvent (跨线程必崩, 见文件顶部宏注释)
    if (!kRemoteOpenActuallyFireRPC) {
        LOG(LOG_LEVEL_INFO, TAG " [openbox] [SAFE-MODE] would call PE@%p obj=%p func=%p params=%p",
            (void*)pe, (void*)obj, (void*)func, params);
        return true;  // 假装成功, GUI 反馈 OK
    }

    // 实战路径: 用 sigsetjmp 包住, 一旦内部崩溃可恢复 (但 UE 状态可能已损坏,
    // 下一帧仍可能崩, 自行承担)
    using PEFn = void (*)(uintptr_t self, uintptr_t func, void* params);
    auto fn = reinterpret_cast<PEFn>(pe);

    installSafeReadGuard();
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
        s_safeReadActive = 0;
        LOG(LOG_LEVEL_ERROR, TAG " [openbox] PE 调用触发 SIGSEGV/SIGBUS, 已恢复");
        return false;
    }
    s_safeReadActive = 1;
    fn(obj, func, params);
    s_safeReadActive = 0;
    return true;
}

// 通过 m_interface 解析 InventoryPickup_OpenBox 类的 OpenThisBox UFunction*
static uintptr_t resolveOpenThisBoxUFunctionImpl(ue5dfminf::UE5DfmInterface* iface) {
    if (!iface) return 0;
    // sdk_dump 里这个类名 (无 'A' 前缀, UE5 反射使用裸名)
    static const char* kCandClasses[] = {
        "InventoryPickup_OpenBox",
        "InventoryPickup_OpenBoxServer",
    };
    for (const char* cls : kCandClasses) {
        const auto* cd = iface->findClass(cls);
        if (!cd) continue;
        for (const auto& f : cd->functions) {
            if (f.name == "OpenThisBox" && f.ufunctionPtr != 0) {
                return f.ufunctionPtr;
            }
        }
    }
    return 0;
}

bool DfmMatchMonitor::sendServerOpenPickupBox(uintptr_t pcPtr, uintptr_t boxPtr) const {
    (void)pcPtr;  // OpenThisBox(PC) 把 PC 作为参数; 但根据 sdk_dump 这是 server-only RPC,
                  // 客户端调时 ProcessEvent 把整个 params 打包发给服务端, 服务端在
                  // _Implementation 里读 PC 是 nullptr 时会用自己的 GetOwningPlayerController.
                  // 这里仍把 PC 传过去以满足签名。
    if (!ok(boxPtr)) return false;

    // 一次性 lazy-resolve OpenThisBox UFunction*
    if (!m_openThisBoxResolved) {
        m_openThisBoxUFunction = resolveOpenThisBoxUFunctionImpl(m_interface);
        m_openThisBoxResolved  = true;
        LOG(LOG_LEVEL_INFO, TAG " [openbox] resolve OpenThisBox UFunction* = %p",
            (void*)m_openThisBoxUFunction);
    }
    if (!ok(m_openThisBoxUFunction)) {
        LOG(LOG_LEVEL_WARN, TAG " [openbox] OpenThisBox UFunction* 未解析 (interface 不可用?)");
        return false;
    }

    // 校验 box 类: vtable[1=ClassPrivate]=UClass*, 该 UClass 的 oname 应该包含 OpenBox。
    // 这里不再做严格类校验, 因为 m_aliveContainers 已经在外层卡过 isContainerClass.

    // OpenThisBox(PlayerController* PC) — 单参数
    struct OpenThisBoxParams { uintptr_t PC; } params{pcPtr};
    bool ok2 = callUFunction(boxPtr, m_openThisBoxUFunction, &params);
    LOG(LOG_LEVEL_INFO, TAG " [openbox] PE OpenThisBox box=%p pc=%p uf=%p -> %d",
        (void*)boxPtr, (void*)pcPtr, (void*)m_openThisBoxUFunction, ok2 ? 1 : 0);
    return ok2;
}

void DfmMatchMonitor::processOpenBoxRequests() {
    auto requests = SharedDfmData::getInstance().drainOpenBoxQueue();
    if (requests.empty()) return;

    uintptr_t pc = findLocalPlayerController();
    if (!ok(pc)) {
        LOG(LOG_LEVEL_WARN, TAG " [openbox] 无本地 PlayerController, 丢弃 %zu 个请求",
            requests.size());
        return;
    }

    // 取本地角色位置, 用作距离阀
    FVector3 myPos = getMyPosition();
    const bool myPosValid = std::isfinite(myPos.x) && std::isfinite(myPos.y) && std::isfinite(myPos.z)
                            && (myPos.x != 0.0f || myPos.y != 0.0f);
    // 反作弊"距离异常"阀值: 50m (5000uu)
    // 服务端虽不校验距离, 但 ACE 后台用统计窗口, 远距离开箱会落异常列表
    constexpr float kMaxOpenDistanceUU = 5000.0f;
    constexpr int   kGlobalThrottleMs  = 3000;       // 全局每 3s 最多 1 次

    auto now = std::chrono::steady_clock::now();

    // 全局节流: 总流量上限
    if (m_lastOpenGlobal.time_since_epoch().count() != 0) {
        auto dtAll = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastOpenGlobal).count();
        if (dtAll < kGlobalThrottleMs) {
            LOG(LOG_LEVEL_INFO, TAG " [openbox] 全局节流, 距上次 %lldms < %dms, 丢弃本批 %zu",
                (long long)dtAll, kGlobalThrottleMs, requests.size());
            return;
        }
    }

    bool sentAny = false;
    for (uintptr_t box : requests) {
        if (!isContainerActorAlive(box)) {
            LOG(LOG_LEVEL_WARN, TAG " [openbox] 拒绝脏指针 box=%p (不在存活集合)", (void*)box);
            continue;
        }
        // 单 box 节流: 1 秒内只发一次
        auto it = m_lastOpenTime.find(box);
        if (it != m_lastOpenTime.end()) {
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (dt < 1000) {
                LOG(LOG_LEVEL_INFO, TAG " [openbox] 节流跳过 box=%p (%lldms ago)",
                    (void*)box, (long long)dt);
                continue;
            }
        }

        // 距离阀: 必须在合理交互范围内, 避免触发 ACE "异常远距离 RPC" 统计
        if (myPosValid) {
            FVector3 boxPos{};
            if (getActorLocation(box, boxPos)) {
                float dx = boxPos.x - myPos.x;
                float dy = boxPos.y - myPos.y;
                float dz = boxPos.z - myPos.z;
                float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
                if (dist > kMaxOpenDistanceUU) {
                    LOG(LOG_LEVEL_WARN, TAG " [openbox] 距离过远 box=%p dist=%.0fuu (>%.0f), 丢弃",
                        (void*)box, dist, kMaxOpenDistanceUU);
                    continue;
                }
            }
        }

        // 类型过滤: 密码保险箱 / DrillingSafe 不走本通道
        std::string cn;
        {
            std::lock_guard<std::mutex> lock(m_aliveContainersMu);
            auto cit = m_aliveContainers.find(box);
            if (cit != m_aliveContainers.end()) cn = cit->second;
        }
        if (cn.find("SafeBox") != std::string::npos ||
            cn.find("DrillingSafe") != std::string::npos) {
            LOG(LOG_LEVEL_WARN, TAG " [openbox] 跳过保险箱 box=%p cn=%s", (void*)box, cn.c_str());
            continue;
        }

        m_lastOpenTime[box] = now;
        if (sendServerOpenPickupBox(pc, box)) {
            sentAny = true;
            // 全局节流策略: 一批最多发 1 个, 后续请求等下次 tick
            m_lastOpenGlobal = now;
            break;
        }
    }
    (void)sentAny;
}

} // namespace dfm
