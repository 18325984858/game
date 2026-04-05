#ifndef UE4_DUMPER_H
#define UE4_DUMPER_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

// =====================================================================
//  UE4.18 GNames / GUObjectArray / GWorld Dump
//  Target: com.tencent.tmgp.pubgmhd  (ARM64 Android)
//  从 frida js 脚本转化为 C++ 原生实现
// =====================================================================

namespace ue4 {

// ---- Dumper 类 ----
class UE4Dumper {
public:
    UE4Dumper(uintptr_t moduleBase, uint64_t dqGNames, uint64_t dqGUObjectArray, uint64_t dqGWorld, std::string outputPath = "");
    ~UE4Dumper();

    /// 设置 libUE4.so 基址
    void setModuleBase(uintptr_t base);

    /// 设置 libUE4.so 模块大小
    void setModuleSize(uintptr_t size);

    /// 设置 GNames 数组指针 (已解引用后的值)
    void setGNames(uintptr_t gnames);

    /// 设置 GWorld 指针
    void setGWorld(uintptr_t gworld);

    /// 设置 GUObjectArray 地址
    void setGUObjectArray(uintptr_t guobjectarray);

    /// 设置输出目录路径
    void setOutputPath(const std::string& path);

    /// 初始化: 验证 GNames 有效性, 计算 numNames
    bool init();

    /// dump GNames 到文件
    bool dumpGNames(const char* filePath = nullptr);

    /// dump GUObjectArray 到文件
    bool dumpGObjects(const char* filePath = nullptr);

    /// dump GWorld 信息到文件
    bool dumpGWorld(const char* filePath = nullptr);

    /// 一键 dump 全部
    bool dumpAll();

    /// SDK dump: 导出所有 Class/ScriptStruct/Enum 的字段、函数、继承信息
    bool dumpSDK(const char* filePath = nullptr);

    // ---- Getter ----
    uintptr_t getModuleBase() const { return m_moduleBase; }
    uintptr_t getGNames() const { return m_GNames; }
    uintptr_t getGWorld() const { return m_GWorld; }
    uintptr_t getGUObjectArray() const { return m_GUObjectArray; }
    int getNumNames() const { return m_numNames; }
    const std::string& getOutputPath() const { return m_outputPath; }

    // ---- UObject 信息读取 ----
    std::string readObjectFName(uintptr_t objPtr);
    std::string readClassName(uintptr_t objPtr);
    std::string readFullPath(uintptr_t objPtr);

private:
    // ---- 成员: 三大全局指针 + libUE4 基址 (均由外部赋值) ----
    uintptr_t m_moduleBase;     // libUE4.so 基址
    uintptr_t m_GNames;        // GNames 数组指针
    uintptr_t m_GWorld;        // GWorld 指针
    uintptr_t m_GUObjectArray; // GUObjectArray 地址

    int m_numNames;
    bool m_initialized;
    uintptr_t m_moduleSize;
    std::string m_outputPath;  // 输出目录路径, 默认为""

    // ---- 安全内存读取 ----
    static uintptr_t safeReadPtr(uintptr_t addr);
    static int32_t safeReadS32(uintptr_t addr);
    static uint32_t safeReadU32(uintptr_t addr);

    // ---- 输出路径 ----
    std::string getEffectiveOutputDir() const;

    // ---- FName 解析 ----
    const char* getNameByIndex(int index);
    std::string fnameToString(int nameIdx, int number);

    // ---- UObject 工具 (内部) ----
    std::string getPackageName(uintptr_t objPtr);
    std::string outerChain(uintptr_t objPtr);
    std::string getObjectPath(uintptr_t objPtr);
    bool isModulePtr(uintptr_t ptr);
    std::string getModuleOffsetText(uintptr_t ptr);

    // ---- SDK dump: 属性类型映射 ----
    static const char* getPropTypeName(const std::string& className);

    // ---- SDK dump: 字段/函数收集 ----
    struct FieldInfo {
        std::string ownerName;
        std::string propName;
        std::string typeName;
        std::string enumPath;
        int offset;
        int elemSize;
        int arrayDim;
        uint16_t repIndex;
        std::string repNotifyFunc;
    };

    struct FuncInfo {
        std::string ownerName;
        std::string funcName;
        int32_t funcFlags;
        int numParms;
        std::vector<std::string> params;
        std::string retType;
        uintptr_t funcPtr;
        std::string locationSuffix;
    };

    std::string getBoundEnumPath(uintptr_t propPtr);
    std::vector<FieldInfo> collectDeclaredFields(uintptr_t typePtr);
    std::vector<FieldInfo> collectExpandedFields(uintptr_t typePtr);
    std::vector<FuncInfo> collectDeclaredFunctions(uintptr_t typePtr);
    std::vector<FuncInfo> collectExpandedFunctions(uintptr_t typePtr);

    // ---- SDK dump: 枚举值 ----
    void dumpEnumValues(uintptr_t objPtr, FILE* fp);

    // ---- SDK dump: 类型写入 ----
    void dumpType(uintptr_t objPtr, FILE* fp);

    // ---- UStruct 继承链 ----
    struct HierarchyEntry {
        uintptr_t ptr;
        std::string name;
    };
    std::vector<HierarchyEntry> buildTypeHierarchy(uintptr_t typePtr);

    // ---- GUObjectArray 遍历回调 ----
    typedef void (*ForEachCallback)(uintptr_t objPtr, int globalIdx, void* userData);
    int forEachUObject(uintptr_t arrayBase, ForEachCallback cb, void* userData);
};

} // namespace ue4

#endif // UE4_DUMPER_H
