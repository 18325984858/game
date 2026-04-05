/**
 * @file    start.h
 * @brief   程序启动入口声明 —— il2cpp 运行时初始化后的业务逻辑起点
 * @author  Song
 * @date    2025/11/15
 * @update  2026/03/05
 *
 * @details 声明了 MyStartPoint 入口函数，该函数在 il2cpp 运行时完成初始化后被调用，
 *          接收 il2cpp 基地址及核心元数据注册表指针，用于启动类信息采集、Dump 等业务逻辑。
 */

#ifndef DOBBY_PROJECT_START_H
#define DOBBY_PROJECT_START_H

#include <iostream>

using namespace std;

/**
 * @brief   业务逻辑启动入口函数
 * @param   pli2cppModeBase         il2cpp 模块基地址（libil2cpp.so 在内存中的加载地址）
 * @param   pCodeRegistration       代码注册表指针（Il2CppCodeRegistration*）
 * @param   pMetadataRegistration   元数据注册表指针（Il2CppMetadataRegistration*）
 * @param   pGlobalMetadataHeader   全局元数据头指针（Il2CppGlobalMetadataHeader*）
 * @param   pMetadataImagesTable    元数据镜像表指针（Il2CppImageGlobalMetadata*）
 * @return  成功返回 true，失败返回 false
 */
bool MyStartPoint(void *pli2cppModeBase=nullptr,void *pCodeRegistration=nullptr, void *pMetadataRegistration=nullptr,
                  void *pGlobalMetadataHeader=nullptr,void*pMetadataImagesTable=nullptr);



#endif //DOBBY_PROJECT_START_H
