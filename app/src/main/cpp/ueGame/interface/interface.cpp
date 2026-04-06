#include "interface.h"
#include "../../Log/log.h"

#include <cstring>
#include <cstdio>
#include <algorithm>

#define TAG "[UE4Interface]"

namespace ue4inf {

// =====================================================================
//  构造 / 析构
// =====================================================================
UE4Interface::UE4Interface(ue4::UE4Dumper& dumper)
    : m_dumper(dumper)
{
}

UE4Interface::~UE4Interface() = default;

// =====================================================================
//  获取 UProperty 类型名
// =====================================================================
std::string UE4Interface::getPropertyTypeName(uintptr_t propPtr) {
    // UProperty 的 UClass 名称即为类型名 (如 "FloatProperty", "ObjectProperty")
    return m_dumper.readClassName(propPtr);
}

// =====================================================================
//  收集单个 UClass/UScriptStruct 的字段和函数
// =====================================================================
void UE4Interface::collectClassData(uintptr_t classPtr) {
    using namespace ue4;

    UEClassData cls;
    cls.classPtr = classPtr;
    cls.className = m_dumper.readObjectFName(classPtr);
    cls.fullPath = m_dumper.readFullPath(classPtr);

    // 父类
    auto* ustruct = reinterpret_cast<UStruct*>(classPtr);
    cls.superName = ustruct->SuperStruct ? m_dumper.readObjectFName(reinterpret_cast<uintptr_t>(ustruct->SuperStruct)) : "";
    cls.propertiesSize = ustruct->PropertiesSize;

    // ---- 遍历 Children 链表, 收集字段和函数 ----
    auto* childField = ustruct->Children;
    int depth = 0;
    while (childField != nullptr && depth < 4096) {
        uintptr_t child = reinterpret_cast<uintptr_t>(childField);
        std::string childClassName = m_dumper.readClassName(child);
        std::string childName = m_dumper.readObjectFName(child);

        if (childClassName.find("Property") != std::string::npos) {
            auto* prop = reinterpret_cast<UProperty*>(childField);
            UEFieldInfo field;
            field.name = childName;
            field.typeName = childClassName;
            field.offset = prop->Offset_Internal;
            field.elementSize = prop->ElementSize;
            field.arrayDim = prop->ArrayDim;
            field.propertyFlags = prop->PropertyFlags;
            cls.fields.push_back(field);
        } else if (childClassName == "Function") {
            auto* func = reinterpret_cast<UFunction*>(childField);
            UEFuncInfo fi;
            fi.name = childName;
            fi.funcPtr = reinterpret_cast<uintptr_t>(func->Func);
            fi.funcFlags = func->FunctionFlags;
            fi.numParms = func->NumParms;
            if (fi.funcPtr != 0) {
                fi.location = m_dumper.getModuleOffsetText(fi.funcPtr);
            }
            cls.functions.push_back(fi);
        }

        childField = childField->Next;
        depth++;
    }

    m_classIndex[cls.className] = m_classes.size();
    m_classes.push_back(std::move(cls));
}

// =====================================================================
//  GUObjectArray 遍历回调 (静态)
// =====================================================================
void fillCallback(uintptr_t objPtr, int globalIdx, void* userData) {
    auto* self = reinterpret_cast<UE4Interface*>(userData);
    // 只采集 UClass 和 UScriptStruct
    std::string className = self->m_dumper.readClassName(objPtr);
    if (className == "Class" || className == "ScriptStruct") {
        self->collectClassData(objPtr);
    }
}

// 需要让回调能访问 m_dumper，声明 friend
// 注：已在 collectClassData 中使用 m_dumper，fillCallback 通过 userData 传 this

// =====================================================================
//  fillClassInfo — 遍历 GUObjectArray 采集所有类信息
// =====================================================================
void UE4Interface::fillClassInfo() {
    m_classes.clear();
    m_classIndex.clear();

    LOG(LOG_LEVEL_INFO, TAG " 开始采集 UE4 类信息...");

    uintptr_t arrayBase = m_dumper.getGUObjectArray();
    int total = m_dumper.forEachUObject(arrayBase, fillCallback, this);

    LOG(LOG_LEVEL_INFO, TAG " 采集完成: 扫描 %d 个 UObject, 收集 %d 个类/结构体",
        total, static_cast<int>(m_classes.size()));
}

// =====================================================================
//  查询接口
// =====================================================================
const UEClassData* UE4Interface::findClass(const std::string& className) const {
    auto it = m_classIndex.find(className);
    if (it == m_classIndex.end()) return nullptr;
    return &m_classes[it->second];
}

int32_t UE4Interface::getFieldOffset(const std::string& className, const std::string& fieldName) const {
    const UEFieldInfo* info = getFieldInfo(className, fieldName);
    return info ? info->offset : -1;
}

const UEFieldInfo* UE4Interface::getFieldInfo(const std::string& className, const std::string& fieldName) const {
    const UEClassData* cls = findClass(className);
    if (!cls) return nullptr;
    for (const auto& f : cls->fields) {
        if (f.name == fieldName) return &f;
    }
    return nullptr;
}

uintptr_t UE4Interface::getFuncAddress(const std::string& className, const std::string& funcName) const {
    const UEFuncInfo* info = getFuncInfo(className, funcName);
    return info ? info->funcPtr : 0;
}

const UEFuncInfo* UE4Interface::getFuncInfo(const std::string& className, const std::string& funcName) const {
    const UEClassData* cls = findClass(className);
    if (!cls) return nullptr;
    for (const auto& f : cls->functions) {
        if (f.name == funcName) return &f;
    }
    return nullptr;
}

std::vector<UEFieldInfo> UE4Interface::getFields(const std::string& className) const {
    const UEClassData* cls = findClass(className);
    return cls ? cls->fields : std::vector<UEFieldInfo>{};
}

std::vector<UEFuncInfo> UE4Interface::getFunctions(const std::string& className) const {
    const UEClassData* cls = findClass(className);
    return cls ? cls->functions : std::vector<UEFuncInfo>{};
}

// =====================================================================
//  导出到文件
// =====================================================================
bool UE4Interface::exportToFile(const std::string& filePath) const {
    FILE* fp = fopen(filePath.c_str(), "w");
    if (!fp) {
        LOG(LOG_LEVEL_ERROR, TAG " 无法打开文件: %s", filePath.c_str());
        return false;
    }

    fprintf(fp, "=== UE4 Class Info Export (%d classes) ===\n\n", static_cast<int>(m_classes.size()));

    for (const auto& cls : m_classes) {
        fprintf(fp, "// ----------------------------------------\n");
        fprintf(fp, "// Class: %s", cls.className.c_str());
        if (!cls.superName.empty()) fprintf(fp, " : %s", cls.superName.c_str());
        fprintf(fp, "  (Size: 0x%X)\n", cls.propertiesSize);
        if (!cls.fullPath.empty()) fprintf(fp, "// Path:  %s\n", cls.fullPath.c_str());
        fprintf(fp, "// Ptr:   0x%llX\n", (unsigned long long)cls.classPtr);

        // 字段
        if (!cls.fields.empty()) {
            fprintf(fp, "//\n// Fields (%d):\n", static_cast<int>(cls.fields.size()));
            for (const auto& f : cls.fields) {
                fprintf(fp, "//   +0x%04X  %-30s  %-24s  size=%d",
                        f.offset, f.name.c_str(), f.typeName.c_str(), f.elementSize);
                if (f.arrayDim > 1) fprintf(fp, " [%d]", f.arrayDim);
                fprintf(fp, "\n");
            }
        }

        // 函数
        if (!cls.functions.empty()) {
            fprintf(fp, "//\n// Functions (%d):\n", static_cast<int>(cls.functions.size()));
            for (const auto& f : cls.functions) {
                fprintf(fp, "//   0x%llX  %-30s  params=%d  flags=0x%08X",
                        (unsigned long long)f.funcPtr, f.name.c_str(), f.numParms, f.funcFlags);
                if (!f.location.empty()) fprintf(fp, "  [%s]", f.location.c_str());
                fprintf(fp, "\n");
            }
        }

        fprintf(fp, "\n");
    }

    fclose(fp);
    LOG(LOG_LEVEL_INFO, TAG " 已导出到 %s (%d classes)", filePath.c_str(), static_cast<int>(m_classes.size()));
    return true;
}

} // namespace ue4inf

OBFU_ATTRS_END
