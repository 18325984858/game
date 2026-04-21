//
// Created by Song on 2025/11/19.
//

#include "li2cppdumper.h"
#include "../../core/log/log.h"
#include <sstream>
#include <vector>
#include <iomanip>
#include <unordered_map>
#include <android/log.h>

#define MAX_BUF_VALUE 0x1000

li2cpp::li2cppDumper::li2cppDumper(void* dqil2cppBase,
                                   void *pCodeRegistration,
                                   void *pMetadataRegistration,
                                   void *pGlobalMetadataHeader,
                                   void* pMetadataImagesTable) :
        li2cppApi::cUnityApi(dqil2cppBase,
                             static_cast<Il2CppCodeRegistration *>(pCodeRegistration),
                             static_cast<Il2CppMetadataRegistration *>(pMetadataRegistration),
                             static_cast<Il2CppGlobalMetadataHeader *>(pGlobalMetadataHeader),
                             static_cast<Il2CppImageGlobalMetadata *>(pMetadataImagesTable)) {

    initPackPath("com.tencent.lolm");

    // 初始化文件输出流
    auto initFile = [&](std::shared_ptr<cMyfile>& file, const std::string& path) {
        if (file == nullptr) {
            file = std::make_shared<cMyfile>(path);
            if (file) file->openFile(cMyfile::FileMode::ReadWrite);
        }
    };

    initFile(m_outDumpCs, m_pathDumpCs);
    initFile(m_outDumpstr, m_pathDumpstr);
    initFile(m_outlog, m_pathlog);

    // 初始化容器
    if (m_outPuts == nullptr) m_outPuts = std::make_shared<std::vector<std::string>>();
    if (m_kIl2CppMetadataUsageStringLiteral == nullptr) m_kIl2CppMetadataUsageStringLiteral = std::make_shared<std::map<int, std::string>>();

    // 初始化方法索引表 (保留你原有追踪逻辑的同时，提升查询效率)
    if (m_methodList == nullptr) m_methodList = std::make_shared<std::list<std::shared_ptr<cMethodDefinitionAndMethodSpec>>>();
    if(m_methodMap == nullptr) m_methodMap = std::make_shared<std::unordered_multimap<uint32_t, std::shared_ptr<cMethodDefinitionAndMethodSpec>>>();
}

li2cpp::li2cppDumper::~li2cppDumper() {




    // 析构时统一冲刷缓冲区并关闭文件
    if (m_outDumpCs) {
        writeLog("析构 m_outDumpCs ");
        m_outDumpCs->flush();
        m_outDumpCs->closeFile();
    }

    if (m_outDumpstr) {
        writeLog("析构 m_outDumpstr ");
        m_outDumpstr->flush();
        m_outDumpstr->closeFile();
    }

    if (m_outlog) {
        writeLog("析构 m_outlog ");
        m_outlog->flush();
        m_outlog->closeFile();
    }

    if(m_kIl2CppMetadataUsageStringLiteral){
        writeLog("析构 m_kIl2CppMetadataUsageStringLiteral ");
        m_kIl2CppMetadataUsageStringLiteral->clear();
        m_kIl2CppMetadataUsageStringLiteral = nullptr;
    }

    if(m_methodMap){
        for (auto& it : *this->m_methodMap) {
            it.second = nullptr;
        }
        writeLog("析构 m_methodMap ");
        m_methodMap->clear();
        m_methodMap = nullptr;
    }

    if(m_methodList){
        writeLog("析构 m_methodList ");
        m_methodList->clear();
        m_methodList = nullptr;
    }

    if(m_outPuts){
        writeLog("析构 m_outPuts ");
        m_outPuts->clear();
        m_outPuts = nullptr;
    }


}

bool li2cpp::li2cppDumper::initInfo() {
    if (m_pGlobalMetadataHeader == nullptr || m_pil2CppMetadataRegistration == nullptr) return false;

    char szbuf[0x1000] = {0};
    //sprintf(szbuf, "genericClassesCount : %d", m_pil2CppMetadataRegistration->genericClassesCount); writeLog(szbuf);
    //sprintf(szbuf, "typesCount : %d", m_pil2CppMetadataRegistration->typesCount); writeLog(szbuf);

    // 1. 预处理：建立泛型方法哈希表 (保留并优化你的 dumpGenericsMethod)
    dumpGenericsMethod();

    // 2. 导出类结构 (边解析边实时写入文件，防止大内存占用)
    dumpcs();

    // 3. 导出字符串
    dumpStr();

    return true;
}

/**
 * 核心类型解析：支持泛型实例、嵌套类、一维/多维数组
 */
std::string li2cpp::li2cppDumper::get_type_name(const Il2CppType* type) {
    if (!type) return "void";
    std::string suffix = il2cpp_type_is_byref(type) ? "&" : "";
    Il2CppClass* klass = il2cpp_class_from_type(type);

    // 处理数组 (SZARRAY 一维, ARRAY 多维)
    if (type->type == IL2CPP_TYPE_SZARRAY) {
        return get_type_name(type->data.type) + "[]" + suffix;
    } else if (type->type == IL2CPP_TYPE_ARRAY) {
        int rank = klass?il2cpp_class_get_rank(klass):type->data.array->rank;
        std::string rankStr = "[";
        for (int i = 1; i < rank; i++) rankStr += ",";
        rankStr += "]";

        return get_type_name(il2cpp_class_get_type(il2cpp_type_get_class_or_element_class(type)));
    }

    switch (type->type) {
        case IL2CPP_TYPE_VOID:    return "void" + suffix;
        case IL2CPP_TYPE_BOOLEAN: return "bool" + suffix;
        case IL2CPP_TYPE_I4:      return "int" + suffix;
        case IL2CPP_TYPE_U4:      return "uint" + suffix;
        case IL2CPP_TYPE_I8:      return "long" + suffix;
        case IL2CPP_TYPE_R4:      return "float" + suffix;
        case IL2CPP_TYPE_R8:      return "double" + suffix;
        case IL2CPP_TYPE_STRING:  return "string" + suffix;
        case IL2CPP_TYPE_OBJECT:  return "object" + suffix;
        case IL2CPP_TYPE_VAR:
        case IL2CPP_TYPE_MVAR:    return il2cpp_type_get_name(type) + suffix;
        default: break;
    }

    if (!klass) return "unknown" + suffix;

    std::string typeName = il2cpp_class_get_name(klass);

    // 递归解析嵌套类路径 (Outer.Inner)
    Il2CppClass* declaring = il2cpp_class_get_declaring_type(klass);
    if (declaring) {
        typeName = get_type_name(&declaring->byval_arg) + "." + typeName;
    }

    // 清理反引号 (Dictionary`2 -> Dictionary)
    size_t backtickPos = typeName.find_last_of('`');
    if (backtickPos != std::string::npos) typeName = typeName.substr(0, backtickPos);

    // 解析泛型实例参数 <T1, T2>
    if (type->type == IL2CPP_TYPE_GENERICINST) {
        const Il2CppGenericContext* context = &type->data.generic_class->context;
        const Il2CppGenericInst* inst = context->class_inst ? context->class_inst : context->method_inst;

        if (inst && inst->type_argc > 0) {
            typeName += "<";
            for (uint32_t i = 0; i < inst->type_argc; ++i) {
                if (i > 0) typeName += ", ";
                typeName += get_type_name(inst->type_argv[i]);
            }
            typeName += ">";
        }
    }
    return typeName + suffix;
}

/**
 * 类定义解析
 */
std::string li2cpp::li2cppDumper::dumpType(const Il2CppType *type, int classIndex) {
    std::stringstream outPut;
    auto *klass = il2cpp_class_from_type(type);
    if (!klass) return "";

    outPut << "\n// Namespace: " << il2cpp_class_get_namespace(klass) << "\n";

    auto flags = il2cpp_class_get_flags(klass);
    if (flags & TYPE_ATTRIBUTE_SERIALIZABLE) outPut << "[Serializable]\n";

    // 类修饰符
    auto visibility = flags & TYPE_ATTRIBUTE_VISIBILITY_MASK;
    switch (visibility) {
        case TYPE_ATTRIBUTE_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_PUBLIC:     outPut << "public "; break;
        case TYPE_ATTRIBUTE_NOT_PUBLIC:
        case TYPE_ATTRIBUTE_NESTED_ASSEMBLY:   outPut << "internal "; break;
        case TYPE_ATTRIBUTE_NESTED_PRIVATE:    outPut << "private "; break;
        case TYPE_ATTRIBUTE_NESTED_FAMILY:     outPut << "protected "; break;
        case TYPE_ATTRIBUTE_NESTED_FAM_OR_ASSEM: outPut << "protected internal "; break;
    }

    if ((flags & TYPE_ATTRIBUTE_ABSTRACT) && (flags & TYPE_ATTRIBUTE_SEALED)) outPut << "static ";
    else if (!(flags & TYPE_ATTRIBUTE_INTERFACE) && (flags & TYPE_ATTRIBUTE_ABSTRACT)) outPut << "abstract ";
    else if (!il2cpp_class_is_valuetype(klass) && !il2cpp_class_is_enum(klass) && (flags & TYPE_ATTRIBUTE_SEALED)) outPut << "sealed ";

    if (flags & TYPE_ATTRIBUTE_INTERFACE) outPut << "interface ";
    else if (il2cpp_class_is_enum(klass)) outPut << "enum ";
    else if (il2cpp_class_is_valuetype(klass)) outPut << "struct ";
    else outPut << "class ";

    outPut << get_type_name(type);

    // 继承与接口
    std::vector<std::string> extends;
    auto parent = il2cpp_class_get_parent(klass);
    if (!il2cpp_class_is_valuetype(klass) && !il2cpp_class_is_enum(klass) && parent) {
        auto p_type = il2cpp_class_get_type(parent);
        if (p_type->type != IL2CPP_TYPE_OBJECT) extends.emplace_back(get_type_name(p_type));
    }
    void *itf_iter = nullptr;
    while (auto itf = il2cpp_class_get_interfaces(klass, &itf_iter)) {
        extends.emplace_back(get_type_name(il2cpp_class_get_type(itf)));
    }
    if (!extends.empty()) {
        outPut << " : " << extends[0];
        for (size_t i = 1; i < extends.size(); ++i) outPut << ", " << extends[i];
    }

    if (classIndex >= 0) {
        outPut << " // TypeDefIndex : " << classIndex;
    }

    outPut << "\n{";
    outPut << dump_field(klass);
    outPut << dump_property(klass);

    // 初始化所有方法指针后再 dump（确保 methodPointer 可用）
    // 注意: 不调用 il2cpp_runtime_class_init, 它会触发 .cctor 可能崩溃
    il2cpp_class_init_all_method(klass);

    outPut << dump_method(klass);
    outPut << "}\n";

    return outPut.str();
}

/**
 * 字段解析
 */
std::string li2cpp::li2cppDumper::dump_field(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Fields\n";
    void *iter = nullptr;
    while (auto field = il2cpp_class_get_fields(klass, &iter)) {
        outPut << "\t";
        auto attrs = il2cpp_field_get_flags(field);
        auto access = attrs & FIELD_ATTRIBUTE_FIELD_ACCESS_MASK;

        switch (access) {
            case FIELD_ATTRIBUTE_PRIVATE: outPut << "private "; break;
            case FIELD_ATTRIBUTE_PUBLIC:  outPut << "public "; break;
            case FIELD_ATTRIBUTE_FAMILY:  outPut << "protected "; break;
            case FIELD_ATTRIBUTE_FAM_OR_ASSEM: outPut << "protected internal "; break;
        }

        if (attrs & FIELD_ATTRIBUTE_LITERAL) outPut << "const ";
        else {
            if (attrs & FIELD_ATTRIBUTE_STATIC) outPut << "static ";
            if (attrs & FIELD_ATTRIBUTE_INIT_ONLY) outPut << "readonly ";
        }

        outPut << get_type_name(il2cpp_field_get_type(field)) << " " << il2cpp_field_get_name(field);

        if ((attrs & FIELD_ATTRIBUTE_LITERAL) && il2cpp_class_is_enum(klass)) {
            uint64_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            outPut << " = " << std::dec << val;
        }
        outPut << "; // 0x" << std::hex << il2cpp_field_get_offset(field) << "\n";
    }
    return outPut.str();
}

/**
 * 方法解析 (支持泛型实例追踪)
 */
std::string li2cpp::li2cppDumper::dump_method(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Methods\n";
    void *iter = nullptr;
    while (auto method = il2cpp_class_get_methods(klass, &iter)) {


        if (method->methodPointer) {
            outPut << "\t// RVA: 0x" << std::hex
                   << (uint64_t) method->methodPointer - Getil2cppModuleBase()
                   << " VA: 0x" << (uint64_t) method->methodPointer << "\n";
        } else {
            outPut << "\t// RVA: -1\n";
        }

        outPut << "\t";
        uint32_t iflags = 0;
        outPut << get_method_modifier(il2cpp_method_get_flags(method, &iflags));

        auto return_type = il2cpp_method_get_return_type(method);
        outPut << (return_type->byref ? "ref " : "") << get_type_name(return_type) << " "
               << il2cpp_method_get_name(method) << "(";

        auto param_count = il2cpp_method_get_param_count(method);
        for (int i = 0; i < param_count; ++i) {
            auto param = il2cpp_method_get_param(method, i);
            if (param->byref) {
                if (param->attrs & PARAM_ATTRIBUTE_OUT) outPut << "out ";
                else if (param->attrs & PARAM_ATTRIBUTE_IN) outPut << "in ";
                else outPut << "ref ";
            }
            outPut << get_type_name(param) << " " << il2cpp_method_get_param_name(method, i);
            if (i < param_count - 1) outPut << ", ";
        }
        outPut << ") { }\n";


        // 处理泛型方法定义与其对应的具体实例 (RVA)
        //if (method->methodPointer) {
        // 使用优化后的哈希表查询该 Token 关联的所有泛型实例
        //auto range = m_methodMap->equal_range(method->token);
        //for (auto it = range.first; it != range.second; ++it) {
        //   auto elem = it->second;
        //    std::string specName = get_method_space_name(elem->Spec);
        //    outPut << "\t// [GenericInstance] RVA: 0x" << std::hex << elem->offset << " Spec: " << specName << "\n";
        // }
        //}

        // 处理泛型方法定义与其对应的具体实例 (RVA)
        //if (method->methodPointer == nullptr) {
            auto range = m_methodMap->equal_range(method->token);

            if (range.first != range.second) {
                outPut << "\t// --- Generic Instances Found ---\n"; // 添加一个分割线
                for (auto it = range.first; it != range.second; ++it) {
                    auto elem = it->second;
                    std::string specName = get_method_space_name(elem->Spec);

                    // 优化布局：使用 └─ 符号，并统一十六进制宽度为 8 位（补零）
                    char buffer[MAX_BUF_VALUE] = {0};
                    snprintf(buffer, sizeof(buffer),
                             "\t//   └─ [Generic] RVA: 0x%08llX | Spec: %s\n",
                             (unsigned long long) elem->offset, specName.c_str());

                    outPut << buffer;
                }
            }
        //}
    }
    return outPut.str();
}

/**
 * 建立泛型方法表索引 (保留原有逻辑并加入哈希映射)
 */
std::string li2cpp::li2cppDumper::dumpGenericsMethod() {
    if (!m_methodMap) return "";
    m_methodMap->clear();
    LOG(LOG_LEVEL_INFO,"genericMethodTableCount : %d",m_pil2CppMetadataRegistration->genericMethodTableCount);

    for (int32_t i = 0; i < m_pil2CppMetadataRegistration->genericMethodTableCount; i++) {
        const Il2CppGenericMethodFunctionsDefinitions* genericMethodIndices = m_pil2CppMetadataRegistration->genericMethodTable + i;
        if (genericMethodIndices && genericMethodIndices->genericMethodIndex < m_pil2CppMetadataRegistration->methodSpecsCount) {
            const Il2CppMethodSpec *methodSpec = m_pil2CppMetadataRegistration->methodSpecs + genericMethodIndices->genericMethodIndex;
            if (methodSpec && m_pIl2CppCodeRegistration->genericMethodPointersCount > genericMethodIndices->indices.methodIndex) {
                const Il2CppMethodDefinition *methodDefinition = GetMethodDefinitionFromIndex(methodSpec->methodDefinitionIndex);
                const Il2CppMethodPointer pMethodPointer = m_pIl2CppCodeRegistration->genericMethodPointers[genericMethodIndices->indices.methodIndex];

                uint64_t offset = pMethodPointer ? (uint64_t)pMethodPointer - Getil2cppModuleBase() : 0;
                if (methodDefinition) {
                    auto p = std::make_shared<li2cpp::cMethodDefinitionAndMethodSpec>();
                    p->Spec = methodSpec;
                    p->offset = offset;
                    p->Method = methodDefinition;

                    // 1. 保留在原来的 list 里 (你使用的代码)
                    m_methodList->push_back(p);
                    // 2. 存入哈希表用于 dump_method 快速检索 (优化部分)
                    m_methodMap->emplace(methodDefinition->token, p);
                }
            }
        }
    }
    return "";
}

/**
 * 遍历程序集进行总 Dump (改为实时流式写入)
 */
std::string li2cpp::li2cppDumper::dumpcs() {

    /*il2cpp_domain_get il2cpp_domain_get_assemblies 这两个API可能会被加密*/

    auto domain = il2cpp_domain_get();
    size_t size;
    auto assemblies = il2cpp_domain_get_assemblies(domain, &size);
    for (size_t i = 0; i < size; ++i) {
        auto image = il2cpp_assembly_get_image(assemblies[i]);
        auto classCount = il2cpp_image_get_class_count(image);
        const char* imageName = il2cpp_image_get_name(image);

        for (int j = 0; j < classCount; ++j) {
            auto klass = il2cpp_image_get_class(image, j);
            if (!klass) continue;
            auto type = il2cpp_class_get_type(const_cast<Il2CppClass *>(klass));

            std::string content = "\n// Image: " + std::string(imageName) + dumpType(type, j);

            // 实时写入文件，防止大项目 vector 爆内存
            if (m_outDumpCs) m_outDumpCs->writeLine(content);
        }
    }
    return "";
}

/**
 * 辅助：获取方法定义的完整泛型空间名
 */
std::string li2cpp::li2cppDumper::get_method_space_name(const Il2CppMethodSpec* spec) {
    if (!spec) return "";
    const Il2CppMethodDefinition* methodDef = GetMethodDefinitionFromIndex(spec->methodDefinitionIndex);
    if (!methodDef) return "";

    const Il2CppTypeDefinition* typeDef = GetTypeDefinitionForIndex(methodDef->declaringType);
    if (!typeDef) return "";

    std::string className = GetStringFromIndex(typeDef->nameIndex);
    LOG(LOG_LEVEL_INFO,"className : %s",className.c_str());

    size_t backtickPos = className.find_last_of('`');
    if (backtickPos != std::string::npos) className = className.substr(0, backtickPos);

    std::string classGenerics = get_method_generic_name(spec->classIndexIndex);
    std::string methodGenerics = get_method_generic_name(spec->methodIndexIndex);
    LOG(LOG_LEVEL_INFO,"classGenerics : %s methodGenerics : %s",classGenerics.c_str(),methodGenerics.c_str());

    return className + classGenerics + "." + GetStringFromIndex(methodDef->nameIndex) + methodGenerics;
}

/**
 * 辅助：获取泛型实例的具体参数字符串 <T...>
 */
std::string li2cpp::li2cppDumper::get_method_generic_name(GenericInstIndex index) {
    if (index == kTypeIndexInvalid || index >= (GenericInstIndex)m_pil2CppMetadataRegistration->genericInstsCount) return "";
    const Il2CppGenericInst* inst = m_pil2CppMetadataRegistration->genericInsts[index];
    if (!inst || inst->type_argc == 0 || !inst->type_argv) return "";

    std::string ret = "<";
    for (uint32_t i = 0; i < inst->type_argc; ++i) {
        if (!inst->type_argv[i]) {
            LOG(LOG_LEVEL_ERROR, "Generic arg %d is null, index: %d", i, index);
            continue; // 跳过空参数，而非崩溃
        }

        if (i > 0) ret += ", ";
        LOG(LOG_LEVEL_INFO,"inst->type_argv[i] : %p",inst->type_argv[i]);
        ret += get_type_name(inst->type_argv[i]);
    }
    ret += ">";

    LOG(LOG_LEVEL_INFO,"type_argc : %d",inst->type_argc);

    return ret;
}

/**
 * 属性 Dump
 * 支持泛型属性类型解析
 */
std::string li2cpp::li2cppDumper::dump_property(Il2CppClass *klass) {
    std::stringstream outPut;
    outPut << "\n\t// Properties\n";
    void *iter = nullptr;
    while (auto prop_const = il2cpp_class_get_properties(klass, &iter)) {
        // 部分 Il2Cpp 版本返回的是 const，需要转换以调用获取方法
        auto prop = const_cast<PropertyInfo *>(prop_const);
        auto get = il2cpp_property_get_get_method(prop);
        auto set = il2cpp_property_get_set_method(prop);
        auto prop_name = il2cpp_property_get_name(prop);

        outPut << "\t";
        const Il2CppType* prop_type = nullptr;
        uint32_t iflags = 0;

        // 确定属性类型：优先从 get 方法获取返回类型，否则从 set 方法获取第一个参数类型
        if (get) {
            outPut << get_method_modifier(il2cpp_method_get_flags(get, &iflags));
            prop_type = il2cpp_method_get_return_type(get);
        } else if (set) {
            outPut << get_method_modifier(il2cpp_method_get_flags(set, &iflags));
            prop_type = il2cpp_method_get_param(set, 0);
        }

        if (prop_type) {
            outPut << get_type_name(prop_type) << " " << prop_name << " { ";
            if (get) outPut << "get; ";
            if (set) outPut << "set; ";
            outPut << "}\n";
        }
    }
    return outPut.str();
}

/**
 * 方法修饰符解析 (优化版)
 * 修正了抽象/虚函数/重写的逻辑冲突，并添加了 sealed 支持
 */
std::string li2cpp::li2cppDumper::get_method_modifier(uint32_t flags) {
    std::stringstream outPut;

    // 1. 访问权限 (Access Flags)
    auto access = flags & METHOD_ATTRIBUTE_MEMBER_ACCESS_MASK;
    switch (access) {
        case METHOD_ATTRIBUTE_PRIVATE:        outPut << "private "; break;
        case METHOD_ATTRIBUTE_PUBLIC:         outPut << "public "; break;
        case METHOD_ATTRIBUTE_FAMILY:         outPut << "protected "; break;
        case METHOD_ATTRIBUTE_ASSEM:          outPut << "internal "; break;
        case METHOD_ATTRIBUTE_FAM_AND_ASSEM:  outPut << "private protected "; break;
        case METHOD_ATTRIBUTE_FAM_OR_ASSEM:   outPut << "protected internal "; break;
        case METHOD_ATTRIBUTE_COMPILER_CONTROLLED: outPut << "[compiler_controlled] "; break;
        default: break;
    }

    // 2. 静态修饰符
    if (flags & METHOD_ATTRIBUTE_STATIC) {
        outPut << "static ";
    }

    // 3. 虚函数逻辑处理 (Virtual, Abstract, Override, Sealed)
    // 注意：在 IL2CPP 中，Abstract 必然也是 Virtual 的
    if (flags & METHOD_ATTRIBUTE_VIRTUAL) {
        bool isNewSlot = (flags & METHOD_ATTRIBUTE_VTABLE_LAYOUT_MASK) == METHOD_ATTRIBUTE_NEW_SLOT;
        bool isFinal = (flags & METHOD_ATTRIBUTE_FINAL);
        bool isAbstract = (flags & METHOD_ATTRIBUTE_ABSTRACT);

        if (isAbstract) {
            outPut << "abstract ";
            // 如果不是 NewSlot 且是 Abstract，通常是接口实现或重写了父类的抽象方法
            if (!isNewSlot) outPut << "override ";
        }
        else if (isFinal) {
            // 如果设置了 Final 但又是 Virtual，且不是 NewSlot，说明是 sealed override
            if (!isNewSlot) outPut << "sealed override ";
            // 如果是 NewSlot + Final，那其实就是一个普通方法，不输出 virtual
        }
        else if (isNewSlot) {
            outPut << "virtual ";
        }
        else {
            outPut << "override ";
        }
    }

    // 4. 外部调用
    if (flags & METHOD_ATTRIBUTE_PINVOKE_IMPL) {
        outPut << "extern ";
    }

    return outPut.str();
}
/**
 * 字符串常量 Dump
 * 遍历元数据中的 StringLiteral
 */
std::string li2cpp::li2cppDumper::dumpStr() {
#ifdef UNITY_2018_4_16F1
    int allCount = m_pGlobalMetadataHeader->metadataUsageListsCount/sizeof(Il2CppMetadataUsageList);
    LOG(LOG_LEVEL_INFO,"[DumpStr] metadataUsageListsCount : %d allCount : %d", m_pGlobalMetadataHeader->metadataUsageListsCount,allCount);
    for (int i = 0; i < allCount; ++i) {
        Il2CppMetadataUsageList *metadataUsageLists = (Il2CppMetadataUsageList *) (
                (uint64_t) m_pGlobalMetadata
                + m_pGlobalMetadataHeader->metadataUsageListsOffset
                + 8LL * i);

        if(metadataUsageLists== nullptr || metadataUsageLists->start == 0){
            continue;
        }

        //int32_t dwCount = m_pGlobalMetadataHeader->metadataUsagePairsCount / sizeof(Il2CppMetadataUsagePair);
        for (int j = 0; j < metadataUsageLists->count; ++j) {
            const Il2CppMetadataUsagePair *metadataUsagePairs = (Il2CppMetadataUsagePair *) (
                    (uint64_t) m_pGlobalMetadata
                    + m_pGlobalMetadataHeader->metadataUsagePairsOffset
                    + 8LL * (unsigned int) (j + metadataUsageLists->start));

            if(!metadataUsagePairs){
                continue;
            }
            uint32_t destinationIndex = metadataUsagePairs->destinationIndex;
            uint32_t encodedSourceIndex = metadataUsagePairs->encodedSourceIndex;
            Il2CppMetadataUsage usage = GetEncodedIndexType(encodedSourceIndex);

            if (usage == kIl2CppMetadataUsageStringLiteral) {
                uint32_t decodedIndex = GetDecodedMethodIndex(encodedSourceIndex);
                std::string pstring = il2cpp_Il2CppString_toCString(
                        GetStringLiteralFromIndex(decodedIndex));
                if (m_kIl2CppMetadataUsageStringLiteral) {
                    // 存入 Map 并实时写入文件 (可选)
                    m_kIl2CppMetadataUsageStringLiteral->insert({(int) destinationIndex, pstring});
                }
                if (m_outDumpstr) {
                    char logBuffer[0x100] = {0};
                    snprintf(logBuffer, sizeof(logBuffer), "Index[0x%08X]:", i);
                    m_outDumpstr->writeLine(logBuffer+pstring);
                }
            }
        }
    }
#else
    for (size_t i = 0; i < m_pil2CppMetadataRegistration->metadataUsagesCount; i++) {

        uintptr_t* metadataPointer = reinterpret_cast<uintptr_t*>(m_pil2CppMetadataRegistration->metadataUsages[i]);

        if (metadataPointer) {
            // 调用你原有的运行时元数据解析逻辑
            dump_RuntimeMetadata(metadataPointer);
        }
    }
#endif
    // 写入完成后，如果需要在控制台显示统计，可以在这里处理
    return "";
}

/**
 * 运行时元数据具体解析逻辑 (保留你原有的 encodedToken 处理)
 */
std::string li2cpp::li2cppDumper::dump_RuntimeMetadata(uintptr_t *metadataPointer) {
    std::string pstring = "";
    if (!metadataPointer) return pstring;

    // 获取指针指向的 encodedToken
    uintptr_t metadataValue = *metadataPointer;
    uint32_t encodedToken = static_cast<uint32_t>(metadataValue);

    // 解码 Token 类型和索引
    Il2CppMetadataUsage usage = GetEncodedIndexType(encodedToken);
    uint32_t decodedIndex = GetDecodedMethodIndex(encodedToken);

    // 仅处理字符串字面量
    if (usage == kIl2CppMetadataUsageStringLiteral) {
        LOG(LOG_LEVEL_INFO,"--usage : %p",usage);
        // 使用 il2cpp 内部转换函数
        pstring = il2cpp_Il2CppString_toCString(GetStringLiteralFromIndex(decodedIndex));

        if (m_kIl2CppMetadataUsageStringLiteral) {
            // 存入 Map 并实时写入文件 (可选)
            m_kIl2CppMetadataUsageStringLiteral->insert({(int)decodedIndex, pstring});
            if (m_outDumpstr) {
                m_outDumpstr->writeLine(pstring);
            }
        }
    }
    return pstring;
}

/**
 * 日志记录函数
 * 负责将调试信息写入到 /data/data/com.DefaultCompany.Demo1/cache/log.cs
 */
bool li2cpp::li2cppDumper::writeLog(std::string str) {
    if (m_outlog == nullptr) {
        return false;
    }

    // 格式化输出，增加标识前缀
    //std::string out = "[SFK] : " + str;


    //LOG(LOG_LEVEL_INFO,"%s",str.c_str());
    // 调用你文件类 cMyfile 的接口写入一行
    m_outlog->writeLine(str.c_str());

    // 实时冲刷缓冲区，防止日志丢失
    m_outlog->flush();

    return true;
}

void li2cpp::li2cppDumper::initPackPath(std::string strPackName) {
    m_pathlog = "/data/data/"+strPackName+"/files/log.cs";
    m_pathDumpCs = "/data/data/"+strPackName+"/files/dumpcs.cs";
    m_pathDumpstr = "/data/data/"+strPackName+"/files/dumpstr.cs";
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif
