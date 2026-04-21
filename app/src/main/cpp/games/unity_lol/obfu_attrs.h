/*
 * obfu_attrs.h — Polaris-Obfuscator 函数注解注入头
 *
 * 通过 -include 强制包含此头文件，为所有后续定义的函数自动添加
 * Polaris 所需的 annotate 属性，使混淆 pass 能对函数生效。
 *
 * 仅在 OBFU_ENABLE_ANNOTATIONS 宏被定义时激活（由 CMake 控制）。
 *
 * 用法：每个 .cpp 文件末尾必须添加 OBFU_ATTRS_END 来关闭 pragma 作用域。
 * 如需对个别函数禁用混淆，使用 __attribute__((optnone))。
 */
#ifndef OBFU_ATTRS_H
#define OBFU_ATTRS_H

#ifdef OBFU_ENABLE_ANNOTATIONS

/* 指令替换 (sub) */
#pragma clang attribute push(__attribute__((annotate("substitution"))), apply_to = function)

/* 虚假控制流 (bcf) */
#pragma clang attribute push(__attribute__((annotate("boguscfg"))), apply_to = function)

/* 间接跳转 (indbr) */
#pragma clang attribute push(__attribute__((annotate("indirectbr"))), apply_to = function)

/* 间接调用 (indcall) */
#pragma clang attribute push(__attribute__((annotate("indirectcall"))), apply_to = function)

/* 线性 MBA (mba) */
#pragma clang attribute push(__attribute__((annotate("linearmba"))), apply_to = function)

/* 字符串加密 (strcry) */
#pragma clang attribute push(__attribute__((annotate("strcry"))), apply_to = function)

/* 收尾宏：在每个 .cpp 文件末尾调用以关闭所有 pragma push */
#define OBFU_ATTRS_END \
    _Pragma("clang attribute pop") \
    _Pragma("clang attribute pop") \
    _Pragma("clang attribute pop") \
    _Pragma("clang attribute pop") \
    _Pragma("clang attribute pop") \
    _Pragma("clang attribute pop")

#else
/* 未启用混淆时，OBFU_ATTRS_END 为空操作 */
#define OBFU_ATTRS_END
#endif /* OBFU_ENABLE_ANNOTATIONS */
#endif /* OBFU_ATTRS_H */
