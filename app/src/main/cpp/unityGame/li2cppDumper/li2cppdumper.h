/**
 * @file    li2cppdumper.h
 * @brief   IL2CPP Dumper 核心类 —— 将 IL2CPP 运行时元数据导出为 C# 伪代码（dump.cs）和字符串表
 * @author  Song
 * @date    2025/11/19
 * @update  2026/03/05
 *
 * @details 继承自 cUnityApi，遍历所有程序集/镜像/类，提取字段、属性、方法的完整签名信息，
 *          支持泛型方法解析和运行时元数据（metadataUsages）导出。输出结果包括：
 *          - dump.cs：类似 C# 源码的反编译文本
 *          - dumpstr：字符串字面量表
 *          - 泛型方法地址表
 */

#ifndef DOBBY_PROJECT_LI2CPPDUMPER_H
#define DOBBY_PROJECT_LI2CPPDUMPER_H


#include "../UnityApi/unityapi.h"
#include "../../core/file/file.h"
#include <map>
#include <list>
#include <unordered_map> // 必须添加，用于性能优化
#include <vector>
#include <memory>
#include <string>

namespace li2cpp {

    /**
     * @struct  cMethodDefinitionAndMethodSpec
     * @brief   方法定义与泛型规格的组合结构 —— 用于存储泛型方法的定义、规格和地址
     */
    typedef struct cMethodDefinitionAndMethodSpec{
        const Il2CppMethodDefinition* Method;   ///< 方法定义指针
        const Il2CppMethodSpec* Spec;           ///< 泛型方法规格指针
        uint64_t offset;                        ///< 方法在内存中的地址偏移
    }cMethodDefinitionAndMethodSpec,*cPMethodDefinitionAndMethodSpec;

    /**
     * @class li2cppDumper
     * @brief IL2CPP 元数据导出器
     *
     * @details 提供 dumpcs()、dumpStr()、dumpGenericsMethod() 等核心导出接口，
     *          以及 dump_field()、dump_property()、dump_method() 等按类逐项导出的子函数。
     */
    class li2cppDumper : public li2cppApi::cUnityApi {
    public:
        /**
         * @brief 构造函数 —— 初始化 il2cpp 基地址和核心元数据指针
         * @param dqil2cppBase              il2cpp 模块基地址
         * @param pCodeRegistration         代码注册表指针
         * @param pMetadataRegistration     元数据注册表指针
         * @param pGlobalMetadataHeader     全局元数据头指针
         * @param pMetadataImagesTable      元数据镜像表指针
         */
        li2cppDumper(void* dqil2cppBase=nullptr,
                     void *pCodeRegistration=nullptr,
                     void *pMetadataRegistration=nullptr,
                     void *pGlobalMetadataHeader=nullptr,
                     void* pMetadataImagesTable=nullptr);

        /** @brief 虚析构函数 */
        virtual ~li2cppDumper();

    public:
        /** @brief 初始化信息（泛型方法列表、文件路径等），在 dump 之前调用 */
        bool initInfo();

        /** @brief 导出所有类信息为 C# 伪代码字符串 */
        std::string dumpcs();

        /**
         * @brief   导出 Il2CppType 的类型名称字符串
         * @param   type        类型指针
         * @param   classIndex  类索引，默认 -1
         * @return  类型名称字符串
         */
        std::string dumpType(const Il2CppType *type, int classIndex = -1);

        /** @brief 导出指定类的所有字段信息 */
        std::string dump_field(Il2CppClass *klass);

        /** @brief 导出指定类的所有属性信息 */
        std::string dump_property(Il2CppClass *klass);

        /** @brief 获取方法的访问修饰符字符串（public/private/static 等） */
        std::string get_method_modifier(uint32_t flags);

        /** @brief 导出指定类的所有方法信息 */
        std::string dump_method(Il2CppClass *klass);

        /** @brief 导出字符串字面量表 */
        std::string dumpStr();

        /**
         * @brief   导出运行时元数据使用信息
         * @param   metadataPointer 元数据指针数组
         * @return  导出的元数据字符串
         */
        std::string dump_RuntimeMetadata(uintptr_t* metadataPointer);

        /** @brief 导出所有泛型方法信息 */
        std::string dumpGenericsMethod();

        /** @brief 获取 Il2CppType 的简短类型名 */
        std::string get_type_name(const Il2CppType* type);

        /** @brief 获取泛型方法的命名空间名称 */
        std::string get_method_space_name(const Il2CppMethodSpec* spec);

        /** @brief 获取泛型实例化参数列表字符串 */
        std::string get_method_generic_name(GenericInstIndex genericInstIndex);

    private:
        /**
         * @brief   初始化包名路径（用于确定输出文件存储位置）
         * @param   strPackName 应用包名
         */
        void initPackPath(std::string strPackName);

    public:
        /**
         * @brief   根据 Token 查找匹配的泛型方法列表
         * @param   Token 方法的元数据 Token
         * @return  匹配的方法定义与规格的链表
         */
        std::shared_ptr<std::list<std::shared_ptr<li2cpp::cMethodDefinitionAndMethodSpec>>> GetMethodToList(uint32_t Token);

    public:
        /// 存储所有已解析的泛型方法数据
        std::shared_ptr<std::list<std::shared_ptr<cMethodDefinitionAndMethodSpec>>> m_methodList = nullptr;

    private:
        /// 性能优化辅助：用于根据 Token 快速查找 Spec 实例的多值哈希映射
        std::shared_ptr<std::unordered_multimap<uint32_t, std::shared_ptr<cMethodDefinitionAndMethodSpec>>> m_methodMap = nullptr;

        /**
         * @brief   写入日志到文件
         * @param   str 日志内容
         * @return  成功返回 true，失败返回 false
         */
        bool writeLog(std::string str);

    private:
        std::string m_pathlog = "";                                 ///< 日志文件路径
        std::shared_ptr<cMyfile> m_outlog = nullptr;                ///< 日志文件输出流

    private:
        std::string m_pathDumpCs = "";                              ///< dump.cs 输出文件路径
        std::shared_ptr<cMyfile> m_outDumpCs = nullptr;             ///< dump.cs 文件输出流
        std::shared_ptr<std::vector<std::string>> m_outPuts = nullptr; ///< 输出缓冲区

    private:
        std::string m_pathDumpstr = "";                             ///< 字符串表输出文件路径
        std::shared_ptr<cMyfile> m_outDumpstr = nullptr;            ///< 字符串表文件输出流
        std::shared_ptr<std::map<int,std::string>> m_kIl2CppMetadataUsageStringLiteral = nullptr; ///< 元数据字符串字面量索引映射
    };
};

#endif //DOBBY_PROJECT_LI2CPPDUMPER_H