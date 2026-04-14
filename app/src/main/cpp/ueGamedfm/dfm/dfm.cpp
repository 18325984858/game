/**
 * @file    dfm.cpp
 * @brief   三角洲行动 (DFM) 对局状态监控 + 玩家/物资采集 实现
 *          从 loot_scan.js (Frida) 转写, 遵循 pubgmhd.cpp 代码规范
 */
#include "dfm.h"
#include "../../Log/log.h"
#include "../ilbUE5Struct/UE5DfmStruct.h"
#include "../interface/interface.h"

// IM_COL32 兼容宏 (避免引入 imgui.h)
#ifndef IM_COL32
#define IM_COL32(R,G,B,A) (((uint32_t)(A)<<24)|((uint32_t)(B)<<16)|((uint32_t)(G)<<8)|((uint32_t)(R)))
#endif

#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <chrono>
#include <algorithm>

#define TAG "[DFM]"

using namespace ue5dfm;

namespace dfm {

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

// =====================================================================
//  坐标读取 (来自 IDA K2_GetActorLocation 反编译)
// =====================================================================

bool DfmMatchMonitor::getActorLocation(uintptr_t actorPtr, FVector3& outLoc) const {
    uintptr_t root = safeReadPtr(actorPtr + m_off.Actor_RootComponent);
    if (!ok(root)) return false;

    outLoc.x = safeReadFloat(root + (m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationX)));
    outLoc.y = safeReadFloat(root + (m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationY)));
    outLoc.z = safeReadFloat(root + (m_off.Scene_ComponentToWorld + offsetof(FTransform, TranslationZ)));

    // ComponentToWorld 全零时回退到 RelativeLocation
    if (outLoc.x == 0.0f && outLoc.y == 0.0f && outLoc.z == 0.0f) {
        outLoc.x = safeReadFloat(root + (m_off.Scene_RelativeLocation));
        outLoc.y = safeReadFloat(root + (m_off.Scene_RelativeLocation + offsetof(FVector, Y)));
        outLoc.z = safeReadFloat(root + (m_off.Scene_RelativeLocation + offsetof(FVector, Z)));
    }
    return true;
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
//  对局状态检测
// =====================================================================

MatchState DfmMatchMonitor::getMatchState() const {
    MatchState ms;
    uintptr_t gworldAddr = m_moduleBase + m_offGWorld;
    uintptr_t gworld = safeReadPtr(gworldAddr);
    if (!ok(gworld)) {
        LOG(LOG_LEVEL_ERROR, TAG " [matchState] GWorld 无效: gworldAddr=%p val=%p base=%p offGWorld=0x%X",
            (void*)gworldAddr, (void*)gworld, (void*)m_moduleBase, m_offGWorld);
        return ms;
    }

    ms.worldName = readObjName(gworld);
    ms.gameStatePtr = safeReadPtr(gworld + m_off.World_GameState);
    if (!ok(ms.gameStatePtr)) {
        LOG(LOG_LEVEL_ERROR, TAG " [matchState] GameState 无效: gworld=%p +0x%X=%p world='%s'",
            (void*)gworld, m_off.World_GameState, (void*)ms.gameStatePtr, ms.worldName.c_str());
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
        LOG(LOG_LEVEL_INFO, TAG " [matchState] 安全屋GS: gsClass='%s' world='%s'",
            gsClassName.c_str(), ms.worldName.c_str());
        ms.inMatch = false; return ms;
    }

    // 读取 MatchState FName (动态偏移优先, 回退到多个候选偏移)
    ms.state = readFName(ms.gameStatePtr + m_off.GS_MatchState);
    if (ms.state != "InProgress" && ms.state != "WaitingToStart" &&
        ms.state != "WaitingPostMatch" && ms.state != "LeavingMap") {
        // 动态偏移未命中, 尝试其他候选偏移 (GameMode.MatchState 等)
        static const uint32_t fallbackOffsets[] = {0x410, 0x408};
        for (auto off : fallbackOffsets) {
            std::string s = readFName(ms.gameStatePtr + off);
            if (s == "InProgress" || s == "WaitingToStart" || s == "WaitingPostMatch") {
                ms.state = s;
                break;
            }
        }
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
        uintptr_t ac = safeReadPtr(lv + 0xC8);
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
            item.itemName = (numId > 0) ? getItemDisplayName(numId) : rawId;
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

            PlayerInfo pi;
            pi.playerName = playerName.empty() ? (isAI ? "[AI]" : "(无名)") : playerName;
            pi.className = cn;
            pi.isAI = isAI;
            pi.teamId = isAI ? -1 : getTeamId(actor);
            pi.weapon = getWeaponName(actor);
            pi.characterPtr = actor;

            HealthInfo hi = getCharacterHealth(actor);
            pi.hp = hi.hp; pi.maxHp = hi.maxHp;
            pi.armor = hi.armor; pi.helmet = hi.helmet;

            getActorLocation(actor, pi.pos);
            outData.players.push_back(std::move(pi));
        }

        // ── 物资箱 ──
        if (isContainerClass(cn)) {
            ContainerInfo ci = readContainerInfo(actor, cn);
            ci.items = readContainerItems(actor, cn);
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

        PlayerInfo pi;
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

        getActorLocation(pawn, pi.pos);
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
        ci.opened = (safeReadU8(actorPtr + m_off.Cont_IsEmpty) & 1) != 0;
        ci.finished = ci.opened;
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
                ci.name = getItemDisplayName(itemIdNum);
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
                ci.name = idName;
                ci.count = stackCount;
                try { ci.itemId = std::stoi(idName); } catch (...) {}
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
    // 缓存命中: 验证 PCM 仍然有效
    if (ok(m_cachedPC)) {
        uintptr_t pcm = safeReadPtr(m_cachedPC + m_off.PC_PlayerCameraManager);
        if (ok(pcm)) return m_cachedPC;
        m_cachedPC = 0; // 失效, 重新查找
    }

    uintptr_t gworld = safeReadPtr(m_moduleBase + m_offGWorld);
    if (!ok(gworld)) return 0;

    // ── 路径 1: OwningGameInstance → LocalPlayers[0] → PlayerController ──
    // 扫描 UWorld 中多个候选偏移找 OwningGameInstance
    static const uint32_t giCandidates[] = {0x1A8, 0x1B0, 0x1B8, 0x1C0, 0x198, 0x1A0, 0x1C8, 0x1D0};
    for (uint32_t giOff : giCandidates) {
        uintptr_t gi = safeReadPtr(gworld + giOff);
        if (!ok(gi)) continue;

        // 验证: 有效 UObject (ClassPrivate 指针有效)
        uintptr_t giCls = safeReadPtr(gi + offsetof(UObjectBase, ClassPrivate));
        if (!ok(giCls)) continue;

        // 检查类名包含 "GameInstance"
        std::string clsName = readObjName(giCls);
        if (clsName.find("GameInstance") == std::string::npos) continue;

        // 在 GameInstance 中查找 LocalPlayers TArray (count 应为 1)
        static const uint32_t lpCandidates[] = {0x38, 0x40, 0x48, 0x50, 0x58};
        for (uint32_t lpOff : lpCandidates) {
            uintptr_t lpArr = safeReadPtr(gi + lpOff);
            int32_t lpCnt = safeReadS32(gi + lpOff + 8);
            if (!ok(lpArr) || lpCnt < 1 || lpCnt > 4) continue;

            uintptr_t lp0 = safeReadPtr(lpArr);
            if (!ok(lp0)) continue;

            // 在 LocalPlayer 中查找 PlayerController
            static const uint32_t pcCandidates[] = {0x30, 0x38, 0x28, 0x40};
            for (uint32_t pcOff : pcCandidates) {
                uintptr_t pc = safeReadPtr(lp0 + pcOff);
                if (!ok(pc)) continue;

                uintptr_t pcm = safeReadPtr(pc + m_off.PC_PlayerCameraManager);
                if (ok(pcm)) {
                    m_cachedPC = pc;
                    LOG(LOG_LEVEL_INFO, TAG " [camera] PC found: World+0x%X→GI+0x%X→LP+0x%X PC=%p PCM=%p",
                        giOff, lpOff, pcOff, (void*)pc, (void*)pcm);
                    return pc;
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
                    m_cachedPC = owner;
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
    if (!ok(pc)) return;

    // 读取 ControlRotation (FRotator = 3 floats)
    outData.camPitch = safeReadFloat(pc + m_off.Ctrl_ControlRotation);
    outData.camYaw   = safeReadFloat(pc + m_off.Ctrl_ControlRotation + offsetof(FRotator, Yaw));
    outData.camRoll  = safeReadFloat(pc + m_off.Ctrl_ControlRotation + offsetof(FRotator, Roll));

    // 读取 PlayerCameraManager
    uintptr_t pcm = safeReadPtr(pc + m_off.PC_PlayerCameraManager);
    if (!ok(pcm)) return;

    outData.camFOV = safeReadFloat(pcm + m_off.PCM_DefaultFOV);

    // 读取 CameraCachePrivate.POV (UE5 LWC: double 坐标)
    uintptr_t cache = pcm + m_off.PCM_CameraCachePrivate;

    // 尝试 double 读取 (UE5 LargeWorldCoordinates)
    double locX = safeReadDouble(cache + m_off.CamCache_LocationX);
    double locY = safeReadDouble(cache + m_off.CamCache_LocationY);
    double locZ = safeReadDouble(cache + m_off.CamCache_LocationZ);

    // 验证: 如果 double 值合理 (非零且有限), 使用 double 版本
    bool doubleValid = std::isfinite(locX) && std::isfinite(locY) && std::isfinite(locZ)
        && (std::fabs(locX) > 1.0 || std::fabs(locY) > 1.0 || std::fabs(locZ) > 1.0);

    if (doubleValid) {
        outData.camLocX = static_cast<float>(locX);
        outData.camLocY = static_cast<float>(locY);
        outData.camLocZ = static_cast<float>(locZ);

        // double 旋转
        double pitch = safeReadDouble(cache + m_off.CamCache_RotPitch);
        double yaw   = safeReadDouble(cache + m_off.CamCache_RotYaw);
        double roll  = safeReadDouble(cache + m_off.CamCache_RotRoll);
        if (std::isfinite(pitch) && std::isfinite(yaw)) {
            outData.camPitch = static_cast<float>(pitch);
            outData.camYaw   = static_cast<float>(yaw);
            outData.camRoll  = static_cast<float>(roll);
        }

        float cacheFov = safeReadFloat(cache + m_off.CamCache_FOV);
        if (cacheFov >= 30.0f && cacheFov <= 170.0f) {
            outData.camFOV = cacheFov;
        }
    } else {
        // 回退: 用 float 偏移试 (非 LWC 版本)
        float fx = safeReadFloat(cache + 0x04);
        float fy = safeReadFloat(cache + 0x08);
        float fz = safeReadFloat(cache + 0x0C);
        if (std::isfinite(fx) && (std::fabs(fx) > 1.0f || std::fabs(fy) > 1.0f)) {
            outData.camLocX = fx;
            outData.camLocY = fy;
            outData.camLocZ = fz;
            outData.camPitch = safeReadFloat(cache + 0x10);
            outData.camYaw   = safeReadFloat(cache + 0x14);
            outData.camRoll  = safeReadFloat(cache + 0x18);
            float fov2 = safeReadFloat(cache + 0x1C);
            if (fov2 >= 30.0f && fov2 <= 170.0f) outData.camFOV = fov2;
        } else {
            // 最终回退: 使用本地玩家位置 + ControlRotation 作为相机
            if (std::fabs(outData.myPos.x) > 1.0f || std::fabs(outData.myPos.y) > 1.0f) {
                outData.camLocX = outData.myPos.x;
                outData.camLocY = outData.myPos.y;
                outData.camLocZ = outData.myPos.z + 160.0f; // 大约眼睛高度
                LOG(LOG_LEVEL_INFO, TAG " [camera] 回退: 使用玩家位置作为相机 (%.0f,%.0f,%.0f)",
                    outData.camLocX, outData.camLocY, outData.camLocZ);
            }
        }
    }
}

// =====================================================================
//  本地玩家位置
// =====================================================================

FVector3 DfmMatchMonitor::getMyPosition() const {
    FVector3 pos;
    uintptr_t gworld = safeReadPtr(m_moduleBase + m_offGWorld);
    if (!ok(gworld)) return pos;

    // 尝试从 GameState→PlayerArray 第一个元素获取
    uintptr_t gs = safeReadPtr(gworld + m_off.World_GameState);
    if (ok(gs)) {
        uintptr_t paPtr = safeReadPtr(gs + m_off.GS_PlayerArray);
        if (ok(paPtr)) {
            uintptr_t ps0 = safeReadPtr(paPtr);
            if (ok(ps0)) {
                uintptr_t pawn0 = safeReadPtr(ps0 + m_off.PS_PawnPrivate);
                if (ok(pawn0)) getActorLocation(pawn0, pos);
            }
        }
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
}

void SharedDfmData::getData(DrawDfmData& outData) {
    std::lock_guard<std::mutex> lock(m_mutex);
    outData = m_buffers[m_frontIdx];
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

    // Actor
    TRY_RESOLVE("Actor", "RootComponent", m_off.Actor_RootComponent);

    // SceneComponent
    TRY_RESOLVE("SceneComponent", "RelativeLocation", m_off.Scene_RelativeLocation);
    TRY_RESOLVE("SceneComponent", "ComponentToWorld", m_off.Scene_ComponentToWorld);

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
    TRY_RESOLVE("GPCharacterBase", "GPHealthDataComponent", m_off.Char_HealthComp);
    TRY_RESOLVE("GPCharacterBase", "CacheCurWeapon", m_off.Char_CurWeapon);

    // Actor (通用)
    TRY_RESOLVE("Actor", "Owner", m_off.Actor_Owner);

    // 相机系统 (反射可查)
    TRY_RESOLVE("Controller", "ControlRotation", m_off.Ctrl_ControlRotation);
    TRY_RESOLVE("PlayerController", "PlayerCameraManager", m_off.PC_PlayerCameraManager);
    TRY_RESOLVE("PlayerCameraManager", "DefaultFOV", m_off.PCM_DefaultFOV);
    TRY_RESOLVE("PlayerCameraManager", "CameraCachePrivate", m_off.PCM_CameraCachePrivate);

    // 本地 PlayerController 查找链
    TRY_RESOLVE("World", "OwningGameInstance", m_off.World_OwningGameInstance);
    TRY_RESOLVE("GameInstance", "LocalPlayers", m_off.GI_LocalPlayers);
    TRY_RESOLVE("LocalPlayer", "PlayerController", m_off.LP_PlayerController);

    #undef TRY_RESOLVE

    LOG(LOG_LEVEL_INFO, TAG " 偏移解析完成: RootComp=0x%X PS=0x%X GS_PA=0x%X PCM=0x%X CamCache=0x%X",
        m_off.Actor_RootComponent, m_off.Pawn_PlayerState, m_off.GS_PlayerArray,
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
//  玩家位置快速更新 (只更新坐标/血量/武器, 不重新扫描 Actor)
// =====================================================================

void DfmMatchMonitor::updatePlayerPositions(DrawDfmData& data) const {
    for (auto& p : data.players) {
        if (p.characterPtr == 0) continue;
        getActorLocation(p.characterPtr, p.pos);

        // 更新血量
        HealthInfo hi = getCharacterHealth(p.characterPtr);
        p.hp = hi.hp; p.maxHp = hi.maxHp;
        p.armor = hi.armor; p.helmet = hi.helmet;

        // 更新武器
        p.weapon = getWeaponName(p.characterPtr);
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
        } else if (!ms.inMatch && wasInMatch) {
            LOG(LOG_LEVEL_INFO, TAG " 对局结束");
            persistentData = DrawDfmData{};
            prevData = DrawDfmData{};
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

            SharedDfmData::getInstance().pushData(persistentData);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS));
    }

    LOG(LOG_LEVEL_INFO, TAG " pollLoop 退出");
}

} // namespace dfm
