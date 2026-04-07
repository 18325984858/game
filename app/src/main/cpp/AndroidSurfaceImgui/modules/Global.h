#ifndef GLOBAL_H // !GLOBAL_H
#define GLOBAL_H

#include <android/log.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

#ifndef TAG
#define TAG "AImGui"
#endif

static inline std::string AndroidSurfaceImguiGetBaseProcessName() {
	char processName[256] = {};
	const int cmdlineFd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
	if (cmdlineFd < 0) {
		return {};
	}

	const ssize_t processNameSize = read(cmdlineFd, processName, sizeof(processName) - 1);
	close(cmdlineFd);
	if (processNameSize <= 0) {
		return {};
	}

	processName[processNameSize] = '\0';
	std::string packageName(processName);
	const size_t processSeparator = packageName.find(':');
	if (processSeparator != std::string::npos) {
		packageName.resize(processSeparator);
	}
	return packageName;
}

static inline int AndroidSurfaceImguiOpenTraceFile() {
	static std::string cachedPath;
	const auto tryOpen = [](const std::string& path) -> int {
		if (path.empty()) {
			return -1;
		}
		return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
	};

	if (!cachedPath.empty()) {
		const int cachedFd = tryOpen(cachedPath);
		if (cachedFd >= 0) {
			return cachedFd;
		}
		cachedPath.clear();
	}

	const std::string packageName = AndroidSurfaceImguiGetBaseProcessName();
	if (!packageName.empty()) {
		const std::string packageCachePath = "/data/data/" + packageName + "/cache/ue4_gui_trace.txt";
		const int packageCacheFd = tryOpen(packageCachePath);
		if (packageCacheFd >= 0) {
			cachedPath = packageCachePath;
			return packageCacheFd;
		}

		const std::string packageFilesPath = "/data/data/" + packageName + "/files/ue4_gui_trace.txt";
		const int packageFilesFd = tryOpen(packageFilesPath);
		if (packageFilesFd >= 0) {
			cachedPath = packageFilesPath;
			return packageFilesFd;
		}

		const std::string userCachePath = "/data/user/0/" + packageName + "/cache/ue4_gui_trace.txt";
		const int userCacheFd = tryOpen(userCachePath);
		if (userCacheFd >= 0) {
			cachedPath = userCachePath;
			return userCacheFd;
		}

		const std::string userFilesPath = "/data/user/0/" + packageName + "/files/ue4_gui_trace.txt";
		const int userFilesFd = tryOpen(userFilesPath);
		if (userFilesFd >= 0) {
			cachedPath = userFilesPath;
			return userFilesFd;
		}
	}

	const std::string fallbackPath = "/data/local/tmp/ue4_gui_trace.txt";
	const int fallbackFd = tryOpen(fallbackPath);
	if (fallbackFd >= 0) {
		cachedPath = fallbackPath;
		return fallbackFd;
	}

	return -1;
}

static inline void AndroidSurfaceImguiLogPrint(int priority, const char* tag, const char* formatter, ...) {
	char message[1024] = {};

	va_list args;
	va_start(args, formatter);
	vsnprintf(message, sizeof(message), formatter, args);
	va_end(args);

	__android_log_print(priority, tag, "%s", message);

	const int fd = AndroidSurfaceImguiOpenTraceFile();
	if (fd < 0) {
		return;
	}

	const char* level = priority >= ANDROID_LOG_ERROR ? "E" : (priority == ANDROID_LOG_DEBUG ? "D" : "I");
	char line[1200] = {};
	const int length = snprintf(line, sizeof(line), "[%s][pid=%d][%s] %s\n", level, getpid(), tag, message);
	if (length > 0) {
		write(fd, line, static_cast<size_t>(length));
	}
	close(fd);
}

#define LogInfo(formatter, ...) AndroidSurfaceImguiLogPrint(ANDROID_LOG_INFO, TAG, formatter __VA_OPT__(, ) __VA_ARGS__)
#define LogDebug(formatter, ...) AndroidSurfaceImguiLogPrint(ANDROID_LOG_DEBUG, TAG, formatter __VA_OPT__(, ) __VA_ARGS__)
#define LogError(formatter, ...) AndroidSurfaceImguiLogPrint(ANDROID_LOG_ERROR, TAG, formatter __VA_OPT__(, ) __VA_ARGS__)

#endif // !GLOBAL_H