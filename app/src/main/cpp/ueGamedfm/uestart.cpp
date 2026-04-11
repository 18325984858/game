#include "uestart.h"
#include "../Log/log.h"
#include "UE5DfmDumper.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdarg>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <dlfcn.h>
#include <jni.h>
#include <sys/stat.h>

namespace {

// =====================================================================
//  日志工具 — 同时写入 logcat + 文件
// =====================================================================

std::string resolveTracePath() {
    static std::string cachedPath;
    const auto tryOpen = [](const std::string& path) -> int {
        if (path.empty()) return -1;
        return open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    };

    if (!cachedPath.empty()) {
        const int cachedFd = tryOpen(cachedPath);
        if (cachedFd >= 0) { close(cachedFd); return cachedPath; }
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
                if (fd >= 0) { close(fd); cachedPath = cachePath; return cachedPath; }
            }
        }
    }

    cachedPath = "/data/local/tmp/dfm_trace.txt";
    return cachedPath;
}

void writeTrace(int priority, const char* fmt, va_list args) {
    char message[1024] = {};
    vsnprintf(message, sizeof(message), fmt, args);

    __android_log_print(priority, "UE5-DFM", "%s", message);

    const std::string tracePath = resolveTracePath();
    const int fd = open(tracePath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) return;

    const char* level = priority >= ANDROID_LOG_ERROR ? "E" : "I";
    char line[1200] = {};
    const int length = snprintf(line, sizeof(line), "[%s][pid=%d] %s\n", level, getpid(), message);
    if (length > 0) write(fd, line, static_cast<size_t>(length));
    close(fd);
}

void traceInfo(const char* fmt, ...) {
    va_list args; va_start(args, fmt); writeTrace(ANDROID_LOG_INFO, fmt, args); va_end(args);
}
void traceError(const char* fmt, ...) {
    va_list args; va_start(args, fmt); writeTrace(ANDROID_LOG_ERROR, fmt, args); va_end(args);
}

} // namespace

#define DLOG(...) traceInfo(__VA_ARGS__)
#define DERR(...) traceError(__VA_ARGS__)

// =====================================================================
//  配置读取
// =====================================================================
namespace {

static bool readConfigFlag(const char* key) {
    int fd = open("/data/local/tmp/dobby_config.txt", O_RDONLY);
    if (fd < 0) return false;
    char buf[256] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, key) != nullptr;
}

static bool readDumperEnabled() { return readConfigFlag("ue_dumper=1"); }
static bool readLogEnabled()    { return readConfigFlag("log=1"); }

// =====================================================================
//  JNI Toast 工具
// =====================================================================
namespace toast_util {
    using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);

    static JavaVM* getJavaVM() {
        static JavaVM* s_vm = nullptr;
        static bool s_tried = false;
        if (s_tried) return s_vm;
        s_tried = true;

        JavaVM* vmBuf[2] = {nullptr, nullptr};
        jsize vmCount = 0;

        auto fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
        if (fn && fn(vmBuf, 2, &vmCount) == JNI_OK && vmCount > 0) {
            s_vm = vmBuf[0];
        }

        if (!s_vm) {
            void* libArt = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
            if (libArt) {
                fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(libArt, "JNI_GetCreatedJavaVMs"));
                if (fn && fn(vmBuf, 2, &vmCount) == JNI_OK && vmCount > 0) s_vm = vmBuf[0];
                dlclose(libArt);
            }
        }

        DLOG("getJavaVM: resolved vm=%p", s_vm);
        return s_vm;
    }

    static void showToast(const char* msg) {
        JavaVM* vm = getJavaVM();
        if (!vm) return;
        JNIEnv* env = nullptr;
        bool attached = false;
        jint stat = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (stat == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
            attached = true;
        } else if (!env) return;

        jclass atClass = env->FindClass("android/app/ActivityThread");
        if (!atClass || env->ExceptionCheck()) { env->ExceptionClear(); if (attached) vm->DetachCurrentThread(); return; }
        jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication", "()Landroid/app/Application;");
        jobject ctx = curApp ? env->CallStaticObjectMethod(atClass, curApp) : nullptr;
        env->DeleteLocalRef(atClass);
        if (!ctx || env->ExceptionCheck()) { env->ExceptionClear(); if (attached) vm->DetachCurrentThread(); return; }

        jclass toastClass = env->FindClass("android/widget/Toast");
        jmethodID makeText = toastClass ? env->GetStaticMethodID(toastClass, "makeText",
            "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;") : nullptr;
        jstring jmsg = env->NewStringUTF(msg);
        jobject toast = (makeText && jmsg) ? env->CallStaticObjectMethod(toastClass, makeText, ctx, jmsg, 1) : nullptr;
        if (toast) {
            jmethodID show = env->GetMethodID(toastClass, "show", "()V");
            if (show) env->CallVoidMethod(toast, show);
        }

        if (env->ExceptionCheck()) env->ExceptionClear();
        if (jmsg) env->DeleteLocalRef(jmsg);
        if (toast) env->DeleteLocalRef(toast);
        if (toastClass) env->DeleteLocalRef(toastClass);
        env->DeleteLocalRef(ctx);
        if (attached) vm->DetachCurrentThread();
    }
} // namespace toast_util

} // namespace

// =====================================================================
//  工作线程
// =====================================================================
static void DfmWorkerThread(void* plibUE4ModeBase, void* /*pGNames*/,
                            void* pGWorld, void* pGUObjectArray,
                            uint64_t moduleSize, void* /*pData*/) {
    LOG(LOG_LEVEL_INFO, "[DfmWorker] 工作线程启动");
    LOG(LOG_LEVEL_INFO, "[DfmWorker] libUE4Base=%p GWorld=%p GUObjectArray=%p moduleSize=0x%llX",
        plibUE4ModeBase, pGWorld, pGUObjectArray, (unsigned long long)moduleSize);

    uintptr_t base = reinterpret_cast<uintptr_t>(plibUE4ModeBase);

    // 等待游戏引擎完成初始化
    LOG(LOG_LEVEL_INFO, "[DfmWorker] 等待游戏引擎就绪...");
    {
        constexpr int kMaxWaitSeconds = 120;
        int memFd = open("/proc/self/mem", O_RDONLY);
        bool ready = false;

        for (int i = 0; i < kMaxWaitSeconds * 2; i++) {
            // 验证 NamePool 第一个块指针
            uintptr_t poolBlockAddr = base + ue5dfm::OFF_NAMEPOOL + ue5dfm::OFF_NAMEPOOL_BLOCKS;
            uintptr_t block0 = 0;
            bool namePoolOk = false;
            if (memFd >= 0 && poolBlockAddr >= 0x10000) {
                if (pread(memFd, &block0, sizeof(block0), static_cast<off_t>(poolBlockAddr)) == sizeof(block0)) {
                    namePoolOk = (block0 > 0x10000);
                }
            }

            // 验证 GUObjectArray numElements
            uint32_t numElements = 0;
            bool objArrayOk = false;
            uintptr_t numAddr = base + ue5dfm::OFF_GUOBJECTARRAY_NUM;
            if (memFd >= 0 && numAddr >= 0x10000) {
                if (pread(memFd, &numElements, sizeof(numElements), static_cast<off_t>(numAddr)) == sizeof(numElements)) {
                    objArrayOk = (numElements > 100);
                }
            }

            // 验证 GWorld
            uintptr_t gworldAddr = base + ue5dfm::OFF_GWORLD;
            uintptr_t worldPtr = 0;
            bool worldOk = false;
            if (memFd >= 0 && gworldAddr >= 0x10000) {
                if (pread(memFd, &worldPtr, sizeof(worldPtr), static_cast<off_t>(gworldAddr)) == sizeof(worldPtr)) {
                    worldOk = (worldPtr >= 0x10000);
                }
            }

            if (namePoolOk && objArrayOk && worldOk) {
                LOG(LOG_LEVEL_INFO, "[DfmWorker] 引擎就绪! block0=%p numElements=%u worldPtr=%p (等待了 %.1fs)",
                    (void*)block0, numElements, (void*)worldPtr, i * 0.5f);
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (memFd >= 0) close(memFd);
        if (!ready) {
            LOG(LOG_LEVEL_ERROR, "[DfmWorker] 引擎等待超时 (%ds), 放弃启动", kMaxWaitSeconds);
            return;
        }
    }

    // 额外等待 5 秒让引擎完全稳定
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // ---- SDK Dump ----
    if (readDumperEnabled()) {
        LOG(LOG_LEVEL_INFO, "[DfmWorker] ue_dumper 已启用, 开始 dump");
        DLOG("开始 DFM SDK Dump...");

        ue5dfm::UE5DfmDumper dumper(
            base,
            static_cast<uintptr_t>(moduleSize),
            "/data/data/com.tencent.tmgp.dfm/cache/ue5_dump/"
        );

        if (!dumper.init()) {
            LOG(LOG_LEVEL_ERROR, "[DfmWorker] UE5DfmDumper 初始化失败");
            toast_util::showToast("DFM Dumper 初始化失败");
        } else {
            LOG(LOG_LEVEL_INFO, "[DfmWorker] UE5DfmDumper 初始化成功");
            if (dumper.dumpAll()) {
                LOG(LOG_LEVEL_INFO, "[DfmWorker] dump 全部完成");
                toast_util::showToast("DFM SDK Dump 完成");
                DLOG("DFM SDK Dump 完成");
            } else {
                LOG(LOG_LEVEL_ERROR, "[DfmWorker] dump 部分失败");
                toast_util::showToast("DFM SDK Dump 部分失败");
            }
        }
    } else {
        LOG(LOG_LEVEL_INFO, "[DfmWorker] ue_dumper 未启用, 跳过 dump");
    }

    LOG(LOG_LEVEL_INFO, "[DfmWorker] 工作线程退出");
}

// =====================================================================
//  入口函数
// =====================================================================
extern "C" __attribute__((visibility("default")))
bool MyStartPointUE5(void* plibUE4ModeBase, void* pGNames,
                     void* pGWorld, void* pGUObjectArray,
                     uint64_t moduleSize, void* pData) {
    if (!plibUE4ModeBase || !pGUObjectArray) {
        DERR("MyStartPointUE4: 参数为空 base=%p GUObjectArray=%p",
             plibUE4ModeBase, pGUObjectArray);
        LOG(LOG_LEVEL_ERROR, "[MyStartPointUE4-DFM] 参数为空: base=%p GUObjectArray=%p",
            plibUE4ModeBase, pGUObjectArray);
        return false;
    }

    DLOG("MyStartPointUE4-DFM: base=%p GNames=%p GWorld=%p GUObjectArray=%p moduleSize=0x%llX",
         plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray, (unsigned long long)moduleSize);
    LOG(LOG_LEVEL_INFO, "[MyStartPointUE4-DFM] 启动 DFM 工作线程");

    std::thread(DfmWorkerThread, plibUE4ModeBase, pGNames,
                pGWorld, pGUObjectArray, moduleSize, pData).detach();

    return true;
}
