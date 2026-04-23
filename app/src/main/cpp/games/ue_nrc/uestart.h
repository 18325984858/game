#ifndef UE_NRC_START_H
#define UE_NRC_START_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief NRC (洛克王国手游, UE 4.26) 注入入口 — 启动工作线程
 *
 * @param plibUE4ModeBase     libUE4.so 模块基址
 * @param pNamePool           NamePool 偏移 (uint32 squeezed into pointer)
 * @param pGWorld             GWorld 偏移
 * @param pGUObjectArrayNum   GUObjectArray.NumElements 偏移
 * @param pGUObjectArrayChunks GUObjectArray.Chunks 偏移
 * @param moduleSize          libUE4.so 大小
 * @param pData               预留
 */
__attribute__((visibility("default")))
bool MyStartPointNRC(void* plibUE4ModeBase, void* pNamePool, void* pGWorld,
                     void* pGUObjectArrayNum, void* pGUObjectArrayChunks,
                     uint64_t moduleSize, void* pData);

#ifdef __cplusplus
}
#endif

#endif // UE_NRC_START_H
