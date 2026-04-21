/**
 * @file    lolm.h
 * @brief   LOL 手游（英雄联盟手游）专用业务逻辑层 —— 字符串解密、对局状态查询等
 * @author  Song
 * @date    2026/01/04
 * @update  2026/03/05
 *
 * @details 包含 LOL 手游特定的字符串解密表（byte_B9BBF1B）、加密字符串结构（LolStrStruct）、
 *          对局状态查询类（FEVisi）以及综合业务类（lol）。lol 类提供字符串解密、
 *          元数据字符串索引表初始化等功能。
 */

#ifndef DOBBY_PROJECT_LOLM_H
#define DOBBY_PROJECT_LOLM_H

#include <iostream>
#include <stdint.h>
#include <map>
#include <vector>
#include "LolHeader.h"
#include "../interface/interface.h"

struct Il2CppGlobalMetadataHeader;

namespace lol {

    /** @brief Unity 世界坐标 (float x, y, z) */
    struct UnityVector3 {
        float x;
        float y;
        float z;
    };

    /** @brief 屏幕坐标点（Unity 屏幕空间，原点左下） */
    struct ScreenPoint {
        float x;
        float y;
    };

    /** @brief 敌方英雄信息 */
    struct MiniMapEnemyHeroInfo {
        int32_t     iconType;       ///< 图标类型 (MiniMapIconType)
        uint32_t    heroLevel;      ///< 英雄等级
        float       curHp;          ///< 当前血量
        float       maxHp;          ///< 最大血量
        UnityVector3 worldPos;      ///< 世界坐标
        bool        hasWorldPos;    ///< 坐标是否有效
        int32_t     heroResId;      ///< 英雄配置 ID（_resId）
        std::string heroName;       ///< 英雄名称（从 BattleActor_DC.name 读取）
        std::string summonerName;   ///< 召唤师名（BattlePlayer.get_name）
        float       screenX;        ///< 屏幕坐标 X（Unity 屏幕空间，原点左下）
        float       screenY;        ///< 屏幕坐标 Y（Unity 屏幕空间，原点左下）
        bool        hasScreenPos;   ///< 屏幕坐标是否有效
        float       atkRange;       ///< 普攻最大范围（Fix64 转 float），-1 表示无效
    };

    /** @brief 眼/守卫信息 */
    struct MiniMapWardInfo {
        UnityVector3 worldPos;      ///< 世界坐标
        bool         hasWorldPos;   ///< 坐标是否有效
        int32_t      iconType;      ///< 图标类型
        float        screenX;       ///< 屏幕坐标 X
        float        screenY;       ///< 屏幕坐标 Y
        bool         hasScreenPos;  ///< 屏幕坐标是否有效
    };

    /** @brief 小兵信息 */
    struct MiniMapMinionInfo {
        UnityVector3 worldPos;      ///< 世界坐标
        bool         hasWorldPos;   ///< 坐标是否有效
        float        curHp;         ///< 当前血量
        float        maxHp;         ///< 最大血量
        float        screenX;       ///< 屏幕坐标 X
        float        screenY;       ///< 屏幕坐标 Y
        bool         hasScreenPos;  ///< 屏幕坐标是否有效
        uint32_t     objId;         ///< 目标对象 ID
        int32_t      camp;          ///< 所属阵营
        bool         isEnemy;       ///< 是否为敌方小兵
    };

    /**
     * @brief 四周障碍物检测结果（一个身位距离内各方向是否有地形阻挡）
     * @details 由 FEVisi::scanSurroundingObstacles() 每帧填充，存入 MiniMapData
     */
    struct SurroundingWalls {
        bool up    = false;  ///< Z+ 方向（屏幕上方）有障碍
        bool down  = false;  ///< Z- 方向（屏幕下方）有障碍
        bool left  = false;  ///< X- 方向（屏幕左侧）有障碍
        bool right = false;  ///< X+ 方向（屏幕右侧）有障碍
        bool valid = false;  ///< 本帧数据是否有效（导航网格已初始化时为 true）
        float stepSize = 0.0f; ///< 本次探测使用的步长（世界单位）

        /** @brief 四方向均无障碍返回 true */
        bool allClear() const { return valid && !up && !down && !left && !right; }
        /** @brief 至少一方向有障碍返回 true */
        bool anyBlocked() const { return valid && (up || down || left || right); }
    };

    /** @brief 小地图一次遍历的全部数据快照 */
    struct MiniMapData {
        std::vector<MiniMapEnemyHeroInfo> enemyHeroes;  ///< 敌方英雄列表
        std::vector<MiniMapWardInfo>      wards;        ///< 眼/守卫列表
        std::vector<MiniMapMinionInfo>    minions;      ///< 小兵列表

        // 己方英雄当前技能有效范围
        float       mySkillRange;    ///< 当前技能有效范围(float)，-1 无效
        UnityVector3 myWorldPos;     ///< 己方英雄世界坐标
        float       myScreenX;       ///< 己方屏幕 X
        float       myScreenY;       ///< 己方屏幕 Y
        bool        hasMyScreenPos;  ///< 己方屏幕坐标是否有效
        int32_t     myCamp;          ///< 己方阵营，-1 无效
        std::vector<ScreenPoint> mySkillRangeScreenPoints; ///< 己方攻击范围投影轮廓

        SurroundingWalls walls;      ///< 四周一个身位的障碍物检测结果（每帧更新）

        void clear() {
            enemyHeroes.clear();
            wards.clear();
            minions.clear();
            mySkillRange = -1.0f;
            myWorldPos = {0,0,0};
            myScreenX = 0; myScreenY = 0;
            hasMyScreenPos = false;
            myCamp = -1;
            mySkillRangeScreenPoints.clear();
            walls = {};
        }
    };

    /**
     * @struct  LolStrStruct
     * @brief   加密字符串缓存结构 —— 存储解密状态和原始字符串
     */
    typedef struct LolStrStruct{
        uint32_t isDecryption;  ///< 解密标志（0=未解密, 非0=已解密）
        std::string srcStr;     ///< 原始/解密后的字符串内容
    }LolStrStruct,*PLolStrStruct;

    /**
     * @class FEVisi
     * @brief 对局状态查询类 —— 提供战斗开始状态和玩家信息的查询接口
     *
     * @details 内部持有 fun::function 实例，通过 IL2CPP 运行时 API 查询游戏对局状态。
     */
    class FEVisi{


    public:
        /**
         * @brief 构造函数 —— 初始化 il2cpp 基地址和核心元数据指针
         * @param dqil2cppBase              il2cpp 模块基地址
         * @param pCodeRegistration         代码注册表指针
         * @param pMetadataRegistration     元数据注册表指针
         * @param pGlobalMetadataHeader     全局元数据头指针
         * @param pMetadataImagesTable      元数据镜像表指针
         */
        FEVisi(void* dqil2cppBase=nullptr,
               void *pCodeRegistration=nullptr,
               void *pMetadataRegistration=nullptr,
               void *pGlobalMetadataHeader=nullptr,
               void* pMetadataImagesTable=nullptr);

        /** @brief 析构函数 */
        ~FEVisi();

    public:
        /** @brief 遍历小地图图标，收集敌方英雄/眼位数据到 m_miniMapData */
        void* updateMiniMapData();

        /** @brief 遍历 miniIcons 字典，收集小兵数据到 m_miniMapData.minions */
        void updateMinionData();

        /** @brief 打印 m_miniMapData 中存储的所有数据（调试用） */
        void printMiniMapData() const;

        /** @brief 获取最近一次 updateMiniMapData() 遍历的小地图数据快照 */
        const MiniMapData& getMiniMapData() const { return m_miniMapData; }

        /** @brief 判断当前是否处于对局（战斗）中 */
        bool get_BattleStarted();

        /** @brief 获取 il2cpp 基地址 */
        void* getIl2cppBase() const { return m_il2cppBase; }

        /** @brief 获取所有玩家的队伍管理器对象指针 */
        void* get_battleTeamMgr();

        /**
         * @brief  验证指针指向的内存页是否可读（不触发 SIGSEGV）
         * @param  ptr   要验证的指针
         * @param  size  要读取的字节数
         * @return true=可读, false=不可读/无效
         * @details 使用 pipe write 系统调用验证，内核返回 EFAULT 而非 SIGSEGV
         */
        static bool IsReadableMemory(const void* ptr, size_t size = sizeof(void*));

    private:
        void* m_il2cppBase; // Store base address

        int32_t get_MiniIconBaseCtrlType(void* pData);

        float DecoderFix64(uint64_t value);

        // ========================= MiniMap 拆分函数 =========================//

        /**
         * @brief  获取 IL2CPP 托管对象的完整类型名（Namespace.ClassName）
         * @param  pObject  托管对象指针
         * @return 类型名字符串，对象为空返回 "<null>"
         */
        std::string getManagedTypeName(void* pObject);

        /**
         * @brief  获取小地图图标字典 (miniIcons Dictionary)
         * @return Dictionary 对象指针，失败返回 nullptr
         *
         * @details 链路: UIMainBattleMiniMapCtrl._entityCtrl
         *          → MiniMapEntityCtrl.miniMapIconCtrl
         *          → UIMiniMapIconCtrl.miniIcons
         */
        void* getMiniIconsDictionary();

        /**
         * @brief  尝试通过 FixGameObjectVisi::GetPosition 获取世界坐标
         * @param  pObject  FixGameObjectVisi 或其子类的对象指针
         * @param  outPos   [传出] 获取到的世界坐标
         * @return 成功返回 true
         */
        bool tryGetWorldPosition(void* pObject, UnityVector3& outPos);

        /**
         * @brief  将世界坐标转换为屏幕坐标（通过 Unity Camera.WorldToScreenPoint）
         * @param  worldPos     世界坐标
         * @param  outScreenX   [传出] 屏幕 X 坐标（Unity 屏幕空间，原点左下角）
         * @param  outScreenY   [传出] 屏幕 Y 坐标（Unity 屏幕空间，原点左下角）
         * @return 成功返回 true，失败（相机不存在/在相机背后）返回 false
         *
         * @details 链路: Camera.get_main() → Camera.WorldToScreenPoint(worldPos)
         *          返回的屏幕坐标 z < 0 表示目标在相机后方，此时返回 false
         */
        bool worldToScreen(const UnityVector3& worldPos, float& outScreenX, float& outScreenY);

        /**
         * @brief  获取小地图图标的世界坐标
         * @param  baseCtrl  UIMiniIconBaseCtrl 对象指针
         * @param  actor     BattleActorVisi 对象指针（可为 nullptr）
         * @param  outWorldPos [传出] 获取到的世界坐标（可为 nullptr，为空时仅返回是否成功）
         * @return 成功获取到坐标返回 true，否则返回 false
         *
         * @details 优先从 actor 获取坐标，若失败则从 baseCtrl 的 followObj 获取。
         *          链路: baseCtrl → followObj → tryGetWorldPosition
         */
        bool getIconWorldPos(void* baseCtrl, void* actor, UnityVector3* outWorldPos = nullptr);

        /**
         * @brief  判断小地图图标是否为眼/守卫类
         * @param  iconType          图标类型枚举值
         * @param  baseCtrl          UIMiniIconBaseCtrl 对象
         * @param  actor             BattleActorVisi 对象（可为 nullptr）
         * @param  baseCtrlTypeName  baseCtrl 的类型名
         * @param  actorTypeName     actor 的类型名
         * @return true 表示为眼/守卫类图标
         */
        bool isWardLikeIcon(int32_t iconType, void* actor,
                            const std::string& baseCtrlTypeName,
                            const std::string& actorTypeName);

        /** @brief 根据小地图图标信息追加一条小兵数据，成功追加返回 true */
        bool appendMinionData(int32_t iconType, void* actor,
                      const UnityVector3& worldPos, bool hasWorldPos);

        /** @brief 读取可视对象 objId，失败返回 0 */
        uint32_t readActorObjId(void* actorVisi);

        /** @brief 读取对象 camp，失败返回 -1 */
        int32_t readActorCamp(void* actorVisi);

        /** @brief 基于 WorldToScreenPoint 生成己方攻击范围的屏幕投影轮廓 */
        bool buildProjectedRangeRing(const UnityVector3& centerWorld, float range,
                         std::vector<ScreenPoint>& outPoints);


        /**
         * @brief  读取英雄等级
         * @param  ActorComponentAttribute  属性组件指针
         * @return 英雄等级，失败返回 0
         *
         * @details 读取 nativeData(+0x18) → heroLevel(+0x78) — RVA 0x8495058
         */
        uint32_t readEnemyHeroLeve(void* ActorComponentAttribute);

        /**
         * @brief  从 BattleActorVisi 获取 ActorComponentAttribute
         * @param  actorVisi  BattleActorVisi 对象指针
         * @return ActorComponentAttribute 指针，失败返回 nullptr
         *
         * @details 链路: BattleActorVisi → get_actor() → BattleActor
         *          → get_attribute() → ActorComponentAttribute
         */
        void* getActorAttribute(void* actorVisi);

        /**
         * @brief  获取己方英雄当前技能的有效目标范围
         * @param  actorVisi  BattleActorVisi 对象指针（己方英雄）
         * @return 有效范围(float)，失败返回 -1.0f
         *
         * @details 链路: BattleActorVisi → get_actor() → BattleActor
         *          → get_auxComponent() → ActorComponentAuxiliary
         *          → get_targetSystem() → TargetSystem
         *          → GetSkillValidTargetRange(currentSkill) → Fix64 → float
         */
        float getMySkillValidTargetRange(void* actorVisi, bool isSelf = false);

        void* getSkillUILogicInstance();
        void* getSkillUILogic(int skillID);
        std::vector<void*> getAllSkillUILogics();

    public:
        /**
         * @brief  模拟普通攻击 —— 通过 PlayerControl.OnTriggerSkillButtonDown 发起普攻
         * @return true=成功触发, false=失败（未找到英雄/技能/控制器等）
         *
         * @details 调用链:
         *   1. FEVisi.get_playerControl() → PlayerControl
         *   2. BattleActorVisi → BattleActor → ActorComponentSkillMgr
         *   3. ActorComponentSkillMgr.GetNormalAttackSkill() → ActorSkill
         *   4. PlayerControl.OnTriggerSkillButtonDown(valid, skillID, operID, ...)
         *   5. PlayerControl.OnTriggerSkillButtonUp(...)  完成攻击释放
         */
        bool simulateNormalAttack();

        /**
         * @brief  每 tick 调用 —— 处理延迟的 ButtonUp（普攻第二阶段）
         * @details 必须在数据采集循环中每 tick 调用此函数。
         *          simulateNormalAttack() 仅发送 ButtonDown，
         *          ButtonUp 会在下一个 tick 由此函数执行，
         *          确保游戏 Update 有时间处理 Down 状态。
         */
        void tickPendingAttack();

        /**
         * @brief  模拟英雄移动 —— 通过 PlayerControl.OnTriggerMoveButtonDown/Aim/Up 驱动
         * @param  dirX  X方向 (-1.0 ~ 1.0), 正值=右
         * @param  dirY  Y方向 (-1.0 ~ 1.0), 正值=前(屏幕上方)
         * @return true=成功触发, false=失败
         *
         * @details 必须每 tick 持续调用以维持移动。传入 (0, 0) 停止移动。
         */
        bool simulateMovement(float dirX, float dirY);

        bool simulateMoveUp();
        bool simulateMoveDown();
        bool simulateMoveLeft();
        bool simulateMoveRight();

        /**
         * @brief  检测从当前位置向指定方向迈出一步是否会碰撞地形（墙壁/不可通行区域）
         * @param  currentPos  当前世界坐标
         * @param  worldDirX   X 方向分量（归一化，正值=右）
         * @param  worldDirZ   Z 方向分量（归一化，正值=前/上方）
         * @param  stepSize    步长（世界坐标单位），默认 1.0f
         * @return true = 目标路径被地形阻挡；false = 可以通行
         *
         * @details 原理：
         *   1. 从 FixBuilderAsset MonoBehaviour 获取 FixNavmeshRawData
         *   2. 创建 FixNavMeshEd 实例并 InitNavmeshEd 初始化导航网格
         *   3. Float 坐标 → Fix64 定点数（使用 Fix64.divideOfOne）
         *   4. 调用 FixNavMeshEd.Raycast(srcPos, targetPos, GroundFlag=1, out hit)
         *   5. 返回 hit.hit（true = 路径穿越了导航网格边界 = 撞墙）
         */
        bool checkWallCollision(const UnityVector3& currentPos,
                                float worldDirX, float worldDirZ,
                                float stepSize = 1.0f,
                                bool* outDetectorReady = nullptr);

        /** @brief 判断向上（Z+方向）迈 stepSize 步是否可通行，可通行返回 true */
        bool canMoveUp(float stepSize = 1.0f);
        /** @brief 判断向下（Z-方向）迈 stepSize 步是否可通行，可通行返回 true */
        bool canMoveDown(float stepSize = 1.0f);
        /** @brief 判断向左（X-方向）迈 stepSize 步是否可通行，可通行返回 true */
        bool canMoveLeft(float stepSize = 1.0f);
        /** @brief 判断向右（X+方向）迈 stepSize 步是否可通行，可通行返回 true */
        bool canMoveRight(float stepSize = 1.0f);

        /**
         * @brief  实时检测己方英雄上下左右是否有障碍物，结果写入 m_miniMapData.walls
         * @param  stepSize  探测步长（世界坐标单位），默认 1.0f
         * @return 最近一次检测结果的引用
         *
         * @details 基于当前 myWorldPos 分别检测 Z+、Z-、X-、X+ 四个方向。
         *          适合在主循环中每帧调用，然后通过 getMiniMapData().walls 读取共享结果。
         */
        const SurroundingWalls& detectMyHeroSurroundingObstacles(float stepSize = 1.0f);

        /**
         * @brief  兼容旧接口：实时扫描英雄当前位置四周一个身位距离的障碍物
         * @param  stepSize  探测步长（世界坐标单位），默认 1.0f（一个身位约等于角色碰撞体半径×2）
         * @return 本次扫描结果的副本（同时写入 m_miniMapData.walls）
         *
         * @details 内部调用 detectMyHeroSurroundingObstacles()，保留旧调用方式。
         */
        SurroundingWalls scanSurroundingObstacles(float stepSize = 1.0f);

    private:

        /**
         * @brief  读取敌方英雄的 HP 信息并输出日志
         * @param  actor  BattleActorVisi 对象指针
         *
         * @details 链路: BattleActorVisi → get_actor() → BattleActor
         *          → get_attribute() → ActorComponentAttribute
         *          → nativeData+0x138 (curHP), GetFixAttrValue(2) (maxHP)
         */
        void readEnemyHeroHP(void* actor,float* curHp=nullptr, float* maxHp=nullptr);

        /**
         * @brief  读取英雄配置 ID（_resId）
         * @param  actorVisi  BattleActorVisi 对象指针
         * @return 英雄配置 ID，失败返回 0
         *
         * @details 链路: BattleActorVisi → _resId(0x150)
         */
        int32_t readHeroResId(void* actorVisi);

        /**
         * @brief  读取英雄名称
         * @param  actorVisi  BattleActorVisi 对象指针
         * @return 英雄名称字符串，失败返回空字符串
         *
         * @details 链路: BattleActorVisi → dc(0x90) → BattleActor_DC
         *          → name → Il2CppString → std::string
         *          优先从 BattleActor_DC.name 读取；若失败则尝试 resId 映射
         */
        std::string readHeroName(void* actorVisi);

        /**
         * @brief  将 IL2CPP 托管 System.String 对象转为 C++ std::string (UTF-8)
         * @param  pIl2CppString  Il2CppString 对象指针
         * @return UTF-8 字符串，失败返回空字符串
         */
        std::string readIl2CppString(void* pIl2CppString);

        /**
         * @brief  读取召唤师名（玩家名）
         * @param  actorVisi  BattleActorVisi 对象指针
         * @return 召唤师名字符串，失败返回空字符串
         *
         * @details 链路: BattleActorVisi → get_player() → BattlePlayer
         *          → get_name() → Il2CppString → std::string
         *          get_name 内部: 缓存(this+0xB0), 未命中时通过
         *          roleId(native+0x148) → BattleContext.GetPlayerNameInBattle
         */
        std::string readSummonerName(void* actorVisi);

        //========================= IL2CPP 动态字段查找 API =========================//

        /**
         * @brief  获取托管对象的 Il2CppClass 指针（从对象头部 offset 0x00 读取）
         * @param  pObject  托管对象指针
         * @return Il2CppClass* 或 nullptr
         */
        inline ::Il2CppClass* GetObjectKlass(void* pObject) {
            if (!pObject) return nullptr;
            return reinterpret_cast<::Il2CppClass*>(*reinterpret_cast<uint64_t*>(pObject));
        }

        /**
         * @brief  从运行时 Il2CppClass（含父类链）动态查找指定字段的偏移量
         * @param  klass      目标类（运行时 Il2CppClass*）
         * @param  fieldName  字段名称
         * @return 字段偏移，查找失败返回 INVALID_OFFSET
         */
        uint32_t GetFieldOffsetFromKlass(::Il2CppClass* klass, const char* fieldName);

        /**
         * @brief  从运行时对象（含类层次链）动态查找指定字段的偏移量
         * @param  pObject    对象指针
         * @param  fieldName  字段名称
         * @return 字段偏移，查找失败返回 INVALID_OFFSET
         */
        inline uint32_t GetFieldOffsetFromObject(void* pObject, const char* fieldName) {
            return GetFieldOffsetFromKlass(GetObjectKlass(pObject), fieldName);
        }

        /**
         * @brief  从运行时 Il2CppClass 的静态字段中按名称读取 int32_t 值
         * @param  klass      目标类
         * @param  fieldName  静态字段名称
         * @param  outValue   [传出] 读取到的值
         * @return 成功返回 true，失败返回 false
         */
        bool ReadStaticFieldInt32(::Il2CppClass* klass, const char* fieldName, int32_t& outValue);

        /**
         * @brief   通过对象基地址 + 偏移读取成员值（模板版本，支持任意类型）
         * @tparam  T       要读取的目标类型（如 int32_t, float, bool, void* 等）
         * @param   pObject 对象基地址（Il2CppObject* 或任意运行时对象指针）
         * @param   offset  成员在对象中的偏移（通过 GetStaticMember 的 pOutOffset 获取）
         * @return  读取到的值，若 pObject 为空则返回 T 的默认值
         */
        template<typename T>
        T ReadMemberValue(void* pObject, uint32_t offset) {
            if (pObject == nullptr) return T{};
            return *reinterpret_cast<T*>((uint64_t)pObject + offset);
        }

        /**
         * @brief   查询指定类中任意成员（静态/实例）的偏移量
         * @param   pModuleName     类所在模块名（如 "Assembly-CSharp.dll"）
         * @param   pClassName      类名
         * @param   pTemplateName   泛型名（可为空）
         * @param   pFieldName      字段名
         * @return  字段偏移，查找失败返回 INVALID_OFFSET (0xFFFFFFFF)
         */
        static constexpr uint32_t INVALID_OFFSET = 0xFFFFFFFF;
        inline uint32_t GetFieldOffset(const std::string& pModuleName,
                                       const std::string& pClassName,
                                       const std::string& pTemplateName,
                                       const std::string& pFieldName) {
            if (m_pfunctionInfo == nullptr) return INVALID_OFFSET;
            uint32_t offset = INVALID_OFFSET;
            m_pfunctionInfo->GetMember("ilbil2cpp.so",pModuleName, pClassName, pTemplateName, pFieldName, &offset);
            return offset;
        }

        /**
         * @brief   通过对象基地址 + 偏移读取指针类型成员（便捷版本）
         * @param   pObject 对象基地址
         * @param   offset  成员在对象中的偏移
         * @return  读取到的指针值，若 pObject 为空则返回 nullptr
         */
        inline void* ReadMemberPtr(void* pObject, uint32_t offset) {
            if (pObject == nullptr) return nullptr;
            return *reinterpret_cast<void**>((uint64_t)pObject + offset);
        }

        /**
         * @brief   通过类名和字段名自动查找偏移并读取成员值
         * @tparam  T               要读取的目标类型
         * @param   pObject         对象基地址
         * @param   pModuleName     类所在模块名（如 "Assembly-CSharp.dll"）
         * @param   pClassName      类名
         * @param   pTemplateName   泛型名（可为空）
         * @param   pFieldName      字段名
         * @return  读取到的值，查找失败或 pObject 为空则返回 T 的默认值
         */
        template<typename T>
        T ReadMemberByName(void* pObject, const std::string& pModuleName,
                           const std::string& pClassName, const std::string& pTemplateName,
                           const std::string& pFieldName) {
            if (pObject == nullptr || m_pfunctionInfo == nullptr) return T{};
            uint32_t offset = 0xFFFFFFFF;
            m_pfunctionInfo->GetStaticMember("libil2cpp.so", pModuleName, pClassName, pTemplateName, pFieldName, &offset);
            if (offset == 0xFFFFFFFF) return T{};
            return *reinterpret_cast<T*>((uint64_t)pObject + offset);
        }
    private:
        std::shared_ptr<fun::function> m_pfunctionInfo; ///< 类信息查询接口实例
        MiniMapData m_miniMapData;                      ///< 小地图遍历数据快照
    };

    /**
     * @class lol
     * @brief LOL 手游综合业务类 —— 继承 FEVisi，提供字符串解密和元数据索引表管理
     *
     * @details 负责初始化字符串索引表、判断字符串加密状态、执行字符串解密等核心功能。
     */
    class lol: public FEVisi {
    public:
        /**
         * @brief 构造函数
         * @param dqil2cppBase              il2cpp 模块基地址
         * @param pCodeRegistration         代码注册表指针
         * @param pMetadataRegistration     元数据注册表指针
         * @param pGlobalMetadataHeader     全局元数据头指针
         * @param pMetadataImagesTable      元数据镜像表指针
         */
        lol(void* dqil2cppBase=nullptr,
            void *pCodeRegistration=nullptr,
            void *pMetadataRegistration=nullptr,
            void *pGlobalMetadataHeader=nullptr,
            void* pMetadataImagesTable=nullptr);

        /** @brief 析构函数 */
        ~lol();

    private:
        /**
         * @brief   解密加密字符串
         * @param   Srcstr      加密的原始字符串指针
         * @param   nameIndex   字符串在索引表中的索引
         * @return  解密后的字符串
         */
        std::string decryPtthestring(char*Srcstr, uint32_t nameIndex);

        /**
         * @brief   初始化字符串索引表
         * @param   pGlobalMetadataHeader 全局元数据头指针
         * @return  初始化后的索引表指针
         */
        void* inItStringindexTable(const Il2CppGlobalMetadataHeader* pGlobalMetadataHeader);

        /**
         * @brief   判断指定索引的字符串是否已加密/已解密
         * @param   nameIndex 字符串索引
         * @return  true 表示已加密需要解密，false 表示无需处理
         */
        bool IsEncryptionOrIsDecryption(uint32_t nameIndex);



    private:
        void*parrayStringIndex = NULL;                                                          ///< 字符串索引数组原始指针
        std::shared_ptr<std::map<uint32_t,std::shared_ptr<LolStrStruct>>> m_pMapStringIndex = nullptr; ///< 字符串索引 -> 解密缓存 的映射表
    };
}
#endif //DOBBY_PROJECT_LOLM_H
