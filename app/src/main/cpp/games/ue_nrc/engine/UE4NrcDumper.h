#ifndef UE4_NRC_DUMPER_H
#define UE4_NRC_DUMPER_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>

// =====================================================================
//  UE 4.26 GUObjectArray / NamePool / GWorld / SDK Dump (洛克王国手游 NRC)
//  Target: com.tencent.nrc  (ARM64 Android, libUE4.so)
//  原始 Frida 实现: c:\Users\user\Desktop\nrc\ue_dump_all.js
//
//  本类把 JS 实现的快照算法平移到 C++:
//    1. snapshotNamePool()    — Blocks[] 整块 read 缓存到 m_blocks
//    2. snapshotAllObjects()  — GUObjectArray 全量复制 24 字节 item +
//                                 0x200 字节 UObject snapshot
//    3. dumpSDK()             — 输出 PUBG dump.cs 风格 SDK
//    4. dumpNames()           — NamePool 平铺 [block:offset] name
//    5. dumpObjects()         — GUObjectArray 平铺 [Index] FullPath Class Addr
//    6. dumpGWorld()          — 解 GWorld 并输出 PersistentLevel.Actors 摘要
// =====================================================================

namespace ue4nrc {

class UE4NrcDumper {
public:
    /**
     * @param moduleBase libUE4.so 模块基址
     * @param moduleSize libUE4.so 模块大小
     * @param offNamePool          NamePool 偏移 (相对 moduleBase, JS RVA_NAMEPOOL)
     * @param offGUObjectArrayNum  GUObjectArray.NumElements 偏移
     * @param offGUObjectArrayChunks GUObjectArray.Chunks 偏移
     * @param offGWorld            GWorld 偏移
     * @param outputPath           输出目录
     */
    UE4NrcDumper(uintptr_t moduleBase, uintptr_t moduleSize,
                 uint32_t offNamePool, uint32_t offGUObjectArrayNum,
                 uint32_t offGUObjectArrayChunks, uint32_t offGWorld,
                 const std::string& outputPath = "");
    ~UE4NrcDumper();

    void setModuleBase(uintptr_t base) { m_moduleBase = base; }
    void setModuleSize(uintptr_t size) { m_moduleSize = size; }
    void setOutputPath(const std::string& path) { m_outputPath = path; }

    /// 校验 NamePool / GUObjectArray 有效性
    bool init();

    /// 按 OUT_FILE 等价路径输出 PUBG dump.cs 风格 SDK
    bool dumpSDK(const char* filePath = nullptr);
    /// dump NamePool 全量 (etalon: ue_dump_all.js dumpNames)
    bool dumpNames(const char* filePath = nullptr);
    /// dump GUObjectArray 全量 (etalon: ue_dump_all.js dumpObjects)
    bool dumpObjects(const char* filePath = nullptr);
    /// dump GWorld (etalon: ue_dump_all.js dumpGWorld)
    bool dumpGWorld(const char* filePath = nullptr);

    /// 一键 dump 全部 (sdk + names + objects + gworld)
    bool dumpAll();

    // ---- Getter ----
    uintptr_t          getModuleBase()   const { return m_moduleBase; }
    uintptr_t          getModuleSize()   const { return m_moduleSize; }
    const std::string& getOutputPath()   const { return m_outputPath; }
    uint32_t           getOffGUObjectArrayNum() const { return m_offGUObjectArrayNum; }

    // ---- 公共安全读 (供 interface/header 使用) ----
    static uintptr_t rp(uintptr_t addr);
    static uint32_t  r32(uintptr_t addr);
    static int32_t   rs32(uintptr_t addr);
    static bool      ok(uintptr_t p);

    // ---- 公共名称解析 ----
    std::string oname(uintptr_t objPtr) const;
    std::string ffname(uintptr_t fieldPtr) const;
    std::string ffclassname(uintptr_t fieldPtr) const;
    std::string getFullPath(uintptr_t objPtr) const;

    // ---- GUObjectArray 访问 ----
    uintptr_t getobj(int index) const;

    // ---- FCLS RVA → 类型名映射 (ue_dump_all.js FCLS_RVA_TO_TYPE) ----
    std::string lookupPropTypeByCls(uintptr_t fclsPtr) const;

private:
    // ---- 输入 ----
    uintptr_t   m_moduleBase   = 0;
    uintptr_t   m_moduleSize   = 0;
    uint32_t    m_offNamePool  = 0;
    uint32_t    m_offGUObjectArrayNum    = 0;
    uint32_t    m_offGUObjectArrayChunks = 0;
    uint32_t    m_offGWorld    = 0;
    std::string m_outputPath;
    bool        m_initialized  = false;

    // ---- NamePool 快照 (复制 Blocks[] 整块到内存) ----
    struct NameBlock {
        std::vector<uint8_t> data;
    };
    std::vector<NameBlock> m_blocks;

    // FName cmpIdx -> 解码字符串缓存 (LRU 简化为 unordered_map)
    mutable std::unordered_map<uint32_t, std::string> m_fnameCache;

    bool snapshotNamePool();
    std::string readFNameCmp(uint32_t cmpIdx) const;
    std::string fnameAt(uintptr_t addr) const;

    // ---- 对象快照 ----
    struct ObjEntry {
        uintptr_t           ptr      = 0;
        uint32_t            cmpIdx   = 0;
        uint32_t            cmpNum   = 0;
        uintptr_t           clsPtr   = 0;
        uintptr_t           outerPtr = 0;
        std::string         name;
        std::string         clsName; // 在 phase1c 填充
        std::vector<uint8_t> snap;   // 0x200 bytes
        // 类/结构特有 (按需 walk)
        std::vector<uintptr_t> propPtrs;          // ChildProperties 链
        std::vector<std::vector<uint8_t>> propSnap; // 每条 0xB0 bytes
        // emit 阶段缓存
        mutable std::string fullPath;
    };
    std::vector<ObjEntry> m_objs;
    std::unordered_map<uintptr_t, int> m_idxByPtr;

    bool snapshotAllObjects();
    void walkPropsForStructEntry(int idx);

    // ---- 工具 ----
    static std::string safeIdent(const std::string& s);
    bool inLib(uintptr_t p) const { return p >= m_moduleBase && p < (m_moduleBase + m_moduleSize); }

    int  getIdxByPtr(uintptr_t p) const;
    std::string objNameFromSnap(const ObjEntry& e) const;
    std::string fullPathFromIdx(int i) const;
    std::string shortClassNameFromPtr(uintptr_t p) const;
    std::string propKindFromSnap(const uint8_t* snap) const;
    std::string propTypeStringFromSnap(const uint8_t* snap) const;

    // ---- super chain (root → leaf) ----
    void superChain(int leafIdx, std::vector<int>& outChain) const;

    // ---- emit 字符串构造 ----
    std::string emitField(const ObjEntry& ownerObj, const uint8_t* propSnap, uintptr_t ownerPtr, const std::string& ownerName);
    std::vector<std::string> emitFunctionLines(const ObjEntry& fnEntry, const std::string& ownerName);

    // ---- CDO / VTable 探测 ----
    int32_t   m_cdoOffset      = -1;
    int32_t   m_vtMaxSlots     = 512;
    int32_t   probeCdoOffset(const std::vector<int>& classes);
    void      emitVTable(int classIdx, FILE* out);

    // ---- FCLS RVA → 类型名 (与 JS FCLS_RVA_TO_TYPE 一致) ----
    std::unordered_map<uintptr_t, std::string> m_fclsPtrToType;
    void buildFclsTable();

    // ---- 输出路径 ----
    std::string resolveOutputPath(const char* filename) const;
    FILE*       openOutputFile(const char* filename) const;
};

} // namespace ue4nrc

#endif // UE4_NRC_DUMPER_H
