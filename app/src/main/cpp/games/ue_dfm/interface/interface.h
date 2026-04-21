#ifndef UE5_DFM_INTERFACE_H
#define UE5_DFM_INTERFACE_H

#include "../ilbUE5Dumper/UE5DfmDumper.h"
#include "../ilbUE5Struct/UE5DfmStruct.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

// =====================================================================
//  UE5 DFM 类信息采集与查询接口层
//
//  功能与 ueGamepubgmhd/interface 相同, 但适配 UE5.4 反射系统:
//    - UE5 使用 FField/FProperty 替代 UE4 的 UProperty
//    - UE5 使用 UStruct::ChildProperties (FField* 链表) 遍历属性
//    - UE5 使用 UStruct::Children (UField* 链表) 遍历函数
//    - 所有偏移通过 offsetof() 从结构体定义获取, 不使用硬编码常量
// =====================================================================

namespace ue5dfminf {

// =====================================================================
//  数据结构
// =====================================================================

/// 字段/属性信息 (来自 FField/FProperty)
struct UEFieldInfo {
    std::string name;           ///< 属性名称
    std::string typeName;       ///< 属性类型名 (FFieldClass::Name, 如 "StructProperty")
    int32_t     offset = 0;     ///< 容器内偏移 (FPropertyFlat::Offset_Internal)
    int32_t     elementSize = 0;///< 每个元素大小 (FPropertyFlat::ElementSize)
    int32_t     arrayDim = 1;   ///< 数组维度 (FPropertyFlat::ArrayDim)
    uint32_t    propertyFlags = 0; ///< EPropertyFlags (低 32 位)
};

/// 函数信息 (来自 UFunction)
struct UEFuncInfo {
    std::string name;           ///< 函数名称
    uintptr_t   funcPtr = 0;   ///< C++ 函数指针 (UFunction::Func) — 一般是 exec thunk
    uintptr_t   ufunctionPtr = 0; ///< UFunction UObject* 自身地址 (用于 ProcessEvent 第二参)
    uint32_t    funcFlags = 0;  ///< EFunctionFlags (UFunction::FunctionFlags)
    uint8_t     numParms = 0;   ///< 参数数量 (UFunction::NumParms)
    std::string location;       ///< 模块内偏移文本
};

/// 单个类的完整信息
struct UEClassData {
    std::string className;      ///< 类名
    std::string fullPath;       ///< 完整路径 (Package.Outer.Name)
    std::string superName;      ///< 父类名
    uintptr_t   classPtr = 0;   ///< UClass/UScriptStruct 地址
    int32_t     propertiesSize = 0; ///< 类实例总大小

    std::vector<UEFieldInfo> fields;    ///< 字段列表 (FProperty 链)
    std::vector<UEFuncInfo>  functions; ///< 函数列表 (UFunction Children 链)
};

// =====================================================================
//  UE5DfmInterface — 主类
// =====================================================================
class UE5DfmInterface {
public:
    /// 构造: 传入已初始化的 UE5DfmDumper 引用
    explicit UE5DfmInterface(ue5dfm::UE5DfmDumper& dumper);
    ~UE5DfmInterface();

    // ---- 数据采集 ----

    /// 遍历 GUObjectArray, 采集所有 UClass/UScriptStruct 的字段和函数信息
    void fillClassInfo();

    /// 获取已采集的类数量
    int getClassCount() const { return static_cast<int>(m_classes.size()); }

    // ---- 查询接口 ----

    /// 按类名查找类信息 (返回 nullptr 表示未找到)
    const UEClassData* findClass(const std::string& className) const;

    /// 按类名+字段名查找字段偏移 (返回 -1 表示未找到)
    int32_t getFieldOffset(const std::string& className, const std::string& fieldName) const;

    /// 按类名+字段名查找字段偏移, 自动搜索父类继承链 (返回 -1 表示未找到)
    int32_t getFieldOffsetInHierarchy(const std::string& className, const std::string& fieldName) const;

    /// 按类名+字段名查找字段信息, 自动搜索父类继承链
    const UEFieldInfo* findFieldInHierarchy(const std::string& className, const std::string& fieldName,
                                            std::string* outOwnerClass = nullptr) const;

    /// 按类名+字段名查找完整字段信息
    const UEFieldInfo* getFieldInfo(const std::string& className, const std::string& fieldName) const;

    /// 按类名+函数名查找函数地址 (返回 0 表示未找到)
    uintptr_t getFuncAddress(const std::string& className, const std::string& funcName) const;

    /// 按类名+函数名查找完整函数信息
    const UEFuncInfo* getFuncInfo(const std::string& className, const std::string& funcName) const;

    /// 获取类的所有字段列表
    std::vector<UEFieldInfo> getFields(const std::string& className) const;

    /// 获取类的所有函数列表
    std::vector<UEFuncInfo> getFunctions(const std::string& className) const;

    /// 获取所有已采集的类数据 (只读引用)
    const std::vector<UEClassData>& getAllClasses() const { return m_classes; }

    // ---- 日志/导出 ----

    /// 将所有采集到的类信息导出到文件
    bool exportToFile(const std::string& filePath) const;

private:
    void collectClassData(uintptr_t classPtr);

    ue5dfm::UE5DfmDumper& m_dumper;
    std::vector<UEClassData> m_classes;
    std::unordered_map<std::string, size_t> m_classIndex;
};

} // namespace ue5dfminf

#endif // UE5_DFM_INTERFACE_H
