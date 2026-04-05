#ifndef ILB_UE4_STRUCT_H
#define ILB_UE4_STRUCT_H

#include <cstdint>
#include <pthread.h>

// =====================================================================
//  UE4.18 完整结构体布局 — ARM64 Android (PUBG Mobile / 和平精英)
//  Source: UnrealEngine-4.18 + IDA 反编译验证 (libUE4.so)
//  Target: com.tencent.tmgp.pubgmhd
//
//  腾讯定制版 UE4.18 (CG035/UE4181):
//    - WITHOUT_CASE_PRESERVING_NAME → FName = 8 bytes
//    - FUObjectArray 使用自定义分块版本 (非标准 FFixedUObjectArray)
//    - 路径: D:\CG035\UE4181\Engine\Source\...
//
//  所有成员偏移均通过 IDA MCP 反编译验证
// =====================================================================

namespace ue4 {

// =====================================================================
//  基础容器 (简化声明, 用于结构体内存布局占位)
// =====================================================================

template<typename T>
struct TArray {
    T*      Data;       // +0x00  堆分配数组
    int32_t Num;        // +0x08  当前元素数
    int32_t Max;        // +0x0C  已分配容量
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(TArray<void*>) == 0x10, "TArray size mismatch");
#endif

// =====================================================================
//  FName — 名称标识 (8 bytes)
//  Source: Runtime/Core/Public/UObject/NameTypes.h
// =====================================================================
struct FName {
    int32_t ComparisonIndex;    // +0x00  GNames 索引
    int32_t Number;             // +0x04  实例编号 (0=无后缀, >0 后缀 _N-1)
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FName) == 0x08, "FName size mismatch");
#endif

// =====================================================================
//  FString — 动态宽字符串 (本质是 TArray<TCHAR>)
// =====================================================================
struct FString {
    wchar_t* Data;      // +0x00
    int32_t  Num;       // +0x08
    int32_t  Max;       // +0x0C
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FString) == 0x10, "FString size mismatch");
#endif

// =====================================================================
//  FNameEntry — 名称条目 (变长)
//  IDA 验证: FName::InitInternal_FindOrAddNameEntry
//    strcpy((char*)entry + 12, s) → AnsiName @ +0x0C
//    entry + 8 = Index (bit0=IsWide)
// =====================================================================
struct FNameEntry {
    FNameEntry* HashNext;       // +0x00  哈希链表下一个
    int32_t     Index;          // +0x08  bit0=IsWide
    char        AnsiName[1028]; // +0x0C  ANSI 字符串 (变长, 含对齐)

    const char* getName() const;
    bool isWide() const;
};

// =====================================================================
//  TNameEntryArray — 名称数组 (分块间接数组)
//  IDA 验证: Chunks[640] × 8 = 0x1400 bytes
//    NumElements @ +0x1400, NumChunks @ +0x1404
//    ElementsPerChunk = 16384
// =====================================================================
static constexpr int NAMES_CHUNK_TABLE_SIZE = 640;
static constexpr int NAMES_ELEMENTS_PER_CHUNK = 16384;

struct TNameEntryArray {
    FNameEntry** Chunks[NAMES_CHUNK_TABLE_SIZE]; // +0x0000  chunk 指针表
    int32_t      NumElements;                     // +0x1400  当前元素总数
    int32_t      NumChunks;                       // +0x1404  当前 chunk 数

    FNameEntry* getEntry(int index) const;
};

// =====================================================================
//  UObjectBase — 所有 UObject 的基类 (0x28 = 40 bytes)
//  IDA 验证: struct UObjectBase size=40, cardinality=6
// =====================================================================
struct UObjectBase {
    void*    VTablePtr;         // +0x00  虚函数表指针
    int32_t  ObjectFlags;       // +0x08  EObjectFlags
    int32_t  InternalIndex;     // +0x0C  GUObjectArray 中的索引
    void*    ClassPrivate;      // +0x10  UClass* 所属类
    FName    NamePrivate;       // +0x18  对象名称
    void*    OuterPrivate;      // +0x20  UObject* 外部对象(包)
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UObjectBase) == 0x28, "UObjectBase size mismatch");
#endif

// =====================================================================
//  FUObjectItem — GUObjectArray 中的单个元素 (0x18 = 24 bytes)
//  IDA 验证: struct FUObjectItem size=24
//    遍历: item = chunkBase + 24 * index
// =====================================================================
struct FUObjectItem {
    UObjectBase* Object;        // +0x00  对象指针
    int32_t      Flags;         // +0x08  内部标志
    int32_t      ClusterRootIndex; // +0x0C  集群根索引
    int32_t      SerialNumber;  // +0x10  弱引用序列号
    int32_t      _Padding;      // +0x14  对齐填充
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FUObjectItem) == 0x18, "FUObjectItem size mismatch");
#endif

// =====================================================================
//  FChunkedFUObjectArray — 腾讯定制分块对象数组 (内嵌在 FUObjectArray 中)
//  IDA 验证: sub_A6579EC (AllocateUObjectIndex), sub_A657F94 (FreeUObjectIndex)
//
//    *(void**)(base + 0xC8 + ci * 8)  = ChunkPtrs[ci]
//    *(int*)(base + 0xE8 + ci * 4)    = ChunkElementCounts[ci]
//    *(int*)(base + 0xF8)             = NumChunks
//    *(int*)(base + 0xFC)             = MaxChunks
//    *(int*)(base + 0x100)            = TotalNumElements
//
//  FUObjectArray 前 0xC8 字节 (200 bytes) 包含:
//    +0x00~0x07  内部管理数据
//    +0x08       TArray (sub_56FDFC4 调用)
//    +0x20       TArray (sub_6647888 调用)
//    +0xB8       MaxObjectsNotConsideredByGC (v2+192)
//    +0xBC       ObjLastNonGCIndex (v2+188)
//    +0xC0       bool OpenForDisregardForGC (v2+196)
//    +0xC4       bool _SomeFlag (v2+196 byte)
//
//  遍历方式:
//    for (ci = 0; ci < NumChunks; ci++)
//      chunk = ChunkPtrs[ci]
//      cnt   = ChunkElementCounts[ci]
//      for (wi = 0; wi < cnt; wi++)
//        item = (FUObjectItem*)(chunk + 24 * wi)
// =====================================================================
struct FUObjectArray {
    // +0x00 ~ +0xC7: 前置管理成员 (200 bytes)
    uint8_t  _Head[8];                // +0x00
    TArray<void*> _InternalArray0;    // +0x08  内部数组
    uint8_t  _Pad1C[4];              // +0x1C
    TArray<void*> _InternalArray1;    // +0x20  内部数组
    uint8_t  _Pad30[0x88];           // +0x30  ...各种管理数据...
    int32_t  MaxObjectsNotConsideredByGC; // +0xB8  (v2+184=192? → 实际 +0xC0 - adj)
    int32_t  ObjLastNonGCIndex;       // +0xBC  最后非GC对象索引
    bool     OpenForDisregardForGC;   // +0xC0  标志
    uint8_t  _PadC1[7];              // +0xC1

    // +0xC8 ~ : 核心分块数组
    // 注意: ChunkPtrs 和 ChunkElementCounts 是变长数组,
    //       这里声明为最小尺寸, 实际通过偏移计算访问
    void*    ChunkPtrs[4];            // +0xC8   chunk 指针 (实际数量 = NumChunks)
    int32_t  ChunkElementCounts[4];   // +0xE8   每块元素数
    int32_t  NumChunks;               // +0xF8   当前 chunk 数
    int32_t  MaxChunks;               // +0xFC   最大 chunk 数
    int32_t  TotalNumElements;        // +0x100  对象总数

    // +0x104 之后: 更多管理成员
    // +0x130  _SomeFlag2 (v2+304)
    // +0x138  pthread_mutex_t ObjMutex (v2+312)
    // +0x200  FreeList atomic head (v2+512)
    // +0x280  FreeListCount atomic (v2+640)
    // +0x288  CreateListeners TArray (v2+648)
    // +0x2A0  DeleteListeners TArray (v2+664)

    // 辅助访问方法
    void* getChunkPtr(int ci) const;
    int32_t getChunkCount(int ci) const;
    int32_t getNumChunks() const;
    int32_t getTotalNum() const;
};

// =====================================================================
//  UField — 反射字段基类 (0x30 = 48 bytes)
//  继承: UObjectBase(0x28) + Next(0x08)
// =====================================================================
struct UField : UObjectBase {
    UField*  Next;              // +0x28  链表下一个字段
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UField) == 0x30, "UField size mismatch");
#endif

// =====================================================================
//  UStruct — 结构体/类反射基类 (0x88 = 136 bytes)
//  继承: UField(0x30) -> UStruct
//  IDA 验证: Children @ +0x38, Super @ +0x30, PropertiesSize @ +0x40
// =====================================================================
struct UStruct : UField {
    UStruct*          SuperStruct;       // +0x30  父结构体
    UField*           Children;          // +0x38  Children 链表头
    int32_t           PropertiesSize;    // +0x40  所有属性总大小
    int32_t           MinAlignment;      // +0x44  最小对齐
    TArray<uint8_t>   Script;            // +0x48  字节码
    void*             PropertyLink;      // +0x58  UProperty* 属性链表
    void*             RefLink;           // +0x60  UProperty* 对象引用链表
    void*             DestructorLink;    // +0x68  UProperty* 析构链表
    void*             PostConstructLink; // +0x70  UProperty* 构造后初始化链表
    TArray<void*>     ScriptObjectReferences; // +0x78  脚本对象引用
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UStruct) == 0x88, "UStruct size mismatch");
#endif

// =====================================================================
//  UProperty — 属性反射基类 (0x70 = 112 bytes)
//  继承: UField(0x30) -> UProperty
//  IDA 验证 (sub_C445078):
//    +0x44 Offset_Internal, +0x48 RepNotifyFunc
// =====================================================================
struct UProperty : UField {
    int32_t  ArrayDim;              // +0x30  数组维度
    int32_t  ElementSize;           // +0x34  每个元素大小
    uint64_t PropertyFlags;         // +0x38  EPropertyFlags
    uint16_t RepIndex;              // +0x40  网络复制索引 (0xFFFF=不复制)
    uint8_t  BlueprintReplicationCondition; // +0x42
    uint8_t  _Pad43;                // +0x43
    int32_t  Offset_Internal;       // +0x44  容器内偏移
    FName    RepNotifyFunc;         // +0x48  属性变化通知函数名
    void*    PropertyLinkNext;      // +0x50  UProperty* 属性链表
    void*    NextRef;               // +0x58  UProperty* 对象引用链表
    void*    DestructorLinkNext;    // +0x60  UProperty* 析构链表
    void*    PostConstructLinkNext; // +0x68  UProperty* 构造后初始化链表
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UProperty) == 0x70, "UProperty size mismatch");
#endif

// =====================================================================
//  UNumericProperty — 数值属性基类 (无额外成员)
// =====================================================================
struct UNumericProperty : UProperty {};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UNumericProperty) == 0x70, "UNumericProperty size mismatch");
#endif

// =====================================================================
//  UBoolProperty — 布尔属性 (0x78)
// =====================================================================
struct UBoolProperty : UProperty {
    uint8_t  FieldSize;             // +0x70
    uint8_t  ByteOffset;            // +0x71
    uint8_t  ByteMask;              // +0x72
    uint8_t  FieldMask;             // +0x73
    uint8_t  _Pad74[4];             // +0x74
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UBoolProperty) == 0x78, "UBoolProperty size mismatch");
#endif

// =====================================================================
//  UByteProperty — 字节属性 (0x78)
//  IDA 验证: size = 0x78, Enum @ +0x70
// =====================================================================
struct UByteProperty : UNumericProperty {
    void*    Enum;                  // +0x70  UEnum*
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UByteProperty) == 0x78, "UByteProperty size mismatch");
#endif

// =====================================================================
//  UEnumProperty — 枚举属性 (0x80)
//  IDA 验证: size = 0x80, UnderlyingProp @ +0x70, Enum @ +0x78
// =====================================================================
struct UEnumProperty : UProperty {
    void*    UnderlyingProp;        // +0x70  UNumericProperty*
    void*    Enum;                  // +0x78  UEnum*
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UEnumProperty) == 0x80, "UEnumProperty size mismatch");
#endif

// =====================================================================
//  UObjectPropertyBase — 对象引用属性 (0x78)
// =====================================================================
struct UObjectPropertyBase : UProperty {
    void*    PropertyClass;         // +0x70  UClass*
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UObjectPropertyBase) == 0x78, "UObjectPropertyBase size mismatch");
#endif

// =====================================================================
//  UStructProperty — 结构体属性 (0x78)
// =====================================================================
struct UStructProperty : UProperty {
    void*    Struct;                // +0x70  UScriptStruct*
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UStructProperty) == 0x78, "UStructProperty size mismatch");
#endif

// =====================================================================
//  UArrayProperty — 数组属性 (0x78)
// =====================================================================
struct UArrayProperty : UProperty {
    void*    Inner;                 // +0x70  UProperty*
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UArrayProperty) == 0x78, "UArrayProperty size mismatch");
#endif

// =====================================================================
//  UMapProperty — Map属性 (0x80)
// =====================================================================
struct UMapProperty : UProperty {
    void*    KeyProp;               // +0x70  UProperty*
    void*    ValueProp;             // +0x78  UProperty*
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UMapProperty) == 0x80, "UMapProperty size mismatch");
#endif

// =====================================================================
//  FEnumNamePair — 枚举名值对 (16 bytes)
// =====================================================================
struct FEnumNamePair {
    FName    Name;              // +0x00
    int64_t  Value;             // +0x08
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FEnumNamePair) == 0x10, "FEnumNamePair size mismatch");
#endif

// =====================================================================
//  UEnum — 枚举反射 (0x60)
//  IDA 验证: CppType @ +0x30, Names @ +0x40
// =====================================================================
struct UEnum : UField {
    FString                CppType;  // +0x30  C++ 类型名
    TArray<FEnumNamePair>  Names;    // +0x40  枚举名/值对
    int32_t                CppForm;  // +0x50  ECppForm
    int32_t                _Pad54;   // +0x54
    void*                  EnumDisplayNameFn; // +0x58
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UEnum) == 0x60, "UEnum size mismatch");
#endif

// =====================================================================
//  UFunction — 函数反射 (0xC0)
//  继承: UStruct(0x88) -> UFunction
//  IDA 验证 (sub_A55FF7C):
//    a1[22] = +0xB0 → Func, a1[23] = +0xB8 → PMF adj
// =====================================================================
struct UFunction : UStruct {
    uint32_t FunctionFlags;         // +0x88  EFunctionFlags
    uint8_t  NumParms;              // +0x8C  参数数量
    uint8_t  _Pad8D;                // +0x8D
    uint16_t ParmsSize;             // +0x8E  参数总大小
    uint16_t ReturnValueOffset;     // +0x90  返回值偏移
    uint16_t RPCId;                 // +0x92  RPC 函数 ID
    uint16_t RPCResponseId;         // +0x94  RPC 响应 ID
    uint8_t  _Pad96[2];             // +0x96
    void*    FirstPropertyToInit;   // +0x98  UProperty*
    void*    EventGraphFunction;    // +0xA0  UFunction*
    int32_t  EventGraphCallOffset;  // +0xA8
    int32_t  _PadAC;                // +0xAC
    void*    Func;                  // +0xB0  C++ 函数指针
    int64_t  FuncAdj;               // +0xB8  PMF 调整值
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UFunction) == 0xC0, "UFunction size mismatch");
#endif

// =====================================================================
//  EFunctionFlags — 函数标志位
// =====================================================================
enum EFunctionFlags : uint32_t {
    FUNC_None               = 0x00000000,
    FUNC_Final              = 0x00000001,
    FUNC_Static             = 0x00000002,
    FUNC_Net                = 0x00000040,
    FUNC_Exec               = 0x00000200,
    FUNC_Native             = 0x00000400,
    FUNC_Event              = 0x00000800,
    FUNC_NetMulticast        = 0x00004000,
    FUNC_Public             = 0x00020000,
    FUNC_NetServer          = 0x00200000,
    FUNC_HasOutParms        = 0x00400000,
    FUNC_NetClient          = 0x01000000,
    FUNC_BlueprintCallable  = 0x04000000,
    FUNC_BlueprintEvent     = 0x08000000,
    FUNC_BlueprintPure      = 0x10000000,
    FUNC_Const              = 0x40000000,
};

// =====================================================================
//  EPropertyFlags — 属性标志位 (uint64, 常用部分)
// =====================================================================
enum EPropertyFlags : uint64_t {
    CPF_None             = 0,
    CPF_Parm             = 0x0000000000000080,
    CPF_OutParm          = 0x0000000000000100,
    CPF_ReturnParm       = 0x0000000000000400,
    CPF_Net              = 0x0000000000000020,
    CPF_RepNotify        = 0x0000000100000000,
};

// =====================================================================
//  EObjectFlags — 对象标志位 (常用部分)
// =====================================================================
enum EObjectFlags : uint32_t {
    RF_NoFlags            = 0x00000000,
    RF_Public             = 0x00000001,
    RF_ClassDefaultObject = 0x00000010,
    RF_Transient          = 0x00000040,
};

// =====================================================================
//  全局变量偏移 (相对 libUE4.so 基址)
// =====================================================================
namespace GlobalOffsets {
    constexpr uintptr_t GNames         = 0x146F9F30;
    constexpr uintptr_t GUObjectArray  = 0x14706480;
    constexpr uintptr_t GWorld         = 0x14988578;
}

} // namespace ue4

#endif // ILB_UE4_STRUCT_H
