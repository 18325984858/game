/**
 * @file    il2cpp_structs.h
 * @brief   从 il2cpp.h 提取的游戏结构体定义（SkillUILogic / BattleSkillJoystickUILogic）
 * @details 字段偏移和布局来源于 IDA 反编译 + il2cpp dumper 生成的 il2cpp.h
 *          使用 reinterpret_cast 从 void* 转换到这些类型
 */

#ifndef IL2CPP_STRUCTS_H
#define IL2CPP_STRUCTS_H

#include <stdint.h>

// Forward declarations (避免引入完整 il2cpp.h)
struct Il2CppClass;
struct Il2CppObject;

// ═══════════════════════════════════════════════════════════════════════════════
// SkillUILogic (来源: il2cpp.h 行 360324)
// ═══════════════════════════════════════════════════════════════════════════════

struct SkillUILogic_Fields {
    float displayRadiusSqr;          /* 0x10 */
    int32_t oriSkillID;              /* 0x14 */
    void* slot;                      /* 0x18 SkillSlot */
    int32_t currentLevel;            /* 0x20 */
    bool isStealSkill;               /* 0x24 */
    bool isGameOver;                 /* 0x25 */
    char _pad_26[2];
    int32_t maxLevel;                /* 0x28 */
    bool isSkillR;                   /* 0x2C */
    bool m_isActive;                 /* 0x2D */
    char _pad_2e[2];
    int32_t joystickType;            /* 0x30 */
    uint8_t sortIndex;               /* 0x34 */
    char _pad_35[3];
    void* skillCtrl;                 /* 0x38 SkillUICtrl */
    void* skillUpgradeCtrl;          /* 0x40 */
    void* joystick;                  /* 0x48 SkillJoystick */
    bool isInitBasePunishSkill;      /* 0x50 */
    bool isInitWardSwitchSkill;      /* 0x51 */
    char _pad_52[6];
    void* wardSkillIds;              /* 0x58 List<int> */
    void* joysInfomation;            /* 0x60 */
    void* skillHandlerMgr;           /* 0x68 */
    void* actor;                     /* 0x70 BattleActorVisi */
    void* curSkillOperRes;           /* 0x78 SkillOperateResObject */
    bool isShowCooling;              /* 0x80 */
    float deadZoneRadius;            /* 0x84 */
    bool isTriggerSkillDown;         /* 0x88 */
    bool isPassiveSkill;             /* 0x89 */
    bool isSkillDisable;             /* 0x8A */
    bool isPassiveSkillHighlight;    /* 0x8B */
    bool isSkillCannotCastEnable;    /* 0x8C */
    bool cannotcast;                 /* 0x8D */
    char _pad_8e[2];
    void* rpnFormulaStr;             /* 0x90 String */
    int32_t rpnFormulaBuffId;        /* 0x98 */
    float lastCalDmgTime;            /* 0x9C */
    float lastFingerUIPos_x;         /* 0xA0 Vector2.x */
    float lastFingerUIPos_y;         /* 0xA4 Vector2.y */
    void* OnUpgradeShowHide;         /* 0xA8 UnityAction<bool> */
    int32_t curSlotIndex;            /* 0xB0 SkillSlotIndex_IndexEnum */
    char _pad_b4[4];
    void* curSkill;                  /* 0xB8 ActorSkill */
    float watchForward_x;            /* 0xC0 Vector3.x */
    float watchForward_y;            /* 0xC4 Vector3.y */
    float watchForward_z;            /* 0xC8 Vector3.z */
    float watchStrength;             /* 0xCC */
    float watchMoveSpeed;            /* 0xD0 */
    bool watchIsCancel;              /* 0xD4 */
    char _pad_d5[3];
    int32_t lastSyncFrameCount;      /* 0xD8 */
    bool isWatingSyncUpNextFrame;    /* 0xDC */
    char _pad_dd[3];
    int32_t waitingSyncUpUIFrameCount;/* 0xE0 */
    bool isPressingOtherSkill;       /* 0xE4 */
    // SkillStateWrap 内嵌于 0xE5
    uint8_t skillState_isControled;               /* 0xE5 */
    uint8_t skillState_isCDing;                   /* 0xE6 */
    uint8_t skillState_isCDingWithLifeSpan;       /* 0xE7 */
    uint8_t skillState_isNoEnergy;                /* 0xE8 */
    uint8_t skillState_isShowChargeCdTime;        /* 0xE9 */
    uint8_t skillState_isShowChargeCdTimeWithLifeSpan; /* 0xEA */
    uint8_t skillState_isActorDeadDisableJoystick;/* 0xEB */
    uint8_t skillState_isFullCharge;              /* 0xEC */
    uint8_t skillState_isSlotForbid;              /* 0xED */
    uint8_t skillState_IsCanUseJoystick;          /* 0xEE */
    uint8_t skillState_IsShowJoystickMask;        /* 0xEF */
    uint8_t skillState_IsSkillMsgDown;            /* 0xF0 */
    char _pad_f1[3];
    int32_t cancelStateFrameCount;               /* 0xF4 */
    void* latestFrameSpeed;                      /* 0xF8 Queue<float> */
    int32_t startInCancelAreaFrame;              /* 0x100 */
    char _pad_104[4];
    void* pressDownSkill;                        /* 0x108 ActorSkill */
};

struct SkillUILogic_o {
    Il2CppClass* klass;   /* 0x00 */
    void* monitor;        /* 0x08 */
    SkillUILogic_Fields fields; /* 0x10+ */
};

struct SkillUILogic_StaticFields {
    SkillUILogic_o* curPressedSkill;         /* 0x0 */
    bool isTriggerSkillWhenForbidden;        /* 0x8 */
    char _pad_9[3];
    float ThresholdTimeShowTips;             /* 0xC */
    bool shouldRequestPlayerControl;         /* 0x10 */
    bool shouldTriggerIndicator;             /* 0x11 */
    bool awalyTryTouchDown;                  /* 0x12 */
    bool allowDragSkill;                     /* 0x13 */
    bool isActiveWardSwitchPanel;            /* 0x14 */
};

// ═══════════════════════════════════════════════════════════════════════════════
// BattleSkillJoystickUILogic (来源: il2cpp.h 行 373074)
// ═══════════════════════════════════════════════════════════════════════════════

// BattleUILogic_Fields 基类
struct BattleUILogic_Fields {
    void* uiObject; /* 0x10 GameObject */
};

// List<SkillUILogic> — 通用 C# List 内存布局
struct Il2CppGenericList {
    Il2CppClass* klass;   /* 0x00 */
    void* monitor;        /* 0x08 */
    void* _items;         /* 0x10 Array* */
    int32_t _size;        /* 0x18 */
};

// C# Array 头部
struct Il2CppGenericArrayHeader {
    Il2CppClass* klass;              /* 0x00 */
    void* monitor;                   /* 0x08 */
    void* bounds;                    /* 0x10 */
    uintptr_t max_length;            /* 0x18 */
    // 0x20: 第一个元素
};

struct BattleSkillJoystickUILogic_Fields {
    // 继承自 BattleUILogic_Fields
    void* uiObject;                  /* 0x10 */
    // 自有字段
    void* joystickMgr;               /* 0x18 SkillUIMgr */
    Il2CppGenericList* skillUILogics; /* 0x20 List<SkillUILogic> */
    void* cachedNormalActiveJoystickGOList;  /* 0x28 */
    void* cachedSummonerJoystickGOList;      /* 0x30 */
    void* cachedDynamicActiveJoystickGOList; /* 0x38 */
    void* cachedMinionTowerJoystickGOList;   /* 0x40 */
    void* cachedExDynamicActiveJoystickDict; /* 0x48 */
    void* cachedRepalceJoystickGOList;       /* 0x50 */
    void* cachedNormalSkillJoystickGO;       /* 0x58 */
    void* customDic;                         /* 0x60 */
    bool cachedHasFourActiveSkills;          /* 0x68 */
    char _pad_69[3];
    int32_t remainPoints;                    /* 0x6C */
    void* battleFullScreenSkill;             /* 0x70 */
    // ... 后续字段省略，不影响 skillUILogics 的访问
};

struct BattleSkillJoystickUILogic_o {
    Il2CppClass* klass;   /* 0x00 */
    void* monitor;        /* 0x08 */
    BattleSkillJoystickUILogic_Fields fields; /* 0x10+ */
};

struct BattleSkillJoystickUILogic_StaticFields {
    BattleSkillJoystickUILogic_o* instance;   /* 0x0 */
    SkillUILogic_o* normalUIJoystick;         /* 0x8 */
};

#endif // IL2CPP_STRUCTS_H
