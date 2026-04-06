/**
 * @file    log.h
 * @brief   Android NDK 日志输出宏定义 —— 提供分级日志控制接口
 * @author  Song
 * @date    2025/11/15
 * @update  2026/03/05
 *
 * @details 基于 Android NDK 的 __android_log_print 封装了三级日志宏（INFO / WARN / ERROR），
 *          支持通过 ENABLE_LOGGING 宏开关全局启用或禁用日志输出，
 *          通过 CURRENT_LOG_LEVEL 控制最低输出级别。日志自动附带文件名和行号信息。
 */

#ifndef DOBBY_PROJECT_LOG_H
#define DOBBY_PROJECT_LOG_H

#include <android/log.h>  // 包含 Android NDK 的 log.h 文件
#include <stdarg.h>  // 用于支持可变参数宏

/** @name 日志级别定义 */
///@{
#define LOG_LEVEL_INFO  1   ///< 信息级别 —— 常规运行信息
#define LOG_LEVEL_WARN  2   ///< 警告级别 —— 潜在问题提示
#define LOG_LEVEL_ERROR 3   ///< 错误级别 —— 运行时错误
///@}

/** @brief 日志输出总开关（1=启用, 0=禁用） */
#define ENABLE_LOGGING 1

/** @brief 当前最低日志输出级别，低于此级别的日志将被过滤 */
#define CURRENT_LOG_LEVEL LOG_LEVEL_INFO

/**
 * @def LOG(level, fmt, ...)
 * @brief 日志输出宏，自动附带文件名和行号
 * @param level 日志级别（LOG_LEVEL_INFO / LOG_LEVEL_WARN / LOG_LEVEL_ERROR）
 * @param fmt   格式化字符串（同 printf 语法）
 * @param ...   可变参数列表
 */
#if ENABLE_LOGGING

/** @brief 运行时日志开关（默认启用，可通过配置文件关闭） */
inline bool g_runtimeLogEnabled = true;

#define LOG(level, fmt, ...) \
        do { \
            if (g_runtimeLogEnabled && level >= CURRENT_LOG_LEVEL) { \
                int priority = ANDROID_LOG_INFO; \
                if (level == LOG_LEVEL_INFO) priority = ANDROID_LOG_INFO; \
                else if (level == LOG_LEVEL_WARN) priority = ANDROID_LOG_WARN; \
                else if (level == LOG_LEVEL_ERROR) priority = ANDROID_LOG_ERROR; \
                __android_log_print(priority, "[SFK]", "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
            } \
        } while (0)

/**
 * @def LOGT(tag, level, fmt, ...)
 * @brief \u5e26\u81ea\u5b9a\u4e49 logcat tag \u7684\u65e5\u5fd7\u8f93\u51fa\u5b8f\uff0c\u65b9\u4fbf\u6309\u6a21\u5757\u8fc7\u6ee4
 * @param tag   logcat tag \u5b57\u7b26\u4e32 (\u5982 "MatchMonitor")
 * @param level \u65e5\u5fd7\u7ea7\u522b
 * @param fmt   \u683c\u5f0f\u5316\u5b57\u7b26\u4e32
 */
#define LOGT(tag, level, fmt, ...) \
        do { \
            if (g_runtimeLogEnabled && level >= CURRENT_LOG_LEVEL) { \
                int priority = ANDROID_LOG_INFO; \
                if (level == LOG_LEVEL_INFO) priority = ANDROID_LOG_INFO; \
                else if (level == LOG_LEVEL_WARN) priority = ANDROID_LOG_WARN; \
                else if (level == LOG_LEVEL_ERROR) priority = ANDROID_LOG_ERROR; \
                __android_log_print(priority, tag, "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
            } \
        } while (0)
#define LOG(level, fmt, ...)  // 如果禁用日志，则宏什么都不做
#endif


#endif //DOBBY_PROJECT_LOG_H
