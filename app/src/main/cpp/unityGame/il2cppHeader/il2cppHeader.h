/**
 * @file    il2cppHeader.h
 * @brief   IL2CPP IDA 头文件生成器 —— 将运行时类/方法元数据导出为 IDA 可导入的结构体头文件和 JSON
 * @author  Song
 * @date    2026/02/03
 * @update  2026/03/05
 *
 * @details 定义了 UnityField、UnityMethod、UnityClass 等中间表示结构，以及 li2cppHeader 导出类。
 *          li2cppHeader 继承自 cUnityApi，遍历所有类元数据，生成 IDA 兼容的 C 结构体头文件
 *          和脚本方法 JSON 文件（包含 ScriptMethod / ScriptString / ScriptMetadata 等段）。
 */

#ifndef DOBBY_PROJECT_IL2CPPHEADER_H
#define DOBBY_PROJECT_IL2CPPHEADER_H
#include "../Symbol/Symbol.h"
#include "../UnityApi/unityapi.h"
#include <string>
#include <vector>
#include <cstdint>
#include <memory> // 必须包含此头文件
#include <map>
#include <set>


#define UNITY_2018_4_16F1

#ifdef UNITY_2018_4_16F1
#include "../UnityStruct/unity2018-4-16f1/UnityStructInfo.h"
#else
#include "../UnityStruct/UnityInfo.h"
#endif

namespace li2cppHeader {

    /**
     * @class UnityField
     * @brief 字段信息中间表示 —— 存储字段名、类型名、偏移、静态标记及静态地址
     */
    class UnityField {
    public:
        std::string name;           ///< 字段名称
        std::string typeName;       ///< 字段类型名称
        uint32_t offset;            ///< 字段在实例中的内存偏移
        bool isStatic;              ///< 是否为静态字段
        uintptr_t staticAddr;       ///< 静态字段的内存地址（非静态为0）

        /** @brief 默认构造 */
        UnityField() : offset(0), isStatic(false), staticAddr(0) {}

        /** @brief 参数化构造 */
        UnityField(std::string n, std::string t, uint32_t off, bool s = false, uintptr_t addr = 0)
                : name(std::move(n)), typeName(std::move(t)), offset(off), isStatic(s), staticAddr(addr) {}
    };

    /**
     * @struct VTableEntry
     * @brief  虚表条目信息 —— 记录虚表索引与对应的方法名
     */
    struct VTableEntry {
        int index;                  ///< 虚表索引
        std::string methodName;     ///< 对应的方法名

        VTableEntry(int idx, const std::string& name)
                : index(idx), methodName(name) {}
    };

    /**
     * @class UnityMethod
     * @brief 方法信息中间表示 —— 存储方法名、返回类型、参数类型列表、方法指针和签名
     */
    class UnityMethod {
    public:
        std::string name;                               ///< 方法名称
        std::string returnType;                         ///< 返回值类型名称
        std::vector<std::string> parameterTypes;        ///< 参数类型名称列表
        uintptr_t methodPointer;                        ///< 方法在内存中的函数指针
        uint32_t token;                                 ///< 方法的元数据 Token

        std::string signature;                          ///< 生成的完整方法签名字符串
        std::string typeSignature;                      ///< 类型缩写签名（例如 "viii" 表示 void(int,int,int)）

        /** @brief 默认构造 */
        UnityMethod() : methodPointer(0), token(0) {}

        /** @brief 参数化构造 */
        UnityMethod(std::string n, std::string ret, uintptr_t ptr, uint32_t tk = 0)
                : name(std::move(n)), returnType(std::move(ret)), methodPointer(ptr), token(tk) {}
    };

    /**
     * @class UnityClass
     * @brief 类信息中间表示 —— 存储类的完整元数据（命名空间、类名、字段、方法、接口、虚表等）
     */
    class UnityClass {
    public:
        /**
         * @union TypeFlags
         * @brief 类型属性标志位联合体 —— 以位域方式存储类型分类信息
         */
        union TypeFlags {
            uint32_t raw;                   ///< 原始值（可一次性读写）
            struct {
                uint32_t isStruct : 1;      ///< 是否为结构体
                uint32_t isEnum   : 1;      ///< 是否为枚举
                uint32_t isValueType : 1;   ///< 是否为值类型
                uint32_t isInterface : 1;   ///< 是否为接口
                uint32_t reserved : 28;     ///< 保留位
            } bits;

            TypeFlags() : raw(0) {}
        };

        std::string namespc;                                ///< 命名空间
        std::string className;                              ///< 类名
        std::string assembly;                               ///< 所属程序集名称

        uintptr_t klassPtr;                                 ///< Il2CppClass 原始指针
        uintptr_t parentPtr;                                ///< 父类的 Il2CppClass 指针
        std::string parentSafeName;                         ///< 父类的安全唯一名称（用于继承输出）
        uint32_t instanceSize;                              ///< 类实例大小（字节）
        uint32_t flags;                                     ///< IL2CPP 原生的修饰符标志

        TypeFlags typeAttr;                                 ///< 自定义类型分类标记

        std::vector<UnityField> fields;                     ///< 字段列表
        std::vector<UnityMethod> methods;                   ///< 方法列表
        std::vector<std::string> interfaces;                ///< 实现的接口名称列表
        std::vector<VTableEntry> vtableEntries;             ///< 虚表条目列表

        uint16_t vtableCount;                               ///< 虚表条目数量

        /** @brief 默认构造 */
        UnityClass() : klassPtr(0), parentPtr(0), instanceSize(0), flags(0),vtableCount(0) {
            typeAttr.raw = 0;
        }

        /** @brief 参数化构造 */
        UnityClass(std::string ns, std::string name, uintptr_t ptr)
                : namespc(std::move(ns)), className(std::move(name)), klassPtr(ptr),
                  parentPtr(0), instanceSize(0), flags(0), vtableCount(0) {
            typeAttr.raw = 0;
        }

        /** @brief 获取类的全限定名（命名空间::类名） */
        std::string GetFullName() const {
            return namespc.empty() ? className : namespc + "::" + className;
        }
    };

    //========================= JSON 导出用的数据结构 =========================//

    /** @struct ScriptStringEntry @brief 脚本字符串条目 —— 地址 + 字符串值 */
    struct ScriptStringEntry {
        uintptr_t address;      ///< 字符串在内存中的地址
        std::string value;      ///< 字符串内容
    };

    /** @struct ScriptMetadataEntry @brief 脚本元数据条目 —— 地址 + 名称 + 签名 */
    struct ScriptMetadataEntry {
        uintptr_t address;      ///< 元数据在内存中的地址
        std::string name;       ///< 元数据名称
        std::string signature;  ///< 类型签名
    };

    /** @struct ScriptMetadataMethodEntry @brief 脚本元数据方法条目 —— 地址 + 方法名 + 方法指针 */
    struct ScriptMetadataMethodEntry {
        uintptr_t address;          ///< 元数据在内存中的地址
        std::string name;           ///< 方法名称
        uintptr_t methodAddress;    ///< 方法函数指针地址
    };

    /**
     * @struct MetadataLimits
     * @brief  元数据边界信息 —— 缓存各索引表的最大值，避免重复计算
     */
    struct MetadataLimits {
        uint32_t maxTypeIndex;              ///< 类型索引最大值
        uint32_t maxMethodDefIndex;         ///< 方法定义索引最大值
        uint32_t maxStringLiteralIndex;     ///< 字符串字面量索引最大值
        uint32_t maxFieldRefIndex;          ///< 字段引用索引最大值
        uint32_t maxMethodSpecIndex;        ///< 方法规格索引最大值
        uint32_t maxTypeDefIndex;           ///< 类型定义索引最大值
        uint32_t maxPairsIndex;             ///< 配对索引最大值
    };

    /**
     * @class li2cppHeader
     * @brief IDA 头文件与脚本 JSON 生成器
     *
     * @details 遍历所有 IL2CPP 类元数据，构建 UnityClass 映射表，然后导出为：
     *          1. IDA 可导入的 C 结构体头文件（.h）
     *          2. Il2CppDumper 格式的脚本方法 JSON（script.json）
     */
    class li2cppHeader :public li2cppApi::cUnityApi{
    public:
        /**
         * @brief 构造函数
         * @param dqil2cppBase              il2cpp 模块基地址
         * @param pCodeRegistration         代码注册表指针
         * @param pMetadataRegistration     元数据注册表指针
         * @param pGlobalMetadataHeader     全局元数据头指针
         * @param pMetadataImagesTable      元数据镜像表指针
         */
        li2cppHeader(void* dqil2cppBase=nullptr,
                     void *pCodeRegistration=nullptr,
                     void *pMetadataRegistration=nullptr,
                     void *pGlobalMetadataHeader=nullptr,
                     void* pMetadataImagesTable=nullptr);

        /** @brief 析构函数 */
        ~li2cppHeader();


    public:
        /** @brief 启动入口 —— 执行 Init、SaveToIdaHeader、SaveToMethodJson 的完整流程 */
        void start();

        /**
         * @brief   初始化类映射表 —— 遍历所有程序集/镜像/类并填充 classMap
         * @param   classMap 输出参数，类全名 -> UnityClass 的映射表
         */
        void Init( std::map<std::string, std::shared_ptr<UnityClass>>& classMap);

        /**
         * @brief   获取类的唯一名称（处理同名类冲突）
         * @param   classMap    类映射表
         * @param   targetClass 目标类
         * @param   type        类型指针
         * @return  唯一的类标识名称
         */
        std::string GetClassUniqueName( std::map<std::string, std::shared_ptr<UnityClass>>& classMap, std::shared_ptr<UnityClass>& targetClass,const Il2CppType* type);

        /**
         * @brief   导出指定类的所有字段信息到 UnityClass 对象
         * @param   klass       IL2CPP 类指针
         * @param   unityClass  目标 UnityClass 对象
         */
        void DumpFields(Il2CppClass* klass, std::shared_ptr<UnityClass>& unityClass);

        /**
         * @brief   获取 IDA 兼容的 C 类型名称
         * @param   type Il2CppType 指针
         * @return  IDA 兼容的类型名称字符串
         */
        std::string GetIdaCompatibleType(const Il2CppType* type);

        /**
         * @brief   获取类的安全唯一名称（处理特殊字符和泛型）
         * @param   klass IL2CPP 类指针
         * @return  安全的唯一标识名称
         */
        std::string GetSafeUniqueName(Il2CppClass* klass);

        /**
         * @brief   获取类型的内存大小（字节）
         * @param   type Il2CppType 指针
         * @return  类型大小
         */
        uint32_t GetTypeSize(const Il2CppType* type);

        /**
         * @brief   将类映射表导出为 IDA C 结构体头文件
         * @param   path 输出文件路径
         */
        void SaveToIdaHeader(const std::string& path);

        /**
         * @brief   获取泛型类型的安全名称
         * @param   type Il2CppType 指针
         * @return  泛型安全名称
         */
        std::string GetSafeGenericName(const Il2CppType* type);

        /**
         * @brief   清理标识符中的非法字符（替换为下划线等）
         * @param   name 原始名称
         * @return  清理后的合法标识符
         */
        std::string CleanIdentifier(std::string name);

        /**
         * @brief   导出指定类的所有方法信息到 UnityClass 对象
         * @param   klass       IL2CPP 类指针
         * @param   unityClass  目标 UnityClass 对象
         */
        void DumpMethods(Il2CppClass* klass, std::shared_ptr<UnityClass>& unityClass);

        /**
         * @brief   导出指定类的虚表信息到 UnityClass 对象
         * @param   klass       IL2CPP 类指针
         * @param   unityClass  目标 UnityClass 对象
         */
        void DumpVTable(Il2CppClass* klass, std::shared_ptr<UnityClass>& unityClass);

        /**
         * @brief   获取类型的缩写字符（用于方法签名生成，例如 int→"i", void→"v"）
         * @param   type 类型名称字符串
         * @return  单字符缩写
         */
        std::string GetTypeAbbreviation(const std::string& type);

        /**
         * @brief   将元数据信息导出为脚本方法 JSON 文件
         * @param   path 输出文件路径
         */
        void SaveToMethodJson(const std::string& path);

    private:
        //========================= SaveToMethodJson 拆分出的子函数 =========================//

        /**
         * @brief   通过 TypeDefinitionIndex 获取已解密的 "Namespace.ClassName" 字符串
         * @param   typeDefIndex    类型定义索引
         * @param   limits          元数据边界信息
         * @return  解密后的全限定类名
         */
        std::string GetDecryptedClassName(TypeDefinitionIndex typeDefIndex, const MetadataLimits& limits);

        /**
         * @brief   通过 token 匹配从 Il2CppClass 获取已解密的方法名
         * @param   klass   IL2CPP 类指针
         * @param   token   方法 Token
         * @return  解密后的方法名
         */
        std::string GetDecryptedMethodName(Il2CppClass* klass, uint32_t token);

        /**
         * @brief   收集 ScriptMethod 段数据并写入 JSON
         * @param   fout            输出文件流
         * @param   allAddresses    地址集合（用于去重）
         */
        void WriteScriptMethods(std::ofstream& fout, std::set<uintptr_t>& allAddresses);

        /**
         * @brief   遍历 metadataUsages 收集所有脚本元数据条目
         * @param   scriptStrings           输出的字符串条目列表
         * @param   scriptMetadata          输出的元数据条目列表
         * @param   scriptMetadataMethod    输出的元数据方法条目列表
         * @param   allAddresses            地址集合（用于去重）
         */
        void CollectMetadataUsages(std::vector<ScriptStringEntry>& scriptStrings,
                                   std::vector<ScriptMetadataEntry>& scriptMetadata,
                                   std::vector<ScriptMetadataMethodEntry>& scriptMetadataMethod,
                                   std::set<uintptr_t>& allAddresses);

        //========================= 各 usage 类型的处理函数 =========================//

        /** @brief 处理字符串字面量类型的 metadata usage */
        void HandleStringLiteral(uint32_t decodedIndex, uintptr_t usageAddr,
                                 const MetadataLimits& limits,
                                 std::vector<ScriptStringEntry>& out);

        /** @brief 处理类型元数据类型的 metadata usage */
        void HandleTypeMetadata(uint32_t decodedIndex, uintptr_t usageAddr,
                                const MetadataLimits& limits, const std::string& suffix,
                                const std::string& sig, std::vector<ScriptMetadataEntry>& out);

        /** @brief 处理字段信息类型的 metadata usage */
        void HandleFieldInfo(uint32_t decodedIndex, uintptr_t usageAddr,
                             const MetadataLimits& limits,
                             std::vector<ScriptMetadataEntry>& out);

        /** @brief 处理方法定义类型的 metadata usage */
        void HandleMethodDef(uint32_t decodedIndex, uintptr_t usageAddr,
                             const MetadataLimits& limits,
                             std::vector<ScriptMetadataMethodEntry>& out,
                             std::set<uintptr_t>& allAddresses);

        /** @brief 处理方法引用类型的 metadata usage */
        void HandleMethodRef(uint32_t decodedIndex, uintptr_t usageAddr,
                             const MetadataLimits& limits,
                             std::vector<ScriptMetadataMethodEntry>& out,
                             std::set<uintptr_t>& allAddresses);

        //========================= JSON 各段的写入函数 =========================//

        /** @brief 写入 ScriptString 段到 JSON */
        static void WriteScriptStringSection(std::ofstream& fout, const std::vector<ScriptStringEntry>& entries);

        /** @brief 写入 ScriptMetadata 段到 JSON */
        static void WriteScriptMetadataSection(std::ofstream& fout, const std::vector<ScriptMetadataEntry>& entries);

        /** @brief 写入 ScriptMetadataMethod 段到 JSON */
        static void WriteScriptMetadataMethodSection(std::ofstream& fout, const std::vector<ScriptMetadataMethodEntry>& entries);

        /** @brief 写入 Addresses 段到 JSON */
        static void WriteAddressesSection(std::ofstream& fout, const std::set<uintptr_t>& addresses);

    private:
        //========================= 成员变量 =========================//

        uint64_t m_il2cppbase = 0;                                              ///< il2cpp 模块基地址
        void* m_pGlobalMetadata =nullptr;                                       ///< 全局元数据内存指针
        Il2CppGlobalMetadataHeader *m_pGlobalMetadataHeader = nullptr;          ///< 全局元数据头
        Il2CppMetadataRegistration *m_pil2CppMetadataRegistration = nullptr;    ///< 元数据注册表
        Il2CppCodeRegistration *m_pIl2CppCodeRegistration = nullptr;            ///< 代码注册表
        Il2CppImageGlobalMetadata* m_pMetadataImagesTable = nullptr;            ///< 元数据镜像表

        std::map<std::string, std::shared_ptr<UnityClass>> m_classMap;          ///< 类全名 -> UnityClass 的快速查找映射表
        std::string m_packname = "com.tencent.lolm";                            ///< 目标应用包名

    };
}


#endif //DOBBY_PROJECT_IL2CPPHEADER_H
