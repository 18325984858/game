// =====================================================================
//  UE 4.26 NRC SDK Dumper — C++ 实现 (从 c:\Users\user\Desktop\nrc\ue_dump_all.js 转写)
//  Target: com.tencent.nrc  (洛克王国手游, ARM64 Android, libUE4.so)
// =====================================================================
#include "UE4NrcDumper.h"
#include "UE4NrcStruct.h"
#include "../../../core/log/log.h"

#include <algorithm>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <sys/types.h>

// =====================================================================
//  安全内存读取 — SIGSEGV/SIGBUS 兜底 (复用 dfm 模式)
// =====================================================================
static thread_local sigjmp_buf  s_safeReadJmpBuf;
static thread_local volatile sig_atomic_t s_safeReadActive = 0;
static struct sigaction s_oldSigsegvAction;
static struct sigaction s_oldSigbusAction;
static bool s_safeReadGuardInstalled = false;

static void safeReadSignalHandler(int sig, siginfo_t* info, void* ctx) {
    if (s_safeReadActive) {
        s_safeReadActive = 0;
        siglongjmp(s_safeReadJmpBuf, sig);
    }
    struct sigaction* old = (sig == SIGSEGV) ? &s_oldSigsegvAction : &s_oldSigbusAction;
    if (old->sa_flags & SA_SIGINFO) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static void installSafeReadGuard() {
    if (s_safeReadGuardInstalled) return;
    struct sigaction sa{};
    sa.sa_sigaction = safeReadSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &s_oldSigsegvAction);
    sigaction(SIGBUS,  &sa, &s_oldSigbusAction);
    s_safeReadGuardInstalled = true;
}

namespace {
    // 安全读 N 字节到目标缓冲区, 失败返回 false
    static bool safeReadBytes(uintptr_t addr, void* dst, size_t len) {
        if (addr == 0 || dst == nullptr) return false;
        installSafeReadGuard();
        if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) { s_safeReadActive = 0; return false; }
        s_safeReadActive = 1;
        memcpy(dst, reinterpret_cast<void*>(addr), len);
        s_safeReadActive = 0;
        return true;
    }
}

namespace ue4nrc {

// =====================================================================
//  公共安全读取
// =====================================================================
uintptr_t UE4NrcDumper::rp(uintptr_t addr) {
    uintptr_t v = 0;
    return safeReadBytes(addr, &v, sizeof(v)) ? v : 0;
}
uint32_t UE4NrcDumper::r32(uintptr_t addr) {
    uint32_t v = 0;
    return safeReadBytes(addr, &v, sizeof(v)) ? v : 0u;
}
int32_t UE4NrcDumper::rs32(uintptr_t addr) {
    int32_t v = 0;
    return safeReadBytes(addr, &v, sizeof(v)) ? v : 0;
}
bool UE4NrcDumper::ok(uintptr_t p) { return p > 0x10000u; }

// =====================================================================
//  构造 / 析构
// =====================================================================
UE4NrcDumper::UE4NrcDumper(uintptr_t moduleBase, uintptr_t moduleSize,
                           uint32_t offNamePool, uint32_t offGUObjectArrayNum,
                           uint32_t offGUObjectArrayChunks, uint32_t offGWorld,
                           const std::string& outputPath)
    : m_moduleBase(moduleBase)
    , m_moduleSize(moduleSize)
    , m_offNamePool(offNamePool)
    , m_offGUObjectArrayNum(offGUObjectArrayNum)
    , m_offGUObjectArrayChunks(offGUObjectArrayChunks)
    , m_offGWorld(offGWorld)
    , m_outputPath(outputPath)
{
    buildFclsTable();
}

UE4NrcDumper::~UE4NrcDumper() = default;

// =====================================================================
//  FCLS RVA → 属性类型名 (与 ue_dump_all.js FCLS_RVA_TO_TYPE 一致)
// =====================================================================
void UE4NrcDumper::buildFclsTable() {
    static const struct { uintptr_t rva; const char* name; } kTbl[] = {
        { 0xd03db98, "FloatProperty" },
        { 0xd03d9e0, "ByteProperty" },
        { 0xcaf8b50, "IntProperty" },
        { 0xd046e28, "StructProperty" },
        { 0xbd66670, "Int16Property" },
        { 0xbd66828, "UInt16Property" },
        { 0xbd669e0, "UInt32Property" },
        { 0xbd66b98, "Int64Property" },
        { 0xbd66d50, "UInt64Property" },
        { 0xbd66f08, "DoubleProperty" },
        { 0xd03de28, "EnumProperty" },
        { 0xd03a3e0, "FieldPathProperty" },
        { 0xd03c2b8, "ArrayProperty" },
        { 0xd03c400, "BoolProperty" },
        { 0xd03c548, "MapProperty" },
        { 0xd03d6e0, "SetProperty" },
        { 0xd03dec8, "ObjectProperty" },
        { 0xd03e040, "LazyObjectProperty" },
        { 0xd03e1b8, "SoftObjectProperty" },
        { 0xd03e330, "ClassProperty" },
        { 0xd03e4a8, "SoftClassProperty" },
        { 0xd03e620, "InterfaceProperty" },
        { 0xd03e768, "NameProperty" },
        { 0xd03e8b0, "DelegateProperty" },
        { 0xd03eb70, "MulticastInlineDelegateProperty" },
        { 0xd03ece8, "MulticastSparseDelegateProperty" },
        { 0xd03ee68, "InterfaceProperty_Default" },
        { 0xd040978, "Int8Property" },
        { 0xd046f70, "WeakObjectProperty" },
        { 0xd0470e8, "StrProperty" },
        { 0xd047678, "TextProperty" },
    };
    for (auto& e : kTbl) {
        m_fclsPtrToType[m_moduleBase + e.rva] = e.name;
    }
}

std::string UE4NrcDumper::lookupPropTypeByCls(uintptr_t fclsPtr) const {
    auto it = m_fclsPtrToType.find(fclsPtr);
    return it == m_fclsPtrToType.end() ? std::string("UnknownProp") : it->second;
}

// =====================================================================
//  init — 校验 NamePool 第一个 block + GUObjectArray.Num
// =====================================================================
bool UE4NrcDumper::init() {
    if (m_moduleBase == 0 || m_moduleSize == 0) {
        LOG(LOG_LEVEL_ERROR, "[NrcDumper] init: moduleBase/Size 为 0");
        return false;
    }
    uintptr_t blocks = m_moduleBase + m_offNamePool + offsetof(FNamePool, Blocks);
    uintptr_t b0 = rp(blocks);
    if (!ok(b0)) {
        LOG(LOG_LEVEL_ERROR, "[NrcDumper] init: NamePool block0=%p", (void*)b0);
        return false;
    }
    uint32_t num = r32(m_moduleBase + m_offGUObjectArrayNum);
    if (num < 100 || num > 5000000u) {
        LOG(LOG_LEVEL_ERROR, "[NrcDumper] init: GUObjectArray.Num=%u 异常", num);
        return false;
    }
    LOG(LOG_LEVEL_INFO, "[NrcDumper] init OK: NamePool.block0=%p Num=%u", (void*)b0, num);
    m_initialized = true;
    return true;
}

// =====================================================================
//  NamePool 快照 (复制 Blocks[] 整块)
// =====================================================================
bool UE4NrcDumper::snapshotNamePool() {
    if (!m_blocks.empty()) return true;
    uintptr_t blocksAddr = m_moduleBase + m_offNamePool + offsetof(FNamePool, Blocks);
    LOG(LOG_LEVEL_INFO, "[NrcDumper] snapshot NamePool blocks @ %p ...", (void*)blocksAddr);

    for (uint32_t i = 0; i < FNamePool::MaxBlocks; ++i) {
        uintptr_t bp = rp(blocksAddr + i * sizeof(uint8_t*));
        if (!ok(bp)) break;
        NameBlock blk;
        blk.data.resize(FNamePool::BlockSize);
        if (!safeReadBytes(bp, blk.data.data(), FNamePool::BlockSize)) {
            // 末尾 block 可能不可读完整 128 KiB, 退化到 64 KiB
            blk.data.resize(0x10000);
            if (!safeReadBytes(bp, blk.data.data(), blk.data.size())) {
                break;
            }
        }
        m_blocks.emplace_back(std::move(blk));
    }
    LOG(LOG_LEVEL_INFO, "[NrcDumper] NamePool blocks loaded: %zu", m_blocks.size());
    return !m_blocks.empty();
}

// =====================================================================
//  解 FName cmpIdx → string  (NRC 不带混淆)
// =====================================================================
std::string UE4NrcDumper::readFNameCmp(uint32_t cmpIdx) const {
    auto it = m_fnameCache.find(cmpIdx);
    if (it != m_fnameCache.end()) return it->second;

    uint32_t bi = cmpIdx >> FNamePool::OffsetBits;
    uint32_t bo = (cmpIdx & 0xFFFFu) * FNamePool::Stride; // OffsetBits=16: low 16 bits 是 entry index, ×Stride 得到字节偏移

    if (bi >= m_blocks.size()) { m_fnameCache[cmpIdx]; return std::string(); }
    const auto& buf = m_blocks[bi].data;
    if (bo + 2 > buf.size()) { m_fnameCache[cmpIdx]; return std::string(); }

    uint16_t h = static_cast<uint16_t>(buf[bo]) | (static_cast<uint16_t>(buf[bo + 1]) << 8);
    bool wide = (h & 0x1) != 0;
    uint32_t len = static_cast<uint32_t>(h >> 6);
    if (len == 0 || len > 1024 || bo + 2 + len * (wide ? 2u : 1u) > buf.size()) {
        m_fnameCache[cmpIdx];
        return std::string();
    }

    std::string out;
    out.reserve(len);
    if (wide) {
        for (uint32_t i = 0; i < len; ++i) {
            uint16_t wc = static_cast<uint16_t>(buf[bo + 2 + i * 2])
                        | (static_cast<uint16_t>(buf[bo + 3 + i * 2]) << 8);
            out += static_cast<char>(wc & 0xFF); // 仅取低字节, 与 JS 的 String.fromCharCode(代码点取模) 不完全一致但够用
        }
    } else {
        for (uint32_t i = 0; i < len; ++i) {
            out += static_cast<char>(buf[bo + 2 + i]);
        }
    }
    m_fnameCache[cmpIdx] = out;
    return out;
}

std::string UE4NrcDumper::fnameAt(uintptr_t addr) const {
    uint32_t cmp = r32(addr + 0);
    uint32_t num = r32(addr + 4);
    std::string s = readFNameCmp(cmp);
    if (s.empty()) s = "?";
    if (num > 0) {
        s += "_";
        s += std::to_string(num - 1);
    }
    return s;
}

std::string UE4NrcDumper::oname(uintptr_t objPtr) const {
    return fnameAt(objPtr + offsetof(UObject, NameIndex));
}

std::string UE4NrcDumper::ffname(uintptr_t fieldPtr) const {
    return fnameAt(fieldPtr + offsetof(FField, NameIndex));
}

std::string UE4NrcDumper::ffclassname(uintptr_t fieldPtr) const {
    uintptr_t cls = rp(fieldPtr + offsetof(FField, ClassPrivate));
    if (!ok(cls)) return std::string();
    return lookupPropTypeByCls(cls);
}

std::string UE4NrcDumper::getFullPath(uintptr_t objPtr) const {
    if (!ok(objPtr)) return std::string();
    int idx = getIdxByPtr(objPtr);
    if (idx >= 0) return fullPathFromIdx(idx);
    return oname(objPtr);
}

uintptr_t UE4NrcDumper::getobj(int index) const {
    if (index < 0 || index >= static_cast<int>(m_objs.size())) return 0;
    return m_objs[index].ptr;
}

// =====================================================================
//  对象快照 (Phase 1 + 1b + 1c)
// =====================================================================
bool UE4NrcDumper::snapshotAllObjects() {
    if (!m_objs.empty()) return true;
    // 注: m_offGUObjectArrayNum/Chunks 已是 (base + offsetof(...)) 形式直接传入
    uintptr_t numAddr   = m_moduleBase + m_offGUObjectArrayNum;
    uintptr_t chunkAddr = m_moduleBase + m_offGUObjectArrayChunks;

    uint32_t n = r32(numAddr);
    if (n == 0 || n > 5000000u) {
        LOG(LOG_LEVEL_ERROR, "[NrcDumper] snapshotAllObjects: bad N=%u", n);
        return false;
    }
    uintptr_t chunksPtr = rp(chunkAddr);
    if (!ok(chunksPtr)) {
        LOG(LOG_LEVEL_ERROR, "[NrcDumper] snapshotAllObjects: chunksPtr 无效");
        return false;
    }

    LOG(LOG_LEVEL_INFO, "[NrcDumper] snapshot %u UObjects ...", n);
    m_objs.resize(n);
    m_idxByPtr.reserve(n + 64);

    uint32_t numChunks = ((n - 1) >> 16) + 1;
    std::vector<uintptr_t> chunkPtrs(numChunks);
    for (uint32_t c = 0; c < numChunks; ++c) {
        chunkPtrs[c] = rp(chunksPtr + c * sizeof(void*));
    }

    int valid = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t c = i >> 16;
        uint32_t off = (i & 0xFFFFu) * sizeof(FUObjectItem); // 24 bytes/item
        if (c >= numChunks || chunkPtrs[c] == 0) continue;

        uintptr_t p = rp(chunkPtrs[c] + off + offsetof(FUObjectItem, Object)); // FUObjectItem.Object
        if (!ok(p)) continue;

        ObjEntry& e = m_objs[i];
        e.ptr = p;
        e.snap.resize(0x200);
        if (!safeReadBytes(p, e.snap.data(), 0x200)) {
            e.ptr = 0;
            e.snap.clear();
            continue;
        }
        m_idxByPtr[p] = static_cast<int>(i);
        ++valid;
        (void)valid;
    }

    // Phase 1b: 读 cls/outer/cmpIdx/cmpNum from snap (UObject 头)
    for (uint32_t i = 0; i < n; ++i) {
        ObjEntry& e = m_objs[i];
        if (e.ptr == 0) continue;
        const auto* uo = reinterpret_cast<const UObject*>(e.snap.data());
        e.clsPtr   = reinterpret_cast<uintptr_t>(uo->ClassPrivate);
        e.outerPtr = reinterpret_cast<uintptr_t>(uo->OuterPrivate);
        e.cmpIdx   = static_cast<uint32_t>(uo->NameIndex);
        e.cmpNum   = static_cast<uint32_t>(uo->NameNumber);
        e.name = readFNameCmp(e.cmpIdx);
        if (e.name.empty()) e.name = "?";
        if (e.cmpNum > 0) { e.name += "_"; e.name += std::to_string(e.cmpNum - 1); }
    }
    // Phase 1c: 解析 className (依赖 cls 已知)
    for (uint32_t i = 0; i < n; ++i) {
        ObjEntry& e = m_objs[i];
        if (e.ptr == 0) continue;
        if (!ok(e.clsPtr)) { e.clsName.clear(); continue; }
        auto it = m_idxByPtr.find(e.clsPtr);
        if (it != m_idxByPtr.end()) {
            e.clsName = m_objs[it->second].name;
        } else {
            // class 不在 GUObjectArray, 直接读
            e.clsName = oname(e.clsPtr);
        }
    }
    LOG(LOG_LEVEL_INFO, "[NrcDumper] snapped %d/%u objects", valid, n);
    return true;
}

int UE4NrcDumper::getIdxByPtr(uintptr_t p) const {
    auto it = m_idxByPtr.find(p);
    return it == m_idxByPtr.end() ? -1 : it->second;
}

std::string UE4NrcDumper::objNameFromSnap(const ObjEntry& e) const {
    return e.name;
}

// =====================================================================
//  full path (root.outer....name)
// =====================================================================
std::string UE4NrcDumper::fullPathFromIdx(int i) const {
    if (i < 0 || i >= static_cast<int>(m_objs.size())) return std::string();
    const ObjEntry& root = m_objs[i];
    if (!root.fullPath.empty()) return root.fullPath;

    std::vector<std::string> parts;
    int cur = i;
    for (int d = 0; d < 16; ++d) {
        if (cur < 0 || cur >= static_cast<int>(m_objs.size())) break;
        const ObjEntry& e = m_objs[cur];
        if (e.ptr == 0) break;
        parts.insert(parts.begin(), e.name);
        if (!ok(e.outerPtr)) break;
        auto it = m_idxByPtr.find(e.outerPtr);
        if (it == m_idxByPtr.end()) {
            std::string n = oname(e.outerPtr);
            if (!n.empty()) parts.insert(parts.begin(), n);
            break;
        }
        cur = it->second;
    }
    std::string p;
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) p += '.';
        p += parts[k];
    }
    root.fullPath = p;
    return p;
}

std::string UE4NrcDumper::shortClassNameFromPtr(uintptr_t p) const {
    if (!ok(p)) return std::string();
    int i = getIdxByPtr(p);
    if (i >= 0) return safeIdent(m_objs[i].name);
    std::string n = oname(p);
    return n.empty() ? std::string() : safeIdent(n);
}

std::string UE4NrcDumper::safeIdent(const std::string& s) {
    if (s.empty()) return "_";
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    }
    return out;
}

// =====================================================================
//  Property 类型读取
// =====================================================================
std::string UE4NrcDumper::propKindFromSnap(const uint8_t* snap) const {
    auto* fld = reinterpret_cast<const FField*>(snap);
    return lookupPropTypeByCls(reinterpret_cast<uintptr_t>(fld->ClassPrivate));
}

std::string UE4NrcDumper::propTypeStringFromSnap(const uint8_t* snap) const {
    std::string k = propKindFromSnap(snap);
    if (k == "BoolProperty")           return "bool";
    if (k == "Int8Property")           return "int8";
    if (k == "ByteProperty")           return "uint8";
    if (k == "Int16Property")          return "int16";
    if (k == "UInt16Property")         return "uint16";
    if (k == "IntProperty")            return "int32";
    if (k == "UInt32Property")         return "uint32";
    if (k == "Int64Property")          return "int64";
    if (k == "UInt64Property")         return "uint64";
    if (k == "FloatProperty")          return "float";
    if (k == "DoubleProperty")         return "double";
    if (k == "NameProperty")           return "FName";
    if (k == "StrProperty")            return "FString";
    if (k == "TextProperty")           return "FText";
    if (k == "EnumProperty") {
        auto* prop = reinterpret_cast<const FProperty*>(snap);
        uintptr_t ep = reinterpret_cast<uintptr_t>(prop->SubTypePtr);
        if (ok(ep)) {
            int idx = getIdxByPtr(ep);
            if (idx >= 0) return safeIdent(m_objs[idx].name);
            std::string n = oname(ep);
            if (!n.empty()) return safeIdent(n);
        }
        return "enum_t";
    }
    if (k == "StructProperty")         return "FStruct";
    if (k == "ObjectProperty" || k == "ObjectPropertyBase"
     || k == "WeakObjectProperty" || k == "LazyObjectProperty") return "UObject*";
    if (k == "ClassProperty")          return "UClass*";
    if (k == "SoftObjectProperty")     return "TSoftObjectPtr";
    if (k == "SoftClassProperty")      return "TSoftClassPtr";
    if (k == "InterfaceProperty")      return "TScriptInterface";
    if (k == "ArrayProperty")          return "TArray";
    if (k == "SetProperty")            return "TSet";
    if (k == "MapProperty")            return "TMap";
    if (k == "DelegateProperty")       return "FDelegate";
    if (k == "MulticastInlineDelegateProperty"
     || k == "MulticastSparseDelegateProperty"
     || k == "MulticastDelegateProperty") return "FMulticastDelegate";
    if (k == "FieldPathProperty")      return "TFieldPath";
    return k;
}

// =====================================================================
//  walk ChildProperties 链 → e.propPtrs / e.propSnap
// =====================================================================
void UE4NrcDumper::walkPropsForStructEntry(int idx) {
    ObjEntry& e = m_objs[idx];
    if (e.ptr == 0) return;
    auto* us = reinterpret_cast<const UStruct*>(e.snap.data());
    uintptr_t p = reinterpret_cast<uintptr_t>(us->ChildProperties);
    int n = 0;
    while (ok(p) && n < 4096) {
        std::vector<uint8_t> snap(0xB0);
        if (!safeReadBytes(p, snap.data(), snap.size())) break;
        e.propPtrs.push_back(p);
        uintptr_t next = reinterpret_cast<uintptr_t>(
            reinterpret_cast<const FField*>(snap.data())->Next);
        e.propSnap.emplace_back(std::move(snap));
        p = next;
        ++n;
    }
}

// =====================================================================
//  super chain (root → leaf)
// =====================================================================
void UE4NrcDumper::superChain(int leafIdx, std::vector<int>& out) const {
    out.clear();
    int cur = leafIdx;
    for (int d = 0; d < 32; ++d) {
        if (cur < 0 || cur >= static_cast<int>(m_objs.size())) break;
        out.insert(out.begin(), cur);
        const ObjEntry& e = m_objs[cur];
        uintptr_t sup = reinterpret_cast<uintptr_t>(
            reinterpret_cast<const UStruct*>(e.snap.data())->SuperStruct);
        if (!ok(sup)) break;
        auto it = m_idxByPtr.find(sup);
        if (it == m_idxByPtr.end()) break;
        cur = it->second;
    }
}

// =====================================================================
//  Function flags 字符串
// =====================================================================
namespace {
    static const struct { uint32_t bit; const char* name; } kFuncFlags[] = {
        { 0x00000001, "Final" },
        { 0x00000002, "RequiredAPI" },
        { 0x00000004, "BlueprintAuthorityOnly" },
        { 0x00000008, "BlueprintCosmetic" },
        { 0x00000040, "Net" },
        { 0x00000080, "NetReliable" },
        { 0x00000100, "NetRequest" },
        { 0x00000200, "Exec" },
        { 0x00000400, "Native" },
        { 0x00000800, "Event" },
        { 0x00001000, "NetResponse" },
        { 0x00002000, "Static" },
        { 0x00004000, "NetMulticast" },
        { 0x00008000, "UbergraphFunction" },
        { 0x00010000, "MulticastDelegate" },
        { 0x00020000, "Public" },
        { 0x00040000, "Private" },
        { 0x00080000, "Protected" },
        { 0x00100000, "Delegate" },
        { 0x00200000, "NetServer" },
        { 0x00400000, "HasOutParms" },
        { 0x00800000, "HasDefaults" },
        { 0x01000000, "NetClient" },
        { 0x02000000, "DLLImport" },
        { 0x04000000, "BlueprintCallable" },
        { 0x08000000, "BlueprintEvent" },
        { 0x10000000, "BlueprintPure" },
        { 0x20000000, "EditorOnly" },
        { 0x40000000, "Const" },
        { 0x80000000, "NetValidate" },
    };
    static std::string funcFlagsName(uint32_t f) {
        std::string out;
        for (auto& e : kFuncFlags) {
            if (f & e.bit) {
                if (!out.empty()) out += '|';
                out += e.name;
            }
        }
        return out.empty() ? "(none)" : out;
    }
    static std::string toHexUpper(uintptr_t v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%lX", static_cast<unsigned long>(v));
        return buf;
    }
}

// =====================================================================
//  emit field / function 行
// =====================================================================
std::string UE4NrcDumper::emitField(const ObjEntry& /*ownerObj*/, const uint8_t* snap, uintptr_t ownerPtr, const std::string& ownerName) {
    auto* fld  = reinterpret_cast<const FField*>(snap);
    auto* prop = reinterpret_cast<const FProperty*>(snap);
    uint32_t cmp = static_cast<uint32_t>(fld->NameIndex);
    uint32_t num = static_cast<uint32_t>(fld->NameNumber);
    std::string pname = readFNameCmp(cmp);
    if (pname.empty()) pname = "_";
    if (num > 0) { pname += "_"; pname += std::to_string(num - 1); }
    pname = safeIdent(pname);

    std::string k = propKindFromSnap(snap);
    if (k == "InterfaceProperty_Default") return std::string();
    std::string ptype = propTypeStringFromSnap(snap);

    uint32_t off   = static_cast<uint32_t>(prop->Offset_Internal);
    uint32_t flags = static_cast<uint32_t>(prop->PropertyFlags); // 取低 32 位
    uint16_t rep   = prop->RepIndex;
    int32_t  dim   = prop->ArrayDim;
    std::string arrSuf = (dim > 1) ? std::string("[") + std::to_string(dim) + "]" : std::string();

    std::string extra;
    if (k == "EnumProperty") {
        uintptr_t ep = reinterpret_cast<uintptr_t>(prop->SubTypePtr);
        if (ok(ep)) {
            int idx = getIdxByPtr(ep);
            std::string en = (idx >= 0) ? m_objs[idx].name : oname(ep);
            if (!en.empty()) { extra = " [UEnum: " + en + "]"; }
        }
    }

    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "\t%s %s%s; // Addr: 0x%lX, Offset: 0x%X (Size: 0x%X)%s [RepIndex: %u] [Owner: %s]",
                  ptype.c_str(), pname.c_str(), arrSuf.c_str(),
                  static_cast<unsigned long>(ownerPtr + off),
                  off, flags, extra.c_str(), rep, ownerName.c_str());
    return std::string(buf);
}

std::vector<std::string> UE4NrcDumper::emitFunctionLines(const ObjEntry& fn, const std::string& ownerName) {
    auto* uf = reinterpret_cast<const UFunction*>(fn.snap.data());
    std::string name = safeIdent(fn.name);
    uint32_t  flags    = uf->FunctionFlags;
    uint8_t   numParms = uf->NumParms;
    uintptr_t fnPtr    = reinterpret_cast<uintptr_t>(uf->Func);

    std::string addrField, offsetField;
    if (inLib(fnPtr)) {
        char a[32]; std::snprintf(a, sizeof(a), "0x%lX", static_cast<unsigned long>(fnPtr));
        addrField = a;
        char o[32]; std::snprintf(o, sizeof(o), "0x%lX", static_cast<unsigned long>(fnPtr - m_moduleBase));
        offsetField = o;
    } else {
        addrField = fnPtr ? toHexUpper(fnPtr) : "0x0";
        offsetField = "?";
    }
    std::vector<std::string> out;
    out.push_back("\t// Flags: " + funcFlagsName(flags) + " [Owner: " + ownerName + "]");
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "\tvoid %s(); // Addr: %s, Offset: %s // NumParms: %u",
                  name.c_str(), addrField.c_str(), offsetField.c_str(), numParms);
    out.push_back(buf);
    return out;
}

// =====================================================================
//  CDO 偏移自验证: cdo.cls == this UClass
// =====================================================================
int32_t UE4NrcDumper::probeCdoOffset(const std::vector<int>& classes) {
    int best = -1, bestHits = 0;
    int N = std::min(static_cast<int>(classes.size()), 300);
    for (uint32_t off = UClassCdoProbe::Begin;
                  off <= UClassCdoProbe::End;
                  off += UClassCdoProbe::Step) {
        int hits = 0;
        for (int i = 0; i < N; ++i) {
            const ObjEntry& e = m_objs[classes[i]];
            uintptr_t cdo = 0;
            memcpy(&cdo, e.snap.data() + off, sizeof(uintptr_t));
            if (!ok(cdo)) continue;
            int ci = getIdxByPtr(cdo);
            if (ci < 0) continue;
            if (m_objs[ci].clsPtr == e.ptr) ++hits;
        }
        if (hits > bestHits) { bestHits = hits; best = static_cast<int>(off); }
    }
    return bestHits >= 20 ? best : -1;
}

void UE4NrcDumper::emitVTable(int classIdx, FILE* out) {
    if (m_cdoOffset < 0) return;
    const ObjEntry& e = m_objs[classIdx];
    uintptr_t cdoPtr = 0;
    memcpy(&cdoPtr, e.snap.data() + m_cdoOffset, sizeof(uintptr_t));
    if (!ok(cdoPtr)) return;
    int ci = getIdxByPtr(cdoPtr);
    if (ci < 0) return;
    uintptr_t vptr = rp(cdoPtr);
    if (!ok(vptr)) return;

    std::vector<uintptr_t> slots;
    int slotCap = m_vtMaxSlots;
    std::vector<uint8_t> buf;
    while (slotCap > 0) {
        buf.assign(slotCap * 8, 0);
        if (safeReadBytes(vptr, buf.data(), buf.size())) break;
        slotCap /= 2;
    }
    if (slotCap == 0) return;

    for (int k = 0; k < slotCap; ++k) {
        uintptr_t fp = 0;
        memcpy(&fp, buf.data() + k * 8, sizeof(uintptr_t));
        if (!ok(fp)) break;
        if (!inLib(fp)) break;
        slots.push_back(fp);
    }
    if (slots.empty()) return;

    std::fprintf(out,
                 "\n\t// VTable [CDO: 0x%lX, VPtr: 0x%lX, Offset: 0x%lX, %zu slots]\n",
                 static_cast<unsigned long>(cdoPtr),
                 static_cast<unsigned long>(vptr),
                 static_cast<unsigned long>(vptr - m_moduleBase),
                 slots.size());
    for (size_t k = 0; k < slots.size(); ++k) {
        std::fprintf(out, "\tvirtual void VFunc_%zu(); // Addr: 0x%lX, Offset: 0x%lX\n",
                     k,
                     static_cast<unsigned long>(slots[k]),
                     static_cast<unsigned long>(slots[k] - m_moduleBase));
    }
}

// =====================================================================
//  输出路径 / 文件助手
// =====================================================================
std::string UE4NrcDumper::resolveOutputPath(const char* filename) const {
    if (filename && filename[0] == '/') return filename;
    std::string base = m_outputPath.empty()
        ? std::string("/data/data/com.tencent.nrc/cache/dumps/")
        : m_outputPath;
    if (base.empty() || base.back() != '/') base += '/';
    return base + (filename ? filename : "");
}

FILE* UE4NrcDumper::openOutputFile(const char* filename) const {
    std::string full = resolveOutputPath(filename);
    // mkdir -p 父目录
    std::string dir = full.substr(0, full.find_last_of('/'));
    std::string acc;
    for (char c : dir) {
        if (c == '/') {
            if (!acc.empty()) mkdir(acc.c_str(), 0755);
            acc += '/';
        } else {
            acc += c;
        }
    }
    if (!dir.empty()) mkdir(dir.c_str(), 0755);
    return std::fopen(full.c_str(), "wb");
}

// =====================================================================
//  dumpSDK — 主输出
// =====================================================================
bool UE4NrcDumper::dumpSDK(const char* filePath) {
    if (!m_initialized && !init()) return false;
    if (!snapshotNamePool()) return false;
    if (!snapshotAllObjects()) return false;

    // 分类 + funcsByOwner 预聚合
    std::vector<int> classes, structs, enums;
    std::unordered_map<uintptr_t, std::vector<int>> funcsByOwner;
    int cFunc = 0;
    for (size_t i = 0; i < m_objs.size(); ++i) {
        const ObjEntry& e = m_objs[i];
        if (e.ptr == 0 || e.clsName.empty()) continue;
        const std::string& cn = e.clsName;
        if (cn == "Class" || cn == "BlueprintGeneratedClass"
         || cn == "WidgetBlueprintGeneratedClass" || cn == "AnimBlueprintGeneratedClass") {
            classes.push_back(static_cast<int>(i));
        } else if (cn == "ScriptStruct") {
            structs.push_back(static_cast<int>(i));
        } else if (cn == "Enum" || cn == "UserDefinedEnum") {
            enums.push_back(static_cast<int>(i));
        } else if (cn == "Function" || cn == "DelegateFunction" || cn == "SparseDelegateFunction") {
            funcsByOwner[e.outerPtr].push_back(static_cast<int>(i));
            ++cFunc;
        }
    }
    LOG(LOG_LEVEL_INFO, "[NrcDumper] classified: %zu classes, %zu structs, %zu enums, %d functions",
        classes.size(), structs.size(), enums.size(), cFunc);

    // 一次性 walk ChildProperties
    for (int i : classes) walkPropsForStructEntry(i);
    for (int i : structs) walkPropsForStructEntry(i);

    // CDO 探测
    m_cdoOffset = probeCdoOffset(classes);
    LOG(LOG_LEVEL_INFO, "[NrcDumper] CDO offset = %d", m_cdoOffset);

    // 排序 (按 fullPath)
    auto cmp = [this](int a, int b) {
        return fullPathFromIdx(a) < fullPathFromIdx(b);
    };
    std::sort(classes.begin(), classes.end(), cmp);
    std::sort(structs.begin(), structs.end(), cmp);
    std::sort(enums.begin(),   enums.end(),   cmp);

    FILE* out = openOutputFile(filePath ? filePath : "sdk.cs");
    if (!out) {
        LOG(LOG_LEVEL_ERROR, "[NrcDumper] dumpSDK: open output failed");
        return false;
    }
    int totalTypes = static_cast<int>(classes.size() + structs.size() + enums.size());
    std::fprintf(out, "// UE4 SDK Dump (NRC, ported from ue_dump_all.js)\n");
    std::fprintf(out, "// Target: com.tencent.nrc\n");
    std::fprintf(out, "// Total: %d types\n\n", totalTypes);

    // emit classes
    for (int i : classes) {
        const ObjEntry& e = m_objs[i];
        std::string name = safeIdent(e.name);
        auto* us = reinterpret_cast<const UStruct*>(e.snap.data());
        uintptr_t sup = reinterpret_cast<uintptr_t>(us->SuperStruct);
        std::string supName = shortClassNameFromPtr(sup);
        uint32_t size = static_cast<uint32_t>(us->PropertiesSize);

        std::fprintf(out, "// Class %s\n", fullPathFromIdx(i).c_str());
        std::fprintf(out, "// Size: 0x%X\n", size);
        std::fprintf(out, "class %s%s%s\n{\n",
                     name.c_str(),
                     supName.empty() ? "" : " : public ",
                     supName.c_str());

        // fields (expanded)
        std::vector<int> chain;
        superChain(i, chain);
        struct FOut { uintptr_t ptr; const uint8_t* snap; std::string owner; uintptr_t ownerPtr; uint32_t off; };
        std::vector<FOut> fields;
        for (int ci : chain) {
            const ObjEntry& ce = m_objs[ci];
            for (size_t k = 0; k < ce.propPtrs.size(); ++k) {
                uint32_t off = static_cast<uint32_t>(
                    reinterpret_cast<const FProperty*>(ce.propSnap[k].data())->Offset_Internal);
                fields.push_back({ce.propPtrs[k], ce.propSnap[k].data(), safeIdent(ce.name), ce.ptr, off});
            }
        }
        std::sort(fields.begin(), fields.end(),
                  [](const FOut& a, const FOut& b) { return a.off < b.off; });

        if (!fields.empty()) {
            std::fprintf(out, "\t// Fields (Expanded Inheritance)\n");
            for (auto& f : fields) {
                std::string ln = emitField(e, f.snap, f.ownerPtr, f.owner);
                if (!ln.empty()) std::fprintf(out, "%s\n", ln.c_str());
            }
        }

        // functions (expanded)
        bool hadFuncHeader = false;
        for (int ci : chain) {
            const ObjEntry& ce = m_objs[ci];
            auto it = funcsByOwner.find(ce.ptr);
            if (it == funcsByOwner.end()) continue;
            std::string ownerName = safeIdent(ce.name);
            for (int fi : it->second) {
                const ObjEntry& fe = m_objs[fi];
                if (!hadFuncHeader) {
                    std::fprintf(out, "\n\t// Functions (Expanded Inheritance)\n");
                    hadFuncHeader = true;
                }
                for (auto& ln : emitFunctionLines(fe, ownerName)) {
                    std::fprintf(out, "%s\n", ln.c_str());
                }
            }
        }

        // VTable
        emitVTable(i, out);

        std::fprintf(out, "};\n\n");
    }

    // emit structs
    for (int i : structs) {
        const ObjEntry& e = m_objs[i];
        std::string name = safeIdent(e.name);
        auto* us = reinterpret_cast<const UStruct*>(e.snap.data());
        uintptr_t sup = reinterpret_cast<uintptr_t>(us->SuperStruct);
        std::string supName = shortClassNameFromPtr(sup);
        uint32_t size = static_cast<uint32_t>(us->PropertiesSize);
        std::fprintf(out, "// ScriptStruct %s\n", fullPathFromIdx(i).c_str());
        std::fprintf(out, "// Size: 0x%X\n", size);
        std::fprintf(out, "struct %s%s%s\n{\n", name.c_str(),
                     supName.empty() ? "" : " : public ", supName.c_str());

        std::vector<int> chain; superChain(i, chain);
        struct FOut { const uint8_t* snap; std::string owner; uintptr_t ownerPtr; uint32_t off; };
        std::vector<FOut> fields;
        for (int ci : chain) {
            const ObjEntry& ce = m_objs[ci];
            for (size_t k = 0; k < ce.propPtrs.size(); ++k) {
                uint32_t off = static_cast<uint32_t>(
                    reinterpret_cast<const FProperty*>(ce.propSnap[k].data())->Offset_Internal);
                fields.push_back({ce.propSnap[k].data(), safeIdent(ce.name), ce.ptr, off});
            }
        }
        std::sort(fields.begin(), fields.end(),
                  [](const FOut& a, const FOut& b) { return a.off < b.off; });
        if (!fields.empty()) {
            std::fprintf(out, "\t// Fields (Expanded Inheritance)\n");
            for (auto& f : fields) {
                std::string ln = emitField(e, f.snap, f.ownerPtr, f.owner);
                if (!ln.empty()) std::fprintf(out, "%s\n", ln.c_str());
            }
        }
        std::fprintf(out, "};\n\n");
    }

    // emit enums
    for (int i : enums) {
        const ObjEntry& e = m_objs[i];
        std::string name = safeIdent(e.name);
        std::fprintf(out, "// Enum %s\n", fullPathFromIdx(i).c_str());
        std::fprintf(out, "enum %s {\n", name.c_str());

        auto* ue = reinterpret_cast<const UEnum*>(e.snap.data());
        uintptr_t arrPtr = reinterpret_cast<uintptr_t>(ue->NamesData);
        int32_t num = ue->NamesNum;
        if (!ok(arrPtr) || num <= 0 || num > 16384) {
            std::fprintf(out, "\t// (no enum values)\n");
        } else {
            std::vector<uint8_t> buf(static_cast<size_t>(num) * UEnum::kEnumPairStride);
            if (!safeReadBytes(arrPtr, buf.data(), buf.size())) {
                std::fprintf(out, "\t// (read fail)\n");
            } else {
                for (int k = 0; k < num; ++k) {
                    uint32_t cmp = 0;
                    int64_t  val = 0;
                    memcpy(&cmp, buf.data() + k * UEnum::kEnumPairStride, sizeof(uint32_t));
                    memcpy(&val, buf.data() + k * UEnum::kEnumPairStride + 8, sizeof(int64_t));
                    std::string nm = readFNameCmp(cmp);
                    if (nm.empty()) { nm = name + "_" + std::to_string(k); }
                    std::fprintf(out, "\t%s = %lld,\n", nm.c_str(), static_cast<long long>(val));
                }
            }
        }
        std::fprintf(out, "};\n\n");
    }

    std::fclose(out);
    LOG(LOG_LEVEL_INFO, "[NrcDumper] dumpSDK done -> %s", resolveOutputPath(filePath ? filePath : "sdk.cs").c_str());
    return true;
}

// =====================================================================
//  dumpNames — 平铺 NamePool
// =====================================================================
bool UE4NrcDumper::dumpNames(const char* filePath) {
    if (!snapshotNamePool()) return false;
    FILE* out = openOutputFile(filePath ? filePath : "names.txt");
    if (!out) return false;

    std::fprintf(out, "// UE4 NamePool dump (com.tencent.nrc)\n");
    std::fprintf(out, "// Format: [block:offset] name\n\n");

    int total = 0;
    for (size_t b = 0; b < m_blocks.size(); ++b) {
        const auto& buf = m_blocks[b].data;
        size_t off = 0;
        while (off + 2 <= buf.size()) {
            uint16_t h = static_cast<uint16_t>(buf[off]) | (static_cast<uint16_t>(buf[off + 1]) << 8);
            bool wide = (h & 0x1) != 0;
            uint32_t len = static_cast<uint32_t>(h >> 6);
            if (len == 0) {
                // block 末尾大段 0
                size_t k = off, zero = 0;
                while (k < buf.size() && buf[k] == 0) { ++zero; ++k; if (zero > 16) break; }
                if (zero > 16) break;
                off += 2;
                continue;
            }
            if (len > 1024) break;
            size_t dataLen = len * (wide ? 2u : 1u);
            if (off + 2 + dataLen > buf.size()) break;
            std::string s;
            s.reserve(len);
            if (wide) {
                for (uint32_t i = 0; i < len; ++i) {
                    s += static_cast<char>(buf[off + 2 + i * 2]);
                }
            } else {
                for (uint32_t i = 0; i < len; ++i) {
                    s += static_cast<char>(buf[off + 2 + i]);
                }
            }
            uint32_t cmp = (static_cast<uint32_t>(b) << 16) | static_cast<uint32_t>(off >> 1);
            std::fprintf(out, "[0x%08x] %s\n", cmp, s.c_str());
            ++total;
            off += (2 + dataLen + 1) & ~static_cast<size_t>(1);
        }
    }
    std::fclose(out);
    LOG(LOG_LEVEL_INFO, "[NrcDumper] dumpNames: %d entries", total);
    return true;
}

// =====================================================================
//  dumpObjects
// =====================================================================
bool UE4NrcDumper::dumpObjects(const char* filePath) {
    if (!snapshotAllObjects()) return false;
    FILE* out = openOutputFile(filePath ? filePath : "objects.txt");
    if (!out) return false;
    std::fprintf(out, "// UE4 GUObjectArray dump (com.tencent.nrc)\n");
    std::fprintf(out, "// Format: [Index] FullPath  Class=ClassName  Addr=0x...\n\n");
    int total = 0;
    for (size_t i = 0; i < m_objs.size(); ++i) {
        const ObjEntry& e = m_objs[i];
        if (e.ptr == 0) continue;
        std::fprintf(out, "[%zu] %s  Class=%s  Addr=0x%lX\n",
                     i,
                     fullPathFromIdx(static_cast<int>(i)).c_str(),
                     e.clsName.empty() ? "?" : e.clsName.c_str(),
                     static_cast<unsigned long>(e.ptr));
        ++total;
    }
    std::fclose(out);
    LOG(LOG_LEVEL_INFO, "[NrcDumper] dumpObjects: %d entries", total);
    return true;
}

// =====================================================================
//  dumpGWorld
// =====================================================================
bool UE4NrcDumper::dumpGWorld(const char* filePath) {
    if (!snapshotAllObjects()) return false;
    FILE* out = openOutputFile(filePath ? filePath : "gworld_info.txt");
    if (!out) return false;
    std::fprintf(out, "// UE4 GWorld snapshot (com.tencent.nrc)\n\n");

    uintptr_t slot = m_moduleBase + m_offGWorld;
    std::fprintf(out, "GWorld slot:    0x%lX (offset 0x%X)\n",
                 static_cast<unsigned long>(slot), m_offGWorld);
    uintptr_t world = rp(slot);
    std::fprintf(out, "UWorld*:        0x%lX\n", static_cast<unsigned long>(world));
    if (!ok(world)) {
        std::fprintf(out, "(NULL — game not in a world yet)\n");
        std::fclose(out);
        return true;
    }

    int wi = getIdxByPtr(world);
    if (wi < 0) {
        std::fprintf(out, "WARNING: world ptr not in GUObjectArray\n");
    } else {
        const ObjEntry& we = m_objs[wi];
        std::fprintf(out, "GUObj index:    %d\n", wi);
        std::fprintf(out, "World class:    %s\n", we.clsName.c_str());
        std::fprintf(out, "World name:     %s\n", fullPathFromIdx(wi).c_str());
    }

    // PersistentLevel @ 0x30 (UE 4.26)
    uintptr_t persistentLevel = rp(world + 0x30);
    std::fprintf(out, "PersistentLevel: 0x%lX\n", static_cast<unsigned long>(persistentLevel));
    if (ok(persistentLevel)) {
        // ULevel.Actors @ 0x98 (data ptr), Num @ 0xA0
        uintptr_t actorsData = rp(persistentLevel + 0x98);
        int32_t   actorsNum  = rs32(persistentLevel + 0xA0);
        std::fprintf(out, "Actors:         data=0x%lX  num=%d\n",
                     static_cast<unsigned long>(actorsData), actorsNum);
        int sample = std::min(actorsNum, 32);
        for (int k = 0; k < sample; ++k) {
            uintptr_t a = rp(actorsData + k * 8);
            if (!ok(a)) continue;
            int ai = getIdxByPtr(a);
            std::fprintf(out, "  [%d] 0x%lX  Class=%s  Path=%s\n",
                         k, static_cast<unsigned long>(a),
                         (ai >= 0 ? m_objs[ai].clsName.c_str() : "?"),
                         (ai >= 0 ? fullPathFromIdx(ai).c_str() : "?"));
        }
    }

    // OwningGameInstance @ 0x180
    uintptr_t gi = rp(world + 0x180);
    std::fprintf(out, "OwningGameInstance: 0x%lX\n", static_cast<unsigned long>(gi));
    if (ok(gi)) {
        uintptr_t lpData = rp(gi + 0x38);
        int32_t   lpNum  = rs32(gi + 0x40);
        std::fprintf(out, "LocalPlayers:    data=0x%lX  num=%d\n",
                     static_cast<unsigned long>(lpData), lpNum);
    }

    std::fclose(out);
    LOG(LOG_LEVEL_INFO, "[NrcDumper] dumpGWorld done");
    return true;
}

bool UE4NrcDumper::dumpAll() {
    bool a = dumpSDK();
    bool b = dumpNames();
    bool c = dumpObjects();
    bool d = dumpGWorld();
    return a && b && c && d;
}

} // namespace ue4nrc

OBFU_ATTRS_END
