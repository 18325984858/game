#ifndef UE5_DFM_STRUCT_H
#define UE5_DFM_STRUCT_H

#include <cstdint>

// =====================================================================
//  UE 5.4 完整结构体布局 — ARM64 Android (DFM)
//  Source: sdk_dump.js (Frida) + IDA 反编译验证 (libUE4.so)
//         + UE 5.4 Engine 源码 (D:\CG035\UE5.4\Engine\Source\...)
//  Target: com.tencent.tmgp.dfm
//
//  腾讯定制版 UE 5.4:
//    - NamePool (非 GNames): base +0x1A343A00, blocks at +0x38
//    - NamePool obfuscation: NOT+XOR (length-dependent mask, 9 cases)
//    - GUObjectArray: numElements +0x1A36A75C, chunks +0x1A36A768
//    - GWorld: +0x1A65ECC8
//    - UObject: ClassPrivate +0x08, OuterPrivate +0x10, FName +0x1C, InternalIndex +0x24
//    - UStruct: PropertiesSize +0x3C, SuperStruct +0x40, Children +0x50, ChildProperties +0x68
//    - UField: Next +0x28
//    - FField: Owner +0x08, Next +0x18, ClassPrivate(FFieldClass*) +0x20, FName +0x28
//    - FFieldClass: FName +0x00
//    - FProperty: ArrayDim +0x34, ElementSize +0x3C, PropertyFlags +0x40, Offset +0x4C, SubTypePtr +0x88
//    - UFunction: FunctionFlags +0xB8, NativeFunc +0xD8
//    - UEnum: Names TArray at SuperStruct+0x40 即 EnumPtr+0x40, element stride 16
//
//  所有偏移均通过 Frida runtime + IDA 验证
// =====================================================================

namespace ue5dfm {

// =====================================================================
//  基础容器 (简化声明, 用于结构体内存布局占位)
// =====================================================================

template<typename T>
struct TArray {
    T*      Data;       // +0x00
    int32_t Num;        // +0x08
    int32_t Max;        // +0x0C
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(TArray<void*>) == 0x10, "TArray size mismatch");
#endif

// =====================================================================
//  FName — 名称标识 (8 bytes)
//  UE5.4: FNameEntryId ComparisonIndex + uint32 Number
//  Frida 验证: UObject +0x1C = ComparisonIndex
// =====================================================================
struct FName {
    int32_t ComparisonIndex;    // +0x00  NamePool 索引
    int32_t Number;             // +0x04  实例编号 (0=无后缀)
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FName) == 0x08, "FName size mismatch");
#endif

// =====================================================================
//  FString — 动态宽字符串 (TArray<TCHAR>)
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
//  FRotator — 旋转角 (Pitch/Yaw/Roll, 3 × float = 12 bytes)
//  UE5.4 Source: Runtime/Core/Public/Math/Rotator.h
// =====================================================================
struct FRotator {
    float Pitch;        // +0x00
    float Yaw;          // +0x04
    float Roll;         // +0x08
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FRotator) == 0x0C, "FRotator size mismatch");
#endif

// =====================================================================
//  FVector — 三维向量 (3 × float = 12 bytes, 非 LWC 版本)
//  注: UE5 LargeWorldCoordinates 使用 double, 但 DFM 手游可能仍为 float
// =====================================================================
struct FVector {
    float X;            // +0x00
    float Y;            // +0x04
    float Z;            // +0x08
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FVector) == 0x0C, "FVector size mismatch");
#endif

// =====================================================================
//  FTransform — 变换 (Rotation + Translation + Scale)
//  UE5.4 Source: Runtime/Core/Public/Math/TransformNonVectorized.h
//  内存布局 (float 版):
//    +0x00: FQuat Rotation (4 × float = 16 bytes)
//    +0x10: FVector Translation (3 × float = 12 bytes)
//    +0x1C: pad (4 bytes)
//    +0x20: FVector Scale3D (3 × float = 12 bytes)
//    +0x2C: pad (4 bytes)
//  Total: 0x30 = 48 bytes
// =====================================================================
struct FTransform {
    float RotationX, RotationY, RotationZ, RotationW; // +0x00 Quat
    float TranslationX;  // +0x10
    float TranslationY;  // +0x14
    float TranslationZ;  // +0x18
    float _Pad1C;        // +0x1C
    float Scale3DX;      // +0x20
    float Scale3DY;      // +0x24
    float Scale3DZ;      // +0x28
    float _Pad2C;        // +0x2C
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FTransform) == 0x30, "FTransform size mismatch");
static_assert(offsetof(FTransform, TranslationX) == 0x10, "FTransform::TranslationX offset mismatch");
#endif

// =====================================================================
//  InventoryItemInfo — 箱内物品信息 (Size: 0x690, SDK 确认)
//  DFMCommonItemRow 内 ItemID/Count/Durability 字段
// =====================================================================
struct InventoryItemInfo {
    uint8_t  _Pad00[0x10];       // +0x00
    uint32_t ItemCategory;       // +0x10  ItemID.Category
    uint32_t ItemSequence;       // +0x14  ItemID.Sequence
    uint8_t  _Pad18[0x20];      // +0x18
    int32_t  ItemCount;          // +0x38  数量
    int32_t  ItemNumMax;         // +0x3C  最大数量
    float    ItemDurability;     // +0x40  耐久
    float    ItemDurabilityMax;  // +0x44  最大耐久
    uint8_t  _Pad48[0x648];     // +0x48 ... 到 0x690
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(InventoryItemInfo) == 0x690, "InventoryItemInfo size mismatch");
static_assert(offsetof(InventoryItemInfo, ItemCategory) == 0x10, "InventoryItemInfo::ItemCategory offset mismatch");
static_assert(offsetof(InventoryItemInfo, ItemCount) == 0x38, "InventoryItemInfo::ItemCount offset mismatch");
static_assert(offsetof(InventoryItemInfo, ItemDurability) == 0x40, "InventoryItemInfo::ItemDurability offset mismatch");
#endif

// =====================================================================
//  UObjectBase — UE5.4 所有 UObject 的基类 (0x28 = 40 bytes)
//  腾讯定制版布局 (与 Epic 标准版略有不同):
//    +0x00: VTablePtr (腾讯版保留)
//    +0x08: ClassPrivate
//    +0x10: OuterPrivate
//    +0x18: ObjectFlags + InternalIndex
//    +0x1C: NamePrivate (FName)
//    +0x24: InternalIndex
//  Frida 验证: Class=+0x08, Outer=+0x10, FName=+0x1C, Index=+0x24
// =====================================================================
struct UObjectBase {
    void*    VTablePtr;         // +0x00  虚函数表指针
    void*    ClassPrivate;      // +0x08  UClass*
    void*    OuterPrivate;      // +0x10  UObject* 外部对象(包)
    int32_t  ObjectFlags;       // +0x18  EObjectFlags
    FName    NamePrivate;       // +0x1C  对象名称
    int32_t  InternalIndex;     // +0x24  GUObjectArray 中的索引
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UObjectBase) == 0x28, "UObjectBase size mismatch");
#endif

// =====================================================================
//  FUObjectItem — GUObjectArray 中的单个元素 (0x18 = 24 bytes)
//  UE5.4 Source: Runtime/CoreUObject/Public/UObject/UObjectArray.h
//  Frida 验证: stride = 0x18
// =====================================================================
struct FUObjectItem {
    UObjectBase* Object;        // +0x00  对象指针
    int32_t      Flags;         // +0x08  内部标志 (atomic)
    int32_t      ClusterRootIndex; // +0x0C  集群根索引
    int32_t      SerialNumber;  // +0x10  弱引用序列号
    int32_t      _Padding;      // +0x14  对齐填充
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FUObjectItem) == 0x18, "FUObjectItem size mismatch");
#endif

// =====================================================================
//  UField — 反射字段基类 (0x30 = 48 bytes)
//  继承: UObjectBase(0x28) + Next
//  Frida 验证: Next = +0x28
// =====================================================================
struct UField : UObjectBase {
    UField*  Next;              // +0x28  链表下一个字段
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UField) == 0x30, "UField size mismatch");
#endif

// =====================================================================
//  UStruct — 结构体/类反射基类 (0x80 = 128 bytes)
//  继承: UField(0x30) -> UStruct
//  Frida 验证:
//    PropertiesSize = +0x3C, SuperStruct = +0x40
//    Children = +0x50, ChildProperties = +0x68
//
//  腾讯定制布局 (UField +0x30 起算 → UStruct 绝对偏移):
//    +0x30: _Pad30[0x0C]  (可能含 Script 等内部数据)
//    +0x3C: PropertiesSize
//    +0x40: SuperStruct
//    +0x48: MinAlignment / _Pad
//    +0x50: Children (UField*)
//    +0x58: _Pad58[0x10] (Script TArray 等)
//    +0x68: ChildProperties (FField*)
//    +0x70: PropertyLink / RefLink / DestructorLink / PostConstructLink
// =====================================================================
struct UStruct : UField {
    uint8_t  _Pad30[0x0C];             // +0x30  内部数据 (Script 等)
    int32_t  PropertiesSize;            // +0x3C  所有属性总大小
    void*    SuperStruct;               // +0x40  UStruct* 父结构体
    uint8_t  _Pad48[0x08];             // +0x48  MinAlignment 等
    void*    Children;                  // +0x50  UField* Children 链表头
    uint8_t  _Pad58[0x10];             // +0x58  Script TArray 等
    void*    ChildProperties;           // +0x68  FField* ChildProperties 链表头
    void*    PropertyLink;              // +0x70  FProperty* 属性链表
    void*    RefLink;                   // +0x78  FProperty* 引用链表
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(UStruct) == 0x80, "UStruct size mismatch");
#endif

// =====================================================================
//  FFieldClass — FField 类型信息 (0x40 = 64 bytes)
//  UE5.4 Source: Runtime/CoreUObject/Public/UObject/Field.h
//  Frida 验证: FName = +0x00
// =====================================================================
struct FFieldClass {
    FName    Name;              // +0x00  类型名 (如 "StructProperty")
    uint64_t Id;                // +0x08  类型 ID
    uint64_t CastFlags;         // +0x10  类型转换标志
    uint32_t ClassFlags;        // +0x18  EClassFlags
    uint32_t _Pad1C;            // +0x1C
    void*    SuperClass;        // +0x20  FFieldClass* 父类
    void*    DefaultObject;     // +0x28  FField* 默认对象
    void*    ConstructFn;       // +0x30  构造函数指针
    int32_t  UniqueNameCounter; // +0x38  唯一名称计数器
    uint32_t _Pad3C;            // +0x3C
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FFieldClass) == 0x40, "FFieldClass size mismatch");
#endif

// =====================================================================
//  FField — UE5 反射字段基类 (replaces UProperty 继承链)
//  UE5.4 Source: Runtime/CoreUObject/Public/UObject/Field.h
//  Frida 验证:
//    Owner = +0x08, Next = +0x18, ClassPrivate = +0x20, FName = +0x28
//
//  腾讯定制布局 (比标准 Epic 偏移有差异):
//    +0x00: VTablePtr
//    +0x08: Owner (FFieldVariant, 即 void*)
//    +0x10: _Pad (flags 等)
//    +0x18: Next (FField*)
//    +0x20: ClassPrivate (FFieldClass*)
//    +0x28: NamePrivate (FName)
//    +0x30: FlagsPrivate + padding
// =====================================================================
struct FField {
    void*       VTablePtr;      // +0x00  虚函数表指针
    void*       Owner;          // +0x08  FFieldVariant (UObject* 或 FField*)
    uint8_t     _Pad10[0x08];   // +0x10  flags 等内部数据
    FField*     Next;           // +0x18  链表下一个
    FFieldClass* ClassPrivate;  // +0x20  FFieldClass* 类型信息
    FName       NamePrivate;    // +0x28  字段名称
    uint32_t    FlagsPrivate;   // +0x30  EObjectFlags
    uint32_t    _Pad34;         // +0x34
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FField) == 0x38, "FField size mismatch");
// 注: FField 末尾到 FProperty 起始可能有额外 padding, 由 FProperty 的 _PrefixPad 覆盖
#endif

// =====================================================================
//  FProperty — UE5 属性反射 (替代 UE4 的 UProperty)
//  继承: FField(0x38) -> FProperty
//  Frida 验证 (绝对偏移, 即 从 FField* 起算):
//    ArrayDim       = +0x34 → 注意: 这是修正后偏移, 见下方说明
//    ElementSize    = +0x3C
//    PropertyFlags  = +0x40
//    Offset_Internal = +0x4C
//    SubTypePtr     = +0x88
//
//  实际布局: FField 基类结束于 +0x30 (FlagsPrivate),
//  Frida 实测 ArrayDim 在绝对 +0x34 说明 padding 后紧接 FProperty 成员
// =====================================================================
struct FProperty : FField {
    // FField 继承到 +0x34 (_Pad34)
    // Frida 验证 ArrayDim 绝对偏移 = +0x34, 说明与 _Pad34 重叠或紧接
    // 使用 Frida 实测偏移为准:

    // FField 末尾到 +0x34: 已由基类 _Pad34 覆盖
    // 但 Frida 测得 ArrayDim 在 +0x34, 与基类 _Pad34 位置重合
    // 实际上 FField 到此处只有 0x34 bytes, _Pad34 就是 ArrayDim

    // 为了精确匹配 Frida 偏移, 取消继承, 使用绝对布局:
};

/**
 * FProperty 的完整平坦布局 (从 FField* 基地址起算)
 * 所有偏移都是 Frida/IDA 实测绝对偏移
 */
struct FPropertyFlat {
    // ── FField 基类部分 ──
    void*       VTablePtr;          // +0x00
    void*       Owner;              // +0x08  FFieldVariant
    uint8_t     _Pad10[0x08];       // +0x10  flags 等
    void*       Next;               // +0x18  FField* Next
    FFieldClass* ClassPrivate;      // +0x20  FFieldClass*
    FName       NamePrivate;        // +0x28  字段名称
    uint32_t    FlagsPrivate;       // +0x30  EObjectFlags

    // ── FProperty 成员部分 ──
    int32_t     ArrayDim;           // +0x34  数组维度
    uint8_t     _Pad38[0x04];       // +0x38  RepIndex 等
    int32_t     ElementSize;        // +0x3C  每个元素大小
    uint32_t    PropertyFlags;      // +0x40  EPropertyFlags (低 32 位)
    uint8_t     _Pad44[0x08];       // +0x44  RepNotifyFunc 等
    int32_t     Offset_Internal;    // +0x4C  容器内偏移
    uint8_t     _Pad50[0x38];       // +0x50  PropertyLink / RefLink / 其他
    void*       SubTypePtr;         // +0x88  UObject* (struct/class/enum 的类型引用)
};
#if __SIZEOF_POINTER__ == 8
static_assert(offsetof(FPropertyFlat, ArrayDim) == 0x34, "FPropertyFlat::ArrayDim offset mismatch");
static_assert(offsetof(FPropertyFlat, ElementSize) == 0x3C, "FPropertyFlat::ElementSize offset mismatch");
static_assert(offsetof(FPropertyFlat, PropertyFlags) == 0x40, "FPropertyFlat::PropertyFlags offset mismatch");
static_assert(offsetof(FPropertyFlat, Offset_Internal) == 0x4C, "FPropertyFlat::Offset_Internal offset mismatch");
static_assert(offsetof(FPropertyFlat, SubTypePtr) == 0x88, "FPropertyFlat::SubTypePtr offset mismatch");
#endif

// =====================================================================
//  FEnumNamePair — 枚举名值对 (16 bytes)
//  UE5.4: TPair<FName, int64>
// =====================================================================
struct FEnumNamePair {
    FName    Name;              // +0x00
    int64_t  Value;             // +0x08
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FEnumNamePair) == 0x10, "FEnumNamePair size mismatch");
#endif

// =====================================================================
//  UEnum — 枚举反射
//  继承: UField(0x30) -> UEnum
//  Frida 验证: Names TArray Data = +0x40, Num = +0x48, stride = 16
//
//  布局: UField(0x30) + _Pad[0x10] + Names TArray
// =====================================================================
struct UEnum : UField {
    uint8_t  _Pad30[0x10];                // +0x30  CppType FString 等
    TArray<FEnumNamePair> Names;           // +0x40  枚举名/值对
    // +0x40: Data*, +0x48: Num, +0x4C: Max
};
#if __SIZEOF_POINTER__ == 8
static_assert(offsetof(UEnum, Names) == 0x40, "UEnum::Names offset mismatch");
#endif

// =====================================================================
//  UFunction — 函数反射
//  继承: UStruct(0x80) -> UFunction
//  Frida 验证:
//    FunctionFlags = +0xB8
//    NativeFunc    = +0xD8
//
//  布局: UStruct(0x80) + 0x38 pad + FunctionFlags + ... + NativeFunc
// =====================================================================
struct UFunction : UStruct {
    uint8_t  _Pad80[0x38];             // +0x80  DestructorLink, PostConstructLink, ScriptRefs 等
    uint32_t FunctionFlags;             // +0xB8  EFunctionFlags
    uint8_t  NumParms;                  // +0xBC  参数数量
    uint8_t  _PadBD;                    // +0xBD
    uint16_t ParmsSize;                 // +0xBE  参数总大小
    uint16_t ReturnValueOffset;         // +0xC0  返回值偏移
    uint16_t RPCId;                     // +0xC2  RPC 函数 ID
    uint16_t RPCResponseId;             // +0xC4  RPC 响应 ID
    uint8_t  _PadC6[0x02];             // +0xC6
    void*    FirstPropertyToInit;       // +0xC8  FProperty*
    void*    EventGraphFunction;        // +0xD0  UFunction*
    void*    Func;                      // +0xD8  FNativeFuncPtr (C++ 函数指针)
};
#if __SIZEOF_POINTER__ == 8
static_assert(offsetof(UFunction, FunctionFlags) == 0xB8, "UFunction::FunctionFlags offset mismatch");
static_assert(offsetof(UFunction, Func) == 0xD8, "UFunction::Func offset mismatch");
#endif

// =====================================================================
//  NamePool — 全局名称池 (UE5 特有, 替代 UE4 GNames)
//  UE5.4 Source: Runtime/Core/Public/UObject/NameTypes.h
//               Runtime/Core/Private/UObject/UnrealNames.cpp
//  Frida 验证:
//    base offset = +0x1A343A00
//    blocks 数组起始 = base + 0x38
//    block size = 0x10000
//    offset bits = 18
// =====================================================================



// =====================================================================
//  FNameEntryHeader — 名称条目头 (2 bytes)
//  UE5.4 Source: NameTypes.h
//  DFM 配置: WITHOUT_CASE_PRESERVING_NAME, ProbeHashBits=5, Len=10
// =====================================================================
struct FNameEntryHeader {
    uint16_t Raw;               // +0x00  packed: bIsWide(1) | LowercaseProbeHash(5) | Len(10)

    bool     isWide() const;
    uint16_t getLen() const;
    uint16_t getProbeHash() const;
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FNameEntryHeader) == 0x02, "FNameEntryHeader size mismatch");
#endif

// =====================================================================
//  FNameEntry — 名称条目 (变长, 按 Stride=2 对齐)
//  UE5.4 Source: NameTypes.h
//  DFM (WITHOUT_CASE_PRESERVING_NAME): 无 ComparisonId, Header 在 +0x00
//
//  内存布局:
//    +0x00: FNameEntryHeader  (2 bytes)
//    +0x02: AnsiName[] 或 WideName[]  (变长)
//
//  注意: 腾讯版 NamePool 使用 NOT+XOR 混淆, 读取后需要解码
// =====================================================================
struct FNameEntry {
    FNameEntryHeader Header;    // +0x00  名称头
    union {
        char     AnsiName[1022]; // +0x02  ANSI 字符串 (变长)
        wchar_t  WideName[511];  // +0x02  宽字符串 (变长)
    };

    bool isWide() const;
    int  getLen() const;
    const char* getAnsiName() const;
};

// =====================================================================
//  FNameEntryHandle — 名称池句柄 (块索引 + 块内偏移)
//  UE5.4 Source: UnrealNames.cpp
//
//  FNameEntryId -> Handle 拆分:
//    Block  = Id >> OffsetBits
//    Offset = Id & ((1 << OffsetBits) - 1)
//
//  DFM 定制: OffsetBits = 18 (标准 UE5.4 = 16)
// =====================================================================
struct FNameEntryHandle {
    uint32_t Block;             // +0x00  块索引 (0 ~ MaxBlocks-1)
    uint32_t Offset;            // +0x04  块内偏移 (0 ~ (1<<OffsetBits)-1)

    /// 从 FNameEntryId (ComparisonIndex) 构造
    static FNameEntryHandle fromId(uint32_t id, uint32_t offsetBits = 18);
};
#if __SIZEOF_POINTER__ == 8
static_assert(sizeof(FNameEntryHandle) == 0x08, "FNameEntryHandle size mismatch");
#endif

// =====================================================================
//  FNameEntryAllocator — 名称条目分配器
//  UE5.4 Source: UnrealNames.cpp (class FNameEntryAllocator)
//
//  Frida 验证:
//    Blocks[] 起始 = NamePool + 0x38
//    → Lock + CurrentBlock + CurrentByteCursor = 0x38 bytes
//
//  内存布局 (ARM64 Android):
//    +0x00: Lock (FRWLock, pthread_rwlock_t 相关, 48 bytes)
//    +0x30: CurrentBlock       (uint32)
//    +0x34: CurrentByteCursor  (uint32)
//    +0x38: Blocks[MaxBlocks]  (uint8* × 8192 = 65536 bytes)
// =====================================================================
struct FNameEntryAllocator {
    static constexpr uint32_t Stride          = 2;       // alignof(FNameEntry), DFM 实测
    static constexpr uint32_t MaxBlocks       = 8192;    // FNameMaxBlocks = 1 << 13
    static constexpr uint32_t BlockOffsets    = 0x10000; // 1 << 16 = 65536 (每块最大条目数)
    static constexpr uint32_t OffsetBits      = 18;      // DFM 定制 (标准 UE5.4 = 16)
    static constexpr uint32_t BlockSizeBytes  = Stride * BlockOffsets; // 每块字节数

    uint8_t  Lock[0x30];                     // +0x00  FRWLock (平台相关, 48 bytes)
    uint32_t CurrentBlock;                   // +0x30  当前块索引
    uint32_t CurrentByteCursor;              // +0x34  当前块内字节游标
    uint8_t* Blocks[MaxBlocks];              // +0x38  块指针数组 (8192 个指针)
};
#if __SIZEOF_POINTER__ == 8
static_assert(offsetof(FNameEntryAllocator, CurrentBlock) == 0x30,
              "FNameEntryAllocator::CurrentBlock offset mismatch");
static_assert(offsetof(FNameEntryAllocator, CurrentByteCursor) == 0x34,
              "FNameEntryAllocator::CurrentByteCursor offset mismatch");
static_assert(offsetof(FNameEntryAllocator, Blocks) == 0x38,
              "FNameEntryAllocator::Blocks offset mismatch");
#endif

// =====================================================================
//  全局变量偏移 (相对于 libUE4.so 基址, 由外部传入 Dumper 类)
//  仅保留注释, 实际值在运行时由 Injector 传入
//
//  OFF_NAMEPOOL             = 0x1A343A00  NamePool 结构体基地址
//  OFF_GUOBJECTARRAY_NUM    = 0x1A36A75C  GUObjectArray.NumElements
//  OFF_GUOBJECTARRAY_CHUNKS = 0x1A36A768  GUObjectArray.Chunks 指针表
//  OFF_GWORLD               = 0x1A65ECC8  GWorld 指针
// =====================================================================


// =====================================================================
//  EFunctionFlags — 函数标志位 (UE5.4)
// =====================================================================
enum EFunctionFlags : uint32_t {
    FUNC_None               = 0x00000000,
    FUNC_Final              = 0x00000001,
    FUNC_Net                = 0x00000002,
    FUNC_Exec               = 0x00000200,
    FUNC_Native             = 0x00000400,
    FUNC_Event              = 0x00000800,
    FUNC_Static             = 0x00002000,
    FUNC_BlueprintCallable  = 0x04000000,
    FUNC_BlueprintEvent     = 0x08000000,
    FUNC_BlueprintPure      = 0x10000000,
};

// =====================================================================
//  EPropertyFlags — 属性标志位 (常用, 低 32 位)
// =====================================================================
enum EPropertyFlags : uint32_t {
    CPF_None             = 0x00000000,
    CPF_Parm             = 0x00000080,
    CPF_OutParm          = 0x00000100,
    CPF_ConstParm        = 0x00000200,
    CPF_ReturnParm       = 0x00000400,
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

} // namespace ue5dfm

#endif // UE5_DFM_STRUCT_H
