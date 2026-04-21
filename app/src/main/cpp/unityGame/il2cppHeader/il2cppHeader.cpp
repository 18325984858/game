//
// Created by Song on 2026/2/3.
//

#include "il2cppHeader.h"
#include "../../core/log/log.h"
#include <sstream>
#include <fstream>
#include <set>
#include <functional>


std::string IntToHex(uint32_t value) {
    std::stringstream ss;
    // 使用 std::uppercase 可以输出 0x1A 而不是 0x1a
    ss << "0x" << std::uppercase << std::hex << value;
    return ss.str();
}

static std::string CollapseUnderscores(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    bool lastWasUnderscore = false;
    for (char c : input) {
        if (c == '_') {
            if (!lastWasUnderscore) {
                output += c;
                lastWasUnderscore = true;
            }
            continue;
        }

        output += c;
        lastWasUnderscore = false;
    }

    return output;
}

static bool IsCKeyword(const std::string& name) {
    static const std::set<std::string> keywords = {
        "auto", "break", "case", "char", "const", "continue", "default", "do",
        "double", "else", "enum", "extern", "float", "for", "goto", "if",
        "int", "long", "register", "return", "short", "signed", "sizeof", "static",
        "struct", "switch", "typedef", "union", "unsigned", "void", "volatile", "while",
        "class", "namespace", "template", "this", "new", "delete", "operator",
        "virtual", "public", "private", "protected", "friend", "inline", "explicit",
        "bool", "true", "false", "try", "catch", "throw", "using", "typename",
        "export", "mutable", "override", "final", "nullptr", "alignas", "alignof",
        "asm", "dynamic_cast", "static_cast", "reinterpret_cast", "const_cast",
        "typeid", "decltype", "noexcept", "thread_local", "constexpr"
    };
    return keywords.count(name) != 0;
}

li2cppHeader::li2cppHeader::li2cppHeader(void *dqil2cppBase, void *pCodeRegistration,
                                         void *pMetadataRegistration, void *pGlobalMetadataHeader,
                                         void *pMetadataImagesTable) :
        li2cppApi::cUnityApi(dqil2cppBase,
                             static_cast<Il2CppCodeRegistration *>(pCodeRegistration),
                             static_cast<Il2CppMetadataRegistration *>(pMetadataRegistration),
                             static_cast<Il2CppGlobalMetadataHeader *>(pGlobalMetadataHeader),
                             static_cast<Il2CppImageGlobalMetadata *>(pMetadataImagesTable)),
        m_il2cppbase(reinterpret_cast<uint64_t>(dqil2cppBase)),
        m_pGlobalMetadata(pGlobalMetadataHeader),
        m_pGlobalMetadataHeader(static_cast<Il2CppGlobalMetadataHeader *>(pGlobalMetadataHeader)),
        m_pil2CppMetadataRegistration(static_cast<Il2CppMetadataRegistration *>(pMetadataRegistration)),
        m_pIl2CppCodeRegistration(static_cast<Il2CppCodeRegistration *>(pCodeRegistration)),
        m_pMetadataImagesTable(static_cast<Il2CppImageGlobalMetadata *>(pMetadataImagesTable))
{

}

li2cppHeader::li2cppHeader::~li2cppHeader() {
    LOG(LOG_LEVEL_INFO, "[li2cppHeader] >>> Exiting and Cleaning up resources...");

    // 1. 清空类信息 Map
    // 由于使用了 std::shared_ptr，clear() 会导致引用计数减 1
    // 当计数归零时，UnityClass 及其内部的 vector (fields, methods) 会自动释放内存
    m_classMap.clear();

    // 2. 指针置空（虽然对象即将销毁，但这是个好习惯，防止野指针）
    m_pGlobalMetadata = nullptr;
    m_pGlobalMetadataHeader = nullptr;
    m_pil2CppMetadataRegistration = nullptr;
    m_pIl2CppCodeRegistration = nullptr;
    m_pMetadataImagesTable = nullptr;

    LOG(LOG_LEVEL_INFO, "[li2cppHeader] Cleanup finished.");
}

void li2cppHeader::li2cppHeader::start() {
    LOG(LOG_LEVEL_INFO, "[li2cppHeader] >>> Entering: %s", __FUNCTION__);

    // 1. 初始化并解析所有类
    Init(m_classMap);

    // 2. 遍历完成后，保存到文件
    // 这里建议使用 SD 卡路径或 Data 目录路径
    SaveToIdaHeader("/data/data/"+m_packname+"/files/il2cpp.h");

    SaveToMethodJson("/data/data/"+m_packname+"/files/script.json");

    LOG(LOG_LEVEL_INFO, "[li2cppHeader] <<< Exiting: %s", __FUNCTION__);
}

std::string li2cppHeader::li2cppHeader::GetClassUniqueName(
        std::map<std::string, std::shared_ptr<UnityClass>> &classMap,
        std::shared_ptr<UnityClass>& targetClass,
        const Il2CppType *type) {

    if (!type) return "Unknown_Class";

    // 1. 获取当前类型的基本信息
    Il2CppClass* klass = il2cpp_type_get_class_or_element_class(type);

    // --- 核心过滤逻辑 ---
    // 1. 过滤掉枚举 (Enum)
    if (il2cpp_class_is_enum(klass)) return "Enum_Ignored";

    // 2. 过滤掉原始内置类型 (如 int, float, bool 等)
    // 通常这些类型没有父类或者父类是 ValueType
    auto flags = il2cpp_class_get_flags(klass);
    if (flags & 0x00000020) return "Interface_Ignored"; // 如果你也不想要接口

    const char* rawTypeName = il2cpp_type_get_name(type);
    std::string baseName = rawTypeName ? rawTypeName : "Unknown";

    std::string uniqueIdentifier = baseName;

    // 2. 特殊逻辑：如果是类（非值类型）且有父类，进行拼接
    if (klass && !il2cpp_class_is_valuetype(klass)) {
        Il2CppClass* parentKlass = il2cpp_class_get_parent(klass);
        if (parentKlass) {
            const char* parentName = il2cpp_class_get_name(parentKlass);
            if (parentName && std::string(parentName) != "Object") { // 忽略最顶层的 Object
                uniqueIdentifier += "_" + std::string(parentName);
            }
        }
    }

    // 3. 统一字符清洗 (点号转下划线，过滤非字母数字)
    std::replace(uniqueIdentifier.begin(), uniqueIdentifier.end(), '.', '_');
    for (char &c : uniqueIdentifier) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            c = '_';
        }
    }

    // 4. 清理连续下划线并去除首尾多余部分
    uniqueIdentifier = CollapseUnderscores(uniqueIdentifier);
    if (!uniqueIdentifier.empty() && uniqueIdentifier[0] == '_') uniqueIdentifier.erase(0, 1);
    if (!uniqueIdentifier.empty() && uniqueIdentifier.back() == '_') uniqueIdentifier.pop_back();

    // 5. 冲突处理
    if (targetClass != nullptr) {
        if (classMap.count(uniqueIdentifier) && classMap[uniqueIdentifier]->klassPtr != targetClass->klassPtr) {
            uniqueIdentifier += "_" + std::to_string(targetClass->klassPtr);
        }
    }

    return uniqueIdentifier;
}


void li2cppHeader::li2cppHeader::Init(
        std::map<std::string, std::shared_ptr<UnityClass>> &classMap) {

    LOG(LOG_LEVEL_INFO, "[li2cppHeader] >>> Entering: %s", __FUNCTION__);

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

            // 1. 统一获取经过清洗的唯一名称 (内部已处理 Namespace 和点号)
            // 这一步是解决 "前缀冗余" 的关键：所有地方都必须用这个函数获取名字
            std::string uniqueName = GetSafeUniqueName(const_cast<Il2CppClass *>(klass));

            // 2. 提取基本信息
            const char* namespc = il2cpp_class_get_namespace(const_cast<Il2CppClass *>(klass));
            const char* name = il2cpp_class_get_name(const_cast<Il2CppClass *>(klass));

            // 3. 创建 UnityClass 实例
            auto unityClass = std::make_shared<UnityClass>(
                    namespc ? namespc : "",
                    name ? name : "",
                    reinterpret_cast<uintptr_t>(klass)
            );

            unityClass->assembly = imageName;
            unityClass->instanceSize = il2cpp_class_instance_size(const_cast<Il2CppClass *>(klass));
            unityClass->flags = il2cpp_class_get_flags(klass);

            // 4. 处理父类继承
            auto parent = il2cpp_class_get_parent(const_cast<Il2CppClass *>(klass));
            if (parent) {
                unityClass->parentPtr = reinterpret_cast<uintptr_t>(parent);
                unityClass->parentSafeName = GetSafeUniqueName(const_cast<Il2CppClass *>(parent));
            }

            // 5. 处理类型属性 (位域存储)
            bool isValueType = il2cpp_class_is_valuetype(klass);
            bool isEnum = il2cpp_class_is_enum(klass);
            unityClass->typeAttr.bits.isValueType = isValueType;
            unityClass->typeAttr.bits.isEnum = isEnum;
            unityClass->typeAttr.bits.isStruct = isValueType && !isEnum;
            unityClass->typeAttr.bits.isInterface = (unityClass->flags & 0x00000020);

            // 6. 初始化类的所有方法指针（确保 methodPointer 可用）
            // 注意: 不调用 il2cpp_runtime_class_init, 它会触发 .cctor 可能崩溃
            il2cpp_class_init_all_method(const_cast<Il2CppClass *>(klass));

            // 7. 获取成员 (注意：DumpFields 内部调用的 GetIdaCompatibleType 也要改)
            // 确保 DumpFields 内部引用其他类时，也是调用 GetSafeUniqueName
            DumpFields(const_cast<Il2CppClass *>(klass), unityClass);

            // --- 新增：获取成员方法 ---
            DumpMethods(const_cast<Il2CppClass *>(klass), unityClass);

            // --- 新增：获取虚表信息 ---
            DumpVTable(const_cast<Il2CppClass *>(klass), unityClass);

            //存入 Map
            // uniqueName 是经过 CleanIdentifier 处理的，例如 "FrameEngine_Logic_BattleTeam"
            classMap[uniqueName] = unityClass;

            LOG(LOG_LEVEL_INFO, "[li2cppHeader] Processed: %s", uniqueName.c_str());
        }
    }

    LOG(LOG_LEVEL_INFO, "[li2cppHeader] <<< Exiting: %s, Total: %zu", __FUNCTION__, classMap.size());
}

void li2cppHeader::li2cppHeader::DumpFields(Il2CppClass* klass, std::shared_ptr<UnityClass>& unityClass) {
    if (!klass || !unityClass) return;

    void* iter = nullptr;
    FieldInfo* field = nullptr;

    // 记录上一个字段的末尾位置，用于计算 Padding
    // 如果有父类且不是值类型，使用父类的 instance_size 作为起始偏移
    // 这样父类字段占据的空间不会被误标记为 padding（因为 _Fields 结构体通过继承已包含父类字段）
    // 对象头部（klass + monitor）在 64 位下占用 0x10 (16字节)
    uint32_t currentOffset = 0x10;
    if (!il2cpp_class_is_valuetype(klass)) {
        Il2CppClass* parent = il2cpp_class_get_parent(klass);
        if (parent) {
            uint32_t parentSize = il2cpp_class_instance_size(parent);
            if (parentSize > currentOffset) {
                currentOffset = parentSize;
            }
        }
    }

    while ((field = il2cpp_class_get_fields(klass, &iter))) {
        const char* fieldName = il2cpp_field_get_name(field);
        const Il2CppType* type = il2cpp_field_get_type(field);
        uint32_t flags = il2cpp_field_get_flags(field);
        uint32_t offset = il2cpp_field_get_offset(field);
        bool isStatic = (flags & 0x0010) != 0;

        // 1. 处理实例字段的 Padding（已禁用）
        // if (!isStatic && offset > currentOffset) {
        //     uint32_t paddingSize = offset - currentOffset;
        //     std::string padName = "pad_" + IntToHex(currentOffset) + "[" + std::to_string(paddingSize) + "]";
        //     unityClass->fields.emplace_back(padName, "uint8_t", currentOffset, false, 0);
        // }

        // 2. 获取 IDA 兼容的类型名 (调用之前的 GetIdaTypeName)
        std::string idaTypeName = GetIdaCompatibleType(type);

        // 3. 清洗字段名（处理 <xxx>k__BackingField 等非法C标识符）
        std::string safeFieldName = fieldName ? CleanIdentifier(fieldName) : "Unnamed_Field";

        // 对泛型值类型 (GENERICINST + valuetype)，GetIdaCompatibleType 返回 "uint8_t"
        // 需要加上数组标记 [size] 来保证占用正确的字节数
        if (type->type == IL2CPP_TYPE_GENERICINST && idaTypeName == "uint8_t") {
            uint32_t valSize = GetTypeSize(type);
            if (valSize == 0) valSize = 4;
            safeFieldName += "[" + std::to_string(valSize) + "]";
        }

        // 4. 同一结构体内字段名去重，避免清洗后重名导致 IDA 解析失败
        {
            int dupCount = 0;
            const std::string baseName = safeFieldName;
            for (const auto& existing : unityClass->fields) {
                if (existing.isStatic == isStatic && existing.name == safeFieldName) {
                    ++dupCount;
                    safeFieldName = baseName + "_" + std::to_string(dupCount);
                }
            }
        }

        // 5. 存储到结构体
        unityClass->fields.emplace_back(
                safeFieldName,
                idaTypeName,
                offset,
                isStatic,
                isStatic ? reinterpret_cast<uintptr_t>(field) : 0
        );

        // 4. 更新当前偏移位置 (仅针对实例字段)
        if (!isStatic) {
            // 粗略估算字段占用大小，也可以使用 il2cpp_class_instance_size 处理 ValueType
            // 这里简单处理：指针对齐 8 字节，基础类型按实际大小
            currentOffset = offset + GetTypeSize(type);
        }

        LOG(LOG_LEVEL_INFO, "  [Field] %s | Offset: 0x%X | Type: %s", fieldName, offset, idaTypeName.c_str());
    }
}

std::string li2cppHeader::li2cppHeader::GetIdaCompatibleType(const Il2CppType* type) {
    if (!type) return "void*";

    // 获取底层枚举类型 (IL2CPP_TYPE_I4, IL2CPP_TYPE_CLASS 等)
    Il2CppTypeEnum typeEnum = type->type;

    switch (typeEnum) {
        case IL2CPP_TYPE_VOID:     return "void";
        case IL2CPP_TYPE_BOOLEAN:  return "bool";
        case IL2CPP_TYPE_CHAR:     return "uint16_t"; // C# Char
        case IL2CPP_TYPE_I1:       return "int8_t";
        case IL2CPP_TYPE_U1:       return "uint8_t";
        case IL2CPP_TYPE_I2:       return "int16_t";
        case IL2CPP_TYPE_U2:       return "uint16_t";
        case IL2CPP_TYPE_I4:       return "int32_t";
        case IL2CPP_TYPE_U4:       return "uint32_t";
        case IL2CPP_TYPE_I8:       return "int64_t";
        case IL2CPP_TYPE_U8:       return "uint64_t";
        case IL2CPP_TYPE_R4:       return "float";
        case IL2CPP_TYPE_R8:       return "double";
        case IL2CPP_TYPE_STRING:   return "struct System_String_o*";
        case IL2CPP_TYPE_PTR:      return "void*";
        case IL2CPP_TYPE_FNPTR:    return "void*";
        case IL2CPP_TYPE_OBJECT:  return "Il2CppObject*";

        case IL2CPP_TYPE_SZARRAY: {
            Il2CppClass* elementKlass = il2cpp_class_from_type(type->data.type);
            // 直接使用 GetSafeUniqueName，不再手动拼前缀
            return "struct " + GetSafeUniqueName(elementKlass) + "_array*";
        }

        case IL2CPP_TYPE_CLASS: {
            Il2CppClass* klass = il2cpp_class_from_type(type);
            return "struct " + GetSafeUniqueName(klass) + "_o*";
        }

        case IL2CPP_TYPE_GENERICINST: {
            Il2CppClass* klass = il2cpp_class_from_type(type);
            // 泛型值类型 (如 Nullable<T>, KeyValuePair<K,V>) 需要内联而不是指针
            if (il2cpp_class_is_valuetype(klass)) {
                if (il2cpp_class_is_enum(klass)) return "int32_t";
                uint32_t valSize = il2cpp_class_instance_size(klass);
                valSize = (valSize > 0x10) ? (valSize - 0x10) : valSize;
                if (valSize == 0) valSize = 4;
                return "uint8_t";
            }
            return "struct " + GetSafeUniqueName(klass) + "_o*";
        }

        case IL2CPP_TYPE_VALUETYPE: {
            Il2CppClass* klass = il2cpp_class_from_type(type);
            if (il2cpp_class_is_enum(klass)) return "int32_t";
            return "struct " + GetSafeUniqueName(klass) + "_Fields";
        }

        default: return "void*";
    }
}

uint32_t li2cppHeader::li2cppHeader::GetTypeSize(const Il2CppType* type) {
    switch (type->type) {
        case IL2CPP_TYPE_BOOLEAN:
        case IL2CPP_TYPE_I1:
        case IL2CPP_TYPE_U1:       return 1;
        case IL2CPP_TYPE_I2:
        case IL2CPP_TYPE_U2:
        case IL2CPP_TYPE_CHAR:     return 2;
        case IL2CPP_TYPE_I4:
        case IL2CPP_TYPE_U4:
        case IL2CPP_TYPE_R4:       return 4;
        case IL2CPP_TYPE_I8:
        case IL2CPP_TYPE_U8:
        case IL2CPP_TYPE_R8:       return 8;
        case IL2CPP_TYPE_VALUETYPE: {
            Il2CppClass* klass = il2cpp_class_from_type(type);
            return il2cpp_class_instance_size(klass) - 0x10; // 减去对象头偏移
        }
        default: return 8; // 指针、类、数组在 64 位下均为 8 字节
    }
}

/**
 * 递归解析 IL2CPP 类型名，并转换为 IDA 安全的标识符
 * 目标：处理嵌套泛型，如 Dictionary<string, List<int>> -> Dictionary_string_List_int_
 */
std::string li2cppHeader::li2cppHeader::GetSafeGenericName(const Il2CppType* type) {
    if (!type) return "UnknownType";

    // 1. 获取基础类型名 (IL2CPP 会返回类似 List<int> 或 List`1 的字符串)
    char* rawName = const_cast<char *>(il2cpp_type_get_name(type));
    std::string fullName = rawName ? rawName : "Unnamed";
    // 注意：某些环境下 il2cpp_type_get_name 返回的内存需要释放，根据你的 il2cpp 版本决定是否调用 il2cpp_free

    // 2. 特殊符号全量清洗
    // 我们需要把所有 C 语言结构体命名中非法的字符替换为下划线
    // 关键字符：< > (泛型), [ ] (数组/泛型), , (参数分隔), ` (泛型计数), . (命名空间)
    for (char &c : fullName) {
        if (c == '<' || c == '>' || c == '[' || c == ']' ||
            c == ',' || c == '`' || c == '.' || c == '+' ||
            c == '/' || c == ' ' || c == '*' || c == ':' ||
            c == '-' || c == '=') {
            c = '_';
        }
    }

    // 3. 规范化处理：去除连续下划线，使名字更美观
    fullName = CollapseUnderscores(fullName);

    // 4. 去除首尾可能多余的下划线
    if (!fullName.empty() && fullName[0] == '_') fullName.erase(0, 1);
    if (!fullName.empty() && fullName.back() == '_') fullName.pop_back();

    return fullName;
}


/**
 * 优化后的唯一名获取：内部直接调用清洗函数
 */
std::string li2cppHeader::li2cppHeader::GetSafeUniqueName(Il2CppClass* klass) {
    if (!klass) return "Il2CppObject";

    const Il2CppType* type = il2cpp_class_get_type(klass);
    char* rawName = const_cast<char *>(il2cpp_type_get_name(type));

    // 如果 rawName 已经包含命名空间(通常点号分隔)，CleanIdentifier 会把它变下划线
    std::string fullName = rawName ? rawName : il2cpp_class_get_name(klass);

    // 如果发现 rawName 不带命名空间，再手动补一下
    if (fullName.find('.') == std::string::npos) {
        const char* ns = il2cpp_class_get_namespace(klass);
        if (ns && strlen(ns) > 0) {
            fullName = std::string(ns) + "_" + fullName;
        }
    }

    return CleanIdentifier(fullName);
}

/**
 * 核心清洗函数：将所有非法字符替换为下划线，并压缩连续下划线
 */
std::string li2cppHeader::li2cppHeader::CleanIdentifier(std::string name) {
    for (char &c : name) {
        // 替换点号、尖括号、方括号、逗号、空格等所有 C 语法不支持的符号
        if (c == '.' || c == '<' || c == '>' || c == '`' ||
            c == ',' || c == '[' || c == ']' || c == '+' ||
            c == '/' || c == ' ' || c == '*' || c == ':' ||
            c == '-' || c == '=') {
            c = '_';
        }
    }
    name = CollapseUnderscores(name);

    // 移除首尾多余的下划线
    if (!name.empty() && name[0] == '_') name.erase(0, 1);
    if (!name.empty() && name.back() == '_') name.pop_back();

    // C 标识符不能以数字开头，加前缀 _
    if (!name.empty() && name[0] >= '0' && name[0] <= '9') {
        name = "_" + name;
    }

    if (name.empty()) name = "_unnamed";

    // C/C++ 保留关键字加前缀，避免导入 IDA 时语法错误
    if (IsCKeyword(name)) {
        name = "_" + name;
    }

    return name;
}

void li2cppHeader::li2cppHeader::SaveToIdaHeader(const std::string& path) {
    std::ofstream fout(path);
    if (!fout.is_open()) return;

    // ====== 基础类型定义（与 Resource.cpp 完全一致）======
    fout << "typedef void(*Il2CppMethodPointer)();\n\n";
    fout << "struct MethodInfo;\n\n";

    fout << "struct VirtualInvokeData\n{\n";
    fout << "    Il2CppMethodPointer methodPtr;\n";
    fout << "    const struct MethodInfo* method;\n";
    fout << "};\n\n";

    fout << "struct Il2CppType\n{\n";
    fout << "    void* data;\n";
    fout << "    unsigned int bits;\n";
    fout << "};\n\n";

    fout << "struct Il2CppClass;\n\n";

    fout << "struct Il2CppObject\n{\n";
    fout << "    Il2CppClass *klass;\n";
    fout << "    void *monitor;\n";
    fout << "};\n\n";

    fout << "union Il2CppRGCTXData\n{\n";
    fout << "    void* rgctxDataDummy;\n";
    fout << "    const struct MethodInfo* method;\n";
    fout << "    const Il2CppType* type;\n";
    fout << "    Il2CppClass* klass;\n";
    fout << "};\n\n";

    fout << "struct Il2CppRuntimeInterfaceOffsetPair\n{\n";
    fout << "    Il2CppClass* interfaceType;\n";
    fout << "    int32_t offset;\n";
    fout << "};\n";

    fout << "struct Il2CppClass_1\n{\n";
    fout << "    void* image;\n";
    fout << "    void* gc_desc;\n";
    fout << "    const char* name;\n";
    fout << "    const char* namespaze;\n";
    fout << "    Il2CppType byval_arg;\n";
    fout << "    Il2CppType this_arg;\n";
    fout << "    Il2CppClass* element_class;\n";
    fout << "    Il2CppClass* castClass;\n";
    fout << "    Il2CppClass* declaringType;\n";
    fout << "    Il2CppClass* parent;\n";
    fout << "    void *generic_class;\n";
    fout << "    void* typeMetadataHandle;\n";
    fout << "    void* interopData;\n";
    fout << "    Il2CppClass* klass;\n";
    fout << "    void* fields;\n";
    fout << "    void* events;\n";
    fout << "    void* properties;\n";
    fout << "    void* methods;\n";
    fout << "    Il2CppClass** nestedTypes;\n";
    fout << "    Il2CppClass** implementedInterfaces;\n";
    fout << "    Il2CppRuntimeInterfaceOffsetPair* interfaceOffsets;\n";
    fout << "};\n\n";

    fout << "struct Il2CppClass_2\n{\n";
    fout << "    Il2CppClass** typeHierarchy;\n";
    fout << "    void *unity_user_data;\n";
    fout << "    uint32_t initializationExceptionGCHandle;\n";
    fout << "    uint32_t cctor_started;\n";
    fout << "    uint32_t cctor_finished;\n";
    fout << "    size_t cctor_thread;\n";
    fout << "    void* genericContainerHandle;\n";
    fout << "    uint32_t instance_size;\n";
    fout << "    uint32_t actualSize;\n";
    fout << "    uint32_t element_size;\n";
    fout << "    int32_t native_size;\n";
    fout << "    uint32_t static_fields_size;\n";
    fout << "    uint32_t thread_static_fields_size;\n";
    fout << "    int32_t thread_static_fields_offset;\n";
    fout << "    uint32_t flags;\n";
    fout << "    uint32_t token;\n";
    fout << "    uint16_t method_count;\n";
    fout << "    uint16_t property_count;\n";
    fout << "    uint16_t field_count;\n";
    fout << "    uint16_t event_count;\n";
    fout << "    uint16_t nested_type_count;\n";
    fout << "    uint16_t vtable_count;\n";
    fout << "    uint16_t interfaces_count;\n";
    fout << "    uint16_t interface_offsets_count;\n";
    fout << "    uint8_t typeHierarchyDepth;\n";
    fout << "    uint8_t genericRecursionDepth;\n";
    fout << "    uint8_t rank;\n";
    fout << "    uint8_t minimumAlignment;\n";
    fout << "    uint8_t naturalAligment;\n";
    fout << "    uint8_t packingSize;\n";
    fout << "    uint8_t bitflags1;\n";
    fout << "    uint8_t bitflags2;\n";
    fout << "};\n\n";

    fout << "struct Il2CppClass\n{\n";
    fout << "    Il2CppClass_1 _1;\n";
    fout << "    void* static_fields;\n";
    fout << "    Il2CppRGCTXData* rgctx_data;\n";
    fout << "    Il2CppClass_2 _2;\n";
    fout << "    VirtualInvokeData vtable[255];\n";
    fout << "};\n\n";

    fout << "typedef uintptr_t il2cpp_array_size_t;\n";
    fout << "typedef int32_t il2cpp_array_lower_bound_t;\n";
    fout << "struct Il2CppArrayBounds\n{\n";
    fout << "    il2cpp_array_size_t length;\n";
    fout << "    il2cpp_array_lower_bound_t lower_bound;\n";
    fout << "};\n\n";

    fout << "typedef void (*InvokerMethod)(Il2CppMethodPointer, const MethodInfo*, void*, void**, void*);\n";
    fout << "struct MethodInfo\n{\n";
    fout << "    Il2CppMethodPointer methodPointer;\n";
    fout << "    Il2CppMethodPointer virtualMethodPointer;\n";
    fout << "    InvokerMethod invoker_method;\n";
    fout << "    const char* name;\n";
    fout << "    Il2CppClass *klass;\n";
    fout << "    const Il2CppType *return_type;\n";
    fout << "    const Il2CppType** parameters;\n";
    fout << "    union\n    {\n";
    fout << "        const Il2CppRGCTXData* rgctx_data;\n";
    fout << "        const void* methodMetadataHandle;\n";
    fout << "    };\n";
    fout << "    union\n    {\n";
    fout << "        const void* genericMethod;\n";
    fout << "        const void* genericContainerHandle;\n";
    fout << "    };\n";
    fout << "    uint32_t token;\n";
    fout << "    uint16_t flags;\n";
    fout << "    uint16_t iflags;\n";
    fout << "    uint16_t slot;\n";
    fout << "    uint8_t parameters_count;\n";
    fout << "    uint8_t bitflags;\n";
    fout << "};\n\n";

    // ====== 遍历生成每个类的结构体 ======
    // 判断一个父类名是否是不需要继承的基础类型
    auto isBaseParent = [](const std::string& name) -> bool {
        return name.empty() || name == "System_Object" || name == "System_ValueType" || name == "System_Enum";
    };

    // ====== 拓扑排序：确保父类和值类型依赖在引用者之前输出 ======
    std::vector<std::string> sortedKeys;
    {
        std::set<std::string> visited;
        std::set<std::string> inStack; // 检测循环依赖
        std::function<void(const std::string&)> topoVisit = [&](const std::string& key) {
            if (visited.count(key)) return;
            if (inStack.count(key)) return; // 循环依赖，跳过
            inStack.insert(key);
            auto it = m_classMap.find(key);
            if (it == m_classMap.end()) { inStack.erase(key); return; }
            auto& cls = it->second;
            // 1. 父类依赖
            if (!isBaseParent(cls->parentSafeName) && !cls->typeAttr.bits.isValueType) {
                if (m_classMap.count(cls->parentSafeName)) {
                    topoVisit(cls->parentSafeName);
                }
            }
            // 2. 值类型字段依赖 (内联嵌入的 struct XXX_Fields)
            for (const auto& field : cls->fields) {
                const std::string& t = field.typeName;
                // 匹配 "struct XXX_Fields" 模式
                if (t.size() > 14 && t.compare(0, 7, "struct ") == 0
                    && t.compare(t.size() - 7, 7, "_Fields") == 0) {
                    std::string depKey = t.substr(7, t.size() - 14);
                    if (m_classMap.count(depKey)) {
                        topoVisit(depKey);
                    }
                }
            }
            inStack.erase(key);
            visited.insert(key);
            sortedKeys.push_back(key);
        };
        for (auto const& [key, _] : m_classMap) {
            topoVisit(key);
        }

        // 值类型按值内联使用最频繁，统一提前输出，降低 IDA 对后置定义的报错概率。
        std::stable_sort(sortedKeys.begin(), sortedKeys.end(), [&](const std::string& lhs, const std::string& rhs) {
            auto lit = m_classMap.find(lhs);
            auto rit = m_classMap.find(rhs);
            const bool lValueType = (lit != m_classMap.end()) && lit->second->typeAttr.bits.isValueType;
            const bool rValueType = (rit != m_classMap.end()) && rit->second->typeAttr.bits.isValueType;
            if (lValueType != rValueType) return lValueType > rValueType;
            return false;
        });
    }

    // ====== 收集所有被引用但不在 classMap 中的值类型，生成前向声明 ======
    std::set<std::string> emittedForwardDecls;
    for (const auto& uniqueKey : sortedKeys) {
        auto& cls = m_classMap[uniqueKey];
        for (const auto& field : cls->fields) {
            const std::string& t = field.typeName;
            if (t.size() > 14 && t.compare(0, 7, "struct ") == 0
                && t.compare(t.size() - 7, 7, "_Fields") == 0) {
                std::string depKey = t.substr(7, t.size() - 14);
                std::string fwdName = depKey + "_Fields";
                if (!m_classMap.count(depKey) && !emittedForwardDecls.count(fwdName)) {
                    // 未定义的值类型，生成空前向声明让 IDA 不报错
                    fout << "struct " << fwdName << " { }; /* forward decl */\n";
                    emittedForwardDecls.insert(fwdName);
                }
            }
        }
    }

    for (const auto& uniqueKey : sortedKeys) {
        auto& cls = m_classMap[uniqueKey];
        bool isValueType = cls->typeAttr.bits.isValueType;

        // 判断是否有静态字段
        bool hasStatic = false;
        // 判断是否有实例字段（不含padding）
        bool hasInstance = false;
        for (const auto& field : cls->fields) {
            if (field.isStatic) hasStatic = true;
            else hasInstance = true;
        }

        // 判断是否需要继承父类 Fields
        bool hasParentInherit = false;
        std::string parentFieldsName;
        if (!isBaseParent(cls->parentSafeName)) {
            // 只有非值类型的类才做 Fields 继承
            if (!isValueType) {
                hasParentInherit = true;
                parentFieldsName = cls->parentSafeName + "_Fields";
            }
        }

        // --- 1. _Fields 结构体 ---
        if (hasParentInherit) {
            // 如果父类不在 classMap 中，先输出一个空的前向定义，避免编译错误
            if (!m_classMap.count(cls->parentSafeName) && !emittedForwardDecls.count(parentFieldsName)) {
                fout << "struct " << parentFieldsName << " {};\n";
                emittedForwardDecls.insert(parentFieldsName);
            }
            fout << "struct " << uniqueKey << "_Fields : " << parentFieldsName << " {\n";
        } else {
            fout << "struct " << uniqueKey << "_Fields {\n";
        }
        for (const auto& field : cls->fields) {
            if (!field.isStatic) {
                fout << "    " << field.typeName << " " << field.name << "; /* " << IntToHex(field.offset) << " */\n";
            }
        }
        fout << "};\n";

        // --- 2. _VTable 结构体 ---
        fout << "struct " << uniqueKey << "_VTable {\n";
        if (cls->vtableEntries.empty()) {
            fout << "    VirtualInvokeData _0_dummy;\n";
        } else {
            for (const auto& vEntry : cls->vtableEntries) {
                fout << "    VirtualInvokeData " << vEntry.methodName << ";\n";
            }
        }
        fout << "};\n";

        // --- 3. _c 类结构体 ---
        fout << "struct " << uniqueKey << "_c {\n";
        fout << "    Il2CppClass_1 _1;\n";
        if (hasStatic) {
            fout << "    struct " << uniqueKey << "_StaticFields* static_fields;\n";
        } else {
            fout << "    void* static_fields;\n";
        }
        fout << "    Il2CppRGCTXData* rgctx_data;\n";
        fout << "    Il2CppClass_2 _2;\n";
        fout << "    " << uniqueKey << "_VTable vtable;\n";
        fout << "};\n";

        // --- 4. _o 对象结构体 ---
        fout << "struct " << uniqueKey << "_o {\n";
        if (isValueType) {
            // 值类型没有 klass/monitor
            fout << "    " << uniqueKey << "_Fields fields;\n";
        } else {
            fout << "    " << uniqueKey << "_c *klass;\n";
            fout << "    void *monitor;\n";
            fout << "    " << uniqueKey << "_Fields fields;\n";
        }
        fout << "};\n";

        // --- 5. _StaticFields 结构体（仅在有静态字段时输出）---
        if (hasStatic) {
            fout << "struct " << uniqueKey << "_StaticFields {\n";
            for (const auto& field : cls->fields) {
                if (field.isStatic) {
                    fout << "    " << field.typeName << " " << field.name << "; /* " << IntToHex(field.offset) << " */\n";
                }
            }
            fout << "};\n";
        }
    }

    fout.close();
}

std::string li2cppHeader::li2cppHeader::GetTypeAbbreviation(const std::string& type) {
    if (type == "void") return "v";
    if (type == "bool") return "b";
    if (type == "int32_t" || type == "uint32_t") return "i";
    if (type == "int64_t" || type == "uint64_t") return "j";
    if (type == "float") return "f";
    if (type == "double") return "d";
    if (type.find('*') != std::string::npos) return "i"; // 指针在 IL2CPP 中通常视为 native int (i)
    return "i"; // 默认作为对象/指针处理
}

void li2cppHeader::li2cppHeader::DumpMethods(Il2CppClass* klass, std::shared_ptr<UnityClass>& unityClass) {
    void* iter = nullptr;
    const MethodInfo* method = nullptr;
    std::string classSafeName = GetSafeUniqueName(klass);

    while ((method = il2cpp_class_get_methods(klass, &iter))) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(method->methodPointer);
        if (addr == 0) continue; // 跳过没有实现的虚方法或抽象方法

        const char* mName = il2cpp_method_get_name(method);
        const Il2CppType* retType = il2cpp_method_get_return_type(method);
        std::string retTypeName = GetIdaCompatibleType(retType);

        UnityMethod uMethod(mName, retTypeName, addr, method->token);

        // --- 生成 TypeSignature (如 viii) ---
        uMethod.typeSignature = GetTypeAbbreviation(retTypeName);

        // --- 生成 Signature ---
        // 格式：ReturnType Class_Method (ThisType __this, Params...);
        std::string sig = retTypeName + " " + classSafeName + "_" + CleanIdentifier(mName) + " (";

        // 注入 __this 指针 (实例方法)
        sig += "struct " + classSafeName + "_o* __this";
        uMethod.typeSignature += "i"; // __this 也是一个指针

        uint32_t pCount = il2cpp_method_get_param_count(method);
        for (uint32_t i = 0; i < pCount; ++i) {
            const Il2CppType* pType = il2cpp_method_get_param(method, i);
            std::string pTypeName = GetIdaCompatibleType(pType);

            sig += ", " + pTypeName + " p" + std::to_string(i);
            uMethod.typeSignature += GetTypeAbbreviation(pTypeName);
        }
        sig += ", const struct MethodInfo* method);";
        uMethod.typeSignature += "i"; // 结尾的 MethodInfo*

        uMethod.signature = sig;
        unityClass->methods.push_back(uMethod);
    }
}

void li2cppHeader::li2cppHeader::DumpVTable(Il2CppClass* klass, std::shared_ptr<UnityClass>& unityClass) {
    if (!klass || !unityClass) return;

    // ★ 关键检查1：类的虚表是否已初始化
    // Il2CppClass 中 is_vtable_initialized 位域标记虚表是否已填充
    // 如果未初始化，vtable_count 可能为垃圾值，vtable[] 中的指针可能无效
    if (!klass->is_vtable_initialized) {
        LOG(LOG_LEVEL_WARN, "  [VTable] %s vtable NOT initialized, skipping", klass->name ? klass->name : "unknown");
        unityClass->vtableCount = 0;
        return;
    }

    uint16_t vtableCount = klass->vtable_count;
    // ★ 关键检查2：vtable_count 合理性校验，防止垃圾值导致越界访问
    if (vtableCount > 1024) {
        LOG(LOG_LEVEL_WARN, "  [VTable] %s vtable_count=%d too large, skipping",
            klass->name ? klass->name : "unknown", vtableCount);
        unityClass->vtableCount = 0;
        return;
    }
    unityClass->vtableCount = vtableCount;

    for (uint16_t i = 0; i < vtableCount; ++i) {
        const VirtualInvokeData& vData = klass->vtable[i];
        const MethodInfo* methodInfo = vData.method;

        std::string entryName;
        // ★ 关键检查3：methodInfo 指针有效性
        if (methodInfo) {
            // 安全读取 name：先检查指针非空再解引用
            const char* mName = nullptr;
            try {
                mName = methodInfo->name;
            } catch (...) {
                mName = nullptr;
            }

            std::string safeName = (mName && mName[0] != '\0') ? CleanIdentifier(mName) : "unknown";

            // 安全读取 declaring class
            Il2CppClass* declaringClass = nullptr;
            try {
                declaringClass = methodInfo->klass;
            } catch (...) {
                declaringClass = nullptr;
            }

            // 如果声明类不是当前类，添加前缀（类似 System_IConvertible_ToBoolean）
            if (declaringClass && declaringClass != klass) {
                // ★ 关键检查4：对外部类调用 GetSafeUniqueName 前确保其 name 可访问
                std::string declaringName;
                try {
                    const char* dName = declaringClass->name;
                    if (dName && dName[0] != '\0') {
                        declaringName = GetSafeUniqueName(declaringClass);
                    } else {
                        declaringName = "UnknownClass";
                    }
                } catch (...) {
                    declaringName = "UnknownClass";
                }
                entryName = "_" + std::to_string(i) + "_" + declaringName + "_" + safeName;
            } else {
                entryName = "_" + std::to_string(i) + "_" + safeName;
            }
        } else {
            entryName = "_" + std::to_string(i) + "_null";
        }

        unityClass->vtableEntries.emplace_back(i, entryName);
    }

    LOG(LOG_LEVEL_INFO, "  [VTable] %s vtable_count: %d", GetSafeUniqueName(klass).c_str(), vtableCount);
}

// JSON 字符串转义辅助函数
static std::string EscapeJsonString(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 16);
    for (unsigned char c : input) {
        switch (c) {
            case '\"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    output += buf;
                } else {
                    output += (char)c;
                }
                break;
        }
    }
    return output;
}

// =====================================================================
// 辅助：通过 TypeDefinitionIndex 获取已解密的 "Namespace.ClassName"
// =====================================================================
std::string li2cppHeader::li2cppHeader::GetDecryptedClassName(TypeDefinitionIndex typeDefIndex, const MetadataLimits& limits) {
    if (typeDefIndex < 0 || (uint32_t)typeDefIndex >= limits.maxTypeDefIndex) return "UnknownClass";

    const Il2CppTypeDefinition* typeDef = GetTypeDefinitionForIndex(typeDefIndex);
    if (!typeDef || typeDef->byvalTypeIndex < 0 || (uint32_t)typeDef->byvalTypeIndex >= limits.maxTypeIndex)
        return "UnknownClass";

    const Il2CppType* type = GetIl2CppTypeFromIndex(typeDef->byvalTypeIndex);
    if (!type) return "UnknownClass";

    Il2CppClass* klass = il2cpp_class_from_type(type);
    if (!klass) return "UnknownClass";

    const char* ns = il2cpp_class_get_namespace(klass);
    const char* cn = il2cpp_class_get_name(klass);
    if (ns && strlen(ns) > 0)
        return std::string(ns) + "." + (cn ? cn : "");
    return cn ? cn : "UnknownClass";
}

// =====================================================================
// 辅助：通过 token 匹配从 Il2CppClass 中获取已解密的方法名
// =====================================================================
std::string li2cppHeader::li2cppHeader::GetDecryptedMethodName(Il2CppClass* klass, uint32_t token) {
    if (!klass) return "UnknownMethod";
    void* mIter = nullptr;
    while (auto* mInfo = il2cpp_class_get_methods(klass, &mIter)) {
        if (mInfo->token == token) {
            const char* mName = il2cpp_method_get_name(mInfo);
            if (mName && mName[0] != '\0') return mName;
            break;
        }
    }
    return "UnknownMethod";
}

// =====================================================================
// 1. 写入 ScriptMethod 段
// =====================================================================
void li2cppHeader::li2cppHeader::WriteScriptMethods(std::ofstream& fout, std::set<uintptr_t>& allAddresses) {
    fout << "  \"ScriptMethod\": [\n";
    bool first = true;
    for (auto const& [uniqueKey, cls] : m_classMap) {
        for (const auto& method : cls->methods) {
            uintptr_t rva = (method.methodPointer != 0) ? (method.methodPointer - m_il2cppbase) : 0;
            if (rva != 0) allAddresses.insert(rva);

            if (!first) fout << ",\n";
            fout << "    {\n";
            fout << "      \"Address\": " << rva << ",\n";
            std::string dotName = (cls->namespc.empty() ? "" : cls->namespc + ".") + cls->className;
            fout << "      \"Name\": \"" << EscapeJsonString(dotName) << "$$" << EscapeJsonString(method.name) << "\",\n";
            fout << "      \"Signature\": \"" << EscapeJsonString(method.signature) << "\",\n";
            fout << "      \"TypeSignature\": \"" << EscapeJsonString(method.typeSignature) << "\"\n";
            fout << "    }";
            first = false;
        }
    }
    fout << "\n  ],\n";
}

// =====================================================================
// 处理 kIl2CppMetadataUsageStringLiteral
// =====================================================================
void li2cppHeader::li2cppHeader::HandleStringLiteral(
        uint32_t decodedIndex, uintptr_t usageAddr,
        const MetadataLimits& limits,
        std::vector<ScriptStringEntry>& out)
{
    if (decodedIndex >= limits.maxStringLiteralIndex) return;

    // 字符串字面量已加密时，必须走运行时 API 解密，不能直接读 metadata 原始字节。
    Il2CppString* il2cppStr = GetStringLiteralFromIndex(decodedIndex);
    ScriptStringEntry entry{};
    entry.address = usageAddr;
    if (!il2cppStr) {
        entry.value = "";
        out.push_back(entry);
        return;
    }
    entry.value = il2cpp_Il2CppString_toCString(il2cppStr);
    out.push_back(entry);
}

// =====================================================================
// 处理 kIl2CppMetadataUsageTypeInfo / kIl2CppMetadataUsageIl2CppType
// =====================================================================
void li2cppHeader::li2cppHeader::HandleTypeMetadata(
        uint32_t decodedIndex, uintptr_t usageAddr,
        const MetadataLimits& limits, const std::string& suffix,
        const std::string& sig, std::vector<ScriptMetadataEntry>& out)
{
    if (decodedIndex >= limits.maxTypeIndex) return;
    const Il2CppType* type = GetIl2CppTypeFromIndex((TypeIndex)decodedIndex);
    if (!type) return;
    const char* rawName = il2cpp_type_get_name(type);
    std::string name = (rawName && rawName[0] != '\0') ? rawName : "UnknownType";
    out.push_back({usageAddr, name + suffix, sig});
}

// =====================================================================
// 处理 kIl2CppMetadataUsageFieldInfo
// =====================================================================
void li2cppHeader::li2cppHeader::HandleFieldInfo(
        uint32_t decodedIndex, uintptr_t usageAddr,
        const MetadataLimits& limits,
        std::vector<ScriptMetadataEntry>& out)
{
    if (decodedIndex >= limits.maxFieldRefIndex) return;
    auto* fieldRef = reinterpret_cast<const Il2CppFieldRef*>(
            (uint64_t)m_pGlobalMetadata + m_pGlobalMetadataHeader->fieldRefsOffset
            + sizeof(Il2CppFieldRef) * decodedIndex);
    if (!fieldRef) return;
    if (fieldRef->typeIndex < 0 || (uint32_t)fieldRef->typeIndex >= limits.maxTypeDefIndex) return;

    const Il2CppTypeDefinition* typeDef = GetTypeDefinitionForIndex(fieldRef->typeIndex);
    std::string typeName = "UnknownType";
    std::string fieldName = "UnknownField";

    if (typeDef && typeDef->byvalTypeIndex >= 0 && (uint32_t)typeDef->byvalTypeIndex < limits.maxTypeIndex) {
        const Il2CppType* type = GetIl2CppTypeFromIndex(typeDef->byvalTypeIndex);
        if (type) {
            Il2CppClass* klass = il2cpp_class_from_type(type);
            if (klass) {
                // 获取已解密类名
                const char* ns = il2cpp_class_get_namespace(klass);
                const char* cn = il2cpp_class_get_name(klass);
                typeName = (ns && strlen(ns) > 0) ? std::string(ns) + "." + (cn ? cn : "") : (cn ? cn : "UnknownType");

                // 获取已解密字段名
                if (fieldRef->fieldIndex >= 0 && fieldRef->fieldIndex < typeDef->field_count) {
                    void* fIter = nullptr;
                    int fIdx = 0;
                    while (auto* field = il2cpp_class_get_fields(klass, &fIter)) {
                        if (fIdx == fieldRef->fieldIndex) {
                            const char* fName = il2cpp_field_get_name(field);
                            if (fName && fName[0] != '\0') fieldName = fName;
                            break;
                        }
                        fIdx++;
                    }
                }
            } else {
                const char* rawName = il2cpp_type_get_name(type);
                if (rawName && rawName[0] != '\0') typeName = rawName;
            }
        }
    }
    out.push_back({usageAddr, "Field$" + typeName + "." + fieldName, "FieldInfo*"});
}

// =====================================================================
// 处理 kIl2CppMetadataUsageMethodDef
// =====================================================================
void li2cppHeader::li2cppHeader::HandleMethodDef(
        uint32_t decodedIndex, uintptr_t usageAddr,
        const MetadataLimits& limits,
        std::vector<ScriptMetadataMethodEntry>& out,
        std::set<uintptr_t>& allAddresses)
{
    if (decodedIndex >= limits.maxMethodDefIndex) return;
    const auto* methodDef = GetMethodDefinitionFromIndex((MethodIndex)decodedIndex);
    if (!methodDef) return;

    std::string className = GetDecryptedClassName(methodDef->declaringType, limits);
    std::string methodName = "UnknownMethod";

    // 获取 Il2CppClass* 以通过 token 匹配方法名
    if (methodDef->declaringType >= 0 && (uint32_t)methodDef->declaringType < limits.maxTypeDefIndex) {
        const auto* typeDef = GetTypeDefinitionForIndex(methodDef->declaringType);
        if (typeDef && typeDef->byvalTypeIndex >= 0 && (uint32_t)typeDef->byvalTypeIndex < limits.maxTypeIndex) {
            const Il2CppType* type = GetIl2CppTypeFromIndex(typeDef->byvalTypeIndex);
            if (type) {
                Il2CppClass* klass = il2cpp_class_from_type(type);
                methodName = GetDecryptedMethodName(klass, methodDef->token);
            }
        }
    }

    // 获取方法地址
    uintptr_t methodAddr = 0;
    if (methodDef->methodIndex >= 0 &&
        (uint32_t)methodDef->methodIndex < m_pIl2CppCodeRegistration->methodPointersCount) {
        auto ptr = m_pIl2CppCodeRegistration->methodPointers[methodDef->methodIndex];
        if (ptr) {
            methodAddr = (uintptr_t)ptr - m_il2cppbase;
            allAddresses.insert(methodAddr);
        }
    }

    out.push_back({usageAddr, "Method$" + className + "." + methodName + "()", methodAddr});
}

// =====================================================================
// 处理 kIl2CppMetadataUsageMethodRef
// =====================================================================
void li2cppHeader::li2cppHeader::HandleMethodRef(
        uint32_t decodedIndex, uintptr_t usageAddr,
        const MetadataLimits& limits,
        std::vector<ScriptMetadataMethodEntry>& out,
        std::set<uintptr_t>& allAddresses)
{
    if (decodedIndex >= limits.maxMethodSpecIndex) return;
    const auto* methodSpec = m_pil2CppMetadataRegistration->methodSpecs + decodedIndex;
    if (!methodSpec) return;
    if (methodSpec->methodDefinitionIndex < 0 ||
        (uint32_t)methodSpec->methodDefinitionIndex >= limits.maxMethodDefIndex) return;

    const auto* methodDef = GetMethodDefinitionFromIndex(methodSpec->methodDefinitionIndex);
    if (!methodDef) return;

    std::string className = GetDecryptedClassName(methodDef->declaringType, limits);
    std::string methodName = "UnknownMethod";

    if (methodDef->declaringType >= 0 && (uint32_t)methodDef->declaringType < limits.maxTypeDefIndex) {
        const auto* typeDef = GetTypeDefinitionForIndex(methodDef->declaringType);
        if (typeDef && typeDef->byvalTypeIndex >= 0 && (uint32_t)typeDef->byvalTypeIndex < limits.maxTypeIndex) {
            const Il2CppType* type = GetIl2CppTypeFromIndex(typeDef->byvalTypeIndex);
            if (type) {
                Il2CppClass* klass = il2cpp_class_from_type(type);
                methodName = GetDecryptedMethodName(klass, methodDef->token);
            }
        }
    }

    // 在泛型方法表中查找地址
    uintptr_t methodAddr = 0;
    for (int32_t gi = 0; gi < m_pil2CppMetadataRegistration->genericMethodTableCount; gi++) {
        const auto* entry = m_pil2CppMetadataRegistration->genericMethodTable + gi;
        if (entry && entry->genericMethodIndex == (int32_t)decodedIndex) {
            if ((uint32_t)entry->indices.methodIndex < m_pIl2CppCodeRegistration->genericMethodPointersCount) {
                auto ptr = m_pIl2CppCodeRegistration->genericMethodPointers[entry->indices.methodIndex];
                if (ptr) {
                    methodAddr = (uintptr_t)ptr - m_il2cppbase;
                    allAddresses.insert(methodAddr);
                }
            }
            break;
        }
    }

    out.push_back({usageAddr, "Method$" + className + "." + methodName + "()", methodAddr});
}

// =====================================================================
// 遍历 metadataUsages 收集所有数据
// =====================================================================
void li2cppHeader::li2cppHeader::CollectMetadataUsages(
        std::vector<ScriptStringEntry>& scriptStrings,
        std::vector<ScriptMetadataEntry>& scriptMetadata,
        std::vector<ScriptMetadataMethodEntry>& scriptMetadataMethod,
        std::set<uintptr_t>& allAddresses)
{
    if (!m_pGlobalMetadataHeader || !m_pil2CppMetadataRegistration) return;

    int allCount = m_pGlobalMetadataHeader->metadataUsageListsCount / sizeof(Il2CppMetadataUsageList);

    MetadataLimits limits{};
    limits.maxTypeIndex       = (uint32_t)m_pil2CppMetadataRegistration->typesCount;
    limits.maxMethodDefIndex  = m_pGlobalMetadataHeader->methodsCount / sizeof(Il2CppMethodDefinition);
    limits.maxStringLiteralIndex = m_pGlobalMetadataHeader->stringLiteralCount / sizeof(Il2CppStringLiteral);
    limits.maxFieldRefIndex   = m_pGlobalMetadataHeader->fieldRefsCount / sizeof(Il2CppFieldRef);
    limits.maxMethodSpecIndex = (uint32_t)m_pil2CppMetadataRegistration->methodSpecsCount;
    limits.maxTypeDefIndex    = m_pGlobalMetadataHeader->typeDefinitionsCount / sizeof(Il2CppTypeDefinition);
    limits.maxPairsIndex      = m_pGlobalMetadataHeader->metadataUsagePairsCount / sizeof(Il2CppMetadataUsagePair);

    for (int i = 0; i < allCount; ++i) {
        auto* usageList = reinterpret_cast<Il2CppMetadataUsageList*>(
                (uint64_t)m_pGlobalMetadata
                + m_pGlobalMetadataHeader->metadataUsageListsOffset
                + sizeof(Il2CppMetadataUsageList) * i);
        if (!usageList || usageList->count == 0) continue;

        for (uint32_t j = 0; j < usageList->count; ++j) {
            uint32_t pairIndex = j + usageList->start;
            if (pairIndex >= limits.maxPairsIndex) continue;

            auto* usagePair = reinterpret_cast<const Il2CppMetadataUsagePair*>(
                    (uint64_t)m_pGlobalMetadata
                    + m_pGlobalMetadataHeader->metadataUsagePairsOffset
                    + sizeof(Il2CppMetadataUsagePair) * pairIndex);
            if (!usagePair) continue;

            uint32_t destIndex  = usagePair->destinationIndex;
            uint32_t encodedSrc = usagePair->encodedSourceIndex;
            Il2CppMetadataUsage usage = GetEncodedIndexType(encodedSrc);
            uint32_t decodedIndex     = GetDecodedMethodIndex(encodedSrc);

            // 计算运行时地址
            uintptr_t usageAddr = 0;
            if (m_pil2CppMetadataRegistration->metadataUsages &&
                destIndex < m_pil2CppMetadataRegistration->metadataUsagesCount) {
                usageAddr = reinterpret_cast<uintptr_t>(
                        &(m_pil2CppMetadataRegistration->metadataUsages[destIndex]));
                if (usageAddr >= m_il2cppbase) usageAddr -= m_il2cppbase;
            }
            if (usageAddr == 0) continue;

            try {
                switch (usage) {
                    case kIl2CppMetadataUsageStringLiteral:
                        HandleStringLiteral(decodedIndex, usageAddr, limits, scriptStrings);
                        break;
                    case kIl2CppMetadataUsageTypeInfo:
                        HandleTypeMetadata(decodedIndex, usageAddr, limits, "_TypeInfo", "Il2CppClass*", scriptMetadata);
                        break;
                    case kIl2CppMetadataUsageIl2CppType:
                        HandleTypeMetadata(decodedIndex, usageAddr, limits, "_var", "Il2CppType*", scriptMetadata);
                        break;
                    case kIl2CppMetadataUsageFieldInfo:
                        HandleFieldInfo(decodedIndex, usageAddr, limits, scriptMetadata);
                        break;
                    case kIl2CppMetadataUsageMethodDef:
                        HandleMethodDef(decodedIndex, usageAddr, limits, scriptMetadataMethod, allAddresses);
                        break;
                    case kIl2CppMetadataUsageMethodRef:
                        HandleMethodRef(decodedIndex, usageAddr, limits, scriptMetadataMethod, allAddresses);
                        break;
                    default:
                        break;
                }
            } catch (...) {
                LOG(LOG_LEVEL_WARN, "[SaveToMethodJson] Exception at usage list %d, pair %d, skipping", i, j);
            }
        }
    }
}

// =====================================================================
// 写入 ScriptString 段
// =====================================================================
void li2cppHeader::li2cppHeader::WriteScriptStringSection(
        std::ofstream& fout, const std::vector<ScriptStringEntry>& entries)
{
    fout << "  \"ScriptString\": [\n";
    bool first = true;
    for (const auto& e : entries) {
        if (!first) fout << ",\n";
        fout << "    {\n";
        fout << "      \"Address\": " << e.address << ",\n";
        fout << "      \"Value\": \"" << EscapeJsonString(e.value) << "\"\n";
        fout << "    }";
        first = false;
    }
    fout << "\n  ],\n";
}

// =====================================================================
// 写入 ScriptMetadata 段
// =====================================================================
void li2cppHeader::li2cppHeader::WriteScriptMetadataSection(
        std::ofstream& fout, const std::vector<ScriptMetadataEntry>& entries)
{
    fout << "  \"ScriptMetadata\": [\n";
    bool first = true;
    for (const auto& e : entries) {
        if (!first) fout << ",\n";
        fout << "    {\n";
        fout << "      \"Address\": " << e.address << ",\n";
        fout << "      \"Name\": \"" << EscapeJsonString(e.name) << "\",\n";
        fout << "      \"Signature\": \"" << e.signature << "\"\n";
        fout << "    }";
        first = false;
    }
    fout << "\n  ],\n";
}

// =====================================================================
// 写入 ScriptMetadataMethod 段
// =====================================================================
void li2cppHeader::li2cppHeader::WriteScriptMetadataMethodSection(
        std::ofstream& fout, const std::vector<ScriptMetadataMethodEntry>& entries)
{
    fout << "  \"ScriptMetadataMethod\": [\n";
    bool first = true;
    for (const auto& e : entries) {
        if (!first) fout << ",\n";
        fout << "    {\n";
        fout << "      \"Address\": " << e.address << ",\n";
        fout << "      \"Name\": \"" << EscapeJsonString(e.name) << "\",\n";
        fout << "      \"MethodAddress\": " << e.methodAddress << "\n";
        fout << "    }";
        first = false;
    }
    fout << "\n  ],\n";
}

// =====================================================================
// 写入 Addresses 段
// =====================================================================
void li2cppHeader::li2cppHeader::WriteAddressesSection(
        std::ofstream& fout, const std::set<uintptr_t>& addresses)
{
    fout << "  \"Addresses\": [\n";
    bool first = true;
    for (uintptr_t addr : addresses) {
        if (!first) fout << ",\n";
        fout << "    " << addr;
        first = false;
    }
    fout << "\n  ]\n";
}

// =====================================================================
// SaveToMethodJson — 主入口（组装各段）
// =====================================================================
void li2cppHeader::li2cppHeader::SaveToMethodJson(const std::string& path) {
    std::ofstream fout(path);
    if (!fout.is_open()) return;

    std::set<uintptr_t> allAddresses;

    fout << "{\n";

    // 1. ScriptMethod
    WriteScriptMethods(fout, allAddresses);

    // 2~4. 收集元数据
    std::vector<ScriptStringEntry> scriptStrings;
    std::vector<ScriptMetadataEntry> scriptMetadata;
    std::vector<ScriptMetadataMethodEntry> scriptMetadataMethod;
    CollectMetadataUsages(scriptStrings, scriptMetadata, scriptMetadataMethod, allAddresses);

    // 2. ScriptString
    WriteScriptStringSection(fout, scriptStrings);

    // 3. ScriptMetadata
    WriteScriptMetadataSection(fout, scriptMetadata);

    // 4. ScriptMetadataMethod
    WriteScriptMetadataMethodSection(fout, scriptMetadataMethod);

    // 5. Addresses
    WriteAddressesSection(fout, allAddresses);

    fout << "}";

    fout.close();
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif