#ifndef UE4_NRC_STRUCT_H
#define UE4_NRC_STRUCT_H

#include <cstdint>
#include <cstddef>

// =====================================================================
//  UE 4.26 完整结构体布局 — ARM64 Android (洛克王国手游 / NRC)
//  Source:
//    - ue_dump_all.js (Frida 实测偏移)
//    - C:\Users\user\Desktop\UE\4.26\Engine\Source\Runtime\CoreUObject\
//        Public\UObject\{Object,UObjectArray,Class,Field,UnrealType}.h
//        Public\UObject\NameTypes.h
//
//  Target: com.tencent.nrc (libUE4.so)
//
//  关键运行时偏移 (来自 ue_dump_all.js):
//    GUObjectArray base   = +0xD9C06B8
//    NamePool             = +0xD9A4B40
//    GWorld               = +0xDF3A198
//
//  设计:
//    所有偏移以 POD 结构成员形式定义, 使用 offsetof()/reinterpret_cast
//    访问而非魔术数字, 通过 static_assert 在编译期校验偏移正确。
//    snapshot 缓冲区 (e.snap / propSnap) 直接 reinterpret_cast 到对应
//    结构体指针, 用 ->member 语法读取, 等价于原 memcpy +Layout::kXxx.
//
//    注: NRC 的 NamePool 不带 NOT+XOR 混淆 (与 DFM 不同), 直接 ASCII。
// =====================================================================

namespace ue4nrc {

// ---------------------------------------------------------------------
//  基础容器
// ---------------------------------------------------------------------
template<typename T>
struct TArray {
    T*      Data;
    int32_t Num;
    int32_t Max;
};

struct FName {
    int32_t ComparisonIndex;  // +0x00
    int32_t Number;           // +0x04
};
static_assert(sizeof(FName) == 8, "FName must be 8 bytes");

struct FString {
    wchar_t* Data;
    int32_t  Num;
    int32_t  Max;
};

struct FRotator { float Pitch, Yaw, Roll; };
struct FVector  { float X, Y, Z; };

// ---------------------------------------------------------------------
//  FUObjectItem — GUObjectArray 元素 (UObjectArray.h)
// ---------------------------------------------------------------------
struct FUObjectItem {
    void*   Object;            // +0x00
    int32_t Flags;             // +0x08
    int32_t ClusterRootIndex;  // +0x0C
    int32_t SerialNumber;      // +0x10
    int32_t _Pad14;            // +0x14
};
static_assert(sizeof(FUObjectItem)            == 0x18, "FUObjectItem stride must be 24");
static_assert(offsetof(FUObjectItem, Object)  == 0x00);
static_assert(offsetof(FUObjectItem, Flags)   == 0x08);

// ---------------------------------------------------------------------
//  FUObjectArray — 头部布局
//    Chunks @+0x10, MaxElements @+0x20, NumElements @+0x24
// ---------------------------------------------------------------------
struct FUObjectArray {
    uint8_t  _pad0[0x10];
    FUObjectItem** Chunks;         // +0x10
    uint8_t  _pad18[0x20 - 0x18];
    int32_t  MaxElements;          // +0x20
    int32_t  NumElements;          // +0x24

    static constexpr uint32_t ElementsPerChunk = 0x10000;
};
static_assert(offsetof(FUObjectArray, Chunks)      == 0x10);
static_assert(offsetof(FUObjectArray, MaxElements) == 0x20);
static_assert(offsetof(FUObjectArray, NumElements) == 0x24);

// ---------------------------------------------------------------------
//  FNamePool — Blocks[] @+0x40
// ---------------------------------------------------------------------
struct FNameEntryHeader {
    uint16_t Raw;
    bool     isWide() const { return (Raw & 0x1) != 0; }
    uint32_t getLen() const { return static_cast<uint32_t>(Raw >> 6); }
};

struct FNamePool {
    uint8_t  _pad0[0x40];
    uint8_t* Blocks[8192];         // +0x40

    static constexpr uint32_t BlockSize  = 0x20000;
    static constexpr uint32_t Stride     = 2;
    static constexpr uint32_t MaxBlocks  = 8192;
    static constexpr uint32_t OffsetBits = 16;
};
static_assert(offsetof(FNamePool, Blocks) == 0x40);

// ---------------------------------------------------------------------
//  UObject 头 (Object.h)
// ---------------------------------------------------------------------
struct UObject {
    uint8_t   _pad0[0x10];
    void*     ClassPrivate;    // +0x10
    int32_t   NameIndex;       // +0x18
    int32_t   NameNumber;      // +0x1C
    void*     OuterPrivate;    // +0x20
};
static_assert(offsetof(UObject, ClassPrivate) == 0x10);
static_assert(offsetof(UObject, NameIndex)    == 0x18);
static_assert(offsetof(UObject, NameNumber)   == 0x1C);
static_assert(offsetof(UObject, OuterPrivate) == 0x20);

// ---------------------------------------------------------------------
//  UStruct (Class.h)
// ---------------------------------------------------------------------
struct UStruct {
    uint8_t   _pad0[0x40];
    void*     SuperStruct;        // +0x40
    void*     Children;           // +0x48
    void*     ChildProperties;    // +0x50 (UE 4.26)
    int32_t   PropertiesSize;     // +0x58
    int32_t   MinAlignment;       // +0x5C
};
static_assert(offsetof(UStruct, SuperStruct)     == 0x40);
static_assert(offsetof(UStruct, ChildProperties) == 0x50);
static_assert(offsetof(UStruct, PropertiesSize)  == 0x58);

// ---------------------------------------------------------------------
//  FField (Field.h, UE 4.26)
// ---------------------------------------------------------------------
struct FField {
    void*    ClassPrivate;        // +0x00 FFieldClass*
    uint8_t  _pad08[0x20 - 0x08]; // FFieldVariant Owner 占位
    void*    Next;                // +0x20
    int32_t  NameIndex;           // +0x28
    int32_t  NameNumber;          // +0x2C
};
static_assert(offsetof(FField, ClassPrivate) == 0x00);
static_assert(offsetof(FField, Next)         == 0x20);
static_assert(offsetof(FField, NameIndex)    == 0x28);
static_assert(offsetof(FField, NameNumber)   == 0x2C);

// ---------------------------------------------------------------------
//  FProperty (UnrealType.h)
// ---------------------------------------------------------------------
struct FProperty {
    uint8_t   _pad0[0x34];
    int32_t   ArrayDim;            // +0x34
    int32_t   ElementSize;         // +0x38
    uint8_t   _pad3C[0x40 - 0x3C];
    uint64_t  PropertyFlags;       // +0x40
    uint16_t  RepIndex;            // +0x48
    uint8_t   _pad4A[0x4C - 0x4A];
    int32_t   Offset_Internal;     // +0x4C
    uint8_t   _pad50[0x80 - 0x50];
    void*     SubTypePtr;          // +0x80
};
static_assert(offsetof(FProperty, ArrayDim)        == 0x34);
static_assert(offsetof(FProperty, ElementSize)     == 0x38);
static_assert(offsetof(FProperty, PropertyFlags)   == 0x40);
static_assert(offsetof(FProperty, RepIndex)        == 0x48);
static_assert(offsetof(FProperty, Offset_Internal) == 0x4C);
static_assert(offsetof(FProperty, SubTypePtr)      == 0x80);

// ---------------------------------------------------------------------
//  UFunction (Class.h)
// ---------------------------------------------------------------------
struct UFunction {
    uint8_t   _pad0[0xB0];
    uint32_t  FunctionFlags;       // +0xB0
    uint8_t   NumParms;            // +0xB4
    uint8_t   _padB5;
    uint16_t  ParmsSize;           // +0xB6
    uint16_t  ReturnValueOffset;   // +0xB8
    uint8_t   _padBA[0xD8 - 0xBA];
    void*     Func;                // +0xD8
};
static_assert(offsetof(UFunction, FunctionFlags)     == 0xB0);
static_assert(offsetof(UFunction, NumParms)          == 0xB4);
static_assert(offsetof(UFunction, ParmsSize)         == 0xB6);
static_assert(offsetof(UFunction, ReturnValueOffset) == 0xB8);
static_assert(offsetof(UFunction, Func)              == 0xD8);

// ---------------------------------------------------------------------
//  UEnum (Class.h)
// ---------------------------------------------------------------------
struct UEnum {
    uint8_t  _pad0[0x40];
    void*    NamesData;       // +0x40
    int32_t  NamesNum;        // +0x48
    int32_t  NamesMax;        // +0x4C

    static constexpr uint32_t kEnumPairStride = 16;
};
static_assert(offsetof(UEnum, NamesData) == 0x40);
static_assert(offsetof(UEnum, NamesNum)  == 0x48);

// ---------------------------------------------------------------------
//  UClass — CDO 偏移探测范围 (运行时自检, 标称 0x118)
// ---------------------------------------------------------------------
struct UClassCdoProbe {
    static constexpr uint32_t Begin   = 0x100;
    static constexpr uint32_t End     = 0x180;
    static constexpr uint32_t Step    = 0x008;
    static constexpr uint32_t Nominal = 0x118;
};

// ---------------------------------------------------------------------
//  EFunctionFlags / EObjectFlags
// ---------------------------------------------------------------------
enum EFunctionFlags : uint32_t {
    FUNC_None              = 0x00000000,
    FUNC_Final             = 0x00000001,
    FUNC_Net               = 0x00000040,
    FUNC_NetReliable       = 0x00000080,
    FUNC_Exec              = 0x00000200,
    FUNC_Native            = 0x00000400,
    FUNC_Event             = 0x00000800,
    FUNC_Static            = 0x00002000,
    FUNC_Public            = 0x00020000,
    FUNC_Private           = 0x00040000,
    FUNC_Protected         = 0x00080000,
    FUNC_BlueprintCallable = 0x04000000,
    FUNC_BlueprintEvent    = 0x08000000,
    FUNC_BlueprintPure     = 0x10000000,
};

enum EObjectFlags : uint32_t {
    RF_NoFlags            = 0x00000000,
    RF_Public             = 0x00000001,
    RF_ClassDefaultObject = 0x00000010,
    RF_Transient          = 0x00000040,
};

} // namespace ue4nrc

#endif // UE4_NRC_STRUCT_H
