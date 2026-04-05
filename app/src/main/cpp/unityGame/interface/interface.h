/**
 * @file    interface.h
 * @brief   IL2CPP 类信息采集与查询接口层 —— 运行时类/字段/方法元数据的结构化存储与检索
 * @author  Song
 * @date    2026/01/10
 * @update  2026/03/05
 *
 * @details 定义了类信息的层级数据结构（CData → CFieldData / CTemplateData → CClassData → CClassInfo），
 *          以及 function 类提供的核心接口：fillingClassInfo（采集全部类信息）、GetStaticMember（查找
 *          静态成员值）、GetMethodFun（查找方法地址）、FindClassByName（按类名查找类对象）等。
 */

#ifndef DOBBY_PROJECT_INTERFACE_H
#define DOBBY_PROJECT_INTERFACE_H



#include "../UnityApi/unityapi.h"
#include "../../File/file.h"
#include <map>
#include <list>
#include <unordered_map> // 必须添加，用于性能优化
#include <vector>
#include <memory>
#include <string>

namespace fun {

    /**
     * @struct  _Data
     * @brief   通用数据节点 —— 存储名称和偏移量（用于模块信息等通用场景）
     */
    typedef struct _Data{
        std::shared_ptr<std::string> m_pName;   ///< 名称（模块名 / 其他标识）
        uint64_t m_offset;                      ///< 偏移量或地址
    }CData,*PCData;

    /**
     * @struct  _fieldData
     * @brief   字段数据节点 —— 存储字段名称、偏移以及原始 FieldInfo 指针
     */
    typedef struct _fieldData{
        CData m_Data;           ///< 字段名称及偏移信息
        FieldInfo *fieldInfo;   ///< IL2CPP 原始字段信息指针
    }CFieldData,*PCFieldData;

    /**
     * @struct  _TemplateData
     * @brief   模板/泛型数据节点 —— 存储名称、泛型全限定名和偏移量（用于类名和方法名）
     */
    typedef struct _TemplateData{
        std::shared_ptr<std::string> m_pName;           ///< 名称（类名 / 方法名）
        std::shared_ptr<std::string> m_pGenericsName;   ///< 泛型全限定名（含命名空间）
        uint64_t m_offset;                              ///< 偏移量或地址
    }CTemplateData,*PTemplateData;

    /**
     * @struct  _ClassData
     * @brief   单个类的完整数据 —— 包含类名信息、字段列表、方法列表和 Il2CppClass 指针
     */
    typedef struct _ClassData{
        std::shared_ptr<CTemplateData> m_pClassData;                                        ///< 类名及泛型信息
        std::shared_ptr<std::vector<std::shared_ptr<CFieldData>>> m_pVectorStaticData;      ///< 字段（含静态字段）列表
        std::shared_ptr<std::vector<std::shared_ptr<CTemplateData>>> m_pVectorFunctionData; ///< 方法列表
        Il2CppClass* m_pKlass = nullptr;                                                    ///< IL2CPP 原始类对象指针
    }CClassData,*PClassData;

    /**
     * @struct  _ClassInfo
     * @brief   模块级类信息 —— 一个模块下包���的所有类数据
     */
    typedef struct _ClassInfo{
        std::shared_ptr<CData> m_ModuleData;                                                ///< 模块名称及偏移信息
        std::shared_ptr<std::vector<CClassData>> m_pClassStruct;                            ///< 当前模块下的所有类结构列表
    }CClassInfo,*PCClassInfo;

    /**
     * @struct  CGenericMethodData
     * @brief   泛型方法实例数据 —— 存储泛型方法的名称、Spec 全名和实际地址
     *
     * @details 例如对于 DataShellList<CherryTeam,IsDemData,IsSyncData,NotSharedPtr>.get_Count
     *          m_pMethodName  = "get_Count"
     *          m_pSpecName    = "DataShellList<CherryTeam, IsDemData, IsSyncData, NotSharedPtr>.get_Count"
     *          m_pClassName   = "DataShellList"
     *          m_offset       = 实际内存地址（绝对地址，非 RVA）
     */
    typedef struct CGenericMethodData{
        std::string m_pMethodName;      ///< 方法名（如 get_Count）
        std::string m_pSpecName;        ///< Spec 完整名（类名<泛型参数>.方法名）
        std::string m_pClassName;       ///< 去掉反引号后的类名（如 DataShellList）
        uint64_t    m_offset = 0;       ///< 方法在内存中的绝对地址
    }CGenericMethodData;



    /**
     * @class function
     * @brief IL2CPP 类信息采集与查询核心类
     *
     * @details 继承自 cUnityApi，在运行时遍历所有程序集/镜像/类，将元数据结构化存储到
     *          m_pClassInfo 链表中，并提供按名称查找类、静态成员、方法地址等接口。
     */
    class function : public li2cppApi::cUnityApi {
    public:
        /**
         * @brief 构造函数 —— 初始化 il2cpp 基地址、元数据指针，并创建日志文件
         * @param dqil2cppBase              il2cpp 模块基地址
         * @param pCodeRegistration         代码注册表指针
         * @param pMetadataRegistration     元数据注册表指针
         * @param pGlobalMetadataHeader     全局元数据头指针
         * @param pMetadataImagesTable      元数据镜像表指针
         */
        function(void* dqil2cppBase=nullptr,
                 void *pCodeRegistration=nullptr,
                 void *pMetadataRegistration=nullptr,
                 void *pGlobalMetadataHeader=nullptr,
                 void* pMetadataImagesTable=nullptr);

        /** @brief 析构函数 —— 释放类信息链表并关闭日志文件 */
        ~function();

    public:
        /** @brief 采集全部类信息 —— 遍历所有程序集/镜像/类，填充 m_pClassInfo */
        void fillingClassInfo();

        /** @brief 深度释放 m_pClassInfo 中的所有嵌套数据并重置为 nullptr */
        void DeepClearClassInfo();

        /**
         * @brief   写入日志到文件
         * @param   str 日志内容
         * @return  成功返回 true，失败返回 false
         */
        bool writeLog(std::string str);

        /**
         * @brief   初始化包名路径（用于确定日志文件存储位置）
         * @param   strPackName 应用包名（例如 "com.tencent.lolm"）
         */
        void initPackPath(std::string strPackName);

    public:


    public:
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // 参数一：模块名例如 libil2cpp.so
        // 参数二：类所在模块名例如 Assembly-CSharp.dll
        // 参数三：类名 例如 <>c__DisplayClass6_0
        // 参数四：泛型名 例如 JoystickSkillHandler.<>c__DisplayClass6_0
        // 参数五：要查找的静态成员名称
        // 返回值：成功返回找到的值，失败返回0
        uint64_t GetStaticMember(std::string pMainModuleName, std::string pModuleName,std::string pClassName,std::string ptemplateName, std::string pStaticName, uint32_t* pOutOffset = nullptr);
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // 获取指定类中任意成员（不区分静态/实例）的偏移信息
        // 参数一：模块名例如 libil2cpp.so
        // 参数二：类所在模块名例如 Assembly-Csharp.dll
        // 参数三：类名 例如 BattleBaseUI
        // 参数四：泛型名 例如 BattleBaseUI（为空则不限定泛型）
        // 参数五：要查找的成员名称
        // 参数六：[传出] 成员在类中的偏移
        // 参数七：[传出] 成员的 FieldInfo 指针（可为空）
        // 参数八：[传出] 是否为静态字段（可为空）
        // 返回值：成功返回 true，失败返回 false
        bool GetMember(std::string pMainModuleName,std::string pModuleName, std::string pClassName, std::string pTemplateName,std::string pFieldName, uint32_t* pOutOffset= nullptr,FieldInfo** pOutFieldInfo = nullptr, bool* pOutIsStatic = nullptr);
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // 参数一：模块名例如 libil2cpp.so
        // 参数二：类所在模块名例如 Assembly-CSharp.dll
        // 参数三：类名 例如 <>c__DisplayClass6_0
        // 参数四：泛型名 例如 JoystickSkillHandler.<>c__DisplayClass6_0
        // 参数五：要查找成员方法名称
        // 参数六：要查找成员方法的泛型名
        // 参数七：参数个数；传 -1 表示不限制
        // 返回值：成功返回找到的值，失败返回0
        uint64_t GetMethodFun(std::string pMainModuleName, std::string pModuleName,std::string pClassName, std::string ptemplateName,std::string pMethodName,std::string pMethodtemplateName = "", int32_t pParamCount = -1);
        Il2CppClass* GetMethodOwnerClass(std::string pMainModuleName, std::string pModuleName,
                         std::string pClassName, std::string ptemplateName,
                         std::string pMethodName = "", std::string pMethodtemplateName = "",
                         int32_t pParamCount = -1);
        /**
         * @brief   通过运行时 Il2CppClass 直接查找方法地址（适合泛型实例类）
         * @param   pKlass       运行时类对象
         * @param   pMethodName  方法名
         * @param   pParamCount  参数个数；传 -1 表示不限制
         * @return  成功返回方法地址，失败返回 0
         */
        uint64_t GetMethodFunByClass(Il2CppClass* pKlass, std::string pMethodName, int32_t pParamCount = -1);

        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // 获取泛型实例化方法的地址（从 genericMethodTable 中查找）
        // 参数一：Spec 全名中的类名部分（去掉反引号），例如 "DataShellList"
        // 参数二：Spec 全名中的方法名部分，例如 "get_Count"
        // 参数三：Spec 全名中的泛型参数（可选，为空则只匹配类名+方法名），例如 "CherryTeam, IsDemData, IsSyncData, NotSharedPtr"
        // 返回值：成功返回方法在内存中的绝对地址，失败返回 0
        uint64_t GetGenericMethodFun(std::string pClassName, std::string pMethodName, std::string pGenericArgs = "");
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

        /**
         * @brief   添加模块信息到 CClassInfo 结构中
         * @param   pData       目标 CClassInfo 智能指针
         * @param   pModuleName 模块名称
         * @param   offset      模块偏移/地址，默认 -1
         * @return  填充后的 CClassInfo 智能指针
         */
        std::shared_ptr<fun::CClassInfo>AddModuleInfo(std::shared_ptr<fun::CClassInfo> pData, std::string pModuleName,uint64_t offset = -1);

        /**
         * @brief   添加类基础信息（类名、泛型名）到 CClassData 结构中
         * @param   pData           目标 CClassData 智能指针
         * @param   pClassName      类名
         * @param   pGenericsName   泛型名（可为空）
         * @param   offset          类偏移/地址，默认 -1
         * @return  填充后的 CClassData 智能指针
         */
        std::shared_ptr<fun::CClassData>AddClassStructInfo(std::shared_ptr<fun::CClassData> pData, std::string pClassName,std::string pGenericsName, uint64_t offset = -1);

        /**
         * @brief   添加字段数据到字段列表中
         * @param   pData       字段列表智能指针
         * @param   pStaticName 字段名称
         * @param   fieldInfo   IL2CPP 原始 FieldInfo 指针
         * @param   offset      字段偏移，默认 -1
         * @return  更新后的字段列表智能指针
         */
        std::shared_ptr<std::vector<std::shared_ptr<fun::CFieldData>>>AddVectorStaticData(std::shared_ptr<std::vector<std::shared_ptr<fun::CFieldData>>> pData,std::string pStaticName,FieldInfo *fieldInfo, uint64_t offset = -1);

        /**
         * @brief   添加方法数据到方法列表中
         * @param   pData           方法列表智能指针
         * @param   pClassName      方法名称
         * @param   pGenericsName   方法泛型名（可为空）
         * @param   offset          方法地址，默认 -1
         * @return  更新后的方法列表智能指针
         */
        std::shared_ptr<std::vector<std::shared_ptr<fun::CTemplateData>>>AddVectorFunctionData(std::shared_ptr<std::vector<std::shared_ptr<fun::CTemplateData>>> pData,std::string pClassName, std::string pGenericsName,uint64_t offset = -1);

        /**
         * @brief   将模块数据和类结构向量封装为 CClassInfo 并存入全局链表
         * @param   pData           全局链表智能指针
         * @param   pModuleData     模块数据
         * @param   pClassStruct    类结构向量
         * @return  更新后的全局链表智能指针
         */
        std::shared_ptr<std::list<std::shared_ptr<fun::CClassInfo>>>AddClassStruct(std::shared_ptr<std::list<std::shared_ptr<fun::CClassInfo>>> pData,std::shared_ptr<fun::CData> pModuleData,std::shared_ptr<std::vector<fun::CClassData>>pClassStruct);

        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
        // 根据类名查找类对象（支持模块名和泛型名精确匹配，避免同名类冲突）
        // 参数一: 模块名例如 libil2cpp.so
        // 参数二：pModuleName   - 类所在模块名 例如 Assembly-CSharp.dll（为空则不限定模块）
        // 参数三：pClassName    - 要查找的类名 例如 <>c__DisplayClass6_0
        // 参数四：pTemplateName - 泛型名 例如 JoystickSkillHandler.<>c__DisplayClass6_0（为空则不限定泛型）
        // 返回值：成功返回找到的 Il2CppClass* 类对象地址，失败返回 nullptr
        Il2CppClass* FindClassByName(std::string pMainModuleName,std::string pModuleName, std::string pClassName = "", std::string pTemplateName = "");
        //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    private:
        std::string m_pathlog = "";                                                 ///< 日志文件路径
        std::shared_ptr<cMyfile> m_outlog = nullptr;                                ///< 日志文件输出流
        std::shared_ptr<std::list<std::shared_ptr<CClassInfo>>>m_pClassInfo = nullptr; ///< 全局类信息链表（按模块组织）

        /// 泛型方法实例化列表（从 genericMethodTable 遍历得到）
        std::shared_ptr<std::vector<std::shared_ptr<CGenericMethodData>>> m_pGenericMethodList = nullptr;

    private:
        /** @brief 遍历 genericMethodTable，填充 m_pGenericMethodList */
        void fillingGenericMethodInfo();

        /** @brief 获取泛型实例参数字符串 <T1, T2, ...>（从 Il2CppGenericInst 解析） */
        std::string getGenericInstName(GenericInstIndex index);

        /** @brief 获取 Il2CppType 的简短类型名 */
        std::string getTypeName(const Il2CppType* type);
    };

}

#endif //DOBBY_PROJECT_INTERFACE_H
