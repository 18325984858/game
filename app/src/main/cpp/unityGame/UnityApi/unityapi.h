/**
 * @file    unityapi.h
 * @brief   Unity IL2CPP 运行时 API 封装层 —— 对 il2cpp 导出函数的统一 C++ 封装
 * @author  Song
 * @date    2025/12/07
 * @update  2026/03/05
 *
 * @details 封装了 il2cpp 运行时的核心 API（域管理、程序集/镜像枚举、类/字段/属性/方法的
 *          元数据反射、静态字段读取、泛型解析、字符串编解码等），所有函数通过符号查找
 *          动态绑定到 libil2cpp.so 的导出符号。作为整个项目的基础层，被 li2cppDumper、
 *          interface::function、il2cppHeader 等上层模块继承使用。
 */

#ifndef DOBBY_PROJECT_UNITYAPI_H
#define DOBBY_PROJECT_UNITYAPI_H

#include "../Symbol/Symbol.h"


#define UNITY_2018_4_16F1

#ifdef UNITY_2018_4_16F1
#include "../UnityStruct/unity2018-4-16f1/UnityStructInfo.h"
#else
#include "../UnityStruct/UnityInfo.h"
#endif

namespace li2cppApi {

    /**
     * @class cUnityApi
     * @brief IL2CPP 运行时 API 封装基类
     *
     * @details 通过 Symbol 模块在运行时动态查找 libil2cpp.so 中的导出函数，
     *          并以成员函数的形式提供给上层调用。首次调用时自动绑定函数指针并缓存。
     */
    class cUnityApi{
    public:
        /**
         * @brief 构造函数 —— 初始化 il2cpp 基地址和核心元数据指针
         * @param dqil2cppBase              il2cpp 模块基地址
         * @param pCodeRegistration         代码注册表指针
         * @param pMetadataRegistration     元数据注册表指针
         * @param pGlobalMetadataHeader     全局元数据头指针
         * @param pMetadataImagesTable      元数据镜像表指针
         */
        cUnityApi(void*dqil2cppBase=nullptr,
                  Il2CppCodeRegistration *pCodeRegistration=nullptr,
                  Il2CppMetadataRegistration *pMetadataRegistration=nullptr,
                  Il2CppGlobalMetadataHeader *pGlobalMetadataHeader=nullptr,
                  Il2CppImageGlobalMetadata*pMetadataImagesTable=nullptr);

        /** @brief 虚析构函数 */
        virtual ~cUnityApi();

    public:
        /** @brief 获取 libil2cpp.so 在内存中的模块基地址 */
        uint64_t Getil2cppModuleBase();

    public:
        //========================= 类型与类信息 API =========================//

        Il2CppClass* il2cpp_class_from_type(const Il2CppType* type);                            ///< 将 Il2CppType 类型转换成 Il2CppClass 类型
        const char*  il2cpp_class_get_name(Il2CppClass *klass);                                 ///< 获取类名
        const Il2CppAssembly** il2cpp_domain_get_assemblies(const Il2CppDomain* domain, size_t* size); ///< 获取所有已加载代码模块（程序集列表）
        Il2CppDomain* il2cpp_domain_get();                                                      ///< 获取当前进程的托管运行时域（全局根对象）
        const Il2CppImage* il2cpp_assembly_get_image(const Il2CppAssembly *assembly);           ///< 从程序集获取对应的镜像对象
        const char* il2cpp_image_get_name(const Il2CppImage *image);                            ///< 获取镜像（模块）名称
        const char* il2cpp_class_get_namespace(Il2CppClass *klass);                             ///< 获取类的命名空间
        Il2CppClass* il2cpp_class_get_parent(Il2CppClass *klass);                               ///< 获取父类型
        int32_t il2cpp_class_instance_size(Il2CppClass *klass);                                 ///< 获取类的实例大小（字节）

        Il2CppClass* il2cpp_class_get_declaring_type(Il2CppClass* klass);                       ///< 获取外围（声明）类型
        bool il2cpp_class_is_abstract(const Il2CppClass *klass);                                ///< 判断是否是抽象类
        bool il2cpp_class_is_interface(const Il2CppClass *klass);                               ///< 判断是否是接口
        bool il2cpp_class_is_enum(const Il2CppClass *klass);                                    ///< 判断是否是枚举
        const Il2CppClass* il2cpp_image_get_class(const Il2CppImage * image, size_t index);     ///< 按索引获取镜像中的类
        const Il2CppType* il2cpp_class_get_type(Il2CppClass *klass);                            ///< 获取类对应的 Il2CppType
        size_t il2cpp_image_get_class_count(const Il2CppImage * image);                         ///< 获取镜像中的类总数
        bool il2cpp_class_is_inited(const Il2CppClass *klass);                                  ///< 判断类是否已初始化
        const Il2CppType* GetIl2CppTypeFromIndex(TypeIndex index);                              ///< 通过索引获取 Il2CppType
        void* il2cpp_api_lookup_symbol(const char* name);                                       ///< 按名称遍历查找 il2cpp 导出符号
        bool il2cpp_is_debugger_attached();                                                     ///< 获取调试器附加状态

        //========================= 字段(Field) API =========================//

        const char* il2cpp_field_get_name(FieldInfo *field);                                    ///< 获取字段源代码名称
        int il2cpp_class_get_flags(const Il2CppClass *klass);                                   ///< 获取类的修饰符标志
        bool il2cpp_class_is_valuetype(const Il2CppClass* klass);                               ///< 判断是否是值类型
        Il2CppClass* il2cpp_class_get_interfaces(Il2CppClass *klass, void* *iter);              ///< 迭代获取类实现的所有接口
        FieldInfo* il2cpp_class_get_fields(Il2CppClass *klass, void* *iter);                    ///< 迭代获取类声明的所有字段元数据
        int il2cpp_field_get_flags(FieldInfo *field);                                           ///< 获取字段的修饰符标志
        const Il2CppType* il2cpp_field_get_type(FieldInfo *field);                              ///< 获取字段的数据类型信息
        void il2cpp_field_static_get_value(FieldInfo *field, void *value);                      ///< 获取静态字段在内存中的值
        size_t il2cpp_field_get_offset(FieldInfo *field);                                       ///< 获取字段在父类型实例中的内存偏移量

        //========================= 属性(Property) API =========================//

        const PropertyInfo* il2cpp_class_get_properties(Il2CppClass *klass, void* *iter);       ///< 迭代获取类定义的所有属性元数据
        const MethodInfo* il2cpp_property_get_get_method(PropertyInfo *prop);                   ///< 获取属性的 getter 方法
        const MethodInfo* il2cpp_property_get_set_method(PropertyInfo *prop);                   ///< 获取属性的 setter 方法
        const char* il2cpp_property_get_name(PropertyInfo *prop);                               ///< 获取属性名称

        //========================= 方法(Method) API =========================//

        uint32_t il2cpp_method_get_flags(const MethodInfo *method, uint32_t *iflags);           ///< 获取方法的修饰符标志
        const Il2CppType* il2cpp_method_get_return_type(const MethodInfo* method);              ///< 获取方法的返回值类型
        const Il2CppType* il2cpp_method_get_param(const MethodInfo *method, uint32_t index);    ///< 获取方法指定索引处的参数类型
        const MethodInfo* il2cpp_class_get_methods(Il2CppClass *klass, void* *iter);            ///< 迭代遍历类的所有方法
        uint32_t il2cpp_method_get_param_count(const MethodInfo *method);                       ///< 获取方法的参数数量
        const char* il2cpp_method_get_name(const MethodInfo *method);                           ///< 获取方法名称
        const char *il2cpp_method_get_param_name(const MethodInfo *method, uint32_t index);     ///< 获取方法指定索引处的参数名称
        bool _il2cpp_type_is_byref(const Il2CppType *type);                                    ///< 内部函数：判断类型是否为引用传递
        Il2CppClass* il2cpp_class_get_nested_types(Il2CppClass *klass, void* *iter);            ///< 迭代获取嵌套类型
        bool il2cpp_type_is_byref(const Il2CppType *type);                                     ///< 判断类型是否为 ref 引用传递类型
        bool il2cpp_method_is_generic(const MethodInfo *method);                                ///< 判断方法是否是泛型方法定义
        bool il2cpp_method_is_instance(const MethodInfo *method);                               ///< 判断方法是否是实例方法

        //========================= 元数据(Metadata)与泛型 API =========================//

        Il2CppMetadataUsage GetEncodedIndexType(EncodedMethodIndex index);                      ///< 获取编码索引的使用类型
        uint32_t GetDecodedMethodIndex(EncodedMethodIndex index);                               ///< 解码方法索引
        Il2CppString* GetStringLiteralFromIndex(StringLiteralIndex index);                      ///< 通过索引获取字符串字面量
        Il2CppString* il2cpp_string_new_len(const char* str, uint32_t length);                  ///< 创建指定长度的 IL2CPP 字符串
        std::string Utf16ToUtf8(const Il2CppChar *utf16String);                                 ///< UTF-16 转 UTF-8 字符串
        std::string Utf16ToUtf8(const Il2CppChar *utf16String, int maximumSize);                ///< UTF-16 转 UTF-8（限定最大长度）
        std::string il2cpp_Il2CppString_toCString(const Il2CppString* pstr);                    ///< Il2CppString 转 C++ std::string
        const Il2CppGenericMethod* GetGenericMethodFromIndex(GenericMethodIndex index);          ///< 通过索引获取泛型方法
        const MethodInfo* GetMethodInfoFromMethodDefinitionIndex(MethodIndex index);            ///< 通过方法定义索引获取 MethodInfo
        const Il2CppMethodDefinition* GetMethodDefinitionFromIndex(MethodIndex index);          ///< 通过索引获取方法定义
        const char* GetStringFromIndex(StringIndex index);                                      ///< 通过索引获取元数据字符串
        const char* GetWindowsRuntimeStringFromIndex(StringIndex index);                        ///< 通过索引获取 Windows Runtime 字符串
        const Il2CppGenericClass* il2cpp_class_get_generic_class(const Il2CppClass *klass);     ///< 获取类的泛型类信息
        const Il2CppGenericContext* il2cpp_generic_class_get_context(const Il2CppGenericClass* klass); ///< 获取泛型类的上下文
        const Il2CppGenericInst * il2cpp_generic_context_get_generic_class_inst(const Il2CppGenericContext* context); ///< 获取泛型上下文的实例化信息
        const Il2CppTypeDefinition* GetTypeDefinitionForIndex(TypeDefinitionIndex index);       ///< 通过索引获取类型定义
        const char* il2cpp_type_get_name(const Il2CppType *type);                               ///< 获取类型的名称字符串
        int il2cpp_class_get_rank(const Il2CppClass *klass);                                    ///< 获取数组类型的维度
        Il2CppClass* il2cpp_type_get_class_or_element_class(const Il2CppType *type);            ///< 获取类型对应的类或数组元素类
        void il2cpp_runtime_class_init(Il2CppClass* klass);                                     ///< 手动触发类的运行时初始化
        Il2CppClass* il2cpp_field_get_parent(FieldInfo *field);                                 ///< 获取字段所属的父类
        void* il2cpp_class_init_all_method(Il2CppClass* klass);                                 ///< 初始化类的所有方法指针
        Il2CppReflectionType* il2cpp_type_get_object(const Il2CppType* type);                   ///< 将 Il2CppType 转换为 System.Type 反射对象
        Il2CppObject* il2cpp_runtime_invoke(const MethodInfo* method, void* obj, void** params, Il2CppException** exc); ///< 通过 MethodInfo 调用方法

    public:
        //========================= 模板工具函数 =========================//

        /** @brief 判断运行时元数据是否已初始化 */
        template<typename T>
        static inline bool IsRuntimeMetadataInitialized(T item);

        /** @brief UTF-16 转 UTF-8 迭代器实现 */
        template <typename u16bit_iterator, typename octet_iterator>
        static octet_iterator utf16to8 (u16bit_iterator start, u16bit_iterator end, octet_iterator result);

        /** @brief 将 Unicode 码点追加到 UTF-8 输出迭代器 */
        template <typename octet_iterator>
        static octet_iterator append(uint32_t cp, octet_iterator result);

        /** @brief 从元数据中按偏移和索引获取指定类型的数据 */
        template<typename T>
        static T MetadataOffset(const void* metadata, size_t sectionOffset, size_t itemIndex);

    public:
        //========================= UTF-16 辅助函数 =========================//

        /** @brief 判断是否为 UTF-16 尾随代理（trail surrogate） */
        template <typename u16>
        inline static bool is_trail_surrogate(u16 cp);

        /** @brief 对 UTF-16 字符进行 16 位掩码 */
        template<typename u16_type>
        inline static uint16_t mask16(u16_type oc);

        /** @brief 判断是否为 UTF-16 前导代理（lead surrogate） */
        template <typename u16>
        inline static bool is_lead_surrogate(u16 cp);

    public:
        //========================= 成员变量 =========================//

        const std::string m_il2cppStr = "libil2cpp.so";                     ///< il2cpp 动态库名称
        std::shared_ptr<Symbol::Symbol> m_li2cppso = nullptr;               ///< il2cpp 符号查找器实例
        uint64_t m_il2cppbase = 0;                                          ///< il2cpp 模块基地址

    public:
        //std::shared_ptr<lol::lol> m_lolgame = nullptr;
        void* m_pGlobalMetadata =nullptr;                                   ///< 全局元数据内存指针
        Il2CppGlobalMetadataHeader *m_pGlobalMetadataHeader = nullptr;      ///< 全局元数据头
        Il2CppMetadataRegistration *m_pil2CppMetadataRegistration = nullptr;///< 元数据注册表
        Il2CppCodeRegistration *m_pIl2CppCodeRegistration = nullptr;        ///< 代码注册表
        Il2CppImageGlobalMetadata* m_pMetadataImagesTable = nullptr;        ///< 元数据镜像表
    };
};

#endif //DOBBY_PROJECT_UNITYAPI_H