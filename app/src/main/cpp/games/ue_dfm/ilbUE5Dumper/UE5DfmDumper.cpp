#include "UE5DfmDumper.h"
#include "UE5DfmStruct.h"
#include "../../../core/log/log.h"
#include <algorithm>
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <cstddef>
#include <sys/stat.h>

// =====================================================================
//  安全内存读取 — 使用 SIGSEGV 信号捕获防止崩溃
// =====================================================================
static thread_local sigjmp_buf s_safeReadJmpBuf;
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

// =====================================================================
//  UE 5.4 DFM SDK Dumper — C++ 实现 (从 sdk_dump.js 转换)
// =====================================================================

namespace ue5dfm {

// ===================== 构造/析构 =====================================

UE5DfmDumper::UE5DfmDumper(uintptr_t moduleBase, uintptr_t moduleSize,
                           uint32_t offNamePool, uint32_t offGUObjectArrayNum,
                           uint32_t offGUObjectArrayChunks, uint32_t offGWorld,
                           const std::string& outputPath)
    : m_moduleBase(moduleBase)
    , m_moduleSize(moduleSize)
    , m_outputPath(outputPath)
    , m_initialized(false)
    , m_offNamePool(offNamePool)
    , m_offGUObjectArrayNum(offGUObjectArrayNum)
    , m_offGUObjectArrayChunks(offGUObjectArrayChunks)
    , m_offGWorld(offGWorld)
{
}

UE5DfmDumper::~UE5DfmDumper() = default;

void UE5DfmDumper::setModuleBase(uintptr_t base) { m_moduleBase = base; m_initialized = false; }
void UE5DfmDumper::setModuleSize(uintptr_t size) { m_moduleSize = size; }
void UE5DfmDumper::setOutputPath(const std::string& path) { m_outputPath = path; }

// ===================== 安全内存读取 ==================================

uintptr_t UE5DfmDumper::rp(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    uintptr_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) { s_safeReadActive = 0; return 0; }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(uintptr_t));
    s_safeReadActive = 0;
    return val;
}

uint32_t UE5DfmDumper::r32(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    uint32_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) { s_safeReadActive = 0; return 0; }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(uint32_t));
    s_safeReadActive = 0;
    return val;
}

int32_t UE5DfmDumper::rs32(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    int32_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) { s_safeReadActive = 0; return 0; }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(int32_t));
    s_safeReadActive = 0;
    return val;
}

uint16_t UE5DfmDumper::r16(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    uint16_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) { s_safeReadActive = 0; return 0; }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(uint16_t));
    s_safeReadActive = 0;
    return val;
}

uint8_t UE5DfmDumper::r8(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    uint8_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) { s_safeReadActive = 0; return 0; }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(uint8_t));
    s_safeReadActive = 0;
    return val;
}

bool UE5DfmDumper::ok(uintptr_t p) {
    return p != 0 && p > 0x10000;
}

// ===================== NamePool 解码 (混淆) ===========================

uint8_t UE5DfmDumper::amask(int l) {
    switch (l % 9) {
        case 0: return ((l & 0x1f) + l) & 0x80;
        case 1: return ((l ^ 0xdf) + l) & 0x80;
        case 2: return ((l | 0xcf) + l) & 0x80;
        case 3: return (33 * l) & 0x80;
        case 4: return (l + (l >> 2)) & 0x80;
        case 5: return (3 * l + 5) & 0x80;
        case 6: return (((l << 2) | 5) + l) & 0x80;
        case 7: return (((l >> 4) | 7) + l) & 0x80;
        case 8: return ((l ^ 0x0c) + l) & 0x80;
        default: return ((l ^ 0x40) + l) & 0x80;
    }
}

std::string UE5DfmDumper::resolveName(uint32_t id) const {
    if (id == 0) return "None";

    uintptr_t pool = m_moduleBase + m_offNamePool;
    uint32_t bi = id >> FNameEntryAllocator::OffsetBits;
    uint32_t bo = (id & 0x3FFFF) << 1;

    uintptr_t bp = rp(pool + offsetof(FNameEntryAllocator, Blocks) + static_cast<uintptr_t>(bi) * 8);
    if (!ok(bp)) return "?";

    uint16_t hdr = r16(bp + bo);
    if (hdr == 0) return "?";

    int length = hdr >> 6;
    if (length <= 0 || length > 1024) return "?";

    uintptr_t raw = bp + bo + 2;
    uint8_t mk = amask(length);

    std::string out;
    out.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; i++) {
        out += static_cast<char>((0xFF ^ r8(raw + static_cast<uintptr_t>(i)) ^ mk) & 0xFF);
    }
    return out;
}

// ===================== Name / Object helpers =========================

std::string UE5DfmDumper::fname(uintptr_t addr) const {
    uint32_t c = r32(addr);
    uint32_t n = r32(addr + 4);
    std::string s = resolveName(c);
    if (n > 0) {
        s += "_";
        s += std::to_string(n - 1);
    }
    return s;
}

std::string UE5DfmDumper::oname(uintptr_t objPtr) const {
    return fname(objPtr + offsetof(UObjectBase, NamePrivate));
}

std::string UE5DfmDumper::ffname(uintptr_t fieldPtr) const {
    return fname(fieldPtr + offsetof(FField, NamePrivate));
}

std::string UE5DfmDumper::ffclassname(uintptr_t fieldPtr) const {
    uintptr_t cls = rp(fieldPtr + offsetof(FField, ClassPrivate));
    if (!ok(cls)) return "";
    return fname(cls + offsetof(FFieldClass, Name));
}

std::string UE5DfmDumper::className(uintptr_t objPtr) const {
    uintptr_t cls = rp(objPtr + offsetof(UObjectBase, ClassPrivate));
    if (!ok(cls)) return "Unknown";
    return oname(cls);
}

std::string UE5DfmDumper::getFullPath(uintptr_t objPtr) const {
    std::string parts[16];
    int count = 0;
    uintptr_t cur = objPtr;

    while (ok(cur) && count < 16) {
        parts[count++] = oname(cur);
        cur = rp(cur + offsetof(UObjectBase, OuterPrivate));
    }

    std::string result;
    for (int i = count - 1; i >= 0; i--) {
        if (!result.empty()) result += ".";
        result += parts[i];
    }
    return result;
}

// ===================== GUObjectArray 访问 ============================

uintptr_t UE5DfmDumper::getobj(int index) const {
    uintptr_t ct = rp(m_moduleBase + m_offGUObjectArrayChunks);
    if (!ok(ct)) return 0;
    uintptr_t cp = rp(ct + static_cast<uintptr_t>(static_cast<uint32_t>(index) >> 16) * 8);
    if (!ok(cp)) return 0;
    return rp(cp + static_cast<uintptr_t>(index & 0xFFFF) * sizeof(FUObjectItem));
}

// ===================== 属性类型映射 ==================================

std::string UE5DfmDumper::ptype(const std::string& cn, uintptr_t fpPtr) const {
    uintptr_t sub = ok(fpPtr) ? rp(fpPtr + offsetof(FPropertyFlat, SubTypePtr)) : 0;
    std::string sn = ok(sub) ? oname(sub) : "";

    if (cn.find("StructProperty") != std::string::npos)                   return sn.empty() ? "FStruct" : sn;
    if (cn.find("EncryptedObjectProperty") != std::string::npos)          return (sn.empty() ? "UObject" : sn) + "*";
    if (cn.find("ObjectProperty") != std::string::npos)                   return (sn.empty() ? "UObject" : sn) + "*";
    if (cn.find("ClassProperty") != std::string::npos)                    return "TSubclassOf<" + (sn.empty() ? "UObject" : sn) + ">";
    if (cn.find("SoftObjectProperty") != std::string::npos)               return "TSoftObjectPtr<" + (sn.empty() ? "UObject" : sn) + ">";
    if (cn.find("SoftClassProperty") != std::string::npos)                return "TSoftClassPtr<" + (sn.empty() ? "UObject" : sn) + ">";
    if (cn.find("WeakObjectProperty") != std::string::npos)               return "TWeakObjectPtr<" + (sn.empty() ? "UObject" : sn) + ">";
    if (cn.find("LazyObjectProperty") != std::string::npos)               return "TLazyObjectPtr<" + (sn.empty() ? "UObject" : sn) + ">";
    if (cn.find("InterfaceProperty") != std::string::npos)                return "TScriptInterface<" + (sn.empty() ? "UInterface" : sn) + ">";
    if (cn.find("EnumProperty") != std::string::npos)                     return sn.empty() ? "TEnumAsByte" : sn;
    if (cn.find("Bool") != std::string::npos)                             return "bool";
    if (cn.find("Float") != std::string::npos)                            return "float";
    if (cn.find("Double") != std::string::npos)                           return "double";
    if (cn.find("Int8") != std::string::npos)                             return "int8";
    if (cn.find("Int16") != std::string::npos)                            return "int16";
    if (cn.find("UInt16") != std::string::npos)                           return "uint16";
    if (cn.find("UInt32") != std::string::npos)                           return "uint32";
    if (cn.find("Int64") != std::string::npos)                            return "int64";
    if (cn.find("UInt64") != std::string::npos)                           return "uint64";
    if (cn.find("NameProperty") != std::string::npos)                     return "FName";
    if (cn.find("StrProperty") != std::string::npos)                      return "FString";
    if (cn.find("TextProperty") != std::string::npos)                     return "FText";
    if (cn.find("ArrayProperty") != std::string::npos)                    return "TArray";
    if (cn.find("MapProperty") != std::string::npos)                      return "TMap";
    if (cn.find("SetProperty") != std::string::npos)                      return "TSet";
    if (cn.find("MulticastSparseDelegateProperty") != std::string::npos)  return "FMulticastSparseDelegate";
    if (cn.find("MulticastInlineDelegateProperty") != std::string::npos)  return "FMulticastInlineDelegate";
    if (cn.find("MulticastDelegateProperty") != std::string::npos)        return "FMulticastDelegate";
    if (cn.find("DelegateProperty") != std::string::npos)                 return "FDelegate";
    if (cn.find("ByteProperty") != std::string::npos)                     return sn.empty() ? "uint8" : sn;
    if (cn.find("Int") != std::string::npos)                              return "int32";
    return cn;
}

// ===================== SDK dump helpers ===============================

void UE5DfmDumper::getInheritanceChain(uintptr_t objPtr, std::vector<uintptr_t>& chain) const {
    chain.clear();
    chain.push_back(objPtr);
    uintptr_t sp = rp(objPtr + offsetof(UStruct, SuperStruct));
    while (ok(sp)) {
        chain.push_back(sp);
        sp = rp(sp + offsetof(UStruct, SuperStruct));
    }
    std::reverse(chain.begin(), chain.end());
}

std::vector<UE5DfmDumper::FieldInfo> UE5DfmDumper::collectFields(uintptr_t structPtr, const std::string& ownerName) const {
    std::vector<FieldInfo> fields;
    uintptr_t cur = rp(structPtr + offsetof(UStruct, ChildProperties));
    int depth = 0;

    while (ok(cur) && depth < 4096) {
        depth++;
        std::string pcn = ffclassname(cur);
        if (pcn.find("Property") != std::string::npos) {
            int32_t ad = rs32(cur + offsetof(FPropertyFlat, ArrayDim));
            int32_t es = rs32(cur + offsetof(FPropertyFlat, ElementSize));
            FieldInfo f;
            f.typeName = ptype(pcn, cur);
            f.propName = ffname(cur);
            f.propClassName = pcn;
            f.offset = rs32(cur + offsetof(FPropertyFlat, Offset_Internal));
            f.size = es * (ad > 0 ? ad : 1);
            f.pflags = r32(cur + offsetof(FPropertyFlat, PropertyFlags));
            f.owner = ownerName;
            fields.push_back(std::move(f));
        }
        cur = rp(cur + offsetof(FField, Next));
    }
    return fields;
}

std::vector<UE5DfmDumper::FuncInfo> UE5DfmDumper::collectFuncs(uintptr_t classPtr, const std::string& ownerName) const {
    std::vector<FuncInfo> funcs;
    uintptr_t uf = rp(classPtr + offsetof(UStruct, Children));
    int depth = 0;

    while (ok(uf) && depth < 4096) {
        depth++;
        uintptr_t ucl = rp(uf + offsetof(UObjectBase, ClassPrivate));
        if (ok(ucl)) {
            std::string uclName = oname(ucl);
            if (uclName == "Function" || uclName == "DelegateFunction") {
                uint32_t ff = r32(uf + offsetof(UFunction, FunctionFlags));
                uintptr_t nf = rp(uf + offsetof(UFunction, Func));

                std::string retType = "void";
                std::string paramStr;
                int numParms = 0;

                uintptr_t pc = rp(uf + offsetof(UStruct, ChildProperties));
                while (ok(pc)) {
                    numParms++;
                    std::string ppcn = ffclassname(pc);
                    if (ppcn.find("Property") != std::string::npos) {
                        std::string ppt = ptype(ppcn, pc);
                        std::string ppn = ffname(pc);
                        uint32_t pf = r32(pc + offsetof(FPropertyFlat, PropertyFlags));

                        if (pf & CPF_ReturnParm) {
                            retType = ppt;
                        } else if (pf & CPF_Parm) {
                            if (!paramStr.empty()) paramStr += ", ";
                            if (pf & CPF_OutParm) paramStr += "out ";
                            if (pf & CPF_ConstParm) paramStr += "const ";
                            paramStr += ppt + " " + ppn;
                        }
                    }
                    pc = rp(pc + offsetof(FField, Next));
                }

                std::string flags;
                if (ff & FUNC_Final)             flags += "Final|";
                if (ff & FUNC_Native)            flags += "Native|";
                if (ff & FUNC_Event)             flags += "Event|";
                if (ff & FUNC_Net)               flags += "Net|";
                if (ff & FUNC_Exec)              flags += "Exec|";
                if (ff & FUNC_Static)            flags += "Static|";
                if (ff & FUNC_BlueprintCallable) flags += "BlueprintCallable|";
                if (ff & FUNC_BlueprintEvent)    flags += "BlueprintEvent|";
                if (ff & FUNC_BlueprintPure)     flags += "BlueprintPure|";

                FuncInfo fi;
                fi.name = oname(uf);
                fi.flags = flags;
                fi.rva = ok(nf) ? nf - m_moduleBase : 0;
                fi.numParms = numParms;
                fi.owner = ownerName;
                fi.retType = retType;
                fi.paramStr = paramStr;
                funcs.push_back(std::move(fi));
            }
        }
        uf = rp(uf + offsetof(UField, Next));
    }
    return funcs;
}

// ===================== CDO (Class Default Object) 查找 ===============

uintptr_t UE5DfmDumper::findCDO(uintptr_t classPtr) {
    if (!ok(classPtr)) return 0;

    // 已知偏移直接读取
    if (m_cdoOffset >= 0) {
        uintptr_t cdo = rp(classPtr + static_cast<uintptr_t>(m_cdoOffset));
        return ok(cdo) ? cdo : 0;
    }

    // 扫描 UClass 内存找 CDO: CDO.ClassPrivate (+0x08) 应指回 classPtr
    for (int off = 0xC0; off <= 0x260; off += 8) {
        uintptr_t candidate = rp(classPtr + static_cast<uintptr_t>(off));
        if (!ok(candidate)) continue;
        uintptr_t candidateClass = rp(candidate + offsetof(UObjectBase, ClassPrivate));
        if (candidateClass != classPtr) continue;
        // 验证: CDO 名字应包含 "Default__"
        std::string cdoName = oname(candidate);
        if (cdoName.find("Default__") == 0) {
            m_cdoOffset = off;
            LOG(LOG_LEVEL_INFO, "[DfmDumper] CDO found at UClass+0x%X", off);
            return candidate;
        }
    }
    return 0;
}

// ===================== NativeFunc 反查表 ==============================

void UE5DfmDumper::buildNativeFuncMap() {
    m_nativeFuncMap.clear();
    uint32_t maxObj = r32(m_moduleBase + m_offGUObjectArrayNum);

    for (uint32_t i = 0; i < maxObj; i++) {
        uintptr_t obj = getobj(static_cast<int>(i));
        if (!ok(obj)) continue;
        uintptr_t cls = rp(obj + offsetof(UObjectBase, ClassPrivate));
        if (!ok(cls)) continue;
        std::string clsName = oname(cls);
        if (clsName != "Function" && clsName != "DelegateFunction") continue;

        uintptr_t nfPtr = rp(obj + offsetof(UFunction, Func));
        if (!ok(nfPtr)) continue;

        uintptr_t ownerClass = rp(obj + offsetof(UObjectBase, OuterPrivate));
        std::string ownerName = ok(ownerClass) ? oname(ownerClass) : "?";
        m_nativeFuncMap[nfPtr] = ownerName + "::" + oname(obj);
    }
    LOG(LOG_LEVEL_INFO, "[DfmDumper] NativeFunc map: %zu entries",
        m_nativeFuncMap.size());
}

// ===================== 输出路径 ======================================

std::string UE5DfmDumper::resolveOutputPath(const char* filename) const {
    if (m_outputPath.empty()) return filename;
    std::string path = m_outputPath;
    if (path.back() != '/') path += '/';
    path += filename;
    return path;
}

FILE* UE5DfmDumper::openOutputFile(const char* filename) const {
    std::string path = resolveOutputPath(filename);

    // 确保目录存在
    if (!m_outputPath.empty()) {
        mkdir(m_outputPath.c_str(), 0755);
    }

    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) {
        // fallback: /data/local/tmp/
        std::string fallback = std::string("/data/local/tmp/") + filename;
        fp = fopen(fallback.c_str(), "w");
    }
    return fp;
}

// ===================== 初始化 ========================================

bool UE5DfmDumper::init() {
    if (m_moduleBase == 0) {
        LOG(LOG_LEVEL_ERROR, "[DfmDumper] moduleBase 为空");
        return false;
    }

    // 验证 NamePool 块指针
    uintptr_t pool = m_moduleBase + m_offNamePool;
    uintptr_t firstBlock = rp(pool + offsetof(FNameEntryAllocator, Blocks));
    if (!ok(firstBlock)) {
        LOG(LOG_LEVEL_ERROR, "[DfmDumper] NamePool 第一个块指针无效: pool=%p block0=%p",
            (void*)pool, (void*)firstBlock);
        return false;
    }

    // 验证 GUObjectArray
    uint32_t numElements = r32(m_moduleBase + m_offGUObjectArrayNum);
    uintptr_t chunks = rp(m_moduleBase + m_offGUObjectArrayChunks);
    if (!ok(chunks) || numElements == 0) {
        LOG(LOG_LEVEL_ERROR, "[DfmDumper] GUObjectArray 无效: numElements=%u chunks=%p",
            numElements, (void*)chunks);
        return false;
    }

    // 尝试解析第一个名称验证解码
    std::string testName = resolveName(1);
    LOG(LOG_LEVEL_INFO, "[DfmDumper] 初始化成功: NamePool OK (test='%s'), GUObjectArray numElements=%u",
        testName.c_str(), numElements);

    m_initialized = true;
    return true;
}

// ===================== TASK 1: SDK Dump ==============================

bool UE5DfmDumper::dumpSDK(const char* filePath) {
    if (!m_initialized && !init()) return false;

    FILE* fp = filePath ? fopen(filePath, "w") : openOutputFile("sdk_dump.cs");
    if (!fp) {
        LOG(LOG_LEVEL_ERROR, "[DfmDumper] [SDK] 无法打开输出文件");
        return false;
    }

    LOG(LOG_LEVEL_INFO, "[DfmDumper] [SDK] Starting...");
    fprintf(fp, "// UE SDK Dump (C++ native)\n// Target: com.tencent.tmgp.dfm\n// Base: 0x%lX\n\n",
            static_cast<unsigned long>(m_moduleBase));

    // 设置较大的 I/O 缓冲区减少系统调用
    static char ioBuf[1 << 16]; // 64KB
    setvbuf(fp, ioBuf, _IOFBF, sizeof(ioBuf));
    fflush(fp);

    uint32_t maxObj = r32(m_moduleBase + m_offGUObjectArrayNum);
    int typeCount = 0, fieldCount = 0, funcCount = 0;

    // 缓存: 避免重复收集同一个 UStruct 的字段/函数
    std::unordered_map<uintptr_t, std::vector<FieldInfo>> fieldCache;
    std::unordered_map<uintptr_t, std::vector<FuncInfo>> funcCache;

    // Phase 0: 预扫描所有 UFunction, 构建 NativeFunc 地址→名称 反查表
    buildNativeFuncMap();

    for (uint32_t idx = 0; idx < maxObj; idx++) {
        uintptr_t p = getobj(static_cast<int>(idx));
        if (!ok(p)) continue;

        uintptr_t cp = rp(p + offsetof(UObjectBase, ClassPrivate));
        if (!ok(cp)) continue;

        std::string cn = oname(cp);
        int kind = 0; // 1=Class, 2=ScriptStruct, 3=Enum
        if (cn == "Class") kind = 1;
        else if (cn == "ScriptStruct") kind = 2;
        else if (cn == "Enum" || cn == "UserDefinedEnum") kind = 3;
        else continue;

        std::string nm = oname(p);
        uintptr_t outer = rp(p + offsetof(UObjectBase, OuterPrivate));
        std::string outerN = ok(outer) ? oname(outer) : "";

        // ---- Enum ----
        if (kind == 3) {
            typeCount++;
            fprintf(fp, "// Enum %s%s\n", outerN.empty() ? "" : (outerN + ".").c_str(), nm.c_str());
            fprintf(fp, "enum %s {\n", nm.c_str());

            uintptr_t dp = rp(p + offsetof(UEnum, Names));
            int32_t num = rs32(p + (offsetof(UEnum, Names) + 8));
            if (ok(dp) && num > 0 && num < 10000) {
                for (int32_t i = 0; i < num; i++) {
                    uintptr_t elemAddr = dp + static_cast<uintptr_t>(i) * sizeof(FEnumNamePair);
                    std::string elemName = fname(elemAddr);
                    int32_t elemVal = rs32(elemAddr + 8);
                    fprintf(fp, "\t%s = %d,\n", elemName.c_str(), elemVal);
                }
            }
            fprintf(fp, "};\n\n");
            continue;
        }

        // ---- Class / ScriptStruct ----
        typeCount++;
        std::vector<uintptr_t> chain;
        getInheritanceChain(p, chain);
        int32_t sz = rs32(p + offsetof(UStruct, PropertiesSize));

        // 继承链文字
        std::string inhStr;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            if (!inhStr.empty()) inhStr += " -> ";
            inhStr += oname(*it);
        }

        uintptr_t superP = chain.size() > 1 ? chain[chain.size() - 2] : 0;
        std::string superName = ok(superP) ? oname(superP) : "";

        fprintf(fp, "// %s %s%s\n", kind == 1 ? "Class" : "ScriptStruct",
                outerN.empty() ? "" : (outerN + ".").c_str(), nm.c_str());
        fprintf(fp, "// Size: 0x%X\n", static_cast<uint32_t>(sz));
        fprintf(fp, "// Inheritance: %s\n", inhStr.c_str());
        fprintf(fp, "%s %s", kind == 1 ? "class" : "struct", nm.c_str());
        if (!superName.empty()) fprintf(fp, " : public %s", superName.c_str());
        fprintf(fp, "\n{\n");

        // ---- Fields (展开继承) ----
        std::vector<FieldInfo> allFields;
        for (auto& chainPtr : chain) {
            auto it = fieldCache.find(chainPtr);
            if (it == fieldCache.end()) {
                fieldCache[chainPtr] = collectFields(chainPtr, oname(chainPtr));
                it = fieldCache.find(chainPtr);
            }
            for (auto& f : it->second) {
                allFields.push_back(f);
            }
        }

        if (!allFields.empty()) {
            fprintf(fp, "\t// Fields (Expanded Inheritance)\n");
            for (auto& f : allFields) {
                fprintf(fp, "\t%s %s; // 0x%X (Size: 0x%X) [Flags: 0x%X] [Owner: %s]\n",
                        f.typeName.c_str(), f.propName.c_str(),
                        static_cast<uint32_t>(f.offset),
                        static_cast<uint32_t>(f.size),
                        f.pflags, f.owner.c_str());
                fieldCount++;
            }
        }

        // ---- VTable via CDO (仅 Class) ----
        if (kind == 1) {
            uintptr_t cdo = findCDO(p);
            uintptr_t vtbl = ok(cdo) ? rp(cdo) : 0;
            uintptr_t pCdo = ok(superP) ? findCDO(superP) : 0;
            uintptr_t pVtbl = ok(pCdo) ? rp(pCdo) : 0;

            if (ok(vtbl)) {
                // 计算虚表总槽数
                int vtblTotal = 0;
                for (int vc = 0; vc < 1024; vc++) {
                    uintptr_t ve = rp(vtbl + static_cast<uintptr_t>(vc) * 8);
                    if (!ok(ve)) break;
                    if (m_moduleSize > 0 && (ve < m_moduleBase || ve >= m_moduleBase + m_moduleSize)) break;
                    vtblTotal++;
                }
                // 父类虚表槽数
                int pVtblTotal = 0;
                if (ok(pVtbl)) {
                    for (int pvc = 0; pvc < 1024; pvc++) {
                        uintptr_t pvce = rp(pVtbl + static_cast<uintptr_t>(pvc) * 8);
                        if (!ok(pvce)) break;
                        if (m_moduleSize > 0 && (pvce < m_moduleBase || pvce >= m_moduleBase + m_moduleSize)) break;
                        pVtblTotal++;
                    }
                }

                fprintf(fp, "\n\t// C++ VTable: %d slots%s\n",
                        vtblTotal,
                        ok(pVtbl)
                            ? (std::string(" (parent ") + superName + ": " + std::to_string(pVtblTotal) + " slots)").c_str()
                            : "");

                for (int vi = 0; vi < vtblTotal; vi++) {
                    uintptr_t ve = rp(vtbl + static_cast<uintptr_t>(vi) * 8);
                    uintptr_t rva = ve - m_moduleBase;

                    // inherited / override / new
                    const char* tag = "";
                    if (ok(pVtbl) && vi < pVtblTotal) {
                        uintptr_t pe = rp(pVtbl + static_cast<uintptr_t>(vi) * 8);
                        tag = (ve == pe) ? " (inherited)" : " (override)";
                    } else if (vi >= pVtblTotal) {
                        tag = " (new)";
                    }

                    // NativeFunc 反查
                    std::string sym;
                    auto it = m_nativeFuncMap.find(ve);
                    if (it != m_nativeFuncMap.end()) {
                        sym = " " + it->second;
                    }

                    fprintf(fp, "\t// [%d] +0x%X -> 0x%lX%s%s\n",
                            vi, vi * 8,
                            static_cast<unsigned long>(rva),
                            tag, sym.c_str());
                }
            }
        }

        // ---- Functions (仅 Class, 展开继承) ----
        if (kind == 1) {
            std::vector<FuncInfo> allFuncs;
            for (auto& chainPtr : chain) {
                auto it = funcCache.find(chainPtr);
                if (it == funcCache.end()) {
                    funcCache[chainPtr] = collectFuncs(chainPtr, oname(chainPtr));
                    it = funcCache.find(chainPtr);
                }
                for (auto& fn : it->second) {
                    allFuncs.push_back(fn);
                }
            }

            if (!allFuncs.empty()) {
                fprintf(fp, "\n\t// Functions (Expanded Inheritance)\n");
                for (auto& fn : allFuncs) {
                    char addrBuf[32];
                    if (fn.rva != 0) snprintf(addrBuf, sizeof(addrBuf), "0x%lX", static_cast<unsigned long>(fn.rva));
                    else snprintf(addrBuf, sizeof(addrBuf), "N/A");

                    fprintf(fp, "\t// Flags: %s [Owner: %s]\n", fn.flags.c_str(), fn.owner.c_str());
                    fprintf(fp, "\t%s %s(%s); // [Addr: %s] // NumParms: %d\n",
                            fn.retType.c_str(), fn.name.c_str(), fn.paramStr.c_str(),
                            addrBuf, fn.numParms);
                    funcCount++;
                }
            }
        }

        fprintf(fp, "};\n\n");

        // 定期 flush + 日志
        if (idx % 3000 == 0 && idx > 0) {
            fflush(fp);
            LOG(LOG_LEVEL_INFO, "[DfmDumper] [SDK] %u/%u types=%d fields=%d", idx, maxObj, typeCount, fieldCount);
        }
    }

    fflush(fp);
    fclose(fp);
    LOG(LOG_LEVEL_INFO, "[DfmDumper] [SDK] DONE! Types=%d Fields=%d Funcs=%d", typeCount, fieldCount, funcCount);
    return true;
}

// ===================== TASK 2: NamePool Dump =========================

bool UE5DfmDumper::dumpNames(const char* filePath) {
    if (!m_initialized && !init()) return false;

    FILE* fp = filePath ? fopen(filePath, "w") : openOutputFile("NamesDump.txt");
    if (!fp) {
        LOG(LOG_LEVEL_ERROR, "[DfmDumper] [Names] 无法打开输出文件");
        return false;
    }

    LOG(LOG_LEVEL_INFO, "[DfmDumper] [Names] Starting...");

    uintptr_t pool = m_moduleBase + m_offNamePool;
    int numBlocks = 0;
    for (int b = 0; b < static_cast<int>(FNameEntryAllocator::MaxBlocks); b++) {
        uintptr_t bp = rp(pool + offsetof(FNameEntryAllocator, Blocks) + static_cast<uintptr_t>(b) * 8);
        if (!ok(bp)) break;
        numBlocks++;
    }
    LOG(LOG_LEVEL_INFO, "[DfmDumper] [Names] blocks=%d", numBlocks);

    int totalNames = 0;

    for (int bi = 0; bi < numBlocks; bi++) {
        uintptr_t bp = rp(pool + offsetof(FNameEntryAllocator, Blocks) + static_cast<uintptr_t>(bi) * 8);
        if (!ok(bp)) continue;

        uint32_t offset = 0;
        while (offset < FNameEntryAllocator::BlockSizeBytes - 4) {
            uint16_t hdr = r16(bp + offset);
            if (hdr == 0) break;

            bool isWide = (hdr & 1) != 0;
            int length = hdr >> 6;
            if (length <= 0 || length > 1024) break;

            int charBytes = isWide ? 2 : 1;
            int payloadBytes = length * charBytes;
            uint32_t totalSize = (2 + payloadBytes + 1) & ~1u;
            if (offset + totalSize > FNameEntryAllocator::BlockSizeBytes) break;

            uint32_t id = (static_cast<uint32_t>(bi) << FNameEntryAllocator::OffsetBits) | (offset >> 1);
            uintptr_t raw = bp + offset + 2;

            std::string decoded;
            decoded.reserve(static_cast<size_t>(length));

            if (isWide) {
                uint16_t wk = ((amask(length) | 0x7F) + 0x80) & 0xFFFF;
                for (int i = 0; i < length; i++) {
                    uint8_t lo = r8(raw + static_cast<uintptr_t>(i) * 2);
                    uint8_t hi = r8(raw + static_cast<uintptr_t>(i) * 2 + 1);
                    uint16_t ch = ((lo | (hi << 8)) ^ wk) & 0xFFFF;
                    // 简单 UTF-16 → ASCII 截断 (与原始 JS 行为一致)
                    decoded += static_cast<char>(ch & 0xFF);
                }
            } else {
                uint8_t mk = amask(length);
                for (int i = 0; i < length; i++) {
                    decoded += static_cast<char>((0xFF ^ r8(raw + static_cast<uintptr_t>(i)) ^ mk) & 0xFF);
                }
            }

            if (!decoded.empty()) {
                fprintf(fp, "%u %s\n", id, decoded.c_str());
                totalNames++;
            }
            offset += totalSize;
        }

        if (bi % 100 == 0 && bi > 0) {
            fflush(fp);
            LOG(LOG_LEVEL_INFO, "[DfmDumper] [Names] block %d/%d names=%d", bi, numBlocks, totalNames);
        }
    }

    fflush(fp);
    fclose(fp);
    LOG(LOG_LEVEL_INFO, "[DfmDumper] [Names] DONE! %d names", totalNames);
    return true;
}

// ===================== TASK 3: ObjectArray Dump =======================

bool UE5DfmDumper::dumpObjects(const char* filePath) {
    if (!m_initialized && !init()) return false;

    FILE* fp = filePath ? fopen(filePath, "w") : openOutputFile("ObjectsDump.txt");
    if (!fp) {
        LOG(LOG_LEVEL_ERROR, "[DfmDumper] [Objects] 无法打开输出文件");
        return false;
    }

    LOG(LOG_LEVEL_INFO, "[DfmDumper] [Objects] Starting...");

    uint32_t maxObj = r32(m_moduleBase + m_offGUObjectArrayNum);
    uintptr_t ct = rp(m_moduleBase + m_offGUObjectArrayChunks);
    int count = 0;

    for (uint32_t idx = 0; idx < maxObj; idx++) {
        uint32_t ci = idx >> 16;
        uint32_t wi = idx & 0xFFFF;
        uintptr_t cp = rp(ct + static_cast<uintptr_t>(ci) * 8);
        if (!ok(cp)) continue;
        uintptr_t obj = rp(cp + static_cast<uintptr_t>(wi) * sizeof(FUObjectItem));
        if (!ok(obj)) continue;

        uintptr_t cls = rp(obj + offsetof(UObjectBase, ClassPrivate));
        std::string clsName = ok(cls) ? oname(cls) : "None";
        fprintf(fp, "[%u] %s %s\n", idx, clsName.c_str(), getFullPath(obj).c_str());
        count++;

        if (idx % 50000 == 0 && idx > 0) {
            fflush(fp);
            LOG(LOG_LEVEL_INFO, "[DfmDumper] [Objects] %u/%u count=%d", idx, maxObj, count);
        }
    }

    fflush(fp);
    fclose(fp);
    LOG(LOG_LEVEL_INFO, "[DfmDumper] [Objects] DONE! %d objects", count);
    return true;
}

// ===================== TASK 4: GWorld Dump ============================

bool UE5DfmDumper::dumpGWorld(const char* filePath) {
    if (!m_initialized && !init()) return false;

    FILE* fp = filePath ? fopen(filePath, "w") : openOutputFile("GWorldInfo.txt");
    if (!fp) {
        LOG(LOG_LEVEL_ERROR, "[DfmDumper] [GWorld] 无法打开输出文件");
        return false;
    }

    LOG(LOG_LEVEL_INFO, "[DfmDumper] [GWorld] Starting...");

    uintptr_t gworldAddr = m_moduleBase + m_offGWorld;
    uintptr_t gworld = rp(gworldAddr);

    fprintf(fp, "// GWorld Dump\n// GWorld Global: 0x%lX\n// GWorld Ptr: 0x%lX\n\n",
            static_cast<unsigned long>(gworldAddr), static_cast<unsigned long>(gworld));

    if (!ok(gworld)) {
        fprintf(fp, "GWorld is NULL\n");
        fclose(fp);
        LOG(LOG_LEVEL_INFO, "[DfmDumper] [GWorld] GWorld is NULL");
        return true;
    }

    fprintf(fp, "GWorld: %s '%s'\n", className(gworld).c_str(), oname(gworld).c_str());
    fprintf(fp, "FullPath: %s\n\n", getFullPath(gworld).c_str());

    // PersistentLevel 搜索
    fprintf(fp, "// ========== PersistentLevel ==========\n");
    bool levelFound = false;
    for (uint32_t off = 0x20; off <= 0x100; off += 8) {
        uintptr_t lv = rp(gworld + off);
        if (!ok(lv)) continue;
        std::string lvClassName = className(lv);
        if (lvClassName.find("Level") == std::string::npos) continue;

        fprintf(fp, "PersistentLevel (GWorld+0x%X): %s '%s'\n",
                off, lvClassName.c_str(), oname(lv).c_str());
        levelFound = true;

        // Actor 列表搜索
        for (uint32_t aoff = 0x70; aoff <= 0x150; aoff += 8) {
            uintptr_t aPtr = rp(lv + aoff);
            int32_t aCnt = rs32(lv + aoff + 8);
            if (!ok(aPtr) || aCnt < 10 || aCnt > 100000) continue;

            fprintf(fp, "// Actors at Level+0x%X Count=%d\n", aoff, aCnt);
            int shown = 0;
            int limit = aCnt < 500 ? aCnt : 500;
            for (int ai = 0; ai < limit; ai++) {
                uintptr_t actor = rp(aPtr + static_cast<uintptr_t>(ai) * 8);
                if (!ok(actor)) continue;
                fprintf(fp, "  [%d] %s '%s'\n", ai, className(actor).c_str(), oname(actor).c_str());
                shown++;
            }
            fprintf(fp, "  ... (%d/%d shown)\n\n", shown, aCnt);
            break;
        }
        break;
    }
    if (!levelFound) {
        fprintf(fp, "PersistentLevel: NOT FOUND\n\n");
    }

    // Game Framework 搜索
    fprintf(fp, "// ========== Game Framework ==========\n");
    for (uint32_t goff = 0x100; goff <= 0x300; goff += 8) {
        uintptr_t gv = rp(gworld + goff);
        if (!ok(gv)) continue;
        std::string gcn = className(gv);
        if (gcn.find("GameMode") != std::string::npos ||
            gcn.find("GameState") != std::string::npos ||
            gcn.find("GameInstance") != std::string::npos ||
            gcn.find("WorldSettings") != std::string::npos ||
            gcn.find("NavigationSystem") != std::string::npos ||
            gcn.find("AISystem") != std::string::npos) {
            fprintf(fp, "  +0x%X: %s '%s'\n", goff, gcn.c_str(), oname(gv).c_str());
        }
    }

    fflush(fp);
    fclose(fp);
    LOG(LOG_LEVEL_INFO, "[DfmDumper] [GWorld] DONE!");
    return true;
}

// ===================== 一键 dump =====================================

bool UE5DfmDumper::dumpAll() {
    bool ok = true;
    ok &= dumpSDK();
    ok &= dumpNames();
    ok &= dumpObjects();
    ok &= dumpGWorld();
    return ok;
}

} // namespace ue5dfm
