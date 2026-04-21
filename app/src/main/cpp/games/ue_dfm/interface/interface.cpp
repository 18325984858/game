/**
 * @file    interface.cpp
 * @brief   UE5 DFM 类信息采集与查询接口层实现
 *
 *  与 PUBG (UE4) interface 的关键区别:
 *    - UE4 属性遍历: UStruct::Children (UField* 链, UProperty 继承 UField)
 *    - UE5 属性遍历: UStruct::ChildProperties (FField* 链, FProperty 继承 FField)
 *    - UE5 函数遍历: UStruct::Children (UField* 链, 仅 UFunction)
 *    - 所有偏移通过 offsetof(结构体, 成员) 获取
 */
#include "interface.h"
#include "../../../core/log/log.h"

#include <cstring>
#include <cstdio>
#include <cstddef>
#include <algorithm>

#define TAG "[UE5DfmInterface]"

// 从 UE5DfmStruct.h 引入结构体, 用 offsetof 获取偏移
using namespace ue5dfm;

namespace ue5dfminf {

// =====================================================================
//  构造 / 析构
// =====================================================================

UE5DfmInterface::UE5DfmInterface(ue5dfm::UE5DfmDumper& dumper)
    : m_dumper(dumper)
{
}

UE5DfmInterface::~UE5DfmInterface() = default;

// =====================================================================
//  安全内存读取辅助 (复用 Dumper 的静态方法)
// =====================================================================

static uintptr_t rp(uintptr_t addr) { return ue5dfm::UE5DfmDumper::rp(addr); }
static uint32_t  r32(uintptr_t addr) { return ue5dfm::UE5DfmDumper::r32(addr); }
static int32_t   rs32(uintptr_t addr) { return ue5dfm::UE5DfmDumper::rs32(addr); }
static bool      ok(uintptr_t p) { return ue5dfm::UE5DfmDumper::ok(p); }

// =====================================================================
//  收集单个 UClass/UScriptStruct 的字段和函数
// =====================================================================

void UE5DfmInterface::collectClassData(uintptr_t classPtr) {
    UEClassData cls;
    cls.classPtr = classPtr;
    cls.className = m_dumper.oname(classPtr);
    cls.fullPath = m_dumper.getFullPath(classPtr);

    // 父类: UStruct::SuperStruct
    uintptr_t superPtr = rp(classPtr + offsetof(UStruct, SuperStruct));
    cls.superName = ok(superPtr) ? m_dumper.oname(superPtr) : "";

    // 类实例大小: UStruct::PropertiesSize
    cls.propertiesSize = rs32(classPtr + offsetof(UStruct, PropertiesSize));

    // ── 遍历 FField* ChildProperties 链表 (UE5 属性) ──
    uintptr_t fpCur = rp(classPtr + offsetof(UStruct, ChildProperties));
    int fpDepth = 0;
    while (ok(fpCur) && fpDepth < 4096) {
        fpDepth++;

        // 获取 FFieldClass::Name 判断是否是 Property
        std::string ffcn = m_dumper.ffclassname(fpCur);
        if (ffcn.find("Property") != std::string::npos) {
            UEFieldInfo field;
            field.name = m_dumper.ffname(fpCur);
            field.typeName = ffcn;

            // 使用 FPropertyFlat 的 offsetof 读取成员
            field.arrayDim    = rs32(fpCur + offsetof(FPropertyFlat, ArrayDim));
            field.elementSize = rs32(fpCur + offsetof(FPropertyFlat, ElementSize));
            field.offset      = rs32(fpCur + offsetof(FPropertyFlat, Offset_Internal));
            field.propertyFlags = r32(fpCur + offsetof(FPropertyFlat, PropertyFlags));

            cls.fields.push_back(std::move(field));
        }

        // FField::Next
        fpCur = rp(fpCur + offsetof(FField, Next));
    }

    // ── 遍历 UField* Children 链表 (UE5 函数通过 UField::Next) ──
    uintptr_t ufCur = rp(classPtr + offsetof(UStruct, Children));
    int ufDepth = 0;
    while (ok(ufCur) && ufDepth < 4096) {
        ufDepth++;

        // 检查 Children 中的 UClass 名是否为 Function
        uintptr_t ucl = rp(ufCur + offsetof(UObjectBase, ClassPrivate));
        if (ok(ucl)) {
            std::string uclName = m_dumper.oname(ucl);
            if (uclName == "Function" || uclName == "DelegateFunction") {
                UEFuncInfo fi;
                fi.name = m_dumper.oname(ufCur);

                fi.ufunctionPtr = ufCur;  // UFunction UObject* — 用于 ProcessEvent 第二参
                // UFunction 成员通过 offsetof 读取
                fi.funcPtr   = rp(ufCur + offsetof(UFunction, Func));
                fi.funcFlags = r32(ufCur + offsetof(UFunction, FunctionFlags));
                fi.numParms  = static_cast<uint8_t>(
                    rs32(ufCur + offsetof(UFunction, NumParms)) & 0xFF);

                if (fi.funcPtr != 0 && m_dumper.getModuleBase() != 0) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "libUE4.so+0x%lX",
                             static_cast<unsigned long>(fi.funcPtr - m_dumper.getModuleBase()));
                    fi.location = buf;
                }

                cls.functions.push_back(std::move(fi));
            }
        }

        // UField::Next
        ufCur = rp(ufCur + offsetof(UField, Next));
    }

    m_classIndex[cls.className] = m_classes.size();
    m_classes.push_back(std::move(cls));
}

// =====================================================================
//  fillClassInfo — 遍历 GUObjectArray 采集所有类信息
// =====================================================================

void UE5DfmInterface::fillClassInfo() {
    m_classes.clear();
    m_classIndex.clear();

    LOG(LOG_LEVEL_INFO, TAG " 开始采集 UE5 DFM 类信息...");

    uint32_t maxObj = r32(m_dumper.getModuleBase() + m_dumper.getOffGUObjectArrayNum());
    int classCount = 0;

    for (uint32_t idx = 0; idx < maxObj; idx++) {
        uintptr_t p = m_dumper.getobj(static_cast<int>(idx));
        if (!ok(p)) continue;

        uintptr_t cp = rp(p + offsetof(UObjectBase, ClassPrivate));
        if (!ok(cp)) continue;

        std::string cn = m_dumper.oname(cp);
        if (cn == "Class" || cn == "ScriptStruct") {
            collectClassData(p);
            classCount++;
        }
    }

    LOG(LOG_LEVEL_INFO, TAG " 采集完成: 扫描 %u 个 UObject, 收集 %d 个类/结构体",
        maxObj, classCount);
}

// =====================================================================
//  查询接口
// =====================================================================

const UEClassData* UE5DfmInterface::findClass(const std::string& className) const {
    auto it = m_classIndex.find(className);
    if (it == m_classIndex.end()) return nullptr;
    return &m_classes[it->second];
}

int32_t UE5DfmInterface::getFieldOffset(const std::string& className, const std::string& fieldName) const {
    const UEFieldInfo* info = getFieldInfo(className, fieldName);
    return info ? info->offset : -1;
}

int32_t UE5DfmInterface::getFieldOffsetInHierarchy(const std::string& className, const std::string& fieldName) const {
    const UEFieldInfo* info = findFieldInHierarchy(className, fieldName);
    return info ? info->offset : -1;
}

const UEFieldInfo* UE5DfmInterface::findFieldInHierarchy(
        const std::string& className, const std::string& fieldName,
        std::string* outOwnerClass) const {

    const UEClassData* cls = findClass(className);
    if (!cls) return nullptr;

    // 先搜索当前类
    for (const auto& f : cls->fields) {
        if (f.name == fieldName) {
            if (outOwnerClass) *outOwnerClass = cls->className;
            return &f;
        }
    }

    // 沿继承链向上搜索
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

const UEFieldInfo* UE5DfmInterface::getFieldInfo(const std::string& className, const std::string& fieldName) const {
    const UEClassData* cls = findClass(className);
    if (!cls) return nullptr;
    for (const auto& f : cls->fields) {
        if (f.name == fieldName) return &f;
    }
    return nullptr;
}

uintptr_t UE5DfmInterface::getFuncAddress(const std::string& className, const std::string& funcName) const {
    const UEFuncInfo* info = getFuncInfo(className, funcName);
    return info ? info->funcPtr : 0;
}

const UEFuncInfo* UE5DfmInterface::getFuncInfo(const std::string& className, const std::string& funcName) const {
    const UEClassData* cls = findClass(className);
    if (!cls) return nullptr;
    for (const auto& f : cls->functions) {
        if (f.name == funcName) return &f;
    }
    return nullptr;
}

std::vector<UEFieldInfo> UE5DfmInterface::getFields(const std::string& className) const {
    const UEClassData* cls = findClass(className);
    return cls ? cls->fields : std::vector<UEFieldInfo>{};
}

std::vector<UEFuncInfo> UE5DfmInterface::getFunctions(const std::string& className) const {
    const UEClassData* cls = findClass(className);
    return cls ? cls->functions : std::vector<UEFuncInfo>{};
}

// =====================================================================
//  导出到文件
// =====================================================================

bool UE5DfmInterface::exportToFile(const std::string& filePath) const {
    FILE* fp = fopen(filePath.c_str(), "w");
    if (!fp) {
        LOG(LOG_LEVEL_ERROR, TAG " 无法打开文件: %s", filePath.c_str());
        return false;
    }

    fprintf(fp, "=== UE5 DFM Class Info Export (%d classes) ===\n\n",
            static_cast<int>(m_classes.size()));

    for (const auto& cls : m_classes) {
        fprintf(fp, "// ----------------------------------------\n");
        fprintf(fp, "// Class: %s", cls.className.c_str());
        if (!cls.superName.empty()) fprintf(fp, " : %s", cls.superName.c_str());
        fprintf(fp, "  (Size: 0x%X)\n", cls.propertiesSize);
        if (!cls.fullPath.empty()) fprintf(fp, "// Path:  %s\n", cls.fullPath.c_str());
        fprintf(fp, "// Ptr:   0x%llX\n", (unsigned long long)cls.classPtr);

        if (!cls.fields.empty()) {
            fprintf(fp, "//\n// Fields (%d):\n", static_cast<int>(cls.fields.size()));
            for (const auto& f : cls.fields) {
                fprintf(fp, "//   +0x%04X  %-30s  %-24s  size=%d",
                        f.offset, f.name.c_str(), f.typeName.c_str(), f.elementSize);
                if (f.arrayDim > 1) fprintf(fp, " [%d]", f.arrayDim);
                fprintf(fp, "\n");
            }
        }

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
    LOG(LOG_LEVEL_INFO, TAG " 已导出到 %s (%d classes)", filePath.c_str(),
        static_cast<int>(m_classes.size()));
    return true;
}

} // namespace ue5dfminf
