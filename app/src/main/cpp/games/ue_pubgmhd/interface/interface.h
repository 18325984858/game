#ifndef UE4_INTERFACE_H
#define UE4_INTERFACE_H

#include "../engine/UE4Dumper.h"
#include "../engine/UE4Struct.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cstdint>

// =====================================================================
//  UE4 类信息采集与查询接口层 — 运行时 UClass/UProperty/UFunction
//  元数据的结构化存储与检索
//
//  功能类似 unityGame/interface 中的 IL2CPP 接口:
//    - 遍历 GUObjectArray 中所有 UClass/UScriptStruct
//    - 收集每个类的字段(UProperty): 名称、偏移、大小、类型
//    - 收集每个类的函数(UFunction): 名称、地址、标志
//    - 提供按类名查找字段偏移、函数地址的接口
// =====================================================================

namespace ue4inf {

// =====================================================================
//  数据结构
// =====================================================================

/// 字段/属性信息
struct UEFieldInfo {
    std::string name;           ///< 属性名称
    std::string typeName;       ///< 属性类型名 (如 FloatProperty, ObjectProperty)
    int32_t     offset = 0;     ///< 容器内偏移 (Offset_Internal)
    int32_t     elementSize = 0;///< 每个元素大小
    int32_t     arrayDim = 1;   ///< 数组维度
    uint64_t    propertyFlags = 0; ///< EPropertyFlags
};

/// 函数信息
struct UEFuncInfo {
    std::string name;           ///< 函数名称
    uintptr_t   funcPtr = 0;    ///< C++ 函数指针 (UFunction::Func)
    uint32_t    funcFlags = 0;  ///< EFunctionFlags
    uint8_t     numParms = 0;   ///< 参数数量
    std::string location;       ///< 模块内偏移文本 (如 "libUE4.so+0x1234")
};

/// 单个类的完整信息
struct UEClassData {
    std::string className;      ///< 类名 (UObject 名称)
    std::string fullPath;       ///< 完整路径 (Package.Outer.Name)
    std::string superName;      ///< 父类名
    uintptr_t   classPtr = 0;   ///< UClass* 地址
    int32_t     propertiesSize = 0; ///< 类实例总大小

    std::vector<UEFieldInfo> fields;    ///< 字段列表
    std::vector<UEFuncInfo>  functions; ///< 函数列表
};

// =====================================================================
//  UE4Interface — 主类
// =====================================================================
class UE4Interface {
public:
    /// 构造: 传入已初始化的 UE4Dumper 引用
    explicit UE4Interface(ue4::UE4Dumper& dumper);
    ~UE4Interface();

    // GUObjectArray 遍历回调需要访问内部成员
    friend void fillCallback(uintptr_t objPtr, int globalIdx, void* userData);

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

    /// 按类名+字段名查找字段信息, 自动搜索父类继承链 (返回 nullptr 表示未找到)
    const UEFieldInfo* findFieldInHierarchy(const std::string& className, const std::string& fieldName, std::string* outOwnerClass = nullptr) const;

    /// 按类名+字段名查找完整字段信息 (返回 nullptr 表示未找到)
    const UEFieldInfo* getFieldInfo(const std::string& className, const std::string& fieldName) const;

    /// 按类名+函数名查找函数地址 (返回 0 表示未找到)
    uintptr_t getFuncAddress(const std::string& className, const std::string& funcName) const;

    /// 按类名+函数名查找完整函数信息 (返回 nullptr 表示未找到)
    const UEFuncInfo* getFuncInfo(const std::string& className, const std::string& funcName) const;

    /// 获取类的所有字段列表 (返回空 vector 表示未找到)
    std::vector<UEFieldInfo> getFields(const std::string& className) const;

    /// 获取类的所有函数列表 (返回空 vector 表示未找到)
    std::vector<UEFuncInfo> getFunctions(const std::string& className) const;

    /// 获取所有已采集的类数据 (只读引用)
    const std::vector<UEClassData>& getAllClasses() const { return m_classes; }

    // ---- 日志/导出 ----

    /// 将所有采集到的类信息导出到文件
    bool exportToFile(const std::string& filePath) const;

private:
    // ---- 内部辅助 ----
    void collectClassData(uintptr_t classPtr);
    std::string getPropertyTypeName(uintptr_t propPtr);

    // ---- 成员 ----
    ue4::UE4Dumper& m_dumper;
    std::vector<UEClassData> m_classes;
    std::unordered_map<std::string, size_t> m_classIndex; ///< className -> m_classes 下标
};

} // namespace ue4inf

#endif // UE4_INTERFACE_H
