#include "interface.h"
#include "../../../core/log/log.h"

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

    // 父类 — UStruct::SuperStruct @ +0x30, PropertiesSize @ +0x40
    uintptr_t superPtr = m_dumper.safeReadPtr(classPtr + 0x30);
    cls.superName = (superPtr != 0 && superPtr >= 0x10000) ? m_dumper.readObjectFName(superPtr) : "";
    cls.propertiesSize = m_dumper.safeReadS32(classPtr + 0x40);

    // ---- 遍历 Children 链表 (UStruct::Children @ +0x38) ----
    uintptr_t childPtr = m_dumper.safeReadPtr(classPtr + 0x38);
    int depth = 0;
    while (childPtr != 0 && childPtr >= 0x10000 && depth < 4096) {
        std::string childClassName = m_dumper.readClassName(childPtr);
        std::string childName = m_dumper.readObjectFName(childPtr);

        if (childClassName.find("Property") != std::string::npos) {
            UEFieldInfo field;
            field.name = childName;
            field.typeName = childClassName;
            field.offset = m_dumper.safeReadS32(childPtr + 0x44);       // Offset_Internal
            field.elementSize = m_dumper.safeReadS32(childPtr + 0x38);  // ElementSize
            field.arrayDim = m_dumper.safeReadS32(childPtr + 0x30);     // ArrayDim
            // PropertyFlags (uint64 @ +0x48)
            uint64_t flags = 0;
            int32_t lo = m_dumper.safeReadS32(childPtr + 0x48);
            int32_t hi = m_dumper.safeReadS32(childPtr + 0x4C);
            flags = (static_cast<uint64_t>(static_cast<uint32_t>(hi)) << 32) | static_cast<uint32_t>(lo);
            field.propertyFlags = flags;
            cls.fields.push_back(field);
        } else if (childClassName == "Function") {
            UEFuncInfo fi;
            fi.name = childName;
            fi.funcPtr = m_dumper.safeReadPtr(childPtr + 0xB0);       // Func
            fi.funcFlags = static_cast<uint32_t>(m_dumper.safeReadS32(childPtr + 0x88));  // FunctionFlags
            fi.numParms = static_cast<uint8_t>(m_dumper.safeReadS32(childPtr + 0x8E) & 0xFF);  // NumParms
            if (fi.funcPtr != 0) {
                fi.location = m_dumper.getModuleOffsetText(fi.funcPtr);
            }
            cls.functions.push_back(fi);
        }

        childPtr = m_dumper.safeReadPtr(childPtr + 0x28);  // UField::Next
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

int32_t UE4Interface::getFieldOffsetInHierarchy(const std::string& className, const std::string& fieldName) const {
    const UEFieldInfo* info = findFieldInHierarchy(className, fieldName);
    return info ? info->offset : -1;
}

const UEFieldInfo* UE4Interface::findFieldInHierarchy(const std::string& className, const std::string& fieldName, std::string* outOwnerClass) const {
    const UEClassData* cls = findClass(className);
    if (!cls) return nullptr;

    // 先搜索当前类的字段
    for (const auto& f : cls->fields) {
        if (f.name == fieldName) {
            if (outOwnerClass) *outOwnerClass = cls->className;
            return &f;
        }
    }

    // 沿继承链向上搜索父类
    std::string superName = cls->superName;
    int depth = 0;
    while (!superName.empty() && depth < 30) {
        const UEClassData* superCls = findClass(superName);
        if (!superCls) break;
        for (const auto& f : superCls->fields) {
            if (f.name == fieldName) {
                if (outOwnerClass) *outOwnerClass = superCls->className;
                return &f;
            }
        }
        superName = superCls->superName;
        depth++;
    }
    return nullptr;
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
