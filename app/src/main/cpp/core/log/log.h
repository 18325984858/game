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
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>

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

/** @brief 运行时日志开关（默认开启） */
inline bool g_runtimeLogEnabled = true;

namespace log_internal {

inline std::string resolveTracePath() {
    static std::string cachedPath;
    const auto tryOpen = [](const std::string& path) -> int {
        if (path.empty()) return -1;
        return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    };

    if (!cachedPath.empty()) {
        const int cachedFd = tryOpen(cachedPath);
        if (cachedFd >= 0) {
            close(cachedFd);
            return cachedPath;
        }
        cachedPath.clear();
    }

    char processName[256] = {};
    const int cmdlineFd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (cmdlineFd >= 0) {
        const ssize_t sz = read(cmdlineFd, processName, sizeof(processName) - 1);
        close(cmdlineFd);
        if (sz > 0) {
            processName[sz] = '\0';
            std::string packageName(processName);
            const size_t sep = packageName.find(':');
            if (sep != std::string::npos) packageName.resize(sep);
            if (!packageName.empty()) {
                const std::string cachePath = "/data/data/" + packageName + "/cache/dfm_trace.txt";
                const int fd = tryOpen(cachePath);
                if (fd >= 0) {
                    close(fd);
                    cachedPath = cachePath;
                    return cachedPath;
                }
            }
        }
    }

    cachedPath = "/data/data/com.tencent.tmgp.dfm/cache/dfm_trace.txt";
    int fallbackFd = tryOpen(cachedPath);
    if (fallbackFd >= 0) {
        close(fallbackFd);
        return cachedPath;
    }

    cachedPath = "/data/local/tmp/dfm_trace.txt";
    return cachedPath;
}

inline void appendLogLineToFile(int priority, const char* renderedLine) {
    const std::string tracePath = resolveTracePath();
    const int fd = open(tracePath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;

    const char* level = priority >= ANDROID_LOG_ERROR ? "E"
        : (priority >= ANDROID_LOG_WARN ? "W" : "I");
    char line[1792] = {};
    const int length = snprintf(line, sizeof(line), "[%s][pid=%d] %s\n", level, getpid(), renderedLine);
    if (length > 0) {
        const size_t bytesToWrite = static_cast<size_t>(
            length < static_cast<int>(sizeof(line)) ? length : (sizeof(line) - 1));
        write(fd, line, bytesToWrite);
    }
    close(fd);
}

} // namespace log_internal

#define LOG(level, fmt, ...) \
        do { \
            if (g_runtimeLogEnabled && level >= CURRENT_LOG_LEVEL) { \
                int priority = ANDROID_LOG_INFO; \
                if (level == LOG_LEVEL_INFO) priority = ANDROID_LOG_INFO; \
                else if (level == LOG_LEVEL_WARN) priority = ANDROID_LOG_WARN; \
                else if (level == LOG_LEVEL_ERROR) priority = ANDROID_LOG_ERROR; \
                char renderedLine[1536] = {}; \
                snprintf(renderedLine, sizeof(renderedLine), "%s:%d: " fmt, __FILE__, __LINE__, ##__VA_ARGS__); \
                __android_log_print(priority, "[SFK]", "%s", renderedLine); \
                log_internal::appendLogLineToFile(priority, renderedLine); \
            } \
        } while (0)

#else
#define LOG(level, fmt, ...)
#endif


#endif //DOBBY_PROJECT_LOG_H
