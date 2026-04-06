#ifndef PUBGMHD_H
#define PUBGMHD_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include "../libUE4Struct/ilbUE4Struct.h"

// 前向声明
namespace ue4inf { class UE4Interface; }

// =====================================================================
//  PUBG Mobile 和平精英 — 对局状态监控 + 玩家坐标采集 (C++ 原生实现)
//  从 frida_match_monitor.js 转写
//  Target: com.tencent.tmgp.pubgmhd (ARM64 Android, UE4.18 腾讯定制版)
// =====================================================================

namespace pubgmhd {

// =====================================================================
//  引擎常量
// =====================================================================
static constexpr int POLL_INTERVAL_MS         = 2000;
static constexpr int PLAYER_POLL_INTERVAL_MS  = 1000;
static constexpr float MAX_CULL_DIST_SQ       = 1.0e18f;

// =====================================================================
//  ResolvedOffsets — 通过 UE4Interface 动态查找的游戏特定偏移
//  所有值在 MatchMonitor::initOffsets() 中由反射系统解析填充
// =====================================================================
struct ResolvedOffsets {
    // UWorld
    int32_t World_GameState             = -1;
    int32_t World_AuthorityGameMode     = -1;

    // GameStateBase
    int32_t GS_MatchState               = -1;
    int32_t GS_bHasBegunPlay            = -1;
    int32_t GS_ElapsedTime              = -1;
    int32_t GS_PlayerArray              = -1;

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

    // SceneComponent
    int32_t SceneComp_Translation       = -1;

    // UAEPlayerController
    int32_t PC_bIsObserver              = -1;
    int32_t PC_bIsObserverInBattle      = -1;
    int32_t PC_bIsObserverHost          = -1;

    // Character (UAECharacter / STExtraCharacter / STExtraBaseCharacter)
    int32_t Char_Health                 = -1;
    int32_t Char_HealthMax              = -1;
    int32_t Char_TeamID                 = -1;
    int32_t Char_PlayerKey              = -1;
    int32_t Char_PlayerName             = -1;
    int32_t Char_bDead                  = -1;
    int32_t Char_CurrentNetCullDistSq   = -1;

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
    uintptr_t gameStatePtr = 0;
    std::string worldName;
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
    bool isRunning() const { return m_running; }

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

    // ---- 对局状态 ----
    MatchState getMatchState();

    // ---- Actor 位置 ----
    bool getActorLocation(uintptr_t actorPtr, FVector3& outLoc);

    // ---- 类继承链检查 ----
    bool isSubclassOf(uintptr_t classPtr, const char* targetName);

    // ---- 观战类型 ----
    uintptr_t getLocalPlayerController();
    EObserverType detectObserverType();
    bool setObserverType(EObserverType type);

    // ---- 网络可见范围修改 ----
    void patchActorNetCull(uintptr_t actorPtr);

    // ---- GUObjectArray 扫描 Character ----
    int scanCharacters();

    // ---- PlayerArray 遍历更新 ----
    int updatePlayerList(uintptr_t gameStatePtr);

    // ---- 轮询线程 ----
    void pollMatchStateLoop();
    void pollPlayers();

    // ---- 日志 ----
    void openLog();
    void writeLog(const char* line);
    void closeLog();

    // ---- 偏移解析 ----
    bool initOffsets();

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

    volatile bool m_running = false;
    std::string   m_lastMatchState;
    bool          m_isInMatch = false;
    int32_t       m_myTeamID = -1;
    int           m_lastReportedArrayNum = -1;
    int           m_lastReportedTotal = -1;

    PlayerList m_playerList;
    std::unordered_map<uintptr_t, bool> m_characterClassSet;
    std::unordered_map<int, std::string> m_nameCache;

    FILE* m_logFp = nullptr;
    int   m_logLineCount = 0;
    std::mutex m_logMutex;
};

} // namespace pubgmhd

#endif // PUBGMHD_H
