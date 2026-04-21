//
// Created by user on 2026/1/4.
//

#include "lolm.h"
#include "../UnityApi/unityapi.h"
#include "../../../core/log/log.h"
#include "LolOffset.h"
#include <array>
#include <cmath>
#include <cstring>
#include <unistd.h>   // pipe, write, close
#include <errno.h>
#include <chrono>
#include <unordered_map>
#include <sys/mman.h>  // mprotect



std::string lol::lol::decryPtthestring(char*Srcstr, uint32_t nameIndex) {

    LOG(LOG_LEVEL_INFO,"start nameIndex : %d Srcstr : %p Srcstrvalue : %s",nameIndex,Srcstr,Srcstr);

    auto pNode = this->m_pMapStringIndex->find(nameIndex);
    if (pNode != this->m_pMapStringIndex->end()) {
        return pNode->second->srcStr;
    }


    bool ret =IsEncryptionOrIsDecryption(nameIndex);
    LOG(LOG_LEVEL_INFO,"Ret = %d",ret);
    //判断是否加密了字符串
    if(ret){
        uint32_t newNameIndex = (int) nameIndex >= 0 ? nameIndex : nameIndex + 15;
        uint32_t arrayindex = 16LL * (int) (nameIndex - (newNameIndex & 0xFFFFFFF0));

        //判断是否需要第二次解密
        bool IsTwoEncryption = (uint8_t)(*Srcstr ^ byte_B9BBF1B[arrayindex]) == 0;

        //第一次解密
        *Srcstr ^= byte_B9BBF1B[arrayindex];

        if(!IsTwoEncryption){
            int keyIndex = 0;
            int count = 1;
            do{
                int tmpkey = keyIndex + 1;
                int tmpvalue = tmpkey >= 0 ? keyIndex + 1 : keyIndex + 16;
                keyIndex = tmpkey-(tmpvalue&0xFFFFFFF0);

                char srcchar = Srcstr[count];
                IsTwoEncryption = (uint8_t)(srcchar ^ byte_B9BBF1B[keyIndex + (int)arrayindex]) == 0;
                Srcstr[count++] = srcchar ^ byte_B9BBF1B[keyIndex + (int)arrayindex];
            }while(!IsTwoEncryption);
        }
        //设置表的状态
        *(uint32_t *)((uint64_t)this->parrayStringIndex + 4 * ((int)nameIndex >> 5)) |= 1 << (nameIndex & 0x1F);
    }

    //保存解密后的字符串
    auto pnewNode = std::make_shared<LolStrStruct>();
    pnewNode->isDecryption = 1;
    pnewNode->srcStr = Srcstr;
    this->m_pMapStringIndex->insert({nameIndex, pnewNode});
    LOG(LOG_LEVEL_INFO,"end nameIndex : %d Srcstr : %p Srcstrvalue : %s",nameIndex,Srcstr,Srcstr);
    return Srcstr;
}

void* lol::lol::inItStringindexTable(const Il2CppGlobalMetadataHeader* pGlobalMetadataHeader) {
    uint64_t stringCount = pGlobalMetadataHeader->stringCount;
    uint32_t tableSize = (((stringCount >> 3) & 0x3FFFFFFFCLL) + 4) & 0x3FFFFFFFCLL;


     LOG(LOG_LEVEL_INFO,"[DumpStr] pGlobalMetadataHeader : %p stringCount : %llu tableSize : %u",
         pGlobalMetadataHeader, (unsigned long long)stringCount, tableSize);

    //初始化表用与记录那些字符串已经解密了
    if(this->parrayStringIndex == nullptr){
        this->parrayStringIndex = malloc(tableSize);
        if(parrayStringIndex){
            memset(parrayStringIndex,0,tableSize);
        }
    }

    if(this->m_pMapStringIndex == nullptr){
        m_pMapStringIndex = std::make_shared<std::map<uint32_t,std::shared_ptr<LolStrStruct>>>();
    }

    return (void*)this->parrayStringIndex;
}

bool lol::lol::IsEncryptionOrIsDecryption(uint32_t nameIndex) {
    uint32_t IsEncryption = 1 << (nameIndex & 0x1F);                                                                    // 判断是否加密
    uint32_t IsDecryption  = *(uint32_t*)((uint64_t)this->parrayStringIndex+ (unsigned int)((int)nameIndex >> 5));

    LOG(LOG_LEVEL_INFO,"IsEncryption : %d IsDecryption : %d",IsEncryption,IsDecryption);
    // 将nameindex除以32拿到在array中记录的索引// 判断是否重复解密
    return (IsEncryption & IsDecryption) == 0;
}

uint32_t lol::FEVisi::GetFieldOffsetFromKlass(::Il2CppClass* klass, const char* fieldName) {
    if (!klass || !fieldName || !m_pfunctionInfo) return INVALID_OFFSET;
    auto* current = klass;
    while (current) {
        void* iter = nullptr;
        while (auto* field = m_pfunctionInfo->il2cpp_class_get_fields(current, &iter)) {
            const char* name = m_pfunctionInfo->il2cpp_field_get_name(field);
            if (name && strcmp(name, fieldName) == 0) {
                return (uint32_t)m_pfunctionInfo->il2cpp_field_get_offset(field);
            }
        }
        current = m_pfunctionInfo->il2cpp_class_get_parent(current);
    }
    return INVALID_OFFSET;
}

bool lol::FEVisi::ReadStaticFieldInt32(::Il2CppClass* klass, const char* fieldName, int32_t& outValue) {
    if (!klass || !fieldName || !m_pfunctionInfo) return false;
    auto* current = klass;
    while (current) {
        void* iter = nullptr;
        while (auto* field = m_pfunctionInfo->il2cpp_class_get_fields(current, &iter)) {
            const char* name = m_pfunctionInfo->il2cpp_field_get_name(field);
            if (name && strcmp(name, fieldName) == 0) {
                m_pfunctionInfo->il2cpp_field_static_get_value(field, &outValue);
                return true;
            }
        }
        current = m_pfunctionInfo->il2cpp_class_get_parent(current);
    }
    return false;
}

lol::lol::lol(void* dqil2cppBase,void *pCodeRegistration,
              void *pMetadataRegistration,void *pGlobalMetadataHeader,
              void* pMetadataImagesTable):
              FEVisi(dqil2cppBase,pCodeRegistration,pMetadataRegistration,
                     pGlobalMetadataHeader,pMetadataImagesTable){

    LOG(LOG_LEVEL_INFO,"[DumpStr] lol Init pGlobalMetadataHeader : %p",pGlobalMetadataHeader);

    if(pGlobalMetadataHeader) {
        this->inItStringindexTable(static_cast<const Il2CppGlobalMetadataHeader *>(pGlobalMetadataHeader));
    }


    //此处补丁是为了将游戏模式设置成
    // ── Patch: dqil2cppBase + 0xB69B494 ──
    // 原始指令: F3 03 01 2A (mov w19, w1)
    // 目标指令: 73 00 80 52 (mov w19, #3)
    {
        uint8_t* patchAddr = reinterpret_cast<uint8_t*>(dqil2cppBase) + 0xB69B494;
        const uint8_t original[] = {0xF3, 0x03, 0x01, 0x2A};
        const uint8_t patched[]  = {0x93, 0x00, 0x80, 0x52};

        if (memcmp(patchAddr, original, sizeof(original)) == 0) {
            size_t pageSize = sysconf(_SC_PAGESIZE);
            uintptr_t pageStart = reinterpret_cast<uintptr_t>(patchAddr) & ~(pageSize - 1);
            if (mprotect(reinterpret_cast<void*>(pageStart), pageSize * 2, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
                memcpy(patchAddr, patched, sizeof(patched));
                __builtin___clear_cache(reinterpret_cast<char*>(patchAddr),
                                        reinterpret_cast<char*>(patchAddr + sizeof(patched)));
                LOG(LOG_LEVEL_INFO, "[Patch] 0xB69B494 patch 成功: F3 03 01 2A (MOV W19, W1) → 93 00 80 52 (MOV W19, #4)");
            } else {
                LOG(LOG_LEVEL_ERROR, "[Patch] mprotect 失败: %s", strerror(errno));
            }
        } else {
            LOG(LOG_LEVEL_WARN, "[Patch] 0xB69B494 原始字节不匹配, 跳过 (当前: %02X %02X %02X %02X)",
                patchAddr[0], patchAddr[1], patchAddr[2], patchAddr[3]);
        }
    }

    LOG(LOG_LEVEL_INFO,"[DumpStr] lol Init Success! ");
}

lol::lol::~lol() {
    if(this->parrayStringIndex){
        free(this->parrayStringIndex);
    }
    this->m_pMapStringIndex = nullptr;
}

lol::FEVisi::FEVisi(void* dqil2cppBase,void *pCodeRegistration,void *pMetadataRegistration,
                    void *pGlobalMetadataHeader,void* pMetadataImagesTable)
    : m_il2cppBase(dqil2cppBase)
    , m_pfunctionInfo(std::make_shared<fun::function>(
            dqil2cppBase, pCodeRegistration, pMetadataRegistration,
            pGlobalMetadataHeader, pMetadataImagesTable))
{
    m_pfunctionInfo->fillingClassInfo();
}

lol::FEVisi::~FEVisi() = default;


bool lol::FEVisi::get_BattleStarted() {
    typedef bool (*get_BattleStartedpfn)();
    auto pget_BattleStarted = (get_BattleStartedpfn)m_pfunctionInfo->GetMethodFun(
            "ilbil2cpp.so",
            "Assembly-CSharp.dll",
            "FEVisi",
            "FrameEngine.Visual.FEVisi",
            "get_BattleStarted");
    return pget_BattleStarted ? pget_BattleStarted() : false;
}

void *lol::FEVisi::get_battleTeamMgr() {
    typedef void* (*get_battleTeamMgrpfn)();
    auto fn = (get_battleTeamMgrpfn)m_pfunctionInfo->GetMethodFun(
            "ilbil2cpp.so",
            "Assembly-CSharp.dll",
            "FEVisi",
            "FrameEngine.Visual.FEVisi",
            "get_battleTeamMgr");
    return fn ? fn() : nullptr;
}

int32_t lol::FEVisi::get_MiniIconBaseCtrlType(void* pData) {

    static uint32_t iconTypeOffset = INVALID_OFFSET;
    if(iconTypeOffset == INVALID_OFFSET) {
            iconTypeOffset = GetFieldOffset(
                "Assembly-CSharp.dll",
                "UIMiniIconBaseCtrl",
                "UIMiniIconBaseCtrl",
                "iconType");
    }
    if (iconTypeOffset == INVALID_OFFSET) return -1;
    return ReadMemberValue<int32_t>(pData, iconTypeOffset);
}

float lol::FEVisi::DecoderFix64(uint64_t value) {

    static float divideOfOne = 0.0f;
    static bool divideOfOneInit = false;

    if(!divideOfOneInit) {
        divideOfOneInit = true;
        uint64_t rawBits = m_pfunctionInfo->GetStaticMember(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "Fix64", "FrameEngine.Common.Fix64",
                "divideOfOne");
        // GetStaticMember 返回原始 IEEE 754 位模式，需要按位重新解释为 float
        memcpy(&divideOfOne, &rawBits, sizeof(float));
        LOG(LOG_LEVEL_INFO,"[MiniMap][DecoderFix64] rawBits=0x%llX divideOfOne=%f",
            (unsigned long long)rawBits, divideOfOne);
    }
    if (divideOfOne == 0.0f) return -1;

    LOG(LOG_LEVEL_INFO,"[MiniMap][DecoderFix64] value : %llu divideOfOne : %f",
        (unsigned long long)value, divideOfOne);
    return (float)value * divideOfOne;
}

// ═══════════════════════════════════════════════════════════════════════════════
// MiniMap 拆分函数实现
// ═══════════════════════════════════════════════════════════════════════════════

std::string lol::FEVisi::getManagedTypeName(void* pObject) {
    if (pObject == nullptr) return "<null>";

    auto* klass = GetObjectKlass(pObject);
    if (klass == nullptr) return "<no-klass>";

    auto* rKlass = reinterpret_cast<::Il2CppClass*>(klass);
    const char* pNamespace = m_pfunctionInfo->il2cpp_class_get_namespace(rKlass);
    const char* pName = m_pfunctionInfo->il2cpp_class_get_name(rKlass);
    if (pNamespace && pNamespace[0] != '\0') {
        return std::string(pNamespace) + "." + (pName ? pName : "<unnamed>");
    }
    return pName ? std::string(pName) : std::string("<unnamed>");
}

void* lol::FEVisi::getMiniIconsDictionary() {
    auto* _entityCtrl = (void*)m_pfunctionInfo->GetStaticMember(
            "ilbil2cpp.so", "Assembly-CSharp.dll",
            "UIMainBattleMiniMapCtrl", "UIMainBattleMiniMapCtrl",
            "_entityCtrl");
    if (_entityCtrl == nullptr) {
        LOG(LOG_LEVEL_ERROR, "[MiniMap] _entityCtrl 为空");
        return nullptr;
    }

    uint32_t miniMapIconCtrlOffset = GetFieldOffset(
            "Assembly-CSharp.dll", "MiniMapEntityCtrl", "MiniMapEntityCtrl", "miniMapIconCtrl");
    if (miniMapIconCtrlOffset == INVALID_OFFSET) return nullptr;

    void* miniMapIconCtrl = ReadMemberPtr(_entityCtrl, miniMapIconCtrlOffset);
    if (miniMapIconCtrl == nullptr) return nullptr;

    uint32_t miniIconsOffset = GetFieldOffset(
            "Assembly-CSharp.dll", "UIMiniMapIconCtrl", "UIMiniMapIconCtrl", "miniIcons");
    if (miniIconsOffset == INVALID_OFFSET) return nullptr;

    auto* pDict = ReadMemberPtr(miniMapIconCtrl, miniIconsOffset);
    if (pDict == nullptr) {
        LOG(LOG_LEVEL_ERROR, "[MiniMap] miniIcons Dictionary 为空");
    }
    return pDict;
}

bool lol::FEVisi::tryGetWorldPosition(void* pObject, UnityVector3& outPos) {
    using GetPositionFn = UnityVector3(*)(void*);
    static GetPositionFn s_getPosition = nullptr;

    if (!s_getPosition) {
        s_getPosition = reinterpret_cast<GetPositionFn>(m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "FixGameObjectVisi", "FrameEngine.Visual.FixGameObjectVisi",
                "GetPosition"));
    }
    if (pObject == nullptr || s_getPosition == nullptr) return false;
    outPos = s_getPosition(pObject);
    return true;
}

bool lol::FEVisi::getIconWorldPos(void* baseCtrl, void* actor, UnityVector3* outWorldPos) {

    // ── 缓存 followObj 字段偏移 ──
    static uint32_t s_followObjOffset = INVALID_OFFSET;
    if (s_followObjOffset == INVALID_OFFSET) {
        s_followObjOffset = GetFieldOffset(
                "Assembly-CSharp.dll", "UIMiniIconBaseCtrl",
                "UIMiniIconBaseCtrl", "followObj");
    }

    // ── 从 baseCtrl 读取 followObj ──
    void* followObj = nullptr;
    if (baseCtrl && s_followObjOffset != INVALID_OFFSET) {
        followObj = ReadMemberPtr(baseCtrl, s_followObjOffset);
    }

    // ── 优先从 actor 获取世界坐标，失败则从 followObj 获取 ──
    UnityVector3 pos{};
    bool hasWorldPos = tryGetWorldPosition(actor, pos)
                       || tryGetWorldPosition(followObj, pos);

    if (hasWorldPos && outWorldPos) {
        *outWorldPos = pos;
    }
    return hasWorldPos;
}

bool lol::FEVisi::isWardLikeIcon(int32_t iconType, void* actor,
                                  const std::string& baseCtrlTypeName,
                                  const std::string& actorTypeName) {
    using BoolFn = bool(*)(void*);
    static BoolFn s_isNeutralEye = nullptr;
    static BoolFn s_isSummonedWards = nullptr;

    if (!s_isNeutralEye) {
        s_isNeutralEye = reinterpret_cast<BoolFn>(m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActorVisi", "FrameEngine.Visual.BattleActorVisi",
                "IsNeutralEye"));
    }
    if (!s_isSummonedWards) {
        s_isSummonedWards = reinterpret_cast<BoolFn>(m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActorVisi", "FrameEngine.Visual.BattleActorVisi",
                "IsSummonedWards"));
    }

    // 1) 图标类型直接匹配
    if ((MiniMapIconType)iconType == MiniMapIconType_MyTeamWard) return true;

    // 2) 运行时方法检测
    if (actor != nullptr) {
        if (s_isNeutralEye && s_isNeutralEye(actor)) return true;
        if (s_isSummonedWards && s_isSummonedWards(actor)) return true;
    }

    // 3) 类型名关键字匹配
    if (baseCtrlTypeName.find("Ward") != std::string::npos ||
        baseCtrlTypeName.find("Eye")  != std::string::npos ||
        actorTypeName.find("Ward")    != std::string::npos ||
        actorTypeName.find("Eye")     != std::string::npos) {
        return true;
    }

    return false;
}

bool lol::FEVisi::appendMinionData(int32_t iconType, void* actor,
                                   const UnityVector3& worldPos, bool hasWorldPos) {
    if ((MiniMapIconType)iconType != MiniMapIconType_Solider) return false;

    MiniMapMinionInfo minion{};
    minion.worldPos = worldPos;
    minion.hasWorldPos = hasWorldPos;
    minion.curHp = 0.0f;
    minion.maxHp = 0.0f;
    minion.screenX = 0.0f;
    minion.screenY = 0.0f;
    minion.hasScreenPos = false;
    minion.objId = 0;
    minion.camp = -1;
    minion.isEnemy = false;

    LOG(LOG_LEVEL_INFO, "[Minion] 读取到小兵信息 actor：%p", actor);

    if (actor && IsReadableMemory(actor, sizeof(void*))) {
        minion.objId = readActorObjId(actor);
        minion.camp = readActorCamp(actor);
        minion.isEnemy = (m_miniMapData.myCamp >= 0 && minion.camp >= 0 && minion.camp != m_miniMapData.myCamp);

        auto* pAttribute = getActorAttribute(actor);
        LOG(LOG_LEVEL_INFO, "[Minion] actor：%p pAttribute=%p", actor, pAttribute);
        if (pAttribute && IsReadableMemory(pAttribute, 0x20)) {
            readEnemyHeroHP(pAttribute, &minion.curHp, &minion.maxHp);
        }
        LOG(LOG_LEVEL_INFO, "[Minion] #%d actor=%p objId=%u camp=%d myCamp=%d enemy=%d attr=%p HP=%.0f/%.0f pos=(%.1f,%.1f,%.1f) w2s=%d",
            (int)m_miniMapData.minions.size(), actor,
            minion.objId, minion.camp, m_miniMapData.myCamp, minion.isEnemy, pAttribute,
            minion.curHp, minion.maxHp,
            worldPos.x, worldPos.y, worldPos.z, hasWorldPos);
    } else {
        LOG(LOG_LEVEL_WARN, "[Minion] #%d actor=%p 不可读或为空, 跳过血量读取",
            (int)m_miniMapData.minions.size(), actor);
    }

    if (hasWorldPos) {
        float sx = 0.0f;
        float sy = 0.0f;
        if (worldToScreen(worldPos, sx, sy)) {
            minion.screenX = sx;
            minion.screenY = sy;
            minion.hasScreenPos = true;
        }
    }

    m_miniMapData.minions.push_back(minion);
    return true;
}

uint32_t lol::FEVisi::readActorObjId(void* actorVisi) {
    if (!actorVisi || !m_pfunctionInfo || !IsReadableMemory(actorVisi, sizeof(void*))) return 0;

    typedef uint32_t (*FnGetObjId)(void*);
    static FnGetObjId s_getObjId = nullptr;
    if (!s_getObjId) {
        s_getObjId = reinterpret_cast<FnGetObjId>(m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "FixGameObjectVisi", "FrameEngine.Visual.FixGameObjectVisi",
                "get_objId"));
        LOG(LOG_LEVEL_INFO, "[Minion] resolve FixGameObjectVisi.get_objId=%p", (void*)s_getObjId);
    }

    return s_getObjId ? s_getObjId(actorVisi) : 0;
}

int32_t lol::FEVisi::readActorCamp(void* actorVisi) {
    if (!actorVisi || !IsReadableMemory(actorVisi, sizeof(void*))) return -1;

    static uint32_t s_campOffset = INVALID_OFFSET;
    if (s_campOffset == INVALID_OFFSET) {
        s_campOffset = GetFieldOffset(
                "Assembly-CSharp.dll",
                "BattleActorVisi",
                "FrameEngine.Visual.BattleActorVisi",
                "_camp");
        LOG(LOG_LEVEL_INFO, "[Minion] resolve BattleActorVisi._camp offset=%u", s_campOffset);
    }

    if (s_campOffset == INVALID_OFFSET) return -1;
    return ReadMemberValue<int32_t>(actorVisi, s_campOffset);
}


void* lol::FEVisi::getActorAttribute(void* actorVisi) {
    if (!actorVisi) return nullptr;

    // ── BattleActorVisi → get_actor() → BattleActor ──
    typedef void* (*pGetLogicActor)(void*);
    static pGetLogicActor s_getLogicActor = nullptr;
    if (!s_getLogicActor) {
        s_getLogicActor = (pGetLogicActor)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActorVisi", "FrameEngine.Visual.BattleActorVisi",
                "get_actor");
    }
    if (!s_getLogicActor) return nullptr;

    void* pBattleActor = s_getLogicActor(actorVisi);
    if (!pBattleActor) return nullptr;

    // ── BattleActor → get_attribute() → ActorComponentAttribute ──
    typedef void* (*pGetAttribute)(void*);
    static pGetAttribute s_getAttribute = nullptr;
    if (!s_getAttribute) {
        s_getAttribute = (pGetAttribute)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActor", "FrameEngine.Logic.BattleActor",
                "get_attribute");
    }
    if (!s_getAttribute) return nullptr;

    return s_getAttribute(pBattleActor);
}

uint32_t lol::FEVisi::readEnemyHeroLeve(void* pAttribute) {
    if (!pAttribute) return 0;

    typedef int32_t (*pGetLevel)(void*, void*);
    static pGetLevel s_getLevel = nullptr;
    if (!s_getLevel) {
        s_getLevel = (pGetLevel)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "ActorComponentAttribute",
                "FrameEngine.Logic.ActorComponentAttribute",
                "get_level");
        LOG(LOG_LEVEL_INFO, "[MiniMap] resolve get_level=%p", (void*)s_getLevel);
    }
    if (!s_getLevel) return 0;
    return (uint32_t)s_getLevel(pAttribute, nullptr);
}

void lol::FEVisi::readEnemyHeroHP(void* pAttribute,float* curHp, float* maxHp) {
    if (!pAttribute) return;

    // ═══════════════════════════════════════════════════════════════
    // IDA 逆向确认的 HP 读取方式 (已通过血条UI函数交叉验证):
    //   nativeData = *(pAttribute + 0x18)
    //   curHP(Fix64) = *(nativeData + 0x138)  — RVA 0x8495B98
    //   maxHP(Fix64) = GetFixAttrValue(attr, 2) — RVA 0x84959E0
    //   maxHP(备用)  = RVA 0x8495BC8(this)
    //   heroLevel    = *(nativeData + 0x78)   — RVA 0x8495058
    //   shield       = *(nativeData + 0F0)   — RVA 0x849510C
    // ═══════════════════════════════════════════════════════════════

    // curHP: get_curHP()
    typedef int64_t (*pGetCurHP)(void*, void*);
    static pGetCurHP s_getCurHP = nullptr;
    if (!s_getCurHP) {
        s_getCurHP = (pGetCurHP)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "ActorComponentAttribute",
                "FrameEngine.Logic.ActorComponentAttribute",
                "get_curHP");
        LOG(LOG_LEVEL_INFO, "[MiniMap][HP] resolve get_curHP=%p", (void*)s_getCurHP);
    }

    int64_t curHP_raw = s_getCurHP ? s_getCurHP(pAttribute, nullptr) : 0;

    // maxHP: get_maxHPVisi()
    typedef int64_t (*pGetMaxHPVisi)(void*, void*);
    static pGetMaxHPVisi s_getMaxHPVisi = nullptr;
    if (!s_getMaxHPVisi) {
        s_getMaxHPVisi = (pGetMaxHPVisi)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "ActorComponentAttribute",
                "FrameEngine.Logic.ActorComponentAttribute",
                "get_maxHPVisi");
        LOG(LOG_LEVEL_INFO, "[MiniMap][HP] resolve get_maxHPVisi=%p", (void*)s_getMaxHPVisi);
    }

    int64_t maxHP_raw = s_getMaxHPVisi ? s_getMaxHPVisi(pAttribute, nullptr) : 0;

    if(curHp){
        *curHp = DecoderFix64(curHP_raw); // Fix64 转 float
    }
    if(maxHp){
        *maxHp = DecoderFix64(maxHP_raw); // Fix64 转 float
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 英雄名称读取
// ═══════════════════════════════════════════════════════════════════════════════
int32_t lol::FEVisi::readHeroResId(void* actorVisi) {
    if (!actorVisi) return 0;

    static uint32_t s_resIdOffset = INVALID_OFFSET;
    if (s_resIdOffset == INVALID_OFFSET) {
        s_resIdOffset = GetFieldOffset(
                "Assembly-CSharp.dll", "BattleActorVisi",
                "FrameEngine.Visual.BattleActorVisi", "_resId");
        LOG(LOG_LEVEL_INFO, "[HeroName] _resId offset = 0x%X", s_resIdOffset);
    }
    if (s_resIdOffset == INVALID_OFFSET) return 0;
    return ReadMemberValue<int32_t>(actorVisi, s_resIdOffset);
}

std::string lol::FEVisi::readHeroName(void* actorVisi) {
    if (!actorVisi) return "";

    // ═══════════════════════════════════════════════════════════════
    // 英雄名称读取策略（安全版）:
    //
    // ⚠ 不直接调用 BattleActorVisi.get_name()，因其内部会触发
    //   IsHero() → BattleContext.GetPlayerNameInBattle(get_roleId())
    //   等复杂链路，在注���上下文中调用会导致崩溃。
    //
    // 方式1: 直接读取 <name>k__BackingField 缓存字段（安全的内存读取）
    //        IDA: sub_45704A8 → cached = ActorVisi[45] → offset +0x168
    // 方式2: 通过 UIHeroUtils.GetHeroName(resId) 回退查询配置表
    // ═══════════════════════════════════════════════════════════════

    // ── 方式1: 读取 <name>k__BackingField 缓存 ──
    static uint32_t s_cachedNameOffset = INVALID_OFFSET;
    static bool s_offsetResolved = false;
    if (!s_offsetResolved) {
        s_offsetResolved = true;
        s_cachedNameOffset = GetFieldOffset(
                "Assembly-CSharp.dll", "BattleActorVisi",
                "FrameEngine.Visual.BattleActorVisi", "<name>k__BackingField");
        if (s_cachedNameOffset == INVALID_OFFSET) {
            s_cachedNameOffset = 0x168; // IDA 硬编码 fallbackFrameEngine_Visual_BattleActorVisi__get_name
            LOG(LOG_LEVEL_INFO, "[HeroName] 使用 IDA 硬编���缓存偏移 0x168");
        } else {
            LOG(LOG_LEVEL_INFO, "[HeroName] <name>k__BackingField offset = 0x%X", s_cachedNameOffset);
        }
    }

    void* pCachedStr = ReadMemberPtr(actorVisi, s_cachedNameOffset);
    if (pCachedStr) {
        std::string heroName = m_pfunctionInfo->il2cpp_Il2CppString_toCString(
                static_cast<const Il2CppString*>(pCachedStr));
        LOG(LOG_LEVEL_INFO, "[HeroName] 缓存(+0x%X) → \"%s\"", s_cachedNameOffset, heroName.c_str());
        if (!heroName.empty()) return heroName;
    }

    // ── 方式2: 回退 UIHeroUtils.GetHeroName(resId) ──
    int32_t resId = readHeroResId(actorVisi);
    if (resId <= 0) {
        LOG(LOG_LEVEL_WARN, "[HeroName] 缓存为空且 resId=%d 无效，无法获取英雄名", resId);
        return "";
    }

    typedef void* (*pGetHeroName)(int32_t, void*);
    static pGetHeroName s_getHeroName = nullptr;
    static bool s_heroNameResolved = false;
    if (!s_heroNameResolved) {
        s_heroNameResolved = true;
        s_getHeroName = (pGetHeroName)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "UIHeroUtils", "UIHeroUtils",
                "GetHeroName");
        LOG(LOG_LEVEL_INFO, "[HeroName] UIHeroUtils.GetHeroName resolved: %p", (void*)s_getHeroName);
    }
    if (!s_getHeroName) return "";

    void* pFallbackStr = s_getHeroName(resId, nullptr);
    if (!pFallbackStr) return "";

    std::string heroName = m_pfunctionInfo->il2cpp_Il2CppString_toCString(
            static_cast<const Il2CppString*>(pFallbackStr));
    LOG(LOG_LEVEL_INFO, "[HeroName] UIHeroUtils: resId=%d → \"%s\"", resId, heroName.c_str());
    return heroName;
}

std::string lol::FEVisi::readSummonerName(void* actorVisi) {
    if (!actorVisi) return "";

    // ── 1. BattleActorVisi → get_player() → BattlePlayer ──
    typedef void* (*pGetPlayer)(void*);
    static pGetPlayer s_getPlayer = nullptr;
    if (!s_getPlayer) {
        s_getPlayer = (pGetPlayer)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActorVisi", "FrameEngine.Visual.BattleActorVisi",
                "get_player");
        LOG(LOG_LEVEL_INFO, "[SummonerName] get_player resolved: %p", (void*)s_getPlayer);
    }
    if (!s_getPlayer) return "";

    void* pBattlePlayer = s_getPlayer(actorVisi);
    if (!pBattlePlayer) {
        LOG(LOG_LEVEL_WARN, "[SummonerName] BattlePlayer 为空");
        return "";
    }

    // ── 2. 验证 BattlePlayer 是有效的托管对象 ──
    auto* klass = GetObjectKlass(pBattlePlayer);
    if (!klass) {
        LOG(LOG_LEVEL_WARN, "[SummonerName] BattlePlayer klass 无效");
        return "";
    }

    // ── 3. 直接读取已缓存的召唤师名，不调用 get_name() ──
    //    IDA 反汇编 get_name (0x91E7160):
    //      LDR X0, [this, #0xB0]       ; 读取缓存
    //      BL  String::IsNullOrEmpty    ; 检查缓存
    //      若非空 → 直接返回
    //      若为空 → 调用 BattleContext.GetPlayerNameInBattle(roleId)（复杂链路，注入上下文不安全）
    //
    //    因此我们直接读 this+0xB0, 跳过复杂的 BattleContext 查找链

    // 优先通过运行时反射查找 name 字段的精确偏移
    static uint32_t s_nameOffset = INVALID_OFFSET;
    if (s_nameOffset==INVALID_OFFSET) {

        m_pfunctionInfo->GetStaticMember(
                "ilbil2cpp.so",
                "Assembly-CSharp.dll",
                "BattlePlayer",
                "FrameEngine.Logic.BattlePlayer",
                "_name",
                reinterpret_cast<uint32_t *>(&s_nameOffset));
    }

    void* pNameStr = ReadMemberPtr(pBattlePlayer, s_nameOffset);
    if (!pNameStr) {
        LOG(LOG_LEVEL_INFO, "[SummonerName] 名称缓存未填充 (offset=0x%X)", s_nameOffset);

        return "";
    }

    // ── 4. Il2CppString → std::string (UTF-8) ──
    return m_pfunctionInfo->il2cpp_Il2CppString_toCString(
            static_cast<const Il2CppString *>(pNameStr));
}

// ═══════════════════════════════════════════════════════════════════════════════
// worldToScreen() — 世界坐标 → 屏幕坐标（通过 Unity Camera.WorldToScreenPoint）
//
// 使用 FindClassByName + GetMethodFunByClass 精确解析 Camera 类方法：
//   Camera.get_main()              — 0 参数，静态属性 getter
//   Camera.WorldToScreenPoint(V3)  — 1 参数（仅 Vector3），避免匹配 2 参数重载
//
// 返回的屏幕坐标为 Unity 空间（原点左下角，Y 从下到上）。
// 绘制层（ImGui）需自行翻转 Y。
// ═══════════════════════════════════════════════════════════════════════════════

bool lol::FEVisi::worldToScreen(const UnityVector3& worldPos, float& outScreenX, float& outScreenY) {

    // ── 缓存: Camera 类和方法指针 ──
    using GetMainCameraFn     = void*(*)();
    using WorldToScreenPointFn = UnityVector3(*)(void*, UnityVector3);

    static bool                  s_resolved       = false;
    static GetMainCameraFn       s_getMainCamera  = nullptr;
    static WorldToScreenPointFn  s_w2s            = nullptr;

    if (!s_resolved) {
        s_resolved = true;

        // 查找 UnityEngine.Camera 类
        auto* cameraClass = m_pfunctionInfo->FindClassByName(
                "ilbil2cpp.so", "UnityEngine.CoreModule.dll",
                "Camera", "UnityEngine.Camera");

        if (!cameraClass) {
            LOG(LOG_LEVEL_ERROR, "[W2S] 无法找到 UnityEngine.Camera 类");
            return false;
        }

        // 确保类的方法指针已初始化
        m_pfunctionInfo->il2cpp_runtime_class_init(cameraClass);
        m_pfunctionInfo->il2cpp_class_init_all_method(cameraClass);

        // 解析 Camera.get_main (0 参数, 静态 getter)
        s_getMainCamera = reinterpret_cast<GetMainCameraFn>(
                m_pfunctionInfo->GetMethodFunByClass(cameraClass, "get_main", 0));

        // 解析 Camera.WorldToScreenPoint (1 参数: Vector3)
        s_w2s = reinterpret_cast<WorldToScreenPointFn>(
                m_pfunctionInfo->GetMethodFunByClass(cameraClass, "WorldToScreenPoint", 1));

        LOG(LOG_LEVEL_INFO, "[W2S] Camera class=%p  get_main=%p  WorldToScreenPoint=%p",
            cameraClass, (void*)s_getMainCamera, (void*)s_w2s);
    }

    if (!s_getMainCamera || !s_w2s) return false;

    // ── 获取主摄像机 (带帧级缓存, 避免每个英雄都调用 get_main) ──
    static void* s_cachedCamera = nullptr;
    static std::chrono::steady_clock::time_point s_cameraTime{};
    auto now = std::chrono::steady_clock::now();
    // 每 100ms 刷新一次摄像机指针
    if (!s_cachedCamera ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - s_cameraTime).count() > 100) {
        s_cachedCamera = s_getMainCamera();
        s_cameraTime = now;
    }
    void* mainCamera = s_cachedCamera;
    if (!mainCamera || !IsReadableMemory(mainCamera, sizeof(void*))) {
        s_cachedCamera = nullptr;  // 失效时清空缓存
        return false;
    }

    // ── 调用 WorldToScreenPoint ──
    UnityVector3 screenPos = s_w2s(mainCamera, worldPos);

    // z < 0 表示目标在相机后方，不应绘制
    if (screenPos.z < 0.0f) return false;

    outScreenX = screenPos.x;
    outScreenY = screenPos.y;
    return true;
}

bool lol::FEVisi::buildProjectedRangeRing(const UnityVector3& centerWorld, float range,
                                         std::vector<ScreenPoint>& outPoints) {
    outPoints.clear();
    if (range <= 0.0f) return false;

    constexpr int kSegments = 40;
    outPoints.reserve(kSegments);

    for (int i = 0; i < kSegments; ++i) {
        const float theta = (2.0f * 3.1415926535f * (float)i) / (float)kSegments;
        UnityVector3 edgeWorld{
            centerWorld.x + std::cos(theta) * range,
            centerWorld.y,
            centerWorld.z + std::sin(theta) * range,
        };

        float screenX = 0.0f;
        float screenY = 0.0f;
        if (!worldToScreen(edgeWorld, screenX, screenY)) {
            outPoints.clear();
            return false;
        }

        outPoints.push_back({screenX, screenY});
    }

    return outPoints.size() >= 8;
}

// ═══════════════════════════════════════════════════════════════════════════════
// updateMiniMapData() — 小地图主循环（遍历所有图标，收集敌方英雄HP和眼位坐标）
// ═══════════════════════════════════════════════════════════════════════════════

void *lol::FEVisi::updateMiniMapData() {

    // ── 清空上一帧数据 ──
    m_miniMapData.clear();

    // ── 1. 获取小地图图标字典 ──
    auto* pMiniIconsDictionary = getMiniIconsDictionary();
    if (!pMiniIconsDictionary) return nullptr;

    // ── 2. 初始化 Dictionary 迭代元数据（缓存） ──
    static uint32_t s_dictCountOffset = INVALID_OFFSET;
    static uint32_t s_dictEntriesOffset = INVALID_OFFSET;
    static uint32_t s_entryHashCodeOffset = INVALID_OFFSET;
    static uint32_t s_entryValueOffset = INVALID_OFFSET;
    static size_t   s_entryStride = 0;

    if (s_dictCountOffset == INVALID_OFFSET) {
        auto* dictKlass = GetObjectKlass(pMiniIconsDictionary);
        s_dictCountOffset = GetFieldOffsetFromKlass(dictKlass, "count");
        s_dictEntriesOffset = GetFieldOffsetFromKlass(dictKlass, "entries");
        LOG(LOG_LEVEL_INFO, "[MiniMap] Dict offsets: count=%u entries=%u",
            s_dictCountOffset, s_dictEntriesOffset);
    }
    if (s_dictCountOffset == INVALID_OFFSET || s_dictEntriesOffset == INVALID_OFFSET) {
        LOG(LOG_LEVEL_ERROR, "[MiniMap] 无法获取 Dictionary 字段偏移");
        return nullptr;
    }

    int32_t dictCount = ReadMemberValue<int32_t>(pMiniIconsDictionary, s_dictCountOffset);
    void* entriesArray = ReadMemberPtr(pMiniIconsDictionary, s_dictEntriesOffset);
    if (!entriesArray) return nullptr;

    if (s_entryStride == 0) {
        auto* arrRKlass = GetObjectKlass(entriesArray);
        auto* arrType = m_pfunctionInfo->il2cpp_class_get_type(arrRKlass);
        auto* elemRKlass = m_pfunctionInfo->il2cpp_type_get_class_or_element_class(arrType);

        uint32_t rawHashCodeOff = GetFieldOffsetFromKlass(elemRKlass, "hashCode");
        uint32_t rawValueOff    = GetFieldOffsetFromKlass(elemRKlass, "value");

        int32_t instSize    = m_pfunctionInfo->il2cpp_class_instance_size(elemRKlass);
        bool    isValueType = m_pfunctionInfo->il2cpp_class_is_valuetype(elemRKlass);
        s_entryStride = isValueType ? (size_t)(instSize - 0x10) : (size_t)instSize;

        if (isValueType) {
            s_entryHashCodeOffset = rawHashCodeOff - 0x10;
            s_entryValueOffset    = rawValueOff - 0x10;
        } else {
            s_entryHashCodeOffset = rawHashCodeOff;
            s_entryValueOffset    = rawValueOff;
        }

        LOG(LOG_LEVEL_INFO, "[MiniMap] Entry stride=%zu hashCodeOff=%u valueOff=%u (raw: %u, %u)",
            s_entryStride, s_entryHashCodeOffset, s_entryValueOffset,
            rawHashCodeOff, rawValueOff);
    }

    // ── 3. 缓存 UIMiniIconBaseCtrl 字段偏移 ──
    static uint32_t s_cacheFollowPosOffset = INVALID_OFFSET;
    if (s_cacheFollowPosOffset == INVALID_OFFSET) {
        s_cacheFollowPosOffset = GetFieldOffset(
                "Assembly-CSharp.dll", "UIMiniIconBaseCtrl", "UIMiniIconBaseCtrl", "cacheFollowPos");
    }

    typedef void* (*pGetActor)(void*);
    static pGetActor s_getActor = nullptr;
    if (!s_getActor) {
        s_getActor = (pGetActor)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "UIMiniIconBaseCtrl", "UIMiniIconBaseCtrl", "get_actor");
    }

    // ── 4. 遍历字典条目，收集数据到 m_miniMapData ──
    constexpr size_t kArrayHeaderSize = 0x20;

    for (int i = 0; i < dictCount; i++) {
        uint8_t* entryBase = reinterpret_cast<uint8_t*>(entriesArray) + kArrayHeaderSize + i * s_entryStride;

        // ── 第2层防御: 验证 entry 内存可读 ──
        if (!IsReadableMemory(entryBase, s_entryStride)) {
            LOG(LOG_LEVEL_WARN, "[MiniMap] entry[%d] 内存不可读 (%p), 跳过", i, entryBase);
            continue;
        }

        int32_t hashCode = *reinterpret_cast<int32_t*>(entryBase + s_entryHashCodeOffset);
        if (hashCode < 0) continue;

        void* baseCtrl = *reinterpret_cast<void**>(entryBase + s_entryValueOffset);
        if (!baseCtrl) continue;

        // ── 第2层防御: 验证 baseCtrl 对象可读 ──
        if (!IsReadableMemory(baseCtrl, sizeof(void*))) {
            LOG(LOG_LEVEL_WARN, "[MiniMap] baseCtrl[%d]=%p 内存不可读, 跳过", i, baseCtrl);
            continue;
        }

        const int32_t iconType = get_MiniIconBaseCtrlType(baseCtrl);
        void* actor = s_getActor ? s_getActor(baseCtrl) : nullptr;

        // ── 诊断: 打印所有图标类型（确认小兵是否在字典中） ──
        LOG(LOG_LEVEL_INFO, "[MiniMap][Diag] entry[%d] iconType=%d actor=%p baseCtrl=%p",
            i, iconType, actor, baseCtrl);

        // ── 获取世界坐标 ──
        UnityVector3 worldPos{};
        bool hasWorldPos = getIconWorldPos(baseCtrl, actor, &worldPos);

        // ── 后备: 从 cacheFollowPos 读取缓存坐标 (自身英雄 actor/followObj 可能为空) ──
        if (!hasWorldPos && baseCtrl && s_cacheFollowPosOffset != INVALID_OFFSET) {
            auto* posPtr = reinterpret_cast<UnityVector3*>(
                    reinterpret_cast<uint8_t*>(baseCtrl) + s_cacheFollowPosOffset);
            if (IsReadableMemory(posPtr, sizeof(UnityVector3))) {
                UnityVector3 cached = *posPtr;
                if (cached.x != 0.0f || cached.z != 0.0f) {
                    worldPos = cached;
                    hasWorldPos = true;
                }
            }
        }

        // ── 敌方英雄：收集 HP / 等级 / 坐标 / 名称 ──
        if ((MiniMapIconType)iconType == MiniMapIconType_EnemyTeamHero ||
        (MiniMapIconType)iconType ==MiniMapIconType_MyTeamHero) {
            MiniMapEnemyHeroInfo info{};
            info.iconType    = iconType;

            // ── 名称缓存 (按 resId) —— 名称在对局中不变, 只查询一次 ──
            static std::unordered_map<int32_t, std::string> s_heroNameCache;
            static std::unordered_map<int32_t, std::string> s_summonerNameCache;

            // ── 第2层防御: 验证 actor 可读后再深层访问 ──
            if (actor && IsReadableMemory(actor, sizeof(void*))) {
                auto pAttribute = getActorAttribute(actor);
                if (pAttribute && IsReadableMemory(pAttribute, 0x20)) {
                    info.heroLevel = readEnemyHeroLeve(pAttribute);
                    readEnemyHeroHP(pAttribute, &info.curHp, &info.maxHp);
                }
                info.heroResId = readHeroResId(actor);

                // 英雄名: 优先从缓存获取, 缓存未命中才调用 IL2CPP
                if (info.heroResId > 0) {
                    auto itName = s_heroNameCache.find(info.heroResId);
                    if (itName != s_heroNameCache.end()) {
                        info.heroName = itName->second;
                    } else {
                        info.heroName = readHeroName(actor);
                        if (!info.heroName.empty())
                            s_heroNameCache[info.heroResId] = info.heroName;
                    }

                    // 召唤师名: 同理
                    auto itSummoner = s_summonerNameCache.find(info.heroResId);
                    if (itSummoner != s_summonerNameCache.end()) {
                        info.summonerName = itSummoner->second;
                    } else {
                        info.summonerName = readSummonerName(actor);
                        if (!info.summonerName.empty())
                            s_summonerNameCache[info.heroResId] = info.summonerName;
                    }
                }
            }

            info.worldPos    = worldPos;
            info.hasWorldPos = hasWorldPos;

            if ((MiniMapIconType)iconType == MiniMapIconType_MyTeamHero && actor && IsReadableMemory(actor, sizeof(void*))) {
                const int32_t myCamp = readActorCamp(actor);
                if (myCamp >= 0) {
                    m_miniMapData.myCamp = myCamp;
                }
            }

            // ── 世界坐标 → 屏幕坐标 (W2S) ──
            info.hasScreenPos = false;
            if (hasWorldPos) {
                float sx = 0.0f, sy = 0.0f;
                if (worldToScreen(worldPos, sx, sy)) {
                    info.screenX      = sx;
                    info.screenY      = sy;
                    info.hasScreenPos = true;
                }
            }

            // ── 所有英雄: 获取普攻有效范围 ──
            info.atkRange = -1.0f;
            if (actor && IsReadableMemory(actor, sizeof(void*))) {
                bool isSelf = ((MiniMapIconType)iconType == MiniMapIconType_MyTeamHero);
                float skillRange = getMySkillValidTargetRange(actor, isSelf);
                if (skillRange > 0.0f) info.atkRange = skillRange;
            }

            // ── 己方英雄: 额外记录坐标到 myXxx 字段 ──
            if ((MiniMapIconType)iconType == MiniMapIconType_MyTeamHero
                && actor && IsReadableMemory(actor, sizeof(void*))
                && m_miniMapData.mySkillRange < 0.0f) {
                m_miniMapData.myWorldPos = worldPos;
                if (info.hasScreenPos) {
                    m_miniMapData.myScreenX     = info.screenX;
                    m_miniMapData.myScreenY     = info.screenY;
                    m_miniMapData.hasMyScreenPos = true;
                }
                if (info.atkRange > 0.0f) {
                    m_miniMapData.mySkillRange = info.atkRange;
                    buildProjectedRangeRing(worldPos, info.atkRange,
                                            m_miniMapData.mySkillRangeScreenPoints);
                }
            }

            // (高频采集: 日志由 printMiniMapData 定期输出, 此处不逐帧打印)

            m_miniMapData.enemyHeroes.push_back(info);
            continue;
        }

        // ── 眼/守卫类：收集坐标 ──
        const std::string baseCtrlTypeName = getManagedTypeName(baseCtrl);
        const std::string actorTypeName    = getManagedTypeName(actor);

        if (isWardLikeIcon(iconType, actor, baseCtrlTypeName, actorTypeName)) {
            MiniMapWardInfo ward{};
            ward.worldPos    = worldPos;
            ward.hasWorldPos = hasWorldPos;
            ward.iconType    = iconType;
            ward.hasScreenPos = false;
            if (hasWorldPos) {
                float sx = 0.0f, sy = 0.0f;
                if (worldToScreen(worldPos, sx, sy)) {
                    ward.screenX      = sx;
                    ward.screenY      = sy;
                    ward.hasScreenPos = true;
                }
            }
            m_miniMapData.wards.push_back(ward);
        }

        appendMinionData(iconType, actor, worldPos, hasWorldPos);
    }

    return pMiniIconsDictionary;
}


void lol::FEVisi::updateMinionData() {
    m_miniMapData.minions.clear();

    typedef void* (*FnGetActorVisiList)();
    typedef void* (*FnGetLogicActor)(void*);
    typedef bool (*FnIsSoilder)(void*, void*);

    static FnGetActorVisiList s_getActorVisiList = nullptr;
    static FnGetLogicActor s_getLogicActor = nullptr;
    static FnIsSoilder s_isSoilder = nullptr;

    if (!s_getActorVisiList) {
        s_getActorVisiList = (FnGetActorVisiList)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "FEVisi", "FrameEngine.Visual.FEVisi",
                "GetActorVisiList");
        LOG(LOG_LEVEL_INFO, "[Minion] resolve FEVisi.GetActorVisiList=%p", (void*)s_getActorVisiList);
    }
    if (!s_getLogicActor) {
        s_getLogicActor = (FnGetLogicActor)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActorVisi", "FrameEngine.Visual.BattleActorVisi",
                "get_actor");
        LOG(LOG_LEVEL_INFO, "[Minion] resolve BattleActorVisi.get_actor=%p", (void*)s_getLogicActor);
    }
    if (!s_isSoilder) {
        s_isSoilder = (FnIsSoilder)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActor", "FrameEngine.Logic.BattleActor",
                "IsSoilder");
        LOG(LOG_LEVEL_INFO, "[Minion] resolve BattleActor.IsSoilder=%p", (void*)s_isSoilder);
    }

    if (!s_getActorVisiList || !s_getLogicActor || !s_isSoilder) {
        LOG(LOG_LEVEL_ERROR, "[Minion] 小兵遍历依赖方法解析失败");
        return;
    }

    auto* pVisiList = s_getActorVisiList();
    if (!pVisiList || !IsReadableMemory(pVisiList, sizeof(Il2CppGenericList))) {
        LOG(LOG_LEVEL_WARN, "[Minion] ActorVisiList=%p 不可读", pVisiList);
        return;
    }

    auto* pList = reinterpret_cast<Il2CppGenericList*>(pVisiList);
    auto* pItems = reinterpret_cast<Il2CppGenericArrayHeader*>(pList->_items);
    int listSize = pList->_size;
    LOG(LOG_LEVEL_INFO, "[Minion] ActorVisiList: _items=%p _size=%d", pList->_items, listSize);

    if (!pItems || listSize <= 0 || listSize > 5000) {
        LOG(LOG_LEVEL_WARN, "[Minion] ActorVisiList 无效: _items=%p size=%d", (void*)pItems, listSize);
        return;
    }

    auto** elements = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pItems) + sizeof(Il2CppGenericArrayHeader));
    if (!IsReadableMemory(elements, listSize * sizeof(void*))) {
        LOG(LOG_LEVEL_WARN, "[Minion] ActorVisiList elements 内存不可读");
        return;
    }

    for (int i = 0; i < listSize; ++i) {
        void* actorVisi = elements[i];
        if (!actorVisi || !IsReadableMemory(actorVisi, sizeof(void*))) continue;

        void* logicActor = s_getLogicActor(actorVisi);
        if (!logicActor || !IsReadableMemory(logicActor, 0xA0)) continue;
        if (!s_isSoilder(logicActor, nullptr)) continue;

        UnityVector3 worldPos{};
        bool hasWorldPos = tryGetWorldPosition(actorVisi, worldPos);

        appendMinionData(MiniMapIconType_Solider, actorVisi, worldPos, hasWorldPos);
    }

    LOG(LOG_LEVEL_INFO, "[Minion] updateMinionData 完成，count=%zu", m_miniMapData.minions.size());
}

// ═══════════════════════════════════════════════════════════════════════════════
// printMiniMapData() — 打印 m_miniMapData 中存储的数据快照（调试用）
// ═══════════════════════════════════════════════════════════════════════════════

void lol::FEVisi::printMiniMapData() const {
    LOG(LOG_LEVEL_INFO, "[ALLRADAR]╔══════════════ MiniMap Data Snapshot ══════════════╗");
    LOG(LOG_LEVEL_INFO, "[ALLRADAR]║  英雄: %zu  眼/守卫: %zu  小兵: %zu",
        m_miniMapData.enemyHeroes.size(), m_miniMapData.wards.size(), m_miniMapData.minions.size());
    LOG(LOG_LEVEL_INFO, "[ALLRADAR]╠══════════════ Enemy Heroes ═══════════════════════╣");

    for (size_t i = 0; i < m_miniMapData.enemyHeroes.size(); ++i) {
        const auto& h = m_miniMapData.enemyHeroes[i];
        LOG(LOG_LEVEL_INFO,
            "[ALLRADAR]║ [%zu] type=%d Lv%u  HP=%.1f/%.1f  pos=(%.3f,%.3f,%.3f) valid=%d",
            i, h.iconType, h.heroLevel, h.curHp, h.maxHp,
            h.worldPos.x, h.worldPos.y, h.worldPos.z, h.hasWorldPos);
        LOG(LOG_LEVEL_INFO,
            "[ALLRADAR]║       resId=%d  hero=%s  summoner=%s  atkRange=%.2f",
            h.heroResId, h.heroName.c_str(), h.summonerName.c_str(), h.atkRange);
        LOG(LOG_LEVEL_INFO,
            "[ALLRADAR]║       screen=(%.1f,%.1f) screenValid=%d",
            h.screenX, h.screenY, h.hasScreenPos);
    }

    LOG(LOG_LEVEL_INFO, "[ALLRADAR]╠══════════════ Wards ══════════════════════════════╣");

    for (size_t i = 0; i < m_miniMapData.wards.size(); ++i) {
        const auto& w = m_miniMapData.wards[i];
        LOG(LOG_LEVEL_INFO,
            "[ALLRADAR]║ [%zu] type=%d  pos=(%.3f,%.3f,%.3f) valid=%d",
            i, w.iconType, w.worldPos.x, w.worldPos.y, w.worldPos.z, w.hasWorldPos);
    }

    LOG(LOG_LEVEL_INFO, "[ALLRADAR]╚═══════════════════════════════════════════════════╝");

    LOG(LOG_LEVEL_INFO, "[ALLRADAR]╠══════════════ Minions ════════════════════════════╣");
    LOG(LOG_LEVEL_INFO, "[ALLRADAR]║ Count: %zu", m_miniMapData.minions.size());

    for (size_t i = 0; i < m_miniMapData.minions.size(); ++i) {
        const auto& mn = m_miniMapData.minions[i];
        LOG(LOG_LEVEL_INFO,
            "[ALLRADAR]║ [%zu] objId=%u camp=%d enemy=%d HP=%.0f/%.0f pos=(%.1f,%.1f,%.1f) scr=(%.0f,%.0f) scrValid=%d",
            i, mn.objId, mn.camp, mn.isEnemy, mn.curHp, mn.maxHp,
            mn.worldPos.x, mn.worldPos.y, mn.worldPos.z,
            mn.screenX, mn.screenY, mn.hasScreenPos);
    }

    LOG(LOG_LEVEL_INFO, "[ALLRADAR]╚═══════════════════════════════════════════════════╝");
}

// ═══════════════════════════════════════════════════════════════════════════════
// IsReadableMemory — 第2层防御: 使用 pipe write 验证内存可读性
//
// 原理: write() 是系统调用，内核在拷贝用户空间数据时会检查页表：
//   - 可读页 → write 成功（返回 size）
//   - 不可读 → write 失败（返回 -1, errno = EFAULT）
//   - 不会触发 SIGSEGV
// ═══════════════════════════════════════════════════════════════════════════════

bool lol::FEVisi::IsReadableMemory(const void* ptr, size_t size) {
    if (ptr == nullptr || size == 0) return false;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        // pipe 创建失败，保守返回 false
        return false;
    }

    // 尝试将 ptr 指向的数据写入 pipe
    // 内核会在 copy_from_user 时验证内存可读性
    ssize_t ret = write(pipefd[1], ptr, size);

    // 立即关闭 pipe（无论成功与否）
    close(pipefd[0]);
    close(pipefd[1]);

    // ret == size → 内存可读; ret == -1 && errno == EFAULT → 内存不可读
    return (ret == (ssize_t)size);
}

// ═══════════════════════════════════════════════════════════════════════════════
// readIl2CppString — 将 IL2CPP 托管 System.String 对象转为 C++ std::string (UTF-8)
// ═══════════════════════════════════════════════════════════════════════════════

std::string lol::FEVisi::readIl2CppString(void* pIl2CppString) {
    if (pIl2CppString == nullptr) return "";
    if (!IsReadableMemory(pIl2CppString, sizeof(void*))) {
        LOG(LOG_LEVEL_WARN, "[readIl2CppString] 对象指针 %p 内存不可读", pIl2CppString);
        return "";
    }
    return m_pfunctionInfo->il2cpp_Il2CppString_toCString(
            static_cast<const Il2CppString*>(pIl2CppString));
}

// ═══════════════════════════════════════════════════════════════════════════════
// getMySkillValidTargetRange — 获取己方英雄当前技能的有效目标范围
//
// 施法技能检测优先级（按顺序尝试，找到即停止）:
//
//   ┌─ UI 层 (SkillUILogic 静态字段) ───────────────────────────────────────────┐
//   │  方式⑧  SkillUILogic.curPressedSkill — 技能按钮被按下时立即生效         │
//   │         （按下技能图标、出现蓝色瞄准圈时 curPressedSkill != null）       │
//   └──────────────────────────────────────────────────────────────────────────┘
//   ┌─ 逻辑层 (ActorComponentSkillMgr) ────────────────────────────────────────┐
//   │  方式⑤  SkillSlot[0-4].currentSkill — 遍历技能槽位（已验证生效）        │
//   └──────────────────────────────────────────────────────────────────────────┘
//
// 命中后日志打印 "✓ 方式⑤"。
// 若失败，使用普攻技能 (GetNormalAttackSkill) 的 get_curRange。
// ═══════════════════════════════════════════════════════════════════════════════

float lol::FEVisi::getMySkillValidTargetRange(void* actorVisi, bool isSelf) {
    if (!actorVisi) return -1.0f;

    // 1. BattleActorVisi → get_actor() → BattleActor
    typedef void* (*FnGetActor)(void*);
    static FnGetActor s_getActor = nullptr;
    if (!s_getActor) {
        s_getActor = (FnGetActor)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActorVisi", "FrameEngine.Visual.BattleActorVisi",
                "get_actor");
    }
    if (!s_getActor) return -1.0f;
    void* pBattleActor = s_getActor(actorVisi);
    if (!pBattleActor || !IsReadableMemory(pBattleActor, 0x20)) return -1.0f;

    // 2. BattleActor → get_skillMgr()
    typedef void* (*FnGetSkillMgr)(void*);
    static FnGetSkillMgr s_getSkillMgr = nullptr;
    if (!s_getSkillMgr) {
        s_getSkillMgr = (FnGetSkillMgr)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleActor", "FrameEngine.Logic.BattleActor",
                "get_skillMgr");
    }
    if (!s_getSkillMgr) return -1.0f;
    void* pSkillMgr = s_getSkillMgr(pBattleActor);
    if (!pSkillMgr || !IsReadableMemory(pSkillMgr, 0x60)) return -1.0f;

    // 3. GetNormalAttackSkill()
    typedef void* (*FnGetSkill)(void*);
    static FnGetSkill s_getNormalAttackSkill = nullptr;
    if (!s_getNormalAttackSkill) {
        s_getNormalAttackSkill = (FnGetSkill)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "ActorComponentSkillMgr", "FrameEngine.Logic.ActorComponentSkillMgr",
                "GetNormalAttackSkill");
    }
    if (!s_getNormalAttackSkill) return -1.0f;
    void* pSkill = s_getNormalAttackSkill(pSkillMgr);

    // 3.1 通过 SkillUILogic 获取当前正在释放的技能（C# ActorSkill 对象）
    //
    // 关键: currentActiveList 中存的是 native 数据指针，不是 C# 对象，
    //       不能直接传给 IsCasting/IsCast 等 il2cpp 方法。
    //       正确做法: 从 SkillUILogic._curSkill (偏移 0xB8) 获取 C# 对象，
    //       然后调用 IsCasting/IsCast 判断状态。

    typedef bool (*FnActorSkillState)(void*, void*);
    static FnActorSkillState s_isCasting     = nullptr;
    static FnActorSkillState s_isCast        = nullptr;
    static FnActorSkillState s_isUsed        = nullptr;
    static FnActorSkillState s_isCastSuccess = nullptr;
    static FnActorSkillState s_isFinished    = nullptr;
    static FnActorSkillState s_isManualFinish= nullptr;
    static FnActorSkillState s_isReady       = nullptr;
    static FnActorSkillState s_isLearn       = nullptr;
    static FnActorSkillState s_canBeCast     = nullptr;
    static FnActorSkillState s_isCastBackswing = nullptr;
    static bool s_skillStatesResolved = false;

    if (!s_skillStatesResolved) {
        s_skillStatesResolved = true;

        // 辅助 lambda: 按多个候选名称解析方法
        auto resolveMethod = [&](const char* methodNames[], int count, const char* label) -> FnActorSkillState {
            for (int i = 0; i < count; i++) {
                auto fn = (FnActorSkillState)m_pfunctionInfo->GetMethodFun(
                        "ilbil2cpp.so", "Assembly-CSharp.dll",
                        "ActorSkill", "FrameEngine.Logic.ActorSkill", methodNames[i]);
                if (fn) {
                    LOG(LOG_LEVEL_INFO, "[SkillState] resolve %s → %s=%p", label, methodNames[i], (void*)fn);
                    return fn;
                }
            }
            LOG(LOG_LEVEL_WARN, "[SkillState] %s 所有变体均未找到", label);
            return nullptr;
        };

        const char* castingNames[]      = {"IsCasting", "get_IsCasting", "get_isCasting", "isCasting"};
        const char* castNames[]         = {"IsCast", "get_IsCast", "get_isCast", "isCast"};
        const char* usedNames[]         = {"IsUsed", "get_IsUsed", "get_isUsed", "isUsed"};
        const char* castSuccessNames[]  = {"IsCastSuccess", "get_IsCastSuccess"};
        const char* finishedNames[]     = {"IsFinished", "get_IsFinished"};
        const char* manualFinishNames[] = {"IsManualFinish", "get_IsManualFinish"};
        const char* readyNames[]        = {"IsReady", "get_IsReady"};
        const char* learnNames[]        = {"get_isLearn", "isLearn"};
        const char* canBeCastNames[]    = {"get_canBeCast", "canBeCast"};
        const char* backswingNames[]    = {"get_isCastBackswing", "isCastBackswing"};

        s_isCasting      = resolveMethod(castingNames,      4, "IsCasting");
        s_isCast         = resolveMethod(castNames,         4, "IsCast");
        s_isUsed         = resolveMethod(usedNames,         4, "IsUsed");
        s_isCastSuccess  = resolveMethod(castSuccessNames,  2, "IsCastSuccess");
        s_isFinished     = resolveMethod(finishedNames,     2, "IsFinished");
        s_isManualFinish = resolveMethod(manualFinishNames, 2, "IsManualFinish");
        s_isReady        = resolveMethod(readyNames,        2, "IsReady");
        s_isLearn        = resolveMethod(learnNames,        2, "isLearn");
        s_canBeCast      = resolveMethod(canBeCastNames,    2, "canBeCast");
        s_isCastBackswing= resolveMethod(backswingNames,    2, "isCastBackswing");
    }

    // 辅助 lambda: 检查 ActorSkill 是否正在施法，并打印所有状态
    auto checkCasting = [&](void* pActorSkill) -> bool {
        if (!pActorSkill) return false;

        bool casting      = s_isCasting      ? s_isCasting(pActorSkill, nullptr)      : false;
        bool cast         = s_isCast         ? s_isCast(pActorSkill, nullptr)         : false;
        bool used         = s_isUsed         ? s_isUsed(pActorSkill, nullptr)         : false;
        bool castSuccess  = s_isCastSuccess  ? s_isCastSuccess(pActorSkill, nullptr)  : false;
        bool finished     = s_isFinished     ? s_isFinished(pActorSkill, nullptr)     : false;
        bool manualFinish = s_isManualFinish ? s_isManualFinish(pActorSkill, nullptr) : false;
        bool ready        = s_isReady        ? s_isReady(pActorSkill, nullptr)        : false;
        bool learned      = s_isLearn        ? s_isLearn(pActorSkill, nullptr)        : false;
        bool canCast      = s_canBeCast      ? s_canBeCast(pActorSkill, nullptr)      : false;
        bool backswing    = s_isCastBackswing? s_isCastBackswing(pActorSkill, nullptr): false;

        LOG(LOG_LEVEL_INFO,
            "[SkillState] skill=%p | Learned=%d CanBeCast=%d Ready=%d "
            "IsCasting=%d IsCast=%d IsCastSuccess=%d IsCastBackswing=%d "
            "IsUsed=%d IsFinished=%d IsManualFinish=%d",
            pActorSkill, learned, canCast, ready,
            casting, cast, castSuccess, backswing,
            used, finished, manualFinish);

        return casting || cast || used;
    };

    // ─── 查找正在释放的技能 ───
    void* pCastingSkill = nullptr;

    // ─── 方式⑧: 通过 il2cpp 反射 API 读取 SkillUILogic.curPressedSkill 静态字段 ───
    // 当玩家按下技能图标、出现蓝色瞄准圈时，curPressedSkill != null
    // 仅己方英雄触发，避免将自己的 UI 状态错误应用到敌方英雄
    if (isSelf) {
        static ::Il2CppClass* s_skillUILogicKlass = nullptr;
        static uint32_t s_triggerDownOff = INVALID_OFFSET;
        static uint32_t s_curSkillOff    = INVALID_OFFSET;
        static uint32_t s_slotIndexOff   = INVALID_OFFSET;
        static uint32_t s_oriSkillIDOff  = INVALID_OFFSET;
        static bool s_fieldsResolved = false;

        if (!s_skillUILogicKlass && m_pfunctionInfo) {
            s_skillUILogicKlass = m_pfunctionInfo->FindClassByName(
                        "ilbil2cpp.so", "Assembly-CSharp.dll",
                        "SkillUILogic", "");
            LOG(LOG_LEVEL_INFO, "[SkillRange][方式⑧] FindClass SkillUILogic=%p", s_skillUILogicKlass);
        }

        // 一次性解析实例字段偏移
        if (s_skillUILogicKlass && !s_fieldsResolved) {
            s_fieldsResolved = true;
            s_triggerDownOff = GetFieldOffsetFromKlass(s_skillUILogicKlass, "isTriggerSkillDown");
            s_curSkillOff    = GetFieldOffsetFromKlass(s_skillUILogicKlass, "_curSkill");
            s_slotIndexOff   = GetFieldOffsetFromKlass(s_skillUILogicKlass, "_curSlotIndex");
            s_oriSkillIDOff  = GetFieldOffsetFromKlass(s_skillUILogicKlass, "oriSkillID");
            LOG(LOG_LEVEL_INFO,
                "[SkillRange][方式⑧] offsets: triggerDown=0x%X curSkill=0x%X slotIndex=0x%X oriSkillID=0x%X",
                s_triggerDownOff, s_curSkillOff, s_slotIndexOff, s_oriSkillIDOff);
        }

        // 通过 il2cpp_field_static_get_value 读取 curPressedSkill
        if (s_skillUILogicKlass) {
            void* pCurPressed = nullptr;
            void* iter = nullptr;
            ::Il2CppClass* pKlass = s_skillUILogicKlass;
            while (pKlass) {
                iter = nullptr;
                while (auto* field = m_pfunctionInfo->il2cpp_class_get_fields(pKlass, &iter)) {
                    const char* name = m_pfunctionInfo->il2cpp_field_get_name(field);
                    if (name && strcmp(name, "curPressedSkill") == 0) {
                        m_pfunctionInfo->il2cpp_field_static_get_value(field, &pCurPressed);
                        break;
                    }
                }
                if (pCurPressed) break;
                pKlass = m_pfunctionInfo->il2cpp_class_get_parent(pKlass);
            }
            LOG(LOG_LEVEL_INFO, "[SkillRange][方式⑧] curPressedSkill=%p", pCurPressed);

            if (pCurPressed && IsReadableMemory(pCurPressed, 0xC0)
                && s_triggerDownOff != INVALID_OFFSET && s_curSkillOff != INVALID_OFFSET) {
                bool triggerDown = *reinterpret_cast<bool*>(
                        reinterpret_cast<uint8_t*>(pCurPressed) + s_triggerDownOff);
                LOG(LOG_LEVEL_INFO, "[SkillRange][方式⑧] isTriggerSkillDown=%d", triggerDown);
                if (triggerDown) {
                    void* pActorSkill = *reinterpret_cast<void**>(
                            reinterpret_cast<uint8_t*>(pCurPressed) + s_curSkillOff);
                    if (pActorSkill && IsReadableMemory(pActorSkill, 0x80)) {
                        pCastingSkill = pActorSkill;
                        int32_t slotIdx = s_slotIndexOff != INVALID_OFFSET ?
                            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(pCurPressed) + s_slotIndexOff) : -1;
                        int32_t skillID = s_oriSkillIDOff != INVALID_OFFSET ?
                            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(pCurPressed) + s_oriSkillIDOff) : -1;
                        LOG(LOG_LEVEL_INFO,
                            "[SkillRange] ✓ 方式⑧ curPressedSkill: skill=%p slotIdx=%d oriSkillID=%d",
                            pActorSkill, slotIdx, skillID);
                    }
                }
            }
        }
    }

    // ─── 通过 ActorComponentSkillMgr 从逻辑层获取正在施法的技能 ───
    if (!pCastingSkill && pSkillMgr) {

        // ── 一次性: 通过框架 API 发现字段偏移（处理加密元数据） ──
        static bool s_fieldsDumped = false;
        static uint32_t s_slotCurrentSkillOff = INVALID_OFFSET;

        if (!s_fieldsDumped && m_pfunctionInfo) {
            s_fieldsDumped = true;
            LOG(LOG_LEVEL_INFO, "[SkillRange] ── 通过 GetFieldOffset API 查找偏移 ──");

            // 使用框架的 GetFieldOffset（能处理加密元数据）
            s_slotCurrentSkillOff = GetFieldOffset(
                    "Assembly-CSharp.dll", "SkillSlot",
                    "FrameEngine.Logic.SkillSlot", "currentSkill");

            LOG(LOG_LEVEL_INFO,
                "[SkillRange] SkillSlot: currentSkill=0x%X",
                s_slotCurrentSkillOff);

            // 备用: 如果框架 API 也失败, 尝试 il2cpp.h 中的已知偏移
            if (s_slotCurrentSkillOff == INVALID_OFFSET || s_slotCurrentSkillOff == 0) {
                s_slotCurrentSkillOff = 0x38;
                LOG(LOG_LEVEL_INFO, "[SkillRange] currentSkill 回退到硬编码 0x38");
            }
        }

        // ── 方式⑤: SkillSlot[0-4].currentSkill (运行时偏移) ──
        {
            typedef void* (*FnGetSlotByIdx)(void*, int32_t, void*);
            static FnGetSlotByIdx s_getSlotByIdx = nullptr;
            static bool s_triedSlotByIdx = false;
            if (!s_triedSlotByIdx && m_pfunctionInfo) {
                s_triedSlotByIdx = true;
                s_getSlotByIdx = (FnGetSlotByIdx)m_pfunctionInfo->GetMethodFun(
                        "ilbil2cpp.so", "Assembly-CSharp.dll",
                        "ActorComponentSkillMgr", "FrameEngine.Logic.ActorComponentSkillMgr",
                        "GetSkillSlotByIndex");
            }
            if (s_getSlotByIdx && s_slotCurrentSkillOff != INVALID_OFFSET) {
                for (int slotIdx = 0; slotIdx < 4; slotIdx++) {
                    void* pSlotObj = s_getSlotByIdx(pSkillMgr, slotIdx, nullptr);
                    if (!pSlotObj || !IsReadableMemory(pSlotObj, s_slotCurrentSkillOff + 8)) continue;
                    void* pSlotCurSkill = *reinterpret_cast<void**>(
                            reinterpret_cast<uint8_t*>(pSlotObj) + s_slotCurrentSkillOff);
                    if (pSlotCurSkill && IsReadableMemory(pSlotCurSkill, 0x80)) {
                        bool isCast = checkCasting(pSlotCurSkill);
                        LOG(LOG_LEVEL_INFO, "[SkillRange] Slot[%d] curSkill(0x%X)=%p casting=%d",
                            slotIdx, s_slotCurrentSkillOff, pSlotCurSkill, isCast);
                        if (isCast) {
                            pCastingSkill = pSlotCurSkill;
                            LOG(LOG_LEVEL_INFO, "[SkillRange] ✓ 方式⑤ SkillSlot[%d].currentSkill: %p", slotIdx, pSlotCurSkill);
                            break;
                        }
                    }
                }
            }
        }
    }

    // 如果有正在释放的技能，用它替换普攻技能
    if (pCastingSkill) {
        pSkill = pCastingSkill;
        LOG(LOG_LEVEL_INFO, "[SkillRange] 使用施法技能替换普攻");
    } else {
        LOG(LOG_LEVEL_INFO, "[SkillRange] 未找到施法技能，使用普攻");
    }

    if (!pSkill || !IsReadableMemory(pSkill, 0xC0)) return -1.0f;

    // 4. ActorSkill.get_curRange() → Fix64
    typedef int64_t (*FnGetCurRange)(void*, void*);
    static FnGetCurRange s_getCurRange = nullptr;
    if (!s_getCurRange) {
        s_getCurRange = (FnGetCurRange)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "ActorSkill", "FrameEngine.Logic.ActorSkill",
                "get_curRange");
        LOG(LOG_LEVEL_INFO, "[SkillRange] resolved get_curRange=%p", (void*)s_getCurRange);
    }
    if (!s_getCurRange) { LOG(LOG_LEVEL_ERROR, "[SkillRange] get_curRange null"); return -1.0f; }

    LOG(LOG_LEVEL_INFO, "[SkillRange] calling get_curRange(skill=%p)...", pSkill);
    int64_t rangeFix64 = s_getCurRange(pSkill, nullptr);
    LOG(LOG_LEVEL_INFO, "[SkillRange] rangeFix64=0x%llX (%lld)", (unsigned long long)rangeFix64, (long long)rangeFix64);
    if (rangeFix64 <= 0) return -1.0f;

    float NormalAttackSkillRange = DecoderFix64((uint64_t)rangeFix64);
    LOG(LOG_LEVEL_INFO, "[SkillRange] ✓ range=%.4f", NormalAttackSkillRange);


    return NormalAttackSkillRange;
}

// ═══════════════════════════════════════════════════════════════════════════════
// getSkillUILogic — 获取 SkillUILogic UI 控制对象
// ═══════════════════════════════════════════════════════════════════════════════

void* lol::FEVisi::getSkillUILogicInstance() {
    static Il2CppClass* s_joystickUIClass = nullptr;
    if (!s_joystickUIClass && m_pfunctionInfo) {
        s_joystickUIClass = reinterpret_cast<Il2CppClass*>(
                m_pfunctionInfo->FindClassByName(
                    "ilbil2cpp.so", "Assembly-CSharp.dll",
                    "BattleSkillJoystickUILogic", ""));
        LOG(LOG_LEVEL_INFO, "[SkillUI] FindClass BattleSkillJoystickUILogic=%p", s_joystickUIClass);
    }
    if (!s_joystickUIClass) {
        LOG(LOG_LEVEL_WARN, "[SkillUI] joystickUIClass 为空");
        return nullptr;
    }

    auto* pStaticFields = reinterpret_cast<BattleSkillJoystickUILogic_StaticFields*>(
            s_joystickUIClass->static_fields);
    if (!pStaticFields || !IsReadableMemory(pStaticFields, sizeof(BattleSkillJoystickUILogic_StaticFields))) {
        LOG(LOG_LEVEL_WARN, "[SkillUI] static_fields=%p 不可读", s_joystickUIClass->static_fields);
        return nullptr;
    }

    auto* pInstance = pStaticFields->instance;
    if (!pInstance || !IsReadableMemory(pInstance, sizeof(BattleSkillJoystickUILogic_o))) {
        LOG(LOG_LEVEL_WARN, "[SkillUI] instance=%p 不可读", pStaticFields->instance);
        return nullptr;
    }
    return pInstance;
}

void* lol::FEVisi::getSkillUILogic(int skillID) {
    void* pInstance = getSkillUILogicInstance();
    if (!pInstance) return nullptr;

    typedef void* (*FnGetSkillUILogic)(void*, int, void*);
    static FnGetSkillUILogic s_getSkillUILogic = nullptr;
    if (!s_getSkillUILogic && m_pfunctionInfo) {
        s_getSkillUILogic = (FnGetSkillUILogic)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "BattleSkillJoystickUILogic", "",
                "GetSkillUILogic");
    }
    if (!s_getSkillUILogic) return nullptr;
    return s_getSkillUILogic(pInstance, skillID, nullptr);
}

std::vector<void*> lol::FEVisi::getAllSkillUILogics() {
    std::vector<void*> result;
    void* pRawInstance = getSkillUILogicInstance();
    if (!pRawInstance) {
        LOG(LOG_LEVEL_WARN, "[SkillUI] getAllSkillUILogics: instance 为空");
        return result;
    }

    auto* pInstance = reinterpret_cast<BattleSkillJoystickUILogic_o*>(pRawInstance);
    auto* pList = pInstance->fields.skillUILogics;
    if (!pList || !IsReadableMemory(pList, sizeof(Il2CppGenericList))) {
        LOG(LOG_LEVEL_WARN, "[SkillUI] skillUILogics list=%p 不可读", pInstance->fields.skillUILogics);
        return result;
    }

    auto* pItems = reinterpret_cast<Il2CppGenericArrayHeader*>(pList->_items);
    int size = pList->_size;
    LOG(LOG_LEVEL_INFO, "[SkillUI] list: _items=%p _size=%d", pList->_items, size);
    if (!pItems || size <= 0 || size > 20) {
        LOG(LOG_LEVEL_WARN, "[SkillUI] list 无效: _items=%p size=%d", (void*)pItems, size);
        return result;
    }

    auto** elements = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(pItems) + sizeof(Il2CppGenericArrayHeader));
    if (!IsReadableMemory(elements, size * sizeof(void*))) {
        LOG(LOG_LEVEL_WARN, "[SkillUI] elements 内存不可读");
        return result;
    }

    for (int i = 0; i < size; i++) {
        if (elements[i]) result.push_back(elements[i]);
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════════════
// simulateNormalAttack — 模拟普通攻击
// 说明: 先发 ButtonDown，再进入“等待抬起”状态机
//       主循环在满足最小按住时间后补发 ButtonUp，避免同步直调崩溃
// ═══════════════════════════════════════════════════════════════════════════════

enum class NormalAttackButtonState {
    Idle,
    WaitingRelease,
};

static NormalAttackButtonState s_normalAttackButtonState = NormalAttackButtonState::Idle;
static void*    s_pendingPlayerControl = nullptr;
static int32_t  s_pendingSkillID       = 0;
static int32_t  s_pendingOperID        = 0;
static std::chrono::steady_clock::time_point s_pendingButtonDownAt;
static constexpr auto kNormalAttackMinHold = std::chrono::milliseconds(140);

static void clearNormalAttackState() {
    s_normalAttackButtonState = NormalAttackButtonState::Idle;
    s_pendingPlayerControl = nullptr;
    s_pendingSkillID = 0;
    s_pendingOperID = 0;
    s_pendingButtonDownAt = std::chrono::steady_clock::time_point{};
}

void lol::FEVisi::tickPendingAttack() {
    if (s_normalAttackButtonState != NormalAttackButtonState::WaitingRelease) return;

    const auto now = std::chrono::steady_clock::now();
    if (now - s_pendingButtonDownAt < kNormalAttackMinHold) {
        return;
    }

    void* pPlayerControl = s_pendingPlayerControl;
    if (!pPlayerControl || !IsReadableMemory(pPlayerControl, 0xD0)) {
        LOG(LOG_LEVEL_WARN, "[NormalAttack] tickPendingAttack: PlayerControl 已失效，放弃本次 ButtonUp");
        clearNormalAttackState();
        return;
    }

    typedef void (*FnOnTriggerUp)(void*, float, float,
                                  float, float, float,
                                  bool, int32_t, int32_t,
                                  int32_t, int32_t,
                                  bool, bool, void*);
    static FnOnTriggerUp s_onTriggerUp = nullptr;
    if (!s_onTriggerUp && m_pfunctionInfo) {
        s_onTriggerUp = (FnOnTriggerUp)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "PlayerControl", "",
                "OnTriggerSkillButtonUp");
    }
    if (!s_onTriggerUp) {
        LOG(LOG_LEVEL_WARN, "[NormalAttack] ButtonUp 方法解析失败，放弃本次普攻");
        clearNormalAttackState();
        return;
    }

    // 先清状态，再执行 ButtonUp，避免异常返回后重复进入同一个释放分支
    const int32_t skillID = s_pendingSkillID;
    const int32_t operID  = s_pendingOperID;
    clearNormalAttackState();

    LOG(LOG_LEVEL_INFO, "[NormalAttack] >>> ButtonUp (deferred)");
    s_onTriggerUp(pPlayerControl,
                  0.0f, 1.0f,
                  0.0f, 0.0f, 1.0f,
                  true, skillID, operID,
                  0, 0, false, false, nullptr);
    LOG(LOG_LEVEL_INFO, "[NormalAttack] ✓ ButtonUp 完成");
}

bool lol::FEVisi::simulateNormalAttack() {
    if (!m_pfunctionInfo) return false;
    if (s_normalAttackButtonState != NormalAttackButtonState::Idle) {
        LOG(LOG_LEVEL_WARN, "[NormalAttack] 上一次普攻仍在等待抬起，忽略本次点击");
        return false;
    }

    // 1. PlayerControl
    typedef void* (*FnGetPlayerControl)(void*);
    static FnGetPlayerControl s_getPlayerControl = nullptr;
    if (!s_getPlayerControl) {
        s_getPlayerControl = (FnGetPlayerControl)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "FEVisi", "FrameEngine.Visual.FEVisi", "get_playerControl");
    }
    if (!s_getPlayerControl) return false;
    void* pPlayerControl = s_getPlayerControl(nullptr);
    if (!pPlayerControl || !IsReadableMemory(pPlayerControl, 0xD0)) return false;

    // 2. myHero → BattleActor → SkillMgr → NormalAttackSkill
    typedef void* (*FnVoid)(void*);
    static FnVoid s_getMyHero = nullptr, s_getActor = nullptr, s_getSkillMgr = nullptr, s_getNormalSkill = nullptr;
    if (!s_getMyHero) s_getMyHero = (FnVoid)m_pfunctionInfo->GetMethodFun("ilbil2cpp.so","Assembly-CSharp.dll","FEVisi","FrameEngine.Visual.FEVisi","get_myHero");
    if (!s_getActor) s_getActor = (FnVoid)m_pfunctionInfo->GetMethodFun("ilbil2cpp.so","Assembly-CSharp.dll","BattleActorVisi","FrameEngine.Visual.BattleActorVisi","get_actor");
    if (!s_getSkillMgr) s_getSkillMgr = (FnVoid)m_pfunctionInfo->GetMethodFun("ilbil2cpp.so","Assembly-CSharp.dll","BattleActor","FrameEngine.Logic.BattleActor","get_skillMgr");
    if (!s_getNormalSkill) s_getNormalSkill = (FnVoid)m_pfunctionInfo->GetMethodFun("ilbil2cpp.so","Assembly-CSharp.dll","ActorComponentSkillMgr","FrameEngine.Logic.ActorComponentSkillMgr","GetNormalAttackSkill");
    if (!s_getMyHero || !s_getActor || !s_getSkillMgr || !s_getNormalSkill) return false;

    void* hero = s_getMyHero(nullptr);        if (!hero || !IsReadableMemory(hero, 0x160)) return false;
    void* actor = s_getActor(hero);           if (!actor || !IsReadableMemory(actor, 0xB0)) return false;
    void* mgr = s_getSkillMgr(actor);         if (!mgr || !IsReadableMemory(mgr, 0x100)) return false;
    void* skill = s_getNormalSkill(mgr);      if (!skill || !IsReadableMemory(skill, 0xD0)) return false;

    // 3. skillID + operID
    typedef int32_t (*FnInt)(void*, void*);
    static FnInt s_getResId = nullptr, s_getOperResId = nullptr;
    if (!s_getResId) s_getResId = (FnInt)m_pfunctionInfo->GetMethodFun("ilbil2cpp.so","Assembly-CSharp.dll","ActorSkill","FrameEngine.Logic.ActorSkill","get_resId");
    if (!s_getOperResId) s_getOperResId = (FnInt)m_pfunctionInfo->GetMethodFun("ilbil2cpp.so","Assembly-CSharp.dll","ActorSkill","FrameEngine.Logic.ActorSkill","get_iOperModeResId");
    int32_t skillID = s_getResId ? s_getResId(skill, nullptr) : 0;
    int32_t operID  = s_getOperResId ? s_getOperResId(skill, nullptr) : 0;
    LOG(LOG_LEVEL_INFO, "[NormalAttack] skillID=%d operID=%d", skillID, operID);

    // 4. ButtonDown
    typedef void (*FnDown)(void*, bool, int32_t, int32_t, int32_t, int32_t, bool, int32_t, void*);
    static FnDown s_onTriggerDown = nullptr;
    if (!s_onTriggerDown) s_onTriggerDown = (FnDown)m_pfunctionInfo->GetMethodFun("ilbil2cpp.so","Assembly-CSharp.dll","PlayerControl","","OnTriggerSkillButtonDown");
    if (!s_onTriggerDown) return false;

    LOG(LOG_LEVEL_INFO, "[NormalAttack] >>> ButtonDown");
    s_onTriggerDown(pPlayerControl, true, skillID, operID, 0, 0, false, 0, nullptr);

    // 5. 进入等待抬起状态，至少保持一小段按下时间，避免过快抬起导致首次普攻无效
    s_normalAttackButtonState = NormalAttackButtonState::WaitingRelease;
    s_pendingPlayerControl = pPlayerControl;
    s_pendingSkillID       = skillID;
    s_pendingOperID        = operID;
    s_pendingButtonDownAt  = std::chrono::steady_clock::now();
    LOG(LOG_LEVEL_INFO, "[NormalAttack] ButtonDown 完成，ButtonUp 将在满足最小按住时间后执行");
    return true;
}

// ═══════════════════════════════════════════════════════════════════════════════
// simulateMovement — 模拟英雄移动
//
// 当前版本仅保留 PlayerControl.OnTriggerMoveButtonDown/Aim/Up 三态输入链路。
// 必须每 tick 持续调用以维持移动方向；传入 (0,0) 停止移动。
// ═══════════════════════════════════════════════════════════════════════════════

bool lol::FEVisi::simulateMovement(float dirX, float dirY) {
    if (!m_pfunctionInfo) return false;

    struct FixVector2 { int64_t x; int64_t y; };
    enum TriggerArgKind {
        kTriggerArgUnknown = 0,
        kTriggerArgBool,
        kTriggerArgFloat,
        kTriggerArgDouble,
        kTriggerArgInt32,
        kTriggerArgInt64,
        kTriggerArgFix64,
        kTriggerArgFixVector2,
        kTriggerArgUnityVector3,
    };
    struct TriggerMethodBinding {
        const ::MethodInfo* method = nullptr;
        std::array<TriggerArgKind, 3> kinds = {
            kTriggerArgUnknown, kTriggerArgUnknown, kTriggerArgUnknown
        };
        std::array<std::string, 3> typeNames;
        std::array<std::string, 3> paramNames;
        bool supported = false;
    };
    enum TriggerStage {
        kTriggerStageDown = 0,
        kTriggerStageAim,
        kTriggerStageUp,
    };

    typedef void* (*FnVoid)(void*);

    static FnVoid s_getPlayerControl = nullptr;

    static TriggerMethodBinding s_triggerMoveDown;
    static TriggerMethodBinding s_triggerMoveAim;
    static TriggerMethodBinding s_triggerMoveUp;
    static bool  s_triggerMoveHeld = false;
    static float s_lastActiveDirX = 0.0f;
    static float s_lastActiveDirY = 0.0f;

    static float s_divideOfOne = 0.0f;
    static bool  s_resolved    = false;

    auto normalizeTypeName = [](const char* rawName) -> std::string {
        std::string typeName = rawName ? rawName : "";
        for (char& ch : typeName) {
            if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        }
        return typeName;
    };
    auto containsToken = [](const std::string& text, const char* token) -> bool {
        return !text.empty() && token && text.find(token) != std::string::npos;
    };
    auto classifyTriggerArg = [&](const char* rawTypeName) -> TriggerArgKind {
        std::string typeName = normalizeTypeName(rawTypeName);
        if (typeName.empty()) return kTriggerArgUnknown;
        if (containsToken(typeName, "boolean") || typeName == "bool") return kTriggerArgBool;
        if (containsToken(typeName, "single") || typeName == "float") return kTriggerArgFloat;
        if (containsToken(typeName, "double")) return kTriggerArgDouble;
        if (containsToken(typeName, "vector3")) return kTriggerArgUnityVector3;
        if (containsToken(typeName, "fixvector2")) return kTriggerArgFixVector2;
        if (containsToken(typeName, "fix64")) return kTriggerArgFix64;
        if (containsToken(typeName, "int32") || containsToken(typeName, "uint32")) return kTriggerArgInt32;
        if (containsToken(typeName, "int64") || containsToken(typeName, "uint64")) return kTriggerArgInt64;
        return kTriggerArgUnknown;
    };
    auto resolveTriggerBinding = [&](const ::MethodInfo* method) -> TriggerMethodBinding {
        TriggerMethodBinding binding;
        binding.method = method;
        if (!method) return binding;

        uint32_t paramCount = m_pfunctionInfo->il2cpp_method_get_param_count(method);
        if (paramCount != 3) return binding;

        bool supported = true;
        for (uint32_t index = 0; index < 3; ++index) {
            const ::Il2CppType* paramType = m_pfunctionInfo->il2cpp_method_get_param(method, index);
            const char* rawTypeName = paramType ? m_pfunctionInfo->il2cpp_type_get_name(paramType) : nullptr;
            const char* rawParamName = m_pfunctionInfo->il2cpp_method_get_param_name(method, index);
            binding.typeNames[index] = rawTypeName ? rawTypeName : "<null>";
            binding.paramNames[index] = rawParamName ? rawParamName : "";
            binding.kinds[index] = classifyTriggerArg(rawTypeName);
            if (binding.kinds[index] == kTriggerArgUnknown) supported = false;
        }
        binding.supported = supported;
        return binding;
    };
    auto logTriggerBinding = [&](const char* label, const TriggerMethodBinding& binding) {
        if (!binding.method) return;
        LOG(LOG_LEVEL_INFO,
            "[Movement] %s signature: (%s %s, %s %s, %s %s) supported=%d ptr=%p",
            label,
            binding.typeNames[0].c_str(), binding.paramNames[0].empty() ? "p0" : binding.paramNames[0].c_str(),
            binding.typeNames[1].c_str(), binding.paramNames[1].empty() ? "p1" : binding.paramNames[1].c_str(),
            binding.typeNames[2].c_str(), binding.paramNames[2].empty() ? "p2" : binding.paramNames[2].c_str(),
            binding.supported ? 1 : 0,
            (void*)binding.method->methodPointer);
    };
    auto invokeTriggerMethod = [&](void* playerControl,
                                   const TriggerMethodBinding& binding,
                                   TriggerStage stage,
                                   float invokeDirX,
                                   float invokeDirY,
                                   const FixVector2& invokeFixDir) -> bool {
        if (!playerControl || !binding.method) return false;

        const float magnitude = std::sqrt(invokeDirX * invokeDirX + invokeDirY * invokeDirY);
        const bool isPressed = (stage != kTriggerStageUp);
        const bool preferStoredDirection = (stage == kTriggerStageUp && s_triggerMoveHeld);
        const float effectiveDirX = preferStoredDirection ? s_lastActiveDirX : invokeDirX;
        const float effectiveDirY = preferStoredDirection ? s_lastActiveDirY : invokeDirY;
        const float clampedMagnitude = magnitude > 1.0f ? 1.0f : magnitude;
        const float moveStrength = isPressed ? clampedMagnitude : 0.0f;
        const float moveSpeed = isPressed ? clampedMagnitude : 0.0f;
        FixVector2 effectiveFixDir = invokeFixDir;
        if (preferStoredDirection) {
            effectiveFixDir.x = (int64_t)((double)effectiveDirX / (double)s_divideOfOne);
            effectiveFixDir.y = (int64_t)((double)effectiveDirY / (double)s_divideOfOne);
        }
        UnityVector3 forwardVector{0.0f, 0.0f, 0.0f};
        if (magnitude > 0.0001f) {
            const float invMagnitude = 1.0f / magnitude;
            forwardVector.x = effectiveDirX * invMagnitude;
            forwardVector.z = effectiveDirY * invMagnitude;
        }

        int scalarFloatCount = 0;
        int scalarFix64Count = 0;
        for (TriggerArgKind kind : binding.kinds) {
            if (kind == kTriggerArgFloat || kind == kTriggerArgDouble) ++scalarFloatCount;
            if (kind == kTriggerArgFix64 || kind == kTriggerArgInt64) ++scalarFix64Count;
        }

        bool boolStorage[3] = {};
        float floatStorage[3] = {};
        double doubleStorage[3] = {};
        int32_t int32Storage[3] = {};
        int64_t int64Storage[3] = {};
        FixVector2 vectorStorage[3] = {};
        UnityVector3 unityVector3Storage[3] = {};
        void* invokeParams[3] = {};

        int floatOrdinal = 0;
        int fix64Ordinal = 0;
        for (int index = 0; index < 3; ++index) {
            std::string paramName = normalizeTypeName(binding.paramNames[index].c_str());
            switch (binding.kinds[index]) {
                case kTriggerArgBool:
                    boolStorage[index] = isPressed;
                    invokeParams[index] = &boolStorage[index];
                    break;
                case kTriggerArgFloat: {
                    float value = 0.0f;
                    if (containsToken(paramName, "movespeed") || containsToken(paramName, "speed")) {
                        value = moveSpeed;
                    } else if (containsToken(paramName, "movestrength") || containsToken(paramName, "strength")) {
                        value = moveStrength;
                    } else if (containsToken(paramName, "y") || containsToken(paramName, "vertical")) {
                        value = effectiveDirY;
                    } else if (containsToken(paramName, "magnitude") || containsToken(paramName, "length") || containsToken(paramName, "radius")) {
                        value = magnitude;
                    } else if (scalarFloatCount >= 2) {
                        value = (floatOrdinal == 0) ? effectiveDirX : effectiveDirY;
                        ++floatOrdinal;
                    } else {
                        value = effectiveDirX;
                    }
                    floatStorage[index] = value;
                    invokeParams[index] = &floatStorage[index];
                    break;
                }
                case kTriggerArgDouble: {
                    double value = 0.0;
                    if (containsToken(paramName, "movespeed") || containsToken(paramName, "speed")) {
                        value = (double)moveSpeed;
                    } else if (containsToken(paramName, "movestrength") || containsToken(paramName, "strength")) {
                        value = (double)moveStrength;
                    } else if (containsToken(paramName, "y") || containsToken(paramName, "vertical")) {
                        value = (double)effectiveDirY;
                    } else if (containsToken(paramName, "magnitude") || containsToken(paramName, "length") || containsToken(paramName, "radius")) {
                        value = (double)magnitude;
                    } else if (scalarFloatCount >= 2) {
                        value = (floatOrdinal == 0) ? (double)effectiveDirX : (double)effectiveDirY;
                        ++floatOrdinal;
                    } else {
                        value = (double)effectiveDirX;
                    }
                    doubleStorage[index] = value;
                    invokeParams[index] = &doubleStorage[index];
                    break;
                }
                case kTriggerArgInt32:
                    int32Storage[index] = 0;
                    invokeParams[index] = &int32Storage[index];
                    break;
                case kTriggerArgInt64:
                case kTriggerArgFix64: {
                    int64_t value = 0;
                    if (containsToken(paramName, "y") || containsToken(paramName, "vertical")) {
                        value = effectiveFixDir.y;
                    } else if (containsToken(paramName, "magnitude") || containsToken(paramName, "length") || containsToken(paramName, "radius")) {
                        value = (int64_t)((double)magnitude / (double)s_divideOfOne);
                    } else if (scalarFix64Count >= 2) {
                        value = (fix64Ordinal == 0) ? effectiveFixDir.x : effectiveFixDir.y;
                        ++fix64Ordinal;
                    } else {
                        value = effectiveFixDir.x;
                    }
                    int64Storage[index] = value;
                    invokeParams[index] = &int64Storage[index];
                    break;
                }
                case kTriggerArgFixVector2:
                    vectorStorage[index] = effectiveFixDir;
                    invokeParams[index] = &vectorStorage[index];
                    break;
                case kTriggerArgUnityVector3:
                    unityVector3Storage[index] = forwardVector;
                    invokeParams[index] = &unityVector3Storage[index];
                    break;
                default:
                    return false;
            }
        }

        ::Il2CppException* invokeException = nullptr;
        m_pfunctionInfo->il2cpp_runtime_invoke(binding.method, playerControl, invokeParams, &invokeException);
        if (invokeException) {
            LOG(LOG_LEVEL_ERROR, "[Movement] runtime_invoke %s 异常: exc=%p method=%p",
                stage == kTriggerStageDown ? "Down" : (stage == kTriggerStageAim ? "Aim" : "Up"),
                invokeException,
                (void*)binding.method->methodPointer);
            return false;
        }
        return true;
    };

    if (!s_resolved) {
        s_resolved = true;
        s_getPlayerControl = (FnVoid)m_pfunctionInfo->GetMethodFun(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "FEVisi", "FrameEngine.Visual.FEVisi", "get_playerControl");

        {
            auto* pcKlass = m_pfunctionInfo->FindClassByName(
                    "ilbil2cpp.so", "Assembly-CSharp.dll", "PlayerControl", "");
            if (pcKlass) {
                m_pfunctionInfo->il2cpp_class_init_all_method(pcKlass);
                void* iter = nullptr;
                while (auto* method = m_pfunctionInfo->il2cpp_class_get_methods(pcKlass, &iter)) {
                    const char* methodName = m_pfunctionInfo->il2cpp_method_get_name(method);
                    if (!methodName || m_pfunctionInfo->il2cpp_method_get_param_count(method) != 3) continue;

                    if (strcmp(methodName, "OnTriggerMoveButtonDown") == 0) {
                        s_triggerMoveDown = resolveTriggerBinding(method);
                    } else if (strcmp(methodName, "OnTriggerMoveButtonAim") == 0) {
                        s_triggerMoveAim = resolveTriggerBinding(method);
                    } else if (strcmp(methodName, "OnTriggerMoveButtonUp") == 0) {
                        s_triggerMoveUp = resolveTriggerBinding(method);
                    }
                }

                logTriggerBinding("OnTriggerMoveButtonDown", s_triggerMoveDown);
                logTriggerBinding("OnTriggerMoveButtonAim", s_triggerMoveAim);
                logTriggerBinding("OnTriggerMoveButtonUp", s_triggerMoveUp);

                if (s_triggerMoveDown.method && s_triggerMoveAim.method && s_triggerMoveUp.method &&
                    s_triggerMoveDown.supported && s_triggerMoveAim.supported && s_triggerMoveUp.supported) {
                    LOG(LOG_LEVEL_INFO, "[Movement] ✓ 方式A (trigger state machine): Down=%p Aim=%p Up=%p",
                        (void*)s_triggerMoveDown.method->methodPointer,
                        (void*)s_triggerMoveAim.method->methodPointer,
                        (void*)s_triggerMoveUp.method->methodPointer);
                } else {
                    LOG(LOG_LEVEL_ERROR, "[Movement] trigger move methods not fully resolved");
                }
            }
        }
        uint64_t rawBits = m_pfunctionInfo->GetStaticMember(
                "ilbil2cpp.so", "Assembly-CSharp.dll",
                "Fix64", "FrameEngine.Common.Fix64", "divideOfOne");
        memcpy(&s_divideOfOne, &rawBits, sizeof(float));

        LOG(LOG_LEVEL_INFO,
            "[Movement] resolved: triggerDown=%p triggerAim=%p triggerUp=%p getPlayerControl=%p divideOfOne=%e",
            s_triggerMoveDown.method ? (void*)s_triggerMoveDown.method->methodPointer : nullptr,
            s_triggerMoveAim.method ? (void*)s_triggerMoveAim.method->methodPointer : nullptr,
            s_triggerMoveUp.method ? (void*)s_triggerMoveUp.method->methodPointer : nullptr,
            (void*)s_getPlayerControl,
            s_divideOfOne);
    }

    if (!s_getPlayerControl) {
        LOG(LOG_LEVEL_ERROR, "[Movement] get_playerControl 未解析");
        return false;
    }
    if (!s_triggerMoveDown.method || !s_triggerMoveAim.method || !s_triggerMoveUp.method ||
        !s_triggerMoveDown.supported || !s_triggerMoveAim.supported || !s_triggerMoveUp.supported) {
        LOG(LOG_LEVEL_ERROR, "[Movement] trigger move methods unavailable");
        return false;
    }
    if (s_divideOfOne == 0.0f) {
        LOG(LOG_LEVEL_ERROR, "[Movement] Fix64.divideOfOne 为 0");
        return false;
    }

    // ─── 编码方向为 FixVector2 ───
    FixVector2 fixDir;
    fixDir.x = (int64_t)((double)dirX / (double)s_divideOfOne);
    fixDir.y = (int64_t)((double)dirY / (double)s_divideOfOne);
    const bool hasDirection = (std::fabs(dirX) > 0.0001f || std::fabs(dirY) > 0.0001f);

    void* pPlayerControl = s_getPlayerControl(nullptr);
    if (!pPlayerControl || !IsReadableMemory(pPlayerControl, 0xD0)) return false;

    if (!hasDirection) {
        if (!s_triggerMoveHeld) return true;
        if (!invokeTriggerMethod(pPlayerControl, s_triggerMoveUp, kTriggerStageUp, dirX, dirY, fixDir)) {
            LOG(LOG_LEVEL_WARN, "[Movement] 方式A(trigger) ButtonUp 调用失败");
            return false;
        }
        s_triggerMoveHeld = false;
        s_lastActiveDirX = 0.0f;
        s_lastActiveDirY = 0.0f;
        LOG(LOG_LEVEL_INFO, "[Movement] ✓ 方式A(trigger) Up dir=(%.3f,%.3f)", dirX, dirY);
        return true;
    }

    if (!s_triggerMoveHeld) {
        if (!invokeTriggerMethod(pPlayerControl, s_triggerMoveDown, kTriggerStageDown, dirX, dirY, fixDir)) {
            LOG(LOG_LEVEL_WARN, "[Movement] 方式A(trigger) ButtonDown 调用失败");
            return false;
        }
        s_triggerMoveHeld = true;
    }
    if (!invokeTriggerMethod(pPlayerControl, s_triggerMoveAim, kTriggerStageAim, dirX, dirY, fixDir)) {
        LOG(LOG_LEVEL_WARN, "[Movement] 方式A(trigger) ButtonAim 调用失败");
        return false;
    }

    s_lastActiveDirX = dirX;
    s_lastActiveDirY = dirY;
    LOG(LOG_LEVEL_INFO, "[Movement] ✓ 方式A(trigger) Aim dir=(%.3f,%.3f) fix=(0x%llX,0x%llX)",
        dirX, dirY, (unsigned long long)fixDir.x, (unsigned long long)fixDir.y);

    return true;
}

bool lol::FEVisi::simulateMoveUp() {
    return simulateMovement(0.0f, 1.0f);
}

bool lol::FEVisi::simulateMoveDown() {
    return simulateMovement(0.0f, -1.0f);
}

bool lol::FEVisi::simulateMoveLeft() {
    return simulateMovement(-1.0f, 0.0f);
}

bool lol::FEVisi::simulateMoveRight() {
    return simulateMovement(1.0f, 0.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// checkWallCollision — 地形与英雄碰撞的实际流程
//
// 这里不是走 Unity 物理碰撞器，也不是直接看英雄 capsule / box 是否和墙体 mesh 相交，
// 而是走游戏自己的导航网格(navmesh)判定。
//
// 结合 dumpcs.cs 与 IDA 的结论，实际链路如下：
// 1. 地图资源侧持有 MapEditor.FixBuilderAsset。
// 2. FixBuilderAsset.navmeshBuilderData 数组里内嵌了 FixNavmeshBuilderData 结构。
// 3. 每个 FixNavmeshBuilderData 里都带有 FrameEngine.Interface.FixNavmeshRawData。
// 4. 运行时创建 MapEditor.FixNavMeshEd，并调用 InitNavmeshEd(rawData) 把原始导航数据加载成可查询对象。
// 5. 将英雄当前位置 sourcePosition 和“朝目标方向前进一步”的 targetPosition 从 float 世界坐标编码成 Fix64/FixVector3。
// 6. 调用 FixNavMeshEd.Raycast(source, target, navFlag, out hit)。
// 7. 如果这条线段穿过不可行走节点、营地限制节点、墙体边界，Raycast 会返回命中，hit.hit 也会被置位。
// 8. 调用方再把“命中墙/不可走区域”解释成“该方向存在障碍物”，用于 canMoveXxx / 四方向扫描。
//
// 换句话说：
// 英雄与地形的“碰撞”本质上是一次导航网格可通行性测试，
// 不是传统物理引擎刚体碰撞；只要从当前位置到目标点的 navmesh 射线被阻断，
// 我们就认为英雄这一小步会撞墙。
// ═══════════════════════════════════════════════════════════════════════════════
bool lol::FEVisi::checkWallCollision(const UnityVector3& pos, float worldDirX, float worldDirZ, float stepSize, bool* outDetectorReady) {
    if (outDetectorReady) {
        *outDetectorReady = false;
    }
    LOG(LOG_LEVEL_INFO,
        "[WallCollision][Step1] enter pos=(%.3f,%.3f,%.3f) dir=(%.3f,%.3f) stepSize=%.3f",
        pos.x, pos.y, pos.z, worldDirX, worldDirZ, stepSize);
    if (!m_pfunctionInfo) {
        LOG(LOG_LEVEL_WARN, "[WallCollision][Step1] m_pfunctionInfo 为空，无法继续");
        return false;
    }
    if (stepSize <= 0.0f) {
        LOG(LOG_LEVEL_WARN, "[WallCollision][Step1] stepSize 非法: %.3f", stepSize);
        return false;
    }

    const float dirLen = std::sqrt(worldDirX * worldDirX + worldDirZ * worldDirZ);
    LOG(LOG_LEVEL_INFO, "[WallCollision][Step2] direction length=%.6f", dirLen);
    if (dirLen <= 0.0001f) {
        LOG(LOG_LEVEL_WARN, "[WallCollision][Step2] 方向长度过小，跳过碰撞检测");
        return false;
    }

    struct FixNavHitNative {
        bool hit;
        uint8_t pad0[7];
        FrameEngine_Common_FixVector3_Fix64_Fields position;
        FrameEngine_Common_FixVector3_Fix64_Fields normal;
        bool hitCampNode;
        uint8_t pad1[7];
    };
    static_assert(sizeof(FrameEngine_Common_FixVector3_Fix64_Fields) == 0x18,
                  "FixVector3_Fix64 layout mismatch");
    static_assert(sizeof(FixNavHitNative) == 0x40,
                  "FixNavHit layout mismatch");

    using FnObjectNew = ::Il2CppObject* (*)(::Il2CppClass* klass);
    using FnCtor = void (*)(void* self);
    using FnFindObjectsOfTypeAll = ::Il2CppArray* (*)(::Il2CppReflectionType* type);
    using FnInitNavmeshEd = void (*)(void* self, void* navmeshRawData);
    using FnRaycast = bool (*)(void* self,
                               const FrameEngine_Common_FixVector3_Fix64_Fields* sourcePosition,
                               const FrameEngine_Common_FixVector3_Fix64_Fields* targetPosition,
                               int32_t navFlag,
                               FixNavHitNative* hit);

    static void* s_navMeshEdInstance = nullptr;
    static ::Il2CppClass* s_builderAssetClass = nullptr;
    static ::Il2CppClass* s_navMeshEdClass = nullptr;
    static FnObjectNew s_objectNew = nullptr;
    static FnCtor s_ctor = nullptr;
    static FnFindObjectsOfTypeAll s_findObjectsOfTypeAll = nullptr;
    static FnInitNavmeshEd s_initNavmeshEd = nullptr;
    static FnRaycast s_raycast = nullptr;
    static uint32_t s_navmeshBuilderDataOffset = INVALID_OFFSET;
    static float s_divideOfOne = 0.0f;
    static int32_t s_groundNavFlag = -1;
    static std::chrono::steady_clock::time_point s_lastInitAttempt{};

    constexpr uint32_t kFixBuilderAssetNavmeshBuilderDataOffset = 0xA0;
    constexpr size_t kFixNavmeshBuilderDataElementSize = 0x30;
    constexpr size_t kFixNavmeshBuilderDataRawDataOffset = 0x10;

    auto tryInitNavmesh = [&]() -> bool {
        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step3] tryInitNavmesh begin instance=%p raycast=%p divideOfOne=%f groundFlag=%d",
            s_navMeshEdInstance,
            (void*)s_raycast,
            s_divideOfOne,
            s_groundNavFlag);
        if (s_navMeshEdInstance && s_raycast && s_divideOfOne != 0.0f) {
            LOG(LOG_LEVEL_INFO, "[WallCollision][Step3] 复用已初始化的导航上下文");
            return true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (s_lastInitAttempt.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastInitAttempt).count() < 1000) {
            LOG(LOG_LEVEL_WARN, "[WallCollision][Step3] 距离上次初始化不足 1 秒，本次跳过");
            return false;
        }
        s_lastInitAttempt = now;

        if (s_divideOfOne == 0.0f) {
            const uint64_t rawBits = m_pfunctionInfo->GetStaticMember(
                    "ilbil2cpp.so", "Assembly-CSharp.dll",
                    "Fix64", "FrameEngine.Common.Fix64",
                    "divideOfOne");
            memcpy(&s_divideOfOne, &rawBits, sizeof(float));
            LOG(LOG_LEVEL_INFO,
                "[WallCollision][Step4] Fix64.divideOfOne rawBits=0x%llX value=%f",
                (unsigned long long)rawBits,
                s_divideOfOne);
            if (s_divideOfOne == 0.0f) {
                LOG(LOG_LEVEL_WARN, "[WallCollision] Fix64.divideOfOne 解析失败");
                return false;
            }
        }

        if (s_groundNavFlag < 0) {
            const uint64_t rawGroundFlag = m_pfunctionInfo->GetStaticMember(
                    "ilbil2cpp.so", "Assembly-CSharp.dll",
                    "FixNavMesh", "FrameEngine.Common.FixNavMesh",
                    "GroundFlag");
            if (rawGroundFlag != static_cast<uint64_t>(-1)) {
                s_groundNavFlag = static_cast<int32_t>(rawGroundFlag & 0xFFu);
            }
            LOG(LOG_LEVEL_INFO,
                "[WallCollision][Step5] GroundFlag raw=0x%llX parsed=%d",
                (unsigned long long)rawGroundFlag,
                s_groundNavFlag);
        }

        if (!s_objectNew) {
            LOG(LOG_LEVEL_INFO, "[WallCollision][Step6] begin resolve il2cpp_object_new via m_pfunctionInfo");
            s_objectNew = reinterpret_cast<FnObjectNew>(
                    m_pfunctionInfo->il2cpp_api_lookup_symbol("il2cpp_object_new"));

            if (!s_objectNew) {
                LOG(LOG_LEVEL_WARN, "[WallCollision] il2cpp_object_new 未找到");
                return false;
            }
            LOG(LOG_LEVEL_INFO, "[WallCollision][Step6] il2cpp_object_new=%p", (void*)s_objectNew);
        }

        if (!s_builderAssetClass || s_navmeshBuilderDataOffset == INVALID_OFFSET) {
            uint32_t resolvedOffset = INVALID_OFFSET;
            ::FieldInfo* navmeshBuilderDataField = nullptr;
            bool navmeshBuilderDataIsStatic = false;
            if (m_pfunctionInfo->GetMember(
                    "ilbil2cpp.so", "Assembly-CSharp.dll",
                    "FixBuilderAsset", "MapEditor.FixBuilderAsset",
                    "navmeshBuilderData",
                    &resolvedOffset,
                    &navmeshBuilderDataField,
                    &navmeshBuilderDataIsStatic)) {
                if (resolvedOffset != INVALID_OFFSET) {
                    s_navmeshBuilderDataOffset = resolvedOffset;
                }
                if (navmeshBuilderDataField) {
                    s_builderAssetClass = m_pfunctionInfo->il2cpp_field_get_parent(navmeshBuilderDataField);
                }
            }

            if (!s_builderAssetClass) {
                s_builderAssetClass = reinterpret_cast<::Il2CppClass*>(m_pfunctionInfo->GetMethodOwnerClass(
                        "ilbil2cpp.so", "Assembly-CSharp.dll",
                        "FixBuilderAsset", "MapEditor.FixBuilderAsset"));
            }
            if (s_navmeshBuilderDataOffset == INVALID_OFFSET) {
                s_navmeshBuilderDataOffset = kFixBuilderAssetNavmeshBuilderDataOffset;
            }
        }

        if (!s_navMeshEdClass) {
            s_navMeshEdClass = reinterpret_cast<::Il2CppClass*>(m_pfunctionInfo->GetMethodOwnerClass(
                    "ilbil2cpp.so", "Assembly-CSharp.dll",
                    "FixNavMeshEd", "MapEditor.FixNavMeshEd",
                    ".ctor", "", 0));
        }

        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step7] owner class builder=%p navMeshEd=%p",
            s_builderAssetClass,
            s_navMeshEdClass);
        if (!s_builderAssetClass || !s_navMeshEdClass) {
            LOG(LOG_LEVEL_WARN, "[WallCollision] 导航相关类未就绪 builder=%p navMeshEd=%p",
                s_builderAssetClass, s_navMeshEdClass);
            return false;
        }
        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step8] navmeshBuilderData offset=0x%X",
            s_navmeshBuilderDataOffset);

        if (!s_findObjectsOfTypeAll) {
            s_findObjectsOfTypeAll = reinterpret_cast<FnFindObjectsOfTypeAll>(
                    m_pfunctionInfo->GetMethodFun(
                            "libil2cpp.so", "UnityEngine.CoreModule.dll",
                            "Resources", "UnityEngine.Resources",
                            "FindObjectsOfTypeAll", "", 1));
        }
        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step9] Resources.FindObjectsOfTypeAll=%p",
            (void*)s_findObjectsOfTypeAll);
        if (!s_findObjectsOfTypeAll) {
            LOG(LOG_LEVEL_WARN, "[WallCollision] Resources.FindObjectsOfTypeAll 未找到");
            return false;
        }

        ::Il2CppReflectionType* builderAssetType = m_pfunctionInfo->il2cpp_type_get_object(
            m_pfunctionInfo->il2cpp_class_get_type(s_builderAssetClass));
        LOG(LOG_LEVEL_INFO, "[WallCollision][Step10] FixBuilderAsset System.Type=%p", builderAssetType);
        if (!builderAssetType) {
            LOG(LOG_LEVEL_WARN, "[WallCollision] FixBuilderAsset System.Type 获取失败");
            return false;
        }

        auto* builderAssets = s_findObjectsOfTypeAll(builderAssetType);
        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step11] FindObjectsOfTypeAll result assets=%p length=%zu",
            builderAssets,
            builderAssets ? builderAssets->max_length : 0);
        if (!builderAssets || builderAssets->max_length == 0) {
            LOG(LOG_LEVEL_WARN, "[WallCollision] FixBuilderAsset 实例未找到 assets=%p",
                builderAssets);
            return false;
        }

        void* navmeshRawData = nullptr;
        auto* assetArray = reinterpret_cast<::Il2CppArraySize*>(builderAssets);
        for (size_t assetIndex = 0; assetIndex < assetArray->max_length && !navmeshRawData; ++assetIndex) {
            void* builderAsset = assetArray->vector[assetIndex];
            LOG(LOG_LEVEL_INFO,
                "[WallCollision][Step12] scan FixBuilderAsset[%zu]=%p",
                assetIndex,
                builderAsset);
            if (!builderAsset || !IsReadableMemory(builderAsset, s_navmeshBuilderDataOffset + sizeof(void*))) continue;

            auto* builderDataArray = reinterpret_cast<::Il2CppArray*>(ReadMemberPtr(builderAsset, s_navmeshBuilderDataOffset));
            LOG(LOG_LEVEL_INFO,
                "[WallCollision][Step13] builderDataArray=%p length=%zu",
                builderDataArray,
                builderDataArray ? builderDataArray->max_length : 0);
            if (!builderDataArray || !IsReadableMemory(builderDataArray, sizeof(::Il2CppArray))) continue;
            if (builderDataArray->max_length == 0) continue;

            auto* valueArray = reinterpret_cast<::Il2CppArraySize*>(builderDataArray);
            auto* rawBytes = reinterpret_cast<const uint8_t*>(valueArray->vector);
            const size_t totalBytes = static_cast<size_t>(builderDataArray->max_length) * kFixNavmeshBuilderDataElementSize;
            if (!rawBytes || !IsReadableMemory(rawBytes, totalBytes)) continue;

            for (size_t index = 0; index < builderDataArray->max_length; ++index) {
                const uint8_t* builderData = rawBytes + (index * kFixNavmeshBuilderDataElementSize);
                void* candidateRawData = *reinterpret_cast<void* const*>(builderData + kFixNavmeshBuilderDataRawDataOffset);
                LOG(LOG_LEVEL_INFO,
                    "[WallCollision][Step14] builderData[%zu] rawDataCandidate=%p",
                    index,
                    candidateRawData);
                if (candidateRawData && IsReadableMemory(candidateRawData, sizeof(void*))) {
                    navmeshRawData = candidateRawData;
                    LOG(LOG_LEVEL_INFO,
                        "[WallCollision][Step14] 选中 navmeshRawData=%p assetIndex=%zu builderDataIndex=%zu",
                        navmeshRawData,
                        assetIndex,
                        index);
                    break;
                }
            }
        }

        if (!navmeshRawData) {
            LOG(LOG_LEVEL_WARN, "[WallCollision] navmeshRawData 尚未就绪");
            return false;
        }

        s_ctor = reinterpret_cast<FnCtor>(m_pfunctionInfo->GetMethodFun(
            "ilbil2cpp.so", "Assembly-CSharp.dll",
            "FixNavMeshEd", "MapEditor.FixNavMeshEd",
            ".ctor", "", 0));
        s_initNavmeshEd = reinterpret_cast<FnInitNavmeshEd>(m_pfunctionInfo->GetMethodFun(
            "ilbil2cpp.so", "Assembly-CSharp.dll",
            "FixNavMeshEd", "MapEditor.FixNavMeshEd",
            "InitNavmeshEd", "", 1));
        s_raycast = reinterpret_cast<FnRaycast>(m_pfunctionInfo->GetMethodFun(
            "ilbil2cpp.so", "Assembly-CSharp.dll",
            "FixNavMeshEd", "MapEditor.FixNavMeshEd",
            "Raycast", "", 4));
        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step15] FixNavMeshEd methods ctor=%p init=%p raycast=%p",
            (void*)s_ctor,
            (void*)s_initNavmeshEd,
            (void*)s_raycast);
        if (!s_ctor || !s_initNavmeshEd || !s_raycast) {
            LOG(LOG_LEVEL_WARN, "[WallCollision] FixNavMeshEd 方法解析失败 ctor=%p init=%p raycast=%p",
                (void*)s_ctor, (void*)s_initNavmeshEd, (void*)s_raycast);
            return false;
        }

        s_navMeshEdInstance = s_objectNew(s_navMeshEdClass);
        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step16] FixNavMeshEd instance=%p",
            s_navMeshEdInstance);
        if (!s_navMeshEdInstance) {
            LOG(LOG_LEVEL_WARN, "[WallCollision] FixNavMeshEd 实例创建失败");
            return false;
        }

        s_ctor(s_navMeshEdInstance);
        LOG(LOG_LEVEL_INFO, "[WallCollision][Step17] FixNavMeshEd .ctor 调用完成");
        s_initNavmeshEd(s_navMeshEdInstance, navmeshRawData);
        LOG(LOG_LEVEL_INFO,
            "[WallCollision][Step18] InitNavmeshEd 完成 rawData=%p instance=%p",
            navmeshRawData,
            s_navMeshEdInstance);

        LOG(LOG_LEVEL_INFO,
            "[WallCollision] 导航网格初始化成功 builderArrayOff=0x%X rawData=%p groundFlag=%d raycast=%p",
            s_navmeshBuilderDataOffset,
            navmeshRawData,
            s_groundNavFlag,
            (void*)s_raycast);
        return true;
    };

    if (!tryInitNavmesh() || !s_navMeshEdInstance || !s_raycast || s_divideOfOne == 0.0f) {
        LOG(LOG_LEVEL_WARN,
            "[WallCollision][Step19] 导航初始化未完成 instance=%p raycast=%p divideOfOne=%f",
            s_navMeshEdInstance,
            (void*)s_raycast,
            s_divideOfOne);
        return false;
    }
    if (outDetectorReady) {
        *outDetectorReady = true;
    }

    const float normalizedDirX = worldDirX / dirLen;
    const float normalizedDirZ = worldDirZ / dirLen;
    const UnityVector3 targetPos{
            pos.x + normalizedDirX * stepSize,
            pos.y,
            pos.z + normalizedDirZ * stepSize
    };
    LOG(LOG_LEVEL_INFO,
        "[WallCollision][Step20] normalizedDir=(%.6f,%.6f) targetPos=(%.3f,%.3f,%.3f)",
        normalizedDirX,
        normalizedDirZ,
        targetPos.x,
        targetPos.y,
        targetPos.z);

    auto toFix64 = [&](float value) -> int64_t {
        return static_cast<int64_t>((double)value / (double)s_divideOfOne);
    };
    auto makeFixVector3 = [&](const UnityVector3& worldPos) -> FrameEngine_Common_FixVector3_Fix64_Fields {
        FrameEngine_Common_FixVector3_Fix64_Fields fixPos{};
        fixPos.x.rawValue = toFix64(worldPos.x);
        fixPos.y.rawValue = toFix64(worldPos.y);
        fixPos.z.rawValue = toFix64(worldPos.z);
        return fixPos;
    };

    const FrameEngine_Common_FixVector3_Fix64_Fields sourceFix = makeFixVector3(pos);
    const FrameEngine_Common_FixVector3_Fix64_Fields targetFix = makeFixVector3(targetPos);
    LOG(LOG_LEVEL_INFO,
        "[WallCollision][Step21] sourceFix=(0x%llX,0x%llX,0x%llX) targetFix=(0x%llX,0x%llX,0x%llX)",
        (unsigned long long)sourceFix.x.rawValue,
        (unsigned long long)sourceFix.y.rawValue,
        (unsigned long long)sourceFix.z.rawValue,
        (unsigned long long)targetFix.x.rawValue,
        (unsigned long long)targetFix.y.rawValue,
        (unsigned long long)targetFix.z.rawValue);

    int32_t navFlag = s_groundNavFlag;
    if (navFlag < 0 || navFlag == 255) {
        navFlag = (m_miniMapData.myCamp >= 0) ? m_miniMapData.myCamp : 1;
    }
    LOG(LOG_LEVEL_INFO,
        "[WallCollision][Step22] navFlag=%d myCamp=%d groundFlag=%d",
        navFlag,
        m_miniMapData.myCamp,
        s_groundNavFlag);

    FixNavHitNative hit{};
    LOG(LOG_LEVEL_INFO,
        "[WallCollision][Step23] call Raycast instance=%p source=%p target=%p hit=%p",
        s_navMeshEdInstance,
        &sourceFix,
        &targetFix,
        &hit);
    const bool collided = s_raycast(s_navMeshEdInstance, &sourceFix, &targetFix, navFlag, &hit);
    LOG(LOG_LEVEL_INFO,
        "[WallCollision][Step24] Raycast result collided=%d hit.hit=%d hitCampNode=%d hitPos=(0x%llX,0x%llX,0x%llX)",
        collided,
        hit.hit,
        hit.hitCampNode,
        (unsigned long long)hit.position.x.rawValue,
        (unsigned long long)hit.position.y.rawValue,
        (unsigned long long)hit.position.z.rawValue);
    LOG(LOG_LEVEL_INFO, "[WallCollision][Step25] return=%d", collided || hit.hit);
    return collided || hit.hit;
}

bool lol::FEVisi::canMoveUp(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, 0.0f, 1.0f, stepSize); }
bool lol::FEVisi::canMoveDown(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, 0.0f, -1.0f, stepSize); }
bool lol::FEVisi::canMoveLeft(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, -1.0f, 0.0f, stepSize); }
bool lol::FEVisi::canMoveRight(float stepSize) { return !checkWallCollision(m_miniMapData.myWorldPos, 1.0f, 0.0f, stepSize); }

const lol::SurroundingWalls& lol::FEVisi::detectMyHeroSurroundingObstacles(float stepSize) {
    auto& result = m_miniMapData.walls;
    result = {};
    result.stepSize = stepSize;

    LOG(LOG_LEVEL_INFO,
        "[ObstacleScan][Step1] enter myPos=(%.3f,%.3f,%.3f) stepSize=%.3f",
        m_miniMapData.myWorldPos.x,
        m_miniMapData.myWorldPos.y,
        m_miniMapData.myWorldPos.z,
        stepSize);

    if (stepSize <= 0.0f) {
        LOG(LOG_LEVEL_WARN, "[ObstacleScan][Step1] stepSize 非法: %.3f", stepSize);
        return result;
    }

    if (m_miniMapData.myWorldPos.x == 0.0f &&
        m_miniMapData.myWorldPos.y == 0.0f &&
        m_miniMapData.myWorldPos.z == 0.0f) {
        LOG(LOG_LEVEL_WARN, "[ObstacleScan][Step1] myWorldPos 无效，无法执行四方向检测");
        return result;
    }

    const auto& pos = m_miniMapData.myWorldPos;
    bool upReady = false;
    bool downReady = false;
    bool leftReady = false;
    bool rightReady = false;

    LOG(LOG_LEVEL_INFO, "[ObstacleScan][Step2] begin four-direction wall probes");
    result.up = checkWallCollision(pos, 0.0f, 1.0f, stepSize, &upReady);
    result.down = checkWallCollision(pos, 0.0f, -1.0f, stepSize, &downReady);
    result.left = checkWallCollision(pos, -1.0f, 0.0f, stepSize, &leftReady);
    result.right = checkWallCollision(pos, 1.0f, 0.0f, stepSize, &rightReady);
    result.valid = upReady && downReady && leftReady && rightReady;

    LOG(LOG_LEVEL_INFO,
        "[ObstacleScan][Step3] ready(up=%d down=%d left=%d right=%d) blocked(up=%d down=%d left=%d right=%d) valid=%d anyBlocked=%d",
        upReady,
        downReady,
        leftReady,
        rightReady,
        result.up,
        result.down,
        result.left,
        result.right,
        result.valid,
        result.anyBlocked());

    if (!result.valid) {
        LOG(LOG_LEVEL_WARN, "[ObstacleScan][Step3] 导航探测器未完全就绪，本帧障碍结果标记为无效");
    }

    return result;
}

lol::SurroundingWalls lol::FEVisi::scanSurroundingObstacles(float stepSize) {
    return detectMyHeroSurroundingObstacles(stepSize);
}

OBFU_ATTRS_END
