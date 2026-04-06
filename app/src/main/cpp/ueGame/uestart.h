
#ifndef UE_START_H
#define UE_START_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UE4 注入入口 — 启动 UE4 工作线程进行 GNames/GObjects/GWorld/SDK dump
 *
 * @param plibUE4ModeBase  libUE4.so 模块基址
 * @param pGNames          GNames 数组指针 (已解引用)
 * @param pGWorld          GWorld 指针
 * @param pGUObjectArray   GUObjectArray 地址
 * @param moduleSize       libUE4.so 模块大小
 * @param pData            预留扩展数据 (可为空)
 * @return true 工作线程启动成功, false 参数校验失败
 */
__attribute__((visibility("default")))
bool MyStartPointUE4(void *plibUE4ModeBase, void *pGNames, void *pGWorld, void *pGUObjectArray, uint64_t moduleSize, void *pData);

#ifdef __cplusplus
}
#endif

#endif // UE_START_H
