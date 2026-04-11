#ifndef UE5_DFM_STRUCT_H
#define UE5_DFM_STRUCT_H

#include <cstdint>

// =====================================================================
//  UE 5.4 完整结构体布局 — ARM64 Android (DFM)
//  Source: sdk_dump.js (Frida) + IDA 反编译验证 (libUE4.so)
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

// ---- 偏移常量 --------------------------------------------------------

// UObject 布局
static constexpr uint32_t OFF_UOBJ_CLASS          = 0x08;   // UClass*
static constexpr uint32_t OFF_UOBJ_OUTER          = 0x10;   // UObject* OuterPrivate
static constexpr uint32_t OFF_UOBJ_FNAME          = 0x1C;   // FName (ComparisonIndex)
static constexpr uint32_t OFF_UOBJ_INTERNAL_INDEX = 0x24;   // int32

// UStruct 布局
static constexpr uint32_t OFF_USTRUCT_PROPERTIES_SIZE = 0x3C; // int32 PropertiesSize
static constexpr uint32_t OFF_USTRUCT_SUPER           = 0x40; // UStruct* SuperStruct
static constexpr uint32_t OFF_USTRUCT_CHILDREN        = 0x50; // UField* Children
static constexpr uint32_t OFF_USTRUCT_CHILD_PROPS     = 0x68; // FField* ChildProperties

// UField 布局
static constexpr uint32_t OFF_UFIELD_NEXT = 0x28; // UField* Next

// FField 布局
static constexpr uint32_t OFF_FFIELD_OWNER      = 0x08; // void* Owner
static constexpr uint32_t OFF_FFIELD_NEXT        = 0x18; // FField* Next
static constexpr uint32_t OFF_FFIELD_CLASS       = 0x20; // FFieldClass*
static constexpr uint32_t OFF_FFIELD_FNAME       = 0x28; // FName

// FFieldClass 布局
static constexpr uint32_t OFF_FFIELDCLASS_FNAME  = 0x00; // FName

// FProperty 布局
static constexpr uint32_t OFF_FPROP_ARRAY_DIM    = 0x34; // int32
static constexpr uint32_t OFF_FPROP_ELEMENT_SIZE = 0x3C; // int32
static constexpr uint32_t OFF_FPROP_FLAGS        = 0x40; // uint32 PropertyFlags (low 32 bits)
static constexpr uint32_t OFF_FPROP_OFFSET       = 0x4C; // int32 Offset_Internal
static constexpr uint32_t OFF_FPROP_SUBTYPE_PTR  = 0x88; // UObject* (struct/class ref)

// UFunction 布局
static constexpr uint32_t OFF_UFUNC_FLAGS        = 0xB8; // uint32 FunctionFlags
static constexpr uint32_t OFF_UFUNC_NATIVE_FUNC  = 0xD8; // void* NativeFunc

// UEnum 布局 (Names TArray)
static constexpr uint32_t OFF_UENUM_NAMES_DATA   = 0x40; // TPair<FName, int64>* Data
static constexpr uint32_t OFF_UENUM_NAMES_NUM    = 0x48; // int32 Num
static constexpr uint32_t OFF_UENUM_ELEMENT_STRIDE = 16;  // sizeof(TPair<FName, int64>)

// NamePool
static constexpr uint32_t OFF_NAMEPOOL           = 0x1A343A00;
static constexpr uint32_t OFF_NAMEPOOL_BLOCKS    = 0x38; // 块指针数组起始
static constexpr uint32_t NAMEPOOL_BLOCK_SIZE    = 0x10000;
static constexpr uint32_t NAMEPOOL_OFFSET_BITS   = 18;
static constexpr uint32_t NAMEPOOL_MAX_BLOCKS    = 8192;

// GUObjectArray
static constexpr uint32_t OFF_GUOBJECTARRAY_NUM  = 0x1A36A75C;
static constexpr uint32_t OFF_GUOBJECTARRAY_CHUNKS = 0x1A36A768;
static constexpr uint32_t GUOBJ_ITEM_STRIDE      = 0x18; // FUObjectItem stride

// GWorld
static constexpr uint32_t OFF_GWORLD             = 0x1A65ECC8;

// FunctionFlags 位定义
static constexpr uint32_t FUNC_Final              = 0x00000001;
static constexpr uint32_t FUNC_Net                = 0x00000002;
static constexpr uint32_t FUNC_Exec               = 0x00000200;
static constexpr uint32_t FUNC_Native             = 0x00000400;
static constexpr uint32_t FUNC_Event              = 0x00000800;
static constexpr uint32_t FUNC_Static             = 0x00002000;
static constexpr uint32_t FUNC_BlueprintCallable  = 0x04000000;
static constexpr uint32_t FUNC_BlueprintEvent     = 0x08000000;
static constexpr uint32_t FUNC_BlueprintPure      = 0x10000000;

// PropertyFlags 位定义 (常用)
static constexpr uint32_t CPF_ReturnParm          = 0x00000400;
static constexpr uint32_t CPF_Parm                = 0x00000080;
static constexpr uint32_t CPF_OutParm             = 0x00000100;
static constexpr uint32_t CPF_ConstParm           = 0x00000200;

} // namespace ue5dfm

#endif // UE5_DFM_STRUCT_H
