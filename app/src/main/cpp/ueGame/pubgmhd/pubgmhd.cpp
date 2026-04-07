#include "pubgmhd.h"
#include "../libUE4Struct/ilbUE4Struct.h"
#include "../interface/interface.h"
#include "../Draw/UE4Draw.h"
#include "../../Log/log.h"

#include <thread>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>

#define TAG "MatchMonitor"
#define MLOG(level, fmt, ...) LOGT(TAG, level, fmt, ##__VA_ARGS__)

namespace pubgmhd {

using Clock = std::chrono::steady_clock;

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
        MLOG(LOG_LEVEL_INFO, "[InitOffsets] %s.%s = 0x%X", className, fieldName, _off); } \
    else { MLOG(LOG_LEVEL_WARN, "[InitOffsets] 未找到 %s.%s", className, fieldName); } \
} while(0)

// 辅助: 尝试多个类名查找同一字段, 沿继承链搜索 (第一个匹配即返回)
#define RESOLVE_OFFSET_MULTI(target, fieldName, ...) do { \
    const char* _classes[] = { __VA_ARGS__ }; \
    std::string _owner; \
    for (auto* _cn : _classes) { \
        const ue4inf::UEFieldInfo* _fi = m_interface.findFieldInHierarchy(_cn, fieldName, &_owner); \
        if (_fi) { target = _fi->offset; \
            MLOG(LOG_LEVEL_INFO, "[InitOffsets] %s.%s = 0x%X (via %s)", _cn, fieldName, _fi->offset, _owner.c_str()); \
            break; } \
    } \
    if (target < 0) { MLOG(LOG_LEVEL_WARN, "[InitOffsets] 未找到 %s (尝试了 %zu 个类+继承链)", fieldName, sizeof(_classes)/sizeof(_classes[0])); } \
} while(0)

bool MatchMonitor::initOffsets() {
    MLOG(LOG_LEVEL_INFO, "[InitOffsets] 开始通过反射解析偏移...");

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
            m_off.SceneComp_Translation = ctw + 0x10; // FTransform.Translation offset
            MLOG(LOG_LEVEL_INFO, "[InitOffsets] SceneComponent.ComponentToWorld+0x10 = 0x%X", m_off.SceneComp_Translation);
        } else {
            MLOG(LOG_LEVEL_WARN, "[InitOffsets] ComponentToWorld 未找到, 回退 0x200");
            m_off.SceneComp_Translation = 0x200;
        }
    }

    // UAEPlayerController
    RESOLVE_OFFSET_MULTI(m_off.PC_bIsObserver,         "bIsObserver",         "UAEPlayerController", "STExtraPlayerController");
    RESOLVE_OFFSET_MULTI(m_off.PC_bIsObserverInBattle, "bIsObserverInBattle", "UAEPlayerController", "STExtraPlayerController");
    RESOLVE_OFFSET_MULTI(m_off.PC_bIsObserverHost,     "bIsObserverHost",     "UAEPlayerController", "STExtraPlayerController");

    // UAECharacter
    RESOLVE_OFFSET_MULTI(m_off.Char_TeamID,        "TeamID",             "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_PlayerKey,     "PlayerKey",          "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_PlayerName,    "PlayerName",         "UAECharacter", "STExtraCharacter", "STExtraBaseCharacter");

    // STExtraCharacter
    RESOLVE_OFFSET_MULTI(m_off.Char_Health,        "Health",             "STExtraCharacter", "UAECharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_HealthMax,     "HealthMax",          "STExtraCharacter", "UAECharacter", "STExtraBaseCharacter");
    RESOLVE_OFFSET_MULTI(m_off.Char_bDead,         "bDead",              "STExtraCharacter", "UAECharacter", "STExtraBaseCharacter");

    // STExtraBaseCharacter
    RESOLVE_OFFSET_MULTI(m_off.Char_CurrentNetCullDistSq, "CurrentNetCullDistanceSquared", "STExtraBaseCharacter", "STExtraCharacter", "UAECharacter");

    MLOG(LOG_LEVEL_INFO, "[InitOffsets] 解析完成, isValid=%d", m_off.isValid());
    MLOG(LOG_LEVEL_INFO, "[InitOffsets] World.GameState=0x%X GS.PlayerArray=0x%X PS.PlayerKey=0x%X",
        m_off.World_GameState, m_off.GS_PlayerArray, m_off.PS_PlayerKey);
    MLOG(LOG_LEVEL_INFO, "[InitOffsets] Actor.RootComponent=0x%X Char.Health=0x%X Char.HealthMax=0x%X",
        m_off.Actor_RootComponent, m_off.Char_Health, m_off.Char_HealthMax);

    return m_off.isValid();
}

#undef RESOLVE_OFFSET
#undef RESOLVE_OFFSET_MULTI

// =====================================================================
//  安全内存读取
// =====================================================================
uintptr_t MatchMonitor::safeReadPtr(uintptr_t addr) {
    if (addr == 0) return 0;
    uintptr_t val = 0;
    if (memcpy(&val, reinterpret_cast<void*>(addr), sizeof(val))) return val;
    return 0;
}

int32_t MatchMonitor::safeReadS32(uintptr_t addr) {
    if (addr == 0) return 0;
    int32_t val = 0;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(val));
    return val;
}

uint32_t MatchMonitor::safeReadU32(uintptr_t addr) {
    if (addr == 0) return 0;
    uint32_t val = 0;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(val));
    return val;
}

uint8_t MatchMonitor::safeReadU8(uintptr_t addr) {
    if (addr == 0) return 0;
    return *reinterpret_cast<uint8_t*>(addr);
}

float MatchMonitor::safeReadFloat(uintptr_t addr) {
    if (addr == 0) return 0.0f;
    float val = 0.0f;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(val));
    return val;
}

// =====================================================================
//  安全内存写入
// =====================================================================
bool MatchMonitor::writeMemU8(uintptr_t addr, uint8_t val) {
    if (addr == 0) return false;
    *reinterpret_cast<uint8_t*>(addr) = val;
    return true;
}

bool MatchMonitor::writeMemFloat(uintptr_t addr, float val) {
    if (addr == 0) return false;
    memcpy(reinterpret_cast<void*>(addr), &val, sizeof(val));
    return true;
}

// =====================================================================
//  FName 解析
// =====================================================================
std::string MatchMonitor::getNameByIndex(int index) {
    auto it = m_nameCache.find(index);
    if (it != m_nameCache.end()) return it->second;
    if (index < 0 || index >= m_numNames) return "";

    auto* names = reinterpret_cast<ue4::TNameEntryArray*>(m_gNames);
    int ci = index / ue4::NAMES_ELEMENTS_PER_CHUNK;
    int wi = index % ue4::NAMES_ELEMENTS_PER_CHUNK;
    ue4::FNameEntry** chk = names->Chunks[ci];
    if (!chk) return "";
    ue4::FNameEntry* entry = chk[wi];
    if (!entry) return "";

    std::string name;
    if (!entry->isWide()) {
        name = entry->AnsiName;
    } else {
        // Wide (UTF-32)
        uintptr_t a = reinterpret_cast<uintptr_t>(entry->AnsiName);
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
    auto* fn = reinterpret_cast<ue4::FName*>(addr);
    int32_t idx = fn->ComparisonIndex;
    int32_t num = fn->Number;
    if (idx < 0 || idx >= m_numNames) return "<invalid>";
    std::string base = getNameByIndex(idx);
    if (base.empty()) return "<invalid>";
    if (num == 0) return base;
    return base + "_" + std::to_string(num - 1);
}

std::string MatchMonitor::readObjName(uintptr_t objPtr) {
    if (objPtr == 0 || objPtr < 0x10000) return "<invalid>";
    auto* obj = reinterpret_cast<ue4::UObjectBase*>(objPtr);
    return readFName(reinterpret_cast<uintptr_t>(&obj->NamePrivate));
}

std::string MatchMonitor::readClassName(uintptr_t objPtr) {
    if (objPtr == 0 || objPtr < 0x10000) return "<no_class>";
    auto* obj = reinterpret_cast<ue4::UObjectBase*>(objPtr);
    uintptr_t clsPtr = reinterpret_cast<uintptr_t>(obj->ClassPrivate);
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

    std::string result;
    const uint16_t* wstr = reinterpret_cast<const uint16_t*>(dataPtr);
    for (int i = 0; i < num - 1; i++) {
        uint16_t c = wstr[i];
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

// =====================================================================
//  类继承链检
// =====================================================================
bool MatchMonitor::isSubclassOf(uintptr_t classPtr, const char* targetName) {
    auto* cur = reinterpret_cast<ue4::UStruct*>(classPtr);
    int depth = 0;
    while (cur != nullptr && depth < 20) {
        std::string name = readObjName(reinterpret_cast<uintptr_t>(cur));
        if (name == targetName) return true;
        cur = cur->SuperStruct;
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
        MLOG(LOG_LEVEL_INFO, "无法获取本地 PlayerController");
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

    MLOG(LOG_LEVEL_INFO, "[ObserverType] EObserverType_%s (%d) bIsObserver=%d bInBattle=%d bIsHost=%d",
        observerTypeName(obsType), static_cast<int>(obsType), bIsObserver, bInBattle, bIsHost);

    char logBuf[256];
    snprintf(logBuf, sizeof(logBuf), "[ObserverType] EObserverType_%s (%d)", observerTypeName(obsType), static_cast<int>(obsType));
    writeLog(logBuf);

    return obsType;
}

bool MatchMonitor::setObserverType(EObserverType type) {
    uintptr_t pc = getLocalPlayerController();
    if (pc == 0) {
        MLOG(LOG_LEVEL_ERROR, "[SetObserver] 无法获取 PlayerController");
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
            MLOG(LOG_LEVEL_ERROR, "[SetObserver] 无效类型: %d", static_cast<int>(type));
            return false;
    }
    MLOG(LOG_LEVEL_INFO, "[SetObserver] 已设置为 EObserverType_%s (%d)",
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
    if (m_off.Actor_NetCullDistSq >= 0)
        writeMemFloat(actorPtr + m_off.Actor_NetCullDistSq, MAX_CULL_DIST_SQ);
    if (m_off.Char_CurrentNetCullDistSq >= 0)
        writeMemFloat(actorPtr + m_off.Char_CurrentNetCullDistSq, MAX_CULL_DIST_SQ);
}

// =====================================================================
//  GUObjectArray 扫描所Character
// =====================================================================
int MatchMonitor::scanCharacters() {
    auto* objArray = reinterpret_cast<ue4::FUObjectArray*>(m_gUObjectArray);
    int numChunks = objArray->getNumChunks();
    int totalNum = objArray->getTotalNum();
    if (numChunks <= 0 || numChunks > 1000 || totalNum <= 0 || totalNum > 5000000) return 0;

    int globalIdx = 0;
    int newCharsFound = 0;

    for (int ci = 0; ci < numChunks; ci++) {
        auto* chunkBase = reinterpret_cast<ue4::FUObjectItem*>(objArray->getChunkPtr(ci));
        int chunkCount = objArray->getChunkCount(ci);
        if (!chunkBase || chunkCount <= 0) {
            globalIdx += (chunkCount > 0) ? chunkCount : 0;
            continue;
        }

        int remaining = totalNum - globalIdx;
        int readCount = (chunkCount < remaining) ? chunkCount : remaining;
        if (readCount <= 0) break;

        for (int wi = 0; wi < readCount; wi++) {
            ue4::UObjectBase* obj = chunkBase[wi].Object;
            if (!obj) continue;

            uintptr_t objPtr = reinterpret_cast<uintptr_t>(obj);
            uintptr_t classPtr = reinterpret_cast<uintptr_t>(obj->ClassPrivate);
            if (classPtr == 0) continue;

            // 缓存 class 是否Character 子类
            auto it = m_characterClassSet.find(classPtr);
            if (it != m_characterClassSet.end()) {
                if (!it->second) continue;
            } else {
                bool isChar = isSubclassOf(classPtr, "STExtraBaseCharacter");
                m_characterClassSet[classPtr] = isChar;
                if (!isChar) continue;
            }

            uint32_t playerKey = safeReadU32(objPtr + m_off.Char_PlayerKey);
            if (playerKey == 0) continue;

            // PlayerArray 已经更新过的玩家以其数据为准，避免位置在两个来源间来回跳变。
            if (m_playerList.findByKey(playerKey) != nullptr) continue;

            int32_t teamID = safeReadS32(objPtr + m_off.Char_TeamID);
            float health = safeReadFloat(objPtr + m_off.Char_Health);
            float healthMax = safeReadFloat(objPtr + m_off.Char_HealthMax);
            bool bDead = (safeReadU8(objPtr + m_off.Char_bDead) & 1) != 0;
            std::string playerName = readFString(objPtr + m_off.Char_PlayerName);

            FVector3 loc;
            getActorLocation(objPtr, loc);

            if (healthMax <= 0) continue; // 无效对象

            // 修改每个 Character 的网络可见范围为全地
            patchActorNetCull(objPtr);

            PlayerNode data;
            data.teamID     = teamID;
            data.playerName = playerName;
            data.isAI       = false;
            data.liveState  = bDead ? 1 : 0;
            data.health     = health;
            data.healthMax  = healthMax;
            data.kills      = 0;
            data.pos        = loc;
            m_playerList.upsert(playerKey, data);
            newCharsFound++;
        }
        globalIdx += chunkCount;
        if (globalIdx >= totalNum) break;
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
        MLOG(LOG_LEVEL_INFO, "[PlayerCount] PlayerArray=%d TotalPlayerNum=%d PlayerNum=%d AlivePlayerNum=%d AliveRealPlayerNum=%d",
            arrayNum, totalPlayerNum, playerNum, aliveNum, aliveRealNum);
        m_lastReportedArrayNum = arrayNum;
        m_lastReportedTotal = totalPlayerNum;
    }

    if (arrayData == 0 || arrayNum <= 0 || arrayNum > 500) return 0;

    std::unordered_map<uint32_t, bool> seenKeys;
    int updated = 0;

    for (int i = 0; i < arrayNum; i++) {
        uintptr_t psPtr = safeReadPtr(arrayData + i * 8);
        if (psPtr == 0) continue;

        uint32_t playerKey = safeReadU32(psPtr + m_off.PS_PlayerKey);
        if (playerKey == 0) continue;

        int32_t teamID = safeReadS32(psPtr + m_off.PS_TeamID);
        bool isAI = safeReadU8(psPtr + m_off.PS_bAIPlayer) != 0;
        uint8_t liveState = safeReadU8(psPtr + m_off.PS_LiveState);
        float health = safeReadFloat(psPtr + m_off.PS_PlayerHealth);
        float healthMax = safeReadFloat(psPtr + m_off.PS_PlayerHealthMax);
        int32_t kills = safeReadS32(psPtr + m_off.PS_Kills);
        std::string playerName = readFString(psPtr + m_off.PS_PlayerName);

        // 优先通过 CharacterOwner -> RootComponent 获取精确位置
        FVector3 loc;
        uintptr_t charOwner = safeReadPtr(psPtr + m_off.PS_CharacterOwner);
        if (charOwner != 0) {
            getActorLocation(charOwner, loc);
            patchActorNetCull(charOwner);
        }
        // 回退SelfLocAndRot
        if (loc.x == 0 && loc.y == 0 && loc.z == 0) {
            loc.x = safeReadFloat(psPtr + m_off.PS_SelfLocAndRot);
            loc.y = safeReadFloat(psPtr + m_off.PS_SelfLocAndRot + 4);
            loc.z = safeReadFloat(psPtr + m_off.PS_SelfLocAndRot + 8);
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
        m_playerList.upsert(playerKey, data);
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
            toRemove.push_back(cur->playerKey);
        }
        cur = cur->next;
    }
    for (uint32_t key : toRemove) m_playerList.remove(key);

    // 扫描 GUObjectArray 获取附近的所Character (包括敌人)
    scanCharacters();

    return updated;
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

void MatchMonitor::closeLog() {
    std::lock_guard<std::mutex> lock(m_logMutex);
    if (!m_logFp) return;
    writeLog("");
    writeLog("=== Log ended ===");
    fclose(m_logFp);
    MLOG(LOG_LEVEL_INFO, "日志已保存: %s%s (%d lines)",
        m_logDir.c_str(), m_logFile.c_str(), m_logLineCount);
    m_logFp = nullptr;
    m_logLineCount = 0;
}

// =====================================================================
//  玩家数据轮询
// =====================================================================
void MatchMonitor::pollPlayers() {
    static Clock::time_point s_lastPlayerLogTime;
    const bool shouldDumpPlayerLog = shouldLogEvery(s_lastPlayerLogTime, std::chrono::milliseconds(PLAYER_LOG_INTERVAL_MS));

    auto* names = reinterpret_cast<ue4::TNameEntryArray*>(m_gNames);
    m_numNames = names->NumElements;
    MatchState ms = getMatchState();
    if (!ms.inMatch) return;

    int count = updatePlayerList(ms.gameStatePtr);
    if (count <= 0) return;

    int aliveCount = 0, deadCount = 0;
    int aliveTeam = 0, aliveEnemy = 0;
    std::vector<PlayerNode*> enemies;
    std::vector<PlayerNode*> teammates;

    PlayerNode* cur = m_playerList.head();
    while (cur) {
        if (cur->liveState == 0 && cur->health > 0) {
            aliveCount++;
            if (m_myTeamID > 0 && cur->teamID == m_myTeamID) {
                aliveTeam++;
                teammates.push_back(cur);
            } else {
                aliveEnemy++;
                enemies.push_back(cur);
            }
        } else {
            deadCount++;
        }
        cur = cur->next;
    }

    if (shouldDumpPlayerLog) {
        MLOG(LOG_LEVEL_INFO, "[Players] %d alive (%d team + %d enemy) / %d total",
            aliveCount, aliveTeam, aliveEnemy, m_playerList.size());
    }

    char logBuf[512];

    // logcat + 文件日志: 敌人
    if (shouldDumpPlayerLog) {
        int limit = (enemies.size() < 40) ? (int)enemies.size() : 40;
        for (int i = 0; i < limit; i++) {
            PlayerNode* p = enemies[i];
            MLOG(LOG_LEVEL_INFO, "[Enemy] T%d %s %.0f/%.0fHP (%.0f, %.0f, %.0f) K:%d %s",
                 p->teamID, p->isAI ? "AI" : "Real",
                 p->health, p->healthMax,
                 p->pos.x, p->pos.y, p->pos.z,
                 p->kills, p->playerName.c_str());
            snprintf(logBuf, sizeof(logBuf), " ★ T%d %.0f/%.0fHP (%.0f, %.0f, %.0f) %s",
                     p->teamID, p->health, p->healthMax,
                     p->pos.x, p->pos.y, p->pos.z, p->playerName.c_str());
            writeLog(logBuf);
        }
        for (int i = 0; i < (int)teammates.size(); i++) {
            PlayerNode* p = teammates[i];
            MLOG(LOG_LEVEL_INFO, "[Team] T%d %s %.0f/%.0fHP (%.0f, %.0f, %.0f) K:%d %s",
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

    // 推送数据到绘制层
    ue4draw::DrawGameData drawData;
    drawData.inMatch = true;
    drawData.worldName = ms.worldName;
    drawData.matchState = ms.state;
    drawData.myTeamID = m_myTeamID;
    drawData.aliveCount = aliveCount;
    drawData.totalCount = m_playerList.size();
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
    // 读取相机数据: PlayerController(+0x658) -> PlayerCameraManager -> CameraCache.POV
    uintptr_t pc = getLocalPlayerController();
    if (pc != 0) {
        uintptr_t pcm = safeReadPtr(pc + 0x658);  // PlayerController.PlayerCameraManager
        if (pcm != 0) {
            // CameraCache.POV.Location @ PCM+0x650 (3 floats)
            drawData.camLocX = safeReadFloat(pcm + 0x650);
            drawData.camLocY = safeReadFloat(pcm + 0x654);
            drawData.camLocZ = safeReadFloat(pcm + 0x658);
            // CameraCache.POV.Rotation @ PCM+0x668 (Pitch, Yaw, Roll)
            drawData.camPitch = safeReadFloat(pcm + 0x668);
            drawData.camYaw   = safeReadFloat(pcm + 0x66C);
            drawData.camRoll  = safeReadFloat(pcm + 0x670);
            // CameraCache.POV.FOV @ PCM+0x674, fallback to DefaultFOV @ PCM+0x5E0
            drawData.camFOV = safeReadFloat(pcm + 0x674);
            if (drawData.camFOV <= 0.0f || drawData.camFOV > 170.0f) {
                drawData.camFOV = safeReadFloat(pcm + 0x5E0);
            }
            if (drawData.camFOV <= 0.0f || drawData.camFOV > 170.0f) drawData.camFOV = 90.0f;
        }
    }
    // 填充所有玩家
    cur = m_playerList.head();
    while (cur) {
        ue4draw::DrawPlayerInfo dp;
        dp.playerKey = cur->playerKey;
        dp.teamID = cur->teamID;
        dp.playerName = cur->playerName;
        dp.isAI = cur->isAI;
        dp.isAlive = (cur->liveState == 0 && cur->health > 0);
        dp.health = cur->health;
        dp.healthMax = cur->healthMax;
        dp.kills = cur->kills;
        dp.posX = cur->pos.x;
        dp.posY = cur->pos.y;
        dp.posZ = cur->pos.z;
        dp.isTeammate = (m_myTeamID > 0 && cur->teamID == m_myTeamID);
        drawData.players.push_back(dp);
        cur = cur->next;
    }
    ue4draw::SharedUE4Data::getInstance().pushData(drawData);
}

// =====================================================================
//  对局状态轮询循(后台线程)
// =====================================================================
void MatchMonitor::pollMatchStateLoop() {
    MLOG(LOG_LEVEL_INFO, "监控线程启动 (状态%dms, 玩家%dms)",
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

            auto* names = reinterpret_cast<ue4::TNameEntryArray*>(m_gNames);
            m_numNames = names->NumElements;

            MatchState ms = getMatchState();
            lastKnownState = ms;

            bool wasInMatch = m_isInMatch;
            stateChanged = (ms.state != m_lastMatchState) || (ms.inMatch != wasInMatch);
            if (stateChanged) {
                m_isInMatch = ms.inMatch;

                if (m_isInMatch && !wasInMatch) {
                    MLOG(LOG_LEVEL_INFO, "进入对局! State=%s World=%s", ms.state.c_str(), ms.worldName.c_str());
                    ue4draw::SharedUE4Data::getInstance().setInMatch(true);
                    m_playerList.clear();
                    m_characterClassSet.clear();
                    m_myTeamID = -1;
                    m_myPlayerKey = 0;
                    m_lastReportedArrayNum = -1;
                    m_lastReportedTotal = -1;
                    openLog();
                    char logBuf[256];
                    snprintf(logBuf, sizeof(logBuf), ">>> ★ 进入对局 State=%s World=%s", ms.state.c_str(), ms.worldName.c_str());
                    writeLog(logBuf);
                    writeLog("");
                    detectObserverType();
                } else if (!m_isInMatch && wasInMatch) {
                    MLOG(LOG_LEVEL_INFO, "★ 离开对局! 共追踪 %d 名玩家", m_playerList.size());
                    ue4draw::DrawGameData emptyData;
                    emptyData.inMatch = false;
                    ue4draw::SharedUE4Data::getInstance().pushData(emptyData);
                    closeLog();
                    m_playerList.clear();
                    m_myPlayerKey = 0;
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
            MLOG(LOG_LEVEL_INFO, "[%s] State=%s World=%s Players=%d",
                status.c_str(), lastKnownState.state.c_str(), lastKnownState.worldName.c_str(), m_playerList.size());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(MONITOR_IDLE_SLEEP_MS));
    }

    MLOG(LOG_LEVEL_INFO, "监控线程退出");
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
        MLOG(LOG_LEVEL_INFO, "监控已在运行");
        return true;
    }

    // 验证 GNames
    auto* namesArr = reinterpret_cast<ue4::TNameEntryArray*>(m_gNames);
    m_numNames = namesArr->NumElements;
    if (m_numNames <= 0) {
        MLOG(LOG_LEVEL_ERROR, "GNames 无效, numNames=%d", m_numNames);
        return false;
    }
    MLOG(LOG_LEVEL_INFO, "Base=%p GNames=%p numNames=%d GWorld=%p GUObjectArray=%p",
        (void*)m_moduleBase, (void*)m_gNames, m_numNames, (void*)m_gWorld, (void*)m_gUObjectArray);

    // 验证 entry[0] == "None"
    std::string entry0 = getNameByIndex(0);
    MLOG(LOG_LEVEL_INFO, "Entry[0]='%s' %s", entry0.c_str(), (entry0 == "None") ? "OK" : "BAD");

    // 通过 UE4Interface 动态解析所有游戏偏移
    if (!initOffsets()) {
        MLOG(LOG_LEVEL_ERROR, "偏移解析失败, 无法启动监控");
        return false;
    }

    m_running = true;
    m_lastMatchState = "";
    m_isInMatch = false;
    m_playerList.clear();

    std::thread(&MatchMonitor::pollMatchStateLoop, this).detach();
    MLOG(LOG_LEVEL_INFO, "=== 对局监控+玩家采集已启动 ===");
    return true;
}

void MatchMonitor::stop() {
    if (!m_running) return;
    m_running = false;
    // 等待线程安全退出 (轮询周期 + 余量)
    std::this_thread::sleep_for(std::chrono::milliseconds(POLL_INTERVAL_MS + 200));
    closeLog();
    MLOG(LOG_LEVEL_INFO, "=== 监控已停止 ===");
}

} // namespace pubgmhd

OBFU_ATTRS_END
