#include "UE4Dumper.h"
#include "../libUE4Struct/ilbUE4Struct.h"
#include "../../Log/log.h"
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>
#include <csignal>
#include <csetjmp>

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
    // 转发给原处理器
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
//  UE4.18 GNames / GUObjectArray / GWorld Dump — C++ 实现
//  从 frida_ue4_dump.js 转化而来
// =====================================================================

namespace ue4 {

// ===================== 构造/析构 =====================================

UE4Dumper::UE4Dumper(uintptr_t moduleBase, uint64_t dqGNames,
                     uint64_t dqGUObjectArray, uint64_t dqGWorld,
                     uintptr_t moduleSize, std::string outputPath)
    : m_moduleBase(moduleBase)
    , m_GNames(static_cast<uintptr_t>(dqGNames))
    , m_GWorld(static_cast<uintptr_t>(dqGWorld))
    , m_GUObjectArray(static_cast<uintptr_t>(dqGUObjectArray))
    , m_numNames(0)
    , m_initialized(false)
    , m_moduleSize(moduleSize)
    , m_outputPath(std::move(outputPath))
{
}

UE4Dumper::~UE4Dumper() = default;

void UE4Dumper::setModuleBase(uintptr_t base) {
    m_moduleBase = base;
    m_initialized = false;
}

void UE4Dumper::setModuleSize(uintptr_t size) {
    m_moduleSize = size;
}

void UE4Dumper::setGNames(uintptr_t gnames) {
    m_GNames = gnames;
    m_initialized = false;
}

void UE4Dumper::setGWorld(uintptr_t gworld) {
    m_GWorld = gworld;
}

void UE4Dumper::setGUObjectArray(uintptr_t guobjectarray) {
    m_GUObjectArray = guobjectarray;
    m_initialized = false;
}

void UE4Dumper::setOutputPath(const std::string& path) {
    m_outputPath = path;
}

// ===================== 安全内存读取 (信号捕获保护) ====================

uintptr_t UE4Dumper::safeReadPtr(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    uintptr_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
        s_safeReadActive = 0;
        return 0;
    }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(uintptr_t));
    s_safeReadActive = 0;
    return val;
}

int32_t UE4Dumper::safeReadS32(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    int32_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
        s_safeReadActive = 0;
        return 0;
    }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(int32_t));
    s_safeReadActive = 0;
    return val;
}

uint32_t UE4Dumper::safeReadU32(uintptr_t addr) {
    if (addr == 0) return 0;
    installSafeReadGuard();
    uint32_t val = 0;
    if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
        s_safeReadActive = 0;
        return 0;
    }
    s_safeReadActive = 1;
    memcpy(&val, reinterpret_cast<void*>(addr), sizeof(uint32_t));
    s_safeReadActive = 0;
    return val;
}

// ===================== FName 解析 ====================================

const char* UE4Dumper::getNameByIndex(int index) {
    if (index < 0 || index >= m_numNames) return nullptr;

    auto* namesArray = reinterpret_cast<TNameEntryArray*>(m_GNames);
    FNameEntry* entry = namesArray->getEntry(index);
    if (entry == nullptr) return nullptr;

    return entry->getName();
}

std::string UE4Dumper::fnameToString(int nameIdx, int number) {
    const char* base = getNameByIndex(nameIdx);
    if (base == nullptr) return "<invalid>";

    std::string result(base);
    if (number != 0) {
        result += "_";
        result += std::to_string(number - 1);
    }
    return result;
}

std::string UE4Dumper::readObjectFName(uintptr_t objPtr) {
    auto* obj = reinterpret_cast<UObjectBase*>(objPtr);
    return fnameToString(obj->NamePrivate.ComparisonIndex, obj->NamePrivate.Number);
}

std::string UE4Dumper::readClassName(uintptr_t objPtr) {
    auto* obj = reinterpret_cast<UObjectBase*>(objPtr);
    if (obj->ClassPrivate == nullptr) return "<no_class>";
    return readObjectFName(reinterpret_cast<uintptr_t>(obj->ClassPrivate));
}

std::string UE4Dumper::readFullPath(uintptr_t objPtr) {
    std::string parts[64];
    int count = 0;
    auto* cur = reinterpret_cast<UObjectBase*>(objPtr);

    while (cur != nullptr && count < 64) {
        parts[count++] = fnameToString(cur->NamePrivate.ComparisonIndex, cur->NamePrivate.Number);
        cur = reinterpret_cast<UObjectBase*>(cur->OuterPrivate);
    }

    std::string result;
    for (int i = count - 1; i >= 0; i--) {
        if (!result.empty()) result += ".";
        result += parts[i];
    }
    return result;
}

// ===================== GUObjectArray 分块遍历 ========================

int UE4Dumper::forEachUObject(uintptr_t arrayBase, ForEachCallback cb, void* userData) {
    auto* arr = reinterpret_cast<FUObjectArray*>(arrayBase);
    int numChunks = arr->getNumChunks();
    int totalNum  = arr->getTotalNum();

    if (numChunks <= 0 || totalNum <= 0) return 0;

    int globalIdx = 0;
    int total = 0;

    for (int ci = 0; ci < numChunks; ci++) {
        auto* chunkBase = reinterpret_cast<FUObjectItem*>(arr->getChunkPtr(ci));
        int chunkCount = arr->getChunkCount(ci);

        if (chunkBase == nullptr || chunkCount <= 0) {
            globalIdx += (chunkCount > 0 ? chunkCount : 0);
            continue;
        }

        for (int wi = 0; wi < chunkCount; wi++) {
            FUObjectItem& item = chunkBase[wi];
            if (item.Object != nullptr) {
                cb(reinterpret_cast<uintptr_t>(item.Object), globalIdx, userData);
                total++;
            }
            globalIdx++;
            if (globalIdx >= totalNum) break;
        }
        if (globalIdx >= totalNum) break;
    }
    return total;
}

// ===================== 初始化 ========================================

bool UE4Dumper::init() {
    if (m_GNames == 0 || m_GUObjectArray == 0) return false;

    auto* namesArray = reinterpret_cast<TNameEntryArray*>(m_GNames);
    m_numNames = namesArray->NumElements;
    if (m_numNames <= 0) return false;

    const char* name0 = getNameByIndex(0);
    if (name0 == nullptr || strcmp(name0, "None") != 0) {
        return false;
    }

    // 自动获取模块大小 (从 /proc/self/maps 读取)
    if (m_moduleBase != 0 && m_moduleSize == 0) {
        char line[512];
        FILE* maps = fopen("/proc/self/maps", "r");
        if (maps) {
            uintptr_t maxEnd = m_moduleBase;
            while (fgets(line, sizeof(line), maps)) {
                if (strstr(line, "libUE4.so") != nullptr) {
                    uintptr_t start = 0, end = 0;
                    if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                        if (end > maxEnd) maxEnd = end;
                    }
                }
            }
            fclose(maps);
            if (maxEnd > m_moduleBase) {
                m_moduleSize = maxEnd - m_moduleBase;
                LOG(LOG_LEVEL_INFO, "[UE4Dumper] libUE4.so size: 0x%lX", (unsigned long)m_moduleSize);
            }
        }
    }

    m_initialized = true;
    return true;
}

// ===================== Dump 逻辑 =====================================

static void ensureDir(const char* dir) {
    mkdir(dir, 0777);
}

// 获取有效输出目录
std::string UE4Dumper::getEffectiveOutputDir() const {
    return m_outputPath;
}

// ---- Dump GNames ----
bool UE4Dumper::dumpGNames(const char* filePath) {
    if (!m_initialized) return false;

    std::string path;
    if (filePath != nullptr) {
        path = filePath;
    } else {
        path = getEffectiveOutputDir() + "NamesDump.txt";
    }

    ensureDir(getEffectiveOutputDir().c_str());
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return false;

    for (int i = 0; i < m_numNames; i++) {
        const char* name = getNameByIndex(i);
        if (name != nullptr) {
            fprintf(fp, "%d %s\n", i, name);
        }
    }

    fclose(fp);
    return true;
}

// ---- Dump GObjects ----
struct DumpObjectCtx {
    FILE* fp;
    UE4Dumper* dumper;
};

static void dumpObjectCallback(uintptr_t objPtr, int globalIdx, void* userData) {
    auto* ctx = reinterpret_cast<DumpObjectCtx*>(userData);
    std::string className = ctx->dumper->readClassName(objPtr);
    std::string fullPath  = ctx->dumper->readFullPath(objPtr);
    fprintf(ctx->fp, "[%d] %s %s\n", globalIdx, className.c_str(), fullPath.c_str());
}

bool UE4Dumper::dumpGObjects(const char* filePath) {
    if (!m_initialized) return false;

    std::string path;
    if (filePath != nullptr) {
        path = filePath;
    } else {
        path = getEffectiveOutputDir() + "ObjectsDump.txt";
    }

    ensureDir(getEffectiveOutputDir().c_str());
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return false;

    DumpObjectCtx ctx{};
    ctx.fp = fp;
    ctx.dumper = this;

    forEachUObject(m_GUObjectArray, dumpObjectCallback, &ctx);

    fclose(fp);
    return true;
}

// ---- Dump GWorld ----
bool UE4Dumper::dumpGWorld(const char* filePath) {
    if (!m_initialized) return false;

    if (m_GWorld == 0) return false;

    std::string worldName  = readObjectFName(m_GWorld);
    std::string worldClass = readClassName(m_GWorld);
    std::string worldPath  = readFullPath(m_GWorld);

    std::string path;
    if (filePath != nullptr) {
        path = filePath;
    } else {
        path = getEffectiveOutputDir() + "GWorldInfo.txt";
    }

    ensureDir(getEffectiveOutputDir().c_str());
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return false;

    fprintf(fp, "GWorld: 0x%lX\n", (unsigned long)m_GWorld);
    fprintf(fp, "Class: %s\n", worldClass.c_str());
    fprintf(fp, "Name: %s\n", worldName.c_str());
    fprintf(fp, "Path: %s\n", worldPath.c_str());

    fclose(fp);
    return true;
}

// ---- 一键 Dump ----
bool UE4Dumper::dumpAll() {
    if (!m_initialized && !init()) return false;

    bool ok = true;
    ok &= dumpGNames();
    ok &= dumpGObjects();
    ok &= dumpGWorld();
    ok &= dumpSDK();
    return ok;
}

// =====================================================================
//  SDK Dump 实现 (从 frida_ue4_sdk_dump.js 转化)
// =====================================================================

bool UE4Dumper::isModulePtr(uintptr_t p) {
    if (p == 0 || m_moduleBase == 0) return false;
    uintptr_t off = p - m_moduleBase;
    return off < m_moduleSize;
}

std::string UE4Dumper::getModuleOffsetText(uintptr_t p) {
    if (m_moduleBase == 0) return "<unknown>";
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%lX", (unsigned long)(p - m_moduleBase));
    return buf;
}

std::string UE4Dumper::getPackageName(uintptr_t objPtr) {
    auto* cur = reinterpret_cast<UObjectBase*>(objPtr);
    auto* last = cur;
    while (cur != nullptr) {
        last = cur;
        cur = reinterpret_cast<UObjectBase*>(cur->OuterPrivate);
    }
    return readObjectFName(reinterpret_cast<uintptr_t>(last));
}

std::string UE4Dumper::outerChain(uintptr_t objPtr) {
    std::string parts[32];
    int count = 0;
    auto* cur = reinterpret_cast<UObjectBase*>(
        reinterpret_cast<UObjectBase*>(objPtr)->OuterPrivate);
    while (cur != nullptr && count < 32) {
        parts[count++] = readObjectFName(reinterpret_cast<uintptr_t>(cur));
        cur = reinterpret_cast<UObjectBase*>(cur->OuterPrivate);
    }
    std::string result;
    for (int i = count - 1; i >= 0; i--) {
        if (!result.empty()) result += "/";
        result += parts[i];
    }
    return result;
}

std::string UE4Dumper::getObjectPath(uintptr_t objPtr) {
    if (objPtr == 0) return "";
    std::string outer = outerChain(objPtr);
    std::string name = readObjectFName(objPtr);
    return outer.empty() ? name : outer + "." + name;
}

// ---- 属性类型映射 ----
const char* UE4Dumper::getPropTypeName(const std::string& className) {
    // className 去掉 "Property" 后缀后查表
    if (className.size() <= 8) return nullptr;
    std::string base = className.substr(0, className.size() - 8);

    struct { const char* key; const char* val; } map[] = {
        {"Bool", "bool"}, {"Int", "int32"}, {"UInt32", "uint32"},
        {"Int8", "int8"}, {"Int16", "int16"}, {"Int64", "int64"},
        {"UInt16", "uint16"}, {"UInt64", "uint64"}, {"Byte", "uint8"},
        {"Float", "float"}, {"Double", "double"}, {"Str", "FString"},
        {"Name", "FName"}, {"Text", "FText"}, {"Object", "UObject*"},
        {"Class", "UClass*"}, {"SoftObject", "TSoftObjectPtr"},
        {"SoftClass", "TSoftClassPtr"}, {"WeakObject", "TWeakObjectPtr"},
        {"LazyObject", "TLazyObjectPtr"}, {"Interface", "TScriptInterface"},
        {"Struct", "FStruct"}, {"Array", "TArray"}, {"Map", "TMap"},
        {"Set", "TSet"}, {"Delegate", "FDelegate"},
        {"MulticastDelegate", "FMulticastDelegate"},
        {"MulticastInlineDelegate", "FMulticastInlineDelegate"},
        {"Enum", "enum"},
    };
    for (const auto& m : map) {
        if (base == m.key) return m.val;
    }
    return nullptr;
}

// ---- 继承链构建 ----
std::vector<UE4Dumper::HierarchyEntry> UE4Dumper::buildTypeHierarchy(uintptr_t typePtr) {
    std::vector<HierarchyEntry> chain;
    auto* cur = reinterpret_cast<UStruct*>(typePtr);
    int depth = 0;
    while (cur != nullptr && depth < 256) {
        HierarchyEntry e;
        e.ptr = reinterpret_cast<uintptr_t>(cur);
        e.name = readObjectFName(e.ptr);
        chain.push_back(e);
        cur = cur->SuperStruct;
        depth++;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// ---- 绑定枚举路径 ----
std::string UE4Dumper::getBoundEnumPath(uintptr_t propPtr) {
    std::string propClass = readClassName(propPtr);
    uintptr_t enumPtr = 0;
    if (propClass == "EnumProperty") {
        auto* ep = reinterpret_cast<ue4::UEnumProperty*>(propPtr);
        enumPtr = reinterpret_cast<uintptr_t>(ep->Enum);
    } else if (propClass == "ByteProperty") {
        auto* bp = reinterpret_cast<ue4::UByteProperty*>(propPtr);
        enumPtr = reinterpret_cast<uintptr_t>(bp->Enum);
    }
    if (enumPtr == 0) return "";
    std::string enumClass = readClassName(enumPtr);
    if (enumClass != "Enum" && enumClass != "UserDefinedEnum") return "";
    return getObjectPath(enumPtr);
}

// ---- 收集声明字段 ----
std::vector<UE4Dumper::FieldInfo> UE4Dumper::collectDeclaredFields(uintptr_t typePtr) {
    std::vector<FieldInfo> fields;
    std::string ownerName = readObjectFName(typePtr);

    auto* structPtr = reinterpret_cast<UStruct*>(typePtr);
    auto* child = reinterpret_cast<UField*>(structPtr->Children);
    int depth = 0;
    while (child != nullptr && depth < 5000) {
        std::string cc = readClassName(reinterpret_cast<uintptr_t>(child));
        if (cc.size() > 8 && cc.substr(cc.size() - 8) == "Property") {
            auto* prop = reinterpret_cast<UProperty*>(child);
            FieldInfo fi;
            fi.ownerName = ownerName;
            fi.propName = readObjectFName(reinterpret_cast<uintptr_t>(child));

            const char* mapped = getPropTypeName(cc);
            fi.typeName = mapped ? mapped : cc;

            fi.enumPath = getBoundEnumPath(reinterpret_cast<uintptr_t>(child));
            if (!fi.enumPath.empty()) {
                // 使用实际枚举类型名 (取路径最后一段)
                auto dot = fi.enumPath.rfind('.');
                fi.typeName = (dot != std::string::npos) ? fi.enumPath.substr(dot + 1) : fi.enumPath;
            }

            fi.offset   = prop->Offset_Internal;
            fi.elemSize = prop->ElementSize;
            fi.arrayDim = prop->ArrayDim;
            fi.repIndex = prop->RepIndex;
            fi.repNotifyFunc = fnameToString(
                prop->RepNotifyFunc.ComparisonIndex,
                prop->RepNotifyFunc.Number);

            fields.push_back(fi);
        }
        child = child->Next;
        depth++;
    }
    return fields;
}

// ---- 收集展开字段 (含继承) ----
std::vector<UE4Dumper::FieldInfo> UE4Dumper::collectExpandedFields(uintptr_t typePtr) {
    auto hierarchy = buildTypeHierarchy(typePtr);
    std::vector<FieldInfo> fields;
    for (auto& h : hierarchy) {
        auto declared = collectDeclaredFields(h.ptr);
        fields.insert(fields.end(), declared.begin(), declared.end());
    }
    return fields;
}

// ---- 收集声明函数 ----
std::vector<UE4Dumper::FuncInfo> UE4Dumper::collectDeclaredFunctions(uintptr_t typePtr) {
    std::vector<FuncInfo> functions;
    std::string ownerName = readObjectFName(typePtr);

    auto* structPtr = reinterpret_cast<UStruct*>(typePtr);
    auto* child = reinterpret_cast<UField*>(structPtr->Children);
    int depth = 0;
    while (child != nullptr && depth < 5000) {
        if (readClassName(reinterpret_cast<uintptr_t>(child)) == "Function") {
            auto* func = reinterpret_cast<ue4::UFunction*>(child);
            FuncInfo fi;
            fi.ownerName = ownerName;
            fi.funcName  = readObjectFName(reinterpret_cast<uintptr_t>(child));
            fi.funcFlags = func->FunctionFlags;
            fi.numParms  = func->NumParms;
            fi.retType   = "void";

            // 遍历函数参数
            auto* fparam = reinterpret_cast<UField*>(func->Children);
            int pd = 0;
            while (fparam != nullptr && pd < 100) {
                std::string pc = readClassName(reinterpret_cast<uintptr_t>(fparam));
                if (pc.size() > 8 && pc.substr(pc.size() - 8) == "Property") {
                    auto* paramProp = reinterpret_cast<UProperty*>(fparam);
                    std::string pn = readObjectFName(reinterpret_cast<uintptr_t>(fparam));
                    const char* mapped = getPropTypeName(pc);
                    std::string pt = mapped ? mapped : pc;

                    // 如果是枚举属性, 替换为实际枚举类型名
                    std::string enumPath = getBoundEnumPath(reinterpret_cast<uintptr_t>(fparam));
                    if (!enumPath.empty()) {
                        auto dot = enumPath.rfind('.');
                        pt = (dot != std::string::npos) ? enumPath.substr(dot + 1) : enumPath;
                    }

                    uint32_t pfLo = static_cast<uint32_t>(paramProp->PropertyFlags & 0xFFFFFFFF);
                    if (pfLo & 0x400) {
                        fi.retType = pt;
                    } else if (pfLo & 0x80) {
                        std::string param;
                        if (pfLo & 0x100) param = "out ";
                        param += pt + " " + pn;
                        fi.params.push_back(param);
                    }
                }
                fparam = fparam->Next;
                pd++;
            }

            fi.funcPtr = reinterpret_cast<uintptr_t>(func->Func);
            if (fi.funcPtr != 0 && isModulePtr(fi.funcPtr)) {
                fi.locationSuffix = " // [Offset: " + getModuleOffsetText(fi.funcPtr) + "]";
            } else if (fi.funcPtr != 0) {
                char buf[32];
                snprintf(buf, sizeof(buf), " // [Addr: 0x%lX]", (unsigned long)fi.funcPtr);
                fi.locationSuffix = buf;
            }

            functions.push_back(fi);
        }
        child = child->Next;
        depth++;
    }
    return functions;
}

// ---- 收集展开函数 (含继承) ----
std::vector<UE4Dumper::FuncInfo> UE4Dumper::collectExpandedFunctions(uintptr_t typePtr) {
    auto hierarchy = buildTypeHierarchy(typePtr);
    std::vector<FuncInfo> functions;
    for (auto& h : hierarchy) {
        auto declared = collectDeclaredFunctions(h.ptr);
        functions.insert(functions.end(), declared.begin(), declared.end());
    }
    return functions;
}

// ---- 枚举值 dump ----
// UEnum.Names = TArray<TPair<FName, int64>>
// TPair<FName(8), int64(8)> = 16 bytes per entry
void UE4Dumper::dumpEnumValues(uintptr_t objPtr, FILE* fp) {
    auto* uenum = reinterpret_cast<ue4::UEnum*>(objPtr);
    auto& names = uenum->Names;

    if (names.Data == nullptr || names.Num <= 0 || names.Num > 10000) {
        fprintf(fp, "\t// (no enum values)\n");
        return;
    }

    for (int i = 0; i < names.Num; i++) {
        auto& pair = names.Data[i];
        std::string enumName = fnameToString(pair.Name.ComparisonIndex, pair.Name.Number);
        fprintf(fp, "\t%s = %d,\n", enumName.c_str(), (int)pair.Value);
    }
}

// ---- 构建 UFunction 地址→名称映射 (用于 vtable 反查) ----
void UE4Dumper::buildNativeFuncMap(const std::vector<uintptr_t>& targets) {
    m_nativeFuncMap.clear();
    installSafeReadGuard();
    for (auto typePtr : targets) {
        if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
            s_safeReadActive = 0;
            continue; // 跳过导致崩溃的类型
        }
        s_safeReadActive = 1;
        auto funcs = collectDeclaredFunctions(typePtr);
        s_safeReadActive = 0;
        for (auto& fi : funcs) {
            if (fi.funcPtr != 0 && isModulePtr(fi.funcPtr)) {
                m_nativeFuncMap[fi.funcPtr] = fi.ownerName + "::" + fi.funcName;
            }
        }
    }
}

std::string UE4Dumper::lookupVTableFuncName(uintptr_t funcAddr) const {
    auto it = m_nativeFuncMap.find(funcAddr);
    if (it != m_nativeFuncMap.end()) return it->second;
    return "";
}

// ---- 查找 ClassDefaultObject (CDO) ----
uintptr_t UE4Dumper::findClassDefaultObject(uintptr_t classPtr) {
    constexpr int scanStart = 0x28;
    constexpr int scanEnd   = 0x400;
    constexpr uint32_t RF_CDO = 0x10;

    for (int off = scanStart; off < scanEnd; off += sizeof(uintptr_t)) {
        uintptr_t candidate = safeReadPtr(classPtr + off);
        if (candidate == 0) continue;

        uint32_t flags = safeReadU32(candidate + offsetof(UObjectBase, ObjectFlags));
        if ((flags & RF_CDO) == 0) continue;

        uintptr_t objClass = safeReadPtr(candidate + offsetof(UObjectBase, ClassPrivate));
        if (objClass != classPtr) continue;

        return candidate;
    }
    return 0;
}

// ---- 虚函数表 dump ----
void UE4Dumper::dumpClassVTable(uintptr_t classPtr, uintptr_t superPtr, FILE* fp) {
    uintptr_t cdo = findClassDefaultObject(classPtr);
    if (cdo == 0) return;

    uintptr_t vtable = safeReadPtr(cdo);
    if (vtable == 0) return;

    uintptr_t superVTable = 0;
    if (superPtr != 0) {
        uintptr_t superCdo = findClassDefaultObject(superPtr);
        if (superCdo != 0) {
            superVTable = safeReadPtr(superCdo);
        }
    }

    auto hierarchy = buildTypeHierarchy(classPtr);
    std::string inheritText;
    for (size_t i = 0; i < hierarchy.size(); i++) {
        if (i > 0) inheritText += " -> ";
        inheritText += hierarchy[i].name;
    }

    fprintf(fp, "\n\t// C++ VTable (diff vs parent, via CDO)\n");
    fprintf(fp, "\t// Inheritance: %s\n", inheritText.c_str());
    fprintf(fp, "\t// CDO: 0x%lX  VTable: 0x%lX\n",
            (unsigned long)cdo, (unsigned long)vtable);

    std::string className = readObjectFName(classPtr);
    bool sawModuleEntry = false;
    int invalidRun = 0;
    int dumpedCount = 0;

    for (int slot = 0; slot < VTABLE_MAX_SLOTS; slot++) {
        uintptr_t slotOff = slot * sizeof(uintptr_t);

        // 每个 slot 独立保护, 防止单个坏指针终止整个 vtable dump
        if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
            s_safeReadActive = 0;
            if (sawModuleEntry) { invalidRun++; if (invalidRun >= VTABLE_STOP_AFTER_INVALID) break; }
            continue;
        }
        s_safeReadActive = 1;

        uintptr_t target = safeReadPtr(vtable + slotOff);

        if (target == 0 || !isModulePtr(target)) {
            if (sawModuleEntry) {
                invalidRun++;
                if (invalidRun >= VTABLE_STOP_AFTER_INVALID) break;
            }
            continue;
        }

        sawModuleEntry = true;
        invalidRun = 0;

        if (superVTable != 0) {
            uintptr_t superTarget = safeReadPtr(superVTable + slotOff);
            if (superTarget == target) continue;
        }

        std::string implClass = className;
        for (int i = (int)hierarchy.size() - 2; i >= 0; i--) {
            uintptr_t ancestorCdo = findClassDefaultObject(hierarchy[i].ptr);
            if (ancestorCdo == 0) continue;
            uintptr_t ancestorVt = safeReadPtr(ancestorCdo);
            if (ancestorVt == 0) continue;
            uintptr_t ancestorTarget = safeReadPtr(ancestorVt + slotOff);
            if (ancestorTarget == target) {
                implClass = hierarchy[i].name;
            } else {
                break;
            }
        }

        bool inherited = (implClass != className);
        std::string offsetText = getModuleOffsetText(target);

        // 尝试通过 UFunction::Func 反查函数名
        std::string funcName = lookupVTableFuncName(target);
        if (!funcName.empty()) {
            // 有 UFunction 匹配, 使用真实函数名
            fprintf(fp, "\tvirtual void %s(); // [Slot: 0x%lX] [Offset: %s] [%s]\n",
                    funcName.c_str(),
                    (unsigned long)slotOff,
                    offsetText.c_str(),
                    inherited ? "inherited" : "override");
        } else {
            // 无匹配, 使用 sub_类名_偏移 命名
            fprintf(fp, "\tvirtual void sub_%s_%s(); // [Slot: 0x%lX] [%s]\n",
                    implClass.c_str(),
                    offsetText.c_str(),
                    (unsigned long)slotOff,
                    inherited ? "inherited" : "override");
        }

        dumpedCount++;
        s_safeReadActive = 0;
    }

    if (dumpedCount == 0) {
        fprintf(fp, "\t// (no vtable overrides)\n");
    }
}

// ---- 单个类型 dump ----
void UE4Dumper::dumpType(uintptr_t objPtr, FILE* fp) {
    std::string typeCN = readClassName(objPtr);

    bool isClass  = (typeCN == "Class");
    bool isStruct = (typeCN == "ScriptStruct");
    bool isEnum   = (typeCN == "UserDefinedEnum" || typeCN == "Enum");

    if (!isClass && !isStruct && !isEnum) return;

    std::string objName = readObjectFName(objPtr);
    std::string pkg     = getPackageName(objPtr);
    std::string outer   = outerChain(objPtr);

    if (isEnum) {
        fprintf(fp, "// Enum %s.%s\n", outer.c_str(), objName.c_str());
        fprintf(fp, "enum %s {\n", objName.c_str());
        dumpEnumValues(objPtr, fp);
        fprintf(fp, "};\n\n");
        return;
    }

    auto* structObj = reinterpret_cast<UStruct*>(objPtr);
    std::string superName = (structObj->SuperStruct == nullptr) ? "" : readObjectFName(reinterpret_cast<uintptr_t>(structObj->SuperStruct));
    int propSize = structObj->PropertiesSize;

    fprintf(fp, "// %s %s.%s\n", isClass ? "Class" : "ScriptStruct",
            pkg.c_str(), objName.c_str());
    fprintf(fp, "// Size: 0x%X\n", (unsigned)propSize);

    if (superName.empty()) {
        fprintf(fp, "%s %s\n{\n", isClass ? "class" : "struct", objName.c_str());
    } else {
        fprintf(fp, "%s %s : public %s\n{\n",
                isClass ? "class" : "struct", objName.c_str(), superName.c_str());
    }

    // 字段
    auto fields = collectExpandedFields(objPtr);
    if (!fields.empty()) {
        fprintf(fp, "\t// Fields (Expanded Inheritance)\n");
        for (auto& f : fields) {
            fprintf(fp, "\t%s %s", f.typeName.c_str(), f.propName.c_str());
            if (f.arrayDim > 1) fprintf(fp, "[%d]", f.arrayDim);
            fprintf(fp, "; // 0x%X (Size: 0x%X)", (unsigned)f.offset, (unsigned)f.elemSize);
            if (!f.enumPath.empty()) fprintf(fp, " [UEnum: %s]", f.enumPath.c_str());
            if (f.repIndex != 0xFFFF) fprintf(fp, " [RepIndex: %u]", f.repIndex);
            if (!f.repNotifyFunc.empty() && f.repNotifyFunc != "None" && f.repNotifyFunc != "<invalid>")
                fprintf(fp, " [RepNotify: %s]", f.repNotifyFunc.c_str());
            fprintf(fp, " [Owner: %s]\n", f.ownerName.c_str());
        }
    }

    // 虚函数表 (仅 class)
    if (isClass) {
        dumpClassVTable(objPtr, reinterpret_cast<uintptr_t>(structObj->SuperStruct), fp);
    }

    // 函数
    auto funcs = collectExpandedFunctions(objPtr);
    if (!funcs.empty()) {
        fprintf(fp, "\n\t// Functions (Expanded Inheritance)\n");
        for (auto& fn : funcs) {
            // flags
            std::string flagsText;
            if (fn.funcFlags & 0x00000001) flagsText += "Final|";
            if (fn.funcFlags & 0x00000002) flagsText += "Static|";
            if (fn.funcFlags & 0x00000400) flagsText += "Native|";
            if (fn.funcFlags & 0x00000800) flagsText += "Event|";
            if (fn.funcFlags & 0x00002000) flagsText += "NetMulticast|";
            if (fn.funcFlags & 0x00020000) flagsText += "Net|";
            if (fn.funcFlags & 0x00200000) flagsText += "BlueprintCallable|";
            if (fn.funcFlags & 0x00400000) flagsText += "BlueprintEvent|";
            if (fn.funcFlags & 0x04000000) flagsText += "Exec|";
            if (!flagsText.empty()) flagsText.pop_back(); // 去掉末尾 '|'
            else flagsText = "None";
            flagsText += " [Owner: " + fn.ownerName + "]";

            fprintf(fp, "\t// Flags: %s\n", flagsText.c_str());

            // 参数列表
            std::string paramStr;
            for (size_t i = 0; i < fn.params.size(); i++) {
                if (i > 0) paramStr += ", ";
                paramStr += fn.params[i];
            }
            fprintf(fp, "\t%s %s(%s);%s // NumParms: %d\n",
                    fn.retType.c_str(), fn.funcName.c_str(),
                    paramStr.c_str(), fn.locationSuffix.c_str(), fn.numParms);
        }
    }

    fprintf(fp, "};\n\n");
}

// ---- SDK Dump 入口 ----
bool UE4Dumper::dumpSDK(const char* filePath) {
    if (!m_initialized) return false;

    std::string path;
    if (filePath != nullptr) {
        path = filePath;
    } else {
        path = getEffectiveOutputDir() + "dump.cs";
    }

    ensureDir(getEffectiveOutputDir().c_str());

    // 收集所有 Class / ScriptStruct / Enum
    struct CollectCtx {
        UE4Dumper* dumper;
        std::vector<uintptr_t> targets;
    };
    CollectCtx cctx;
    cctx.dumper = this;

    forEachUObject(m_GUObjectArray, [](uintptr_t objPtr, int, void* ud) {
        auto* ctx = reinterpret_cast<CollectCtx*>(ud);
        std::string c = ctx->dumper->readClassName(objPtr);
        if (c == "Class" || c == "ScriptStruct" || c == "Enum" || c == "UserDefinedEnum") {
            ctx->targets.push_back(objPtr);
        }
    }, &cctx);

    // 构建 UFunction 地址→名称映射 (用于 vtable 虚函数名反查)
    buildNativeFuncMap(cctx.targets);

    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return false;

    fprintf(fp, "// UE4 SDK Dump\n");
    fprintf(fp, "// Generated by UE4Dumper (C++)\n");
    fprintf(fp, "// Target: com.tencent.tmgp.pubgmhd\n");
    fprintf(fp, "// Includes: expanded inherited reflected fields/functions, UFunction addresses\n");
    fprintf(fp, "// Total: %zu types\n\n", cctx.targets.size());

    for (auto& objPtr : cctx.targets) {
        if (sigsetjmp(s_safeReadJmpBuf, 1) != 0) {
            s_safeReadActive = 0;
            fprintf(fp, "}; // CRASHED\n\n");
            continue;
        }
        s_safeReadActive = 1;
        dumpType(objPtr, fp);
        s_safeReadActive = 0;
    }

    fclose(fp);
    return true;
}

} // namespace ue4

OBFU_ATTRS_END
