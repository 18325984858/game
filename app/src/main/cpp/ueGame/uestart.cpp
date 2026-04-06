#include "uestart.h"
#include "../Log/log.h"
#include "libUE4Dumper/UE4Dumper.h"
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>

// =====================================================================
//  JNI Toast 工具 — 在安卓主线程显示下方弹框
// =====================================================================
namespace toast_util {
    using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);

    static JavaVM* getJavaVM() {
        static JavaVM* s_vm = nullptr;
        static bool s_tried = false;
        if (s_tried) return s_vm;
        s_tried = true;
        auto tryResolve = [](void* h) -> JavaVM* {
            if (!h) return nullptr;
            auto fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(h, "JNI_GetCreatedJavaVMs"));
            if (!fn) return nullptr;
            JavaVM* buf[2] = {}; jsize cnt = 0;
            return (fn(buf, 2, &cnt) == JNI_OK && cnt > 0) ? buf[0] : nullptr;
        };
        s_vm = tryResolve(RTLD_DEFAULT);
        if (!s_vm) { void* h = dlopen("libart.so", RTLD_NOW|RTLD_NOLOAD); s_vm = tryResolve(h); if (h) dlclose(h); }
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

        // 获取 Application context
        jclass atClass = env->FindClass("android/app/ActivityThread");
        if (!atClass || env->ExceptionCheck()) { env->ExceptionClear(); if (attached) vm->DetachCurrentThread(); return; }
        jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication", "()Landroid/app/Application;");
        jobject ctx = curApp ? env->CallStaticObjectMethod(atClass, curApp) : nullptr;
        env->DeleteLocalRef(atClass);
        if (!ctx || env->ExceptionCheck()) { env->ExceptionClear(); if (attached) vm->DetachCurrentThread(); return; }

        // Looper handler post
        jclass looperClass = env->FindClass("android/os/Looper");
        jmethodID getMainLooper = looperClass ? env->GetStaticMethodID(looperClass, "getMainLooper", "()Landroid/os/Looper;") : nullptr;
        jobject mainLooper = getMainLooper ? env->CallStaticObjectMethod(looperClass, getMainLooper) : nullptr;
        jclass handlerClass = env->FindClass("android/os/Handler");
        jmethodID handlerInit = handlerClass ? env->GetMethodID(handlerClass, "<init>", "(Landroid/os/Looper;)V") : nullptr;
        jobject handler = (handlerInit && mainLooper) ? env->NewObject(handlerClass, handlerInit, mainLooper) : nullptr;

        // 将 Toast 包装在 Runnable 中 post 到主线程
        // 简化方案: 直接在当前线程调用 Toast 并 Looper post
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
        if (handler) env->DeleteLocalRef(handler);
        if (handlerClass) env->DeleteLocalRef(handlerClass);
        if (mainLooper) env->DeleteLocalRef(mainLooper);
        if (looperClass) env->DeleteLocalRef(looperClass);
        env->DeleteLocalRef(ctx);
        if (attached) vm->DetachCurrentThread();
    }
} // namespace toast_util

// 读取 /data/local/tmp/dobby_config.txt 中的 ue_dumper 开关
static bool readUeDumperEnabled() {
    int fd = open("/data/local/tmp/dobby_config.txt", O_RDONLY);
    if (fd < 0) return false;
    char buf[256] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, "ue_dumper=1") != nullptr;
}

static void UE4WorkerThread(void* plibUE4ModeBase, void* pGNames,
                            void* pGWorld, void* pGUObjectArray, void* pData) {
    LOG(LOG_LEVEL_INFO, "[UE4Worker] 工作线程启动");
    LOG(LOG_LEVEL_INFO, "[UE4Worker] libUE4Base=%p GNames=%p GWorld=%p GUObjectArray=%p",
        plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray);

    // 检查界面上的 "启用 ueDumper (导出 dump)" 按钮状态
    if (readUeDumperEnabled()) {
        LOG(LOG_LEVEL_INFO, "[UE4Worker] ue_dumper 已启用, 创建 dump 线程");
        std::thread([=]() {
            ue4::UE4Dumper dumper(
                reinterpret_cast<uintptr_t>(plibUE4ModeBase),
                reinterpret_cast<uint64_t>(pGNames),
                reinterpret_cast<uint64_t>(pGUObjectArray),
                reinterpret_cast<uint64_t>(pGWorld),
                "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/"
            );

            if (!dumper.init()) {
                LOG(LOG_LEVEL_ERROR, "[UE4Worker] UE4Dumper 初始化失败");
                toast_util::showToast("UE4Dumper 初始化失败");
            } else {
                LOG(LOG_LEVEL_INFO, "[UE4Worker] UE4Dumper 初始化成功, NumNames=%d", dumper.getNumNames());
                if (dumper.dumpAll()) {
                    LOG(LOG_LEVEL_INFO, "[UE4Worker] dump 全部完成");
                    toast_util::showToast("UE4 Dump 完成");
                } else {
                    LOG(LOG_LEVEL_ERROR, "[UE4Worker] dump 部分失败");
                    toast_util::showToast("UE4 Dump 部分失败");
                }
            }
        }).detach();
    } else {
        LOG(LOG_LEVEL_INFO, "[UE4Worker] ue_dumper 未启用, 跳过 dump");
    }



    // TODO: 后续逻辑 (数据采集、hook 等)
    LOG(LOG_LEVEL_INFO, "[UE4Worker] 进入主循环");
}

extern "C" __attribute__((visibility("default")))
bool MyStartPointUE4(void* plibUE4ModeBase, void* pGNames,
                     void* pGWorld, void* pGUObjectArray, void* pData) {
    if (!plibUE4ModeBase || !pGNames || !pGWorld || !pGUObjectArray) {
        LOG(LOG_LEVEL_ERROR, "[MyStartPointUE4] 参数为空: base=%p GNames=%p GWorld=%p GUObjectArray=%p",
            plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray);
        return false;
    }
 LOG(LOG_LEVEL_ERROR, "[MyStartPointUE4] 参数: base=%p GNames=%p GWorld=%p GUObjectArray=%p",
            plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray);

    LOG(LOG_LEVEL_INFO, "[MyStartPointUE4] 启动 UE4 工作线程");

    std::thread(UE4WorkerThread, plibUE4ModeBase, pGNames,
                pGWorld, pGUObjectArray, pData).detach();

    return true;
}

OBFU_ATTRS_END
