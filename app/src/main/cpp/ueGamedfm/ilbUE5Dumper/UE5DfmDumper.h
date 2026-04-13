#ifndef UE5_DFM_DUMPER_H
#define UE5_DFM_DUMPER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

// =====================================================================
//  UE 5.4 GUObjectArray / NamePool / GWorld / SDK Dump
//  Target: com.tencent.tmgp.dfm  (ARM64 Android)
//  从 sdk_dump.js (Frida) 转化为 C++ 原生实现
// =====================================================================

namespace ue5dfm {

class UE5DfmDumper {
public:
    /**
     * @param moduleBase  libUE4.so 模块基址
     * @param moduleSize  libUE4.so 模块大小
     * @param offNamePool          NamePool 全局偏移 (相对 moduleBase)
     * @param offGUObjectArrayNum  GUObjectArray.NumElements 全局偏移
     * @param offGUObjectArrayChunks GUObjectArray.Chunks 全局偏移
     * @param offGWorld            GWorld 全局偏移
     * @param outputPath           输出目录路径
     */
    UE5DfmDumper(uintptr_t moduleBase, uintptr_t moduleSize,
                 uint32_t offNamePool, uint32_t offGUObjectArrayNum,
                 uint32_t offGUObjectArrayChunks, uint32_t offGWorld,
                 const std::string& outputPath = "");
    ~UE5DfmDumper();

    void setModuleBase(uintptr_t base);
    void setModuleSize(uintptr_t size);
    void setOutputPath(const std::string& path);

    /// 初始化: 验证 NamePool / GUObjectArray 有效性
    bool init();

    /// dump NamePool 到文件
    bool dumpNames(const char* filePath = nullptr);

    /// dump GUObjectArray 到文件
    bool dumpObjects(const char* filePath = nullptr);

    /// dump GWorld 信息到文件
    bool dumpGWorld(const char* filePath = nullptr);

    /// SDK dump: 导出所有 Class/ScriptStruct/Enum 的字段、函数、继承信息
    bool dumpSDK(const char* filePath = nullptr);

    /// 一键 dump 全部
    bool dumpAll();

    // ---- Getter ----
    uintptr_t getModuleBase() const { return m_moduleBase; }
    uintptr_t getModuleSize() const { return m_moduleSize; }
    const std::string& getOutputPath() const { return m_outputPath; }
    uint32_t getOffGUObjectArrayNum() const { return m_offGUObjectArrayNum; }

    // ---- 安全内存读取 (public, 供 interface 等外部使用) ----
    static uintptr_t rp(uintptr_t addr);
    static uint32_t r32(uintptr_t addr);
    static int32_t rs32(uintptr_t addr);
    static bool ok(uintptr_t p);

    // ---- 名称解析 (public, 供 interface 等外部使用) ----
    std::string oname(uintptr_t objPtr) const;
    std::string ffname(uintptr_t fieldPtr) const;
    std::string ffclassname(uintptr_t fieldPtr) const;
    std::string getFullPath(uintptr_t objPtr) const;

    // ---- GUObjectArray 访问 (public) ----
    uintptr_t getobj(int index) const;

private:
    uintptr_t m_moduleBase;
    uintptr_t m_moduleSize;
    std::string m_outputPath;
    bool m_initialized;

    // 全局变量偏移 (相对于 libUE4.so 基址, 由外部传入)
    uint32_t m_offNamePool;            // NamePool 结构体偏移
    uint32_t m_offGUObjectArrayNum;    // GUObjectArray.NumElements 偏移
    uint32_t m_offGUObjectArrayChunks; // GUObjectArray.Chunks 偏移
    uint32_t m_offGWorld;              // GWorld 偏移

    // ---- 安全内存读取 (private helpers) ----
    static uint16_t r16(uintptr_t addr);
    static uint8_t r8(uintptr_t addr);

    // ---- NamePool 解码 (混淆: NOT+XOR, 9-case mask) ----
    static uint8_t amask(int length);
    std::string resolveName(uint32_t id) const;
    std::string fname(uintptr_t addr) const;
    std::string className(uintptr_t objPtr) const;

    // ---- 属性类型映射 ----
    std::string ptype(const std::string& cn, uintptr_t fpPtr) const;

    // ---- SDK dump 内部 ----
    struct FieldInfo {
        std::string typeName;
        std::string propName;
        std::string propClassName;
        int32_t offset;
        int32_t size;
        uint32_t pflags;
        std::string owner;
    };

    struct FuncInfo {
        std::string name;
        std::string flags;
        uintptr_t rva;
        int numParms;
        std::string owner;
        std::string retType;
        std::string paramStr;
    };

    void getInheritanceChain(uintptr_t objPtr, std::vector<uintptr_t>& chain) const;
    std::vector<FieldInfo> collectFields(uintptr_t structPtr, const std::string& ownerName) const;
    std::vector<FuncInfo> collectFuncs(uintptr_t classPtr, const std::string& ownerName) const;

    // ---- 输出路径 ----
    std::string resolveOutputPath(const char* filename) const;
    FILE* openOutputFile(const char* filename) const;
};

} // namespace ue5dfm

#endif // UE5_DFM_DUMPER_H
