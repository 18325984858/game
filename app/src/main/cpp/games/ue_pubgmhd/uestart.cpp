#include "uestart.h"
#include "../../core/log/log.h"
#include "libUE4Dumper/UE4Dumper.h"
#include "libUE4Header/UE4Header.h"
#include "interface/interface.h"
#include "pubgmhd/pubgmhd.h"
#include "Draw/UE4Draw.h"
#include "AImGui.h"
#include "ANativeWindowCreator.h"
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
#include <stdexcept>
#include <sys/stat.h>
#include <dirent.h>

namespace {
std::string resolveUe4GuiTracePath() {
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
            close(cachedFd);
            return cachedPath;
        }
        cachedPath.clear();
    }

    char processName[256] = {};
    const int cmdlineFd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    if (cmdlineFd >= 0) {
        const ssize_t processNameSize = read(cmdlineFd, processName, sizeof(processName) - 1);
        close(cmdlineFd);
        if (processNameSize > 0) {
            processName[processNameSize] = '\0';
            std::string packageName(processName);
            const size_t processSeparator = packageName.find(':');
            if (processSeparator != std::string::npos) {
                packageName.resize(processSeparator);
            }

            if (!packageName.empty()) {
                const std::string cachePath = "/data/data/" + packageName + "/cache/ue4_gui_trace.txt";
                const int cacheFd = tryOpen(cachePath);
                if (cacheFd >= 0) {
                    close(cacheFd);
                    cachedPath = cachePath;
                    return cachedPath;
                }

                const std::string filesPath = "/data/data/" + packageName + "/files/ue4_gui_trace.txt";
                const int filesFd = tryOpen(filesPath);
                if (filesFd >= 0) {
                    close(filesFd);
                    cachedPath = filesPath;
                    return cachedPath;
                }
            }
        }
    }

    cachedPath = "/data/local/tmp/ue4_gui_trace.txt";
    return cachedPath;
}

void writeGuiTrace(int priority, const char* fmt, va_list args) {
    // GUI 线程日志始终写入文件 (不受 g_runtimeLogEnabled 控制)
    // 因为 GUI 线程先于 WorkerThread 启动, 此时 g_runtimeLogEnabled 尚未设置
    char message[1024] = {};
    vsnprintf(message, sizeof(message), fmt, args);

    __android_log_print(priority, "UE4-GUI", "%s", message);

    const std::string tracePath = resolveUe4GuiTracePath();
    const int fd = open(tracePath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return;
    }

    const char* level = priority >= ANDROID_LOG_ERROR ? "E" : "I";
    char line[1200] = {};
    const int length = snprintf(line, sizeof(line), "[%s][pid=%d] %s\n", level, getpid(), message);
    if (length > 0) {
        write(fd, line, static_cast<size_t>(length));
    }
    close(fd);
}

void guiInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    writeGuiTrace(ANDROID_LOG_INFO, fmt, args);
    va_end(args);
}

void guiError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    writeGuiTrace(ANDROID_LOG_ERROR, fmt, args);
    va_end(args);
}
} // namespace

// 直接日志输出 + 落盘追踪 (避免被进程过滤后完全看不到)
#define GLOG(...) guiInfo(__VA_ARGS__)
#define GERR(...) guiError(__VA_ARGS__)

// =====================================================================
//  JNI Toast 工具 — 在安卓主线程显示下方弹框
// =====================================================================
namespace toast_util {
    using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);
    using AndroidRuntimeGetJavaVMFn = JavaVM* (*)();

    static JavaVM* getJavaVM() {
        static JavaVM* s_vm = nullptr;
        static bool s_tried = false;
        if (s_tried) return s_vm;
        s_tried = true;

        auto resolveFromCreatedVMs = [](void* handle) -> JavaVM* {
            if (!handle) return nullptr;
            auto fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(handle, "JNI_GetCreatedJavaVMs"));
            if (!fn) return nullptr;

            JavaVM* vmBuf[2] = {nullptr, nullptr};
            jsize vmCount = 0;
            if (fn(vmBuf, 2, &vmCount) != JNI_OK || vmCount <= 0) return nullptr;
            return vmBuf[0];
        };

        auto resolveFromAndroidRuntime = [](void* handle) -> JavaVM* {
            if (!handle) return nullptr;
            constexpr const char* symbols[] = {
                "_ZN7android14AndroidRuntime9getJavaVMEv",
                "_ZN7android14AndroidRuntime7getJavaVMEv",
            };
            for (const char* symbol : symbols) {
                auto fn = reinterpret_cast<AndroidRuntimeGetJavaVMFn>(dlsym(handle, symbol));
                if (!fn) continue;
                JavaVM* vm = fn();
                if (vm) return vm;
            }
            return nullptr;
        };

        auto openLibrary = [](const char* name) -> void* {
            void* handle = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
            if (!handle) handle = dlopen(name, RTLD_NOW);
            return handle;
        };

        s_vm = resolveFromCreatedVMs(RTLD_DEFAULT);
        if (!s_vm) {
            void* libArt = openLibrary("libart.so");
            s_vm = resolveFromCreatedVMs(libArt);
            if (libArt) dlclose(libArt);
        }
        if (!s_vm) {
            void* libAndroidRuntime = openLibrary("libandroid_runtime.so");
            s_vm = resolveFromCreatedVMs(libAndroidRuntime);
            if (!s_vm) s_vm = resolveFromAndroidRuntime(libAndroidRuntime);
            if (libAndroidRuntime) dlclose(libAndroidRuntime);
        }

        GLOG("getJavaVM: resolved vm=%p", s_vm);
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

static bool readUeHeaderEnabled() {
    int fd = open("/data/local/tmp/dobby_config.txt", O_RDONLY);
    if (fd < 0) return false;
    char buf[256] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, "ue_header=1") != nullptr;
}

static bool readLogEnabled() {
    int fd = open("/data/local/tmp/dobby_config.txt", O_RDONLY);
    if (fd < 0) return false;
    char buf[256] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, "log=1") != nullptr;
}

struct JavaDisplayInfo {
    int width = 0;
    int height = 0;
    int rotateTheta = 0;
};

static void normalizePublicOverlayDisplayInfo(JavaDisplayInfo& displayInfo) {
    if (displayInfo.width <= 0 || displayInfo.height <= 0) {
        return;
    }

    const int originalWidth = displayInfo.width;
    const int originalHeight = displayInfo.height;
    const int originalRotate = displayInfo.rotateTheta;

    if (displayInfo.width < displayInfo.height || 90 == displayInfo.rotateTheta || 270 == displayInfo.rotateTheta) {
        displayInfo.width = originalWidth > originalHeight ? originalWidth : originalHeight;
        displayInfo.height = originalWidth > originalHeight ? originalHeight : originalWidth;
        displayInfo.rotateTheta = 0;

        GLOG("公开 Overlay 坐标空间归一化: %dx%d r%d -> %dx%d r%d",
             originalWidth,
             originalHeight,
             originalRotate,
             displayInfo.width,
             displayInfo.height,
             displayInfo.rotateTheta);
    }
}

static bool queryJavaDisplayInfo(JavaDisplayInfo& outInfo);

static bool readShellCommandOutput(const char* command, std::string& output) {
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return false;
    }

    char buffer[256] = {};
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }

    const int rc = pclose(pipe);
    return rc == 0 && !output.empty();
}

static bool parseDisplaySizeFromText(const std::string& text, int& width, int& height) {
    for (size_t index = 0; index < text.size(); ++index) {
        int parsedWidth = 0;
        int parsedHeight = 0;
        if (sscanf(text.c_str() + index, "%dx%d", &parsedWidth, &parsedHeight) == 2 &&
            parsedWidth > 0 && parsedHeight > 0) {
            width = parsedWidth;
            height = parsedHeight;
            return true;
        }
    }
    return false;
}

static bool parseRotationQuarterTurnsFromText(const std::string& text, int& quarterTurns) {
    constexpr const char* keys[] = {
        "SurfaceOrientation:",
        "orientation=",
        "mCurrentOrientation=",
    };

    for (const char* key : keys) {
        const char* found = strstr(text.c_str(), key);
        if (!found) continue;

        int value = 0;
        if (sscanf(found + strlen(key), "%d", &value) == 1 && value >= 0 && value <= 3) {
            quarterTurns = value;
            return true;
        }
    }
    return false;
}

static bool queryShellDisplayInfo(JavaDisplayInfo& outInfo) {
    std::string wmSizeOutput;
    if (!readShellCommandOutput("/system/bin/wm size 2>/dev/null", wmSizeOutput) ||
        !parseDisplaySizeFromText(wmSizeOutput, outInfo.width, outInfo.height)) {
        GERR("queryShellDisplayInfo: wm size 解析失败");
        return false;
    }

    std::string dumpsysInputOutput;
    int quarterTurns = -1;
    if (readShellCommandOutput("/system/bin/dumpsys input 2>/dev/null", dumpsysInputOutput)) {
        parseRotationQuarterTurnsFromText(dumpsysInputOutput, quarterTurns);
    }

    if (quarterTurns >= 0) {
        outInfo.rotateTheta = quarterTurns * 90;
    } else if (outInfo.width < outInfo.height) {
        outInfo.rotateTheta = 90;
    } else {
        outInfo.rotateTheta = 0;
    }

    GLOG("queryShellDisplayInfo: width=%d height=%d rotate=%d", outInfo.width, outInfo.height, outInfo.rotateTheta);
    return outInfo.width > 0 && outInfo.height > 0;
}

static bool resolveDisplayInfo(JavaDisplayInfo& outInfo) {
    if (queryJavaDisplayInfo(outInfo)) {
        return true;
    }

    GLOG("resolveDisplayInfo: Java 查询失败, 尝试 shell 回退");
    if (queryShellDisplayInfo(outInfo)) {
        return true;
    }

    outInfo.width = 2400;
    outInfo.height = 1080;
    outInfo.rotateTheta = 0;
    GERR("resolveDisplayInfo: shell 查询失败, 使用兜底尺寸 width=%d height=%d rotate=%d",
         outInfo.width,
         outInfo.height,
         outInfo.rotateTheta);
    return true;
}

static void runSurfacePreflight(const JavaDisplayInfo& displayInfo) {
    try {
        GLOG("AImGui 预检: GetDisplayInfo begin");
        const auto creatorDisplay = android::ANativeWindowCreator::GetDisplayInfo();
        GLOG("AImGui 预检: GetDisplayInfo result width=%d height=%d rotate=%d",
             creatorDisplay.width,
             creatorDisplay.height,
             creatorDisplay.theta);

        GLOG("AImGui 预检: Create begin width=%d height=%d", displayInfo.width, displayInfo.height);
        ANativeWindow* preflightWindow = android::ANativeWindowCreator::Create({
            .name = "UE4-AImGui-Preflight",
            .width = displayInfo.width,
            .height = displayInfo.height,
            .skipScreenshot = true,
        });
        if (!preflightWindow) {
            GERR("AImGui 预检: Create 返回空窗口");
            return;
        }

        GLOG("AImGui 预检: Create 成功 window=%p size=%dx%d",
             preflightWindow,
             ANativeWindow_getWidth(preflightWindow),
             ANativeWindow_getHeight(preflightWindow));

        ANativeWindow_acquire(preflightWindow);
        ANativeWindow_release(preflightWindow);
        android::ANativeWindowCreator::Destroy(preflightWindow);
        GLOG("AImGui 预检: Destroy 完成");
    } catch (const std::exception& exception) {
        GERR("AImGui 预检异常: %s", exception.what());
    } catch (...) {
        GERR("AImGui 预检异常: 未知异常");
    }
}

static bool queryJavaDisplayInfo(JavaDisplayInfo& outInfo) {
    JavaVM* vm = toast_util::getJavaVM();
    if (!vm) {
        GERR("queryJavaDisplayInfo: 无法获取 JavaVM");
        return false;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    const jint stat = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (stat == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            GERR("queryJavaDisplayInfo: AttachCurrentThread 失败");
            return false;
        }
        attached = true;
    }
    if (!env) {
        GERR("queryJavaDisplayInfo: JNIEnv 为空");
        return false;
    }

    bool ok = false;
    jclass atClass = nullptr;
    jobject ctx = nullptr;
    jclass ctxClass = nullptr;
    jobject wm = nullptr;
    jclass displayMetricsClass = nullptr;
    jobject dm = nullptr;
    jobject display = nullptr;
    jclass displayClass = nullptr;

    do {
        atClass = env->FindClass("android/app/ActivityThread");
        if (!atClass || env->ExceptionCheck()) break;
        jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication", "()Landroid/app/Application;");
        if (!curApp || env->ExceptionCheck()) break;
        ctx = env->CallStaticObjectMethod(atClass, curApp);
        if (!ctx || env->ExceptionCheck()) break;

        ctxClass = env->GetObjectClass(ctx);
        if (!ctxClass || env->ExceptionCheck()) break;
        jmethodID getSysSvc = env->GetMethodID(ctxClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        if (!getSysSvc || env->ExceptionCheck()) break;
        jstring wmName = env->NewStringUTF("window");
        wm = env->CallObjectMethod(ctx, getSysSvc, wmName);
        env->DeleteLocalRef(wmName);
        if (!wm || env->ExceptionCheck()) break;

        displayMetricsClass = env->FindClass("android/util/DisplayMetrics");
        if (!displayMetricsClass || env->ExceptionCheck()) break;
        jmethodID dmInit = env->GetMethodID(displayMetricsClass, "<init>", "()V");
        if (!dmInit || env->ExceptionCheck()) break;
        dm = env->NewObject(displayMetricsClass, dmInit);
        if (!dm || env->ExceptionCheck()) break;

        jclass wmClass = env->FindClass("android/view/WindowManager");
        if (!wmClass || env->ExceptionCheck()) break;
        jmethodID getDisplay = env->GetMethodID(wmClass, "getDefaultDisplay", "()Landroid/view/Display;");
        if (!getDisplay || env->ExceptionCheck()) {
            env->DeleteLocalRef(wmClass);
            break;
        }
        display = env->CallObjectMethod(wm, getDisplay);
        env->DeleteLocalRef(wmClass);
        if (!display || env->ExceptionCheck()) break;

        displayClass = env->GetObjectClass(display);
        if (!displayClass || env->ExceptionCheck()) break;
        jmethodID getRealMetrics = env->GetMethodID(displayClass, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V");
        if (!getRealMetrics || env->ExceptionCheck()) break;
        env->CallVoidMethod(display, getRealMetrics, dm);
        if (env->ExceptionCheck()) break;

        jfieldID widthField = env->GetFieldID(displayMetricsClass, "widthPixels", "I");
        jfieldID heightField = env->GetFieldID(displayMetricsClass, "heightPixels", "I");
        if (!widthField || !heightField || env->ExceptionCheck()) break;
        outInfo.width = env->GetIntField(dm, widthField);
        outInfo.height = env->GetIntField(dm, heightField);

        jmethodID getRotation = env->GetMethodID(displayClass, "getRotation", "()I");
        if (getRotation && !env->ExceptionCheck()) {
            outInfo.rotateTheta = env->CallIntMethod(display, getRotation) * 90;
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                outInfo.rotateTheta = 0;
            }
        }

        ok = outInfo.width > 0 && outInfo.height > 0;
    } while (false);

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (displayClass) env->DeleteLocalRef(displayClass);
    if (display) env->DeleteLocalRef(display);
    if (dm) env->DeleteLocalRef(dm);
    if (displayMetricsClass) env->DeleteLocalRef(displayMetricsClass);
    if (wm) env->DeleteLocalRef(wm);
    if (ctxClass) env->DeleteLocalRef(ctxClass);
    if (ctx) env->DeleteLocalRef(ctx);
    if (atClass) env->DeleteLocalRef(atClass);
    if (attached) vm->DetachCurrentThread();

    if (ok) {
        GLOG("queryJavaDisplayInfo: width=%d height=%d rotate=%d", outInfo.width, outInfo.height, outInfo.rotateTheta);
    } else {
        GERR("queryJavaDisplayInfo: 获取显示信息失败");
    }
    return ok;
}

// =====================================================================
//  UE4 GUI 线程 — 使用 AImGui 独立 Surface 绘制 (无 Hook)
// =====================================================================

static std::atomic<bool> g_ue4GuiThreadStarted{false};

namespace {
using Clock = std::chrono::steady_clock;

bool shouldLogEvery(Clock::time_point& lastLogTime, std::chrono::milliseconds interval) {
    const auto now = Clock::now();
    if (lastLogTime.time_since_epoch().count() != 0 && now - lastLogTime < interval) {
        return false;
    }
    lastLogTime = now;
    return true;
}

class UE4GuiThreadResetGuard {
public:
    UE4GuiThreadResetGuard() = default;
    UE4GuiThreadResetGuard(const UE4GuiThreadResetGuard&) = delete;
    UE4GuiThreadResetGuard& operator=(const UE4GuiThreadResetGuard&) = delete;
    ~UE4GuiThreadResetGuard() {
        g_ue4GuiThreadStarted.store(false, std::memory_order_release);
        GLOG("GUI 线程已退出, 释放单例锁");
    }
};
} // namespace

static void UE4GuiThread() {
    UE4GuiThreadResetGuard resetGuard;

    GLOG("GUI 线程已启动, 等待 10 秒...");
    std::this_thread::sleep_for(std::chrono::seconds(10));

    JavaDisplayInfo displayInfo;
    if (!resolveDisplayInfo(displayInfo)) {
        GERR("GUI 线程终止: 无法获取显示信息");
        return;
    }
    normalizePublicOverlayDisplayInfo(displayInfo);

    GLOG("准备初始化 AImGui RenderClient: width=%d height=%d rotate=%d", displayInfo.width, displayInfo.height, displayInfo.rotateTheta);

    android::AImGui::Options imguiOptions{
        .renderType = android::AImGui::RenderType::RenderClient,
        .compressionFrameData = false,
        .autoUpdateOrientation = false,
        .exchangeFontData = true,
        .tcpNoDelay = true,
        .disableVsync = true,
        .styleScale = 1.75f,
        .fontSizePixels = 18.0f,
        .screenWidth = displayInfo.width,
        .screenHeight = displayInfo.height,
        .rotateTheta = displayInfo.rotateTheta,
        .clientConnectAddress = "127.0.0.1",
    };

    GLOG("AImGui RenderClient 选项已准备: width=%d height=%d rotate=%d", imguiOptions.screenWidth, imguiOptions.screenHeight, imguiOptions.rotateTheta);

    std::unique_ptr<android::AImGui> imgui;
    for (int attempt = 1; attempt <= 20; ++attempt) {
        try {
            imgui = std::make_unique<android::AImGui>(imguiOptions);
            GLOG("AImGui RenderClient 构造已返回: attempt=%d state=%d", attempt, *imgui ? 1 : 0);
        } catch (const std::exception& exception) {
            GERR("AImGui RenderClient 构造异常: attempt=%d error=%s", attempt, exception.what());
            imgui.reset();
        } catch (...) {
            GERR("AImGui RenderClient 构造异常: attempt=%d error=unknown", attempt);
            imgui.reset();
        }

        if (imgui && *imgui) {
            break;
        }

        GLOG("等待公开 Overlay 服务就绪: attempt=%d/20", attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!imgui || !(*imgui)) {
        GERR("AImGui RenderClient 初始化失败: 无法连接公开 Overlay 服务");
        return;
    }

    GLOG("AImGui RenderClient 初始化完成");

    // RenderClient 的输入线程: ProcessInputEvent 通过 TCP 阻塞读取输入事件,
    // 不能放在渲染线程中 (poll 最长 1s 会阻塞帧循环)。
    std::atomic<bool> inputThreadRunning{true};
    std::thread inputThread([imguiPtr = imgui.get(), &inputThreadRunning]() {
        GLOG("RenderClient 输入线程启动");
        while (inputThreadRunning.load(std::memory_order_acquire)) {
            // ProcessInputEvent 内部: ReadData(poll) → AddMousePosEvent 等
            imguiPtr->ProcessInputEvent();
        }
        GLOG("RenderClient 输入线程退出");
    });

    ue4draw::DrawGameData gameData;
    ue4draw::UE4Overlay overlay;
    Clock::time_point lastHeartbeatLog;
    Clock::time_point lastDisplayRefresh;
    JavaDisplayInfo activeDisplayInfo = displayInfo;

    // 检测游戏目标进程是否存活 (每 2 秒检查一次)
    auto isGameProcessAlive = []() -> bool {
        // 检查自身进程的 cmdline 是否还包含游戏包名
        // 如果进程被 kill, 这个函数不会执行, 但如果是温和退出可以检测到
        char cmdline[256] = {};
        int fd = open("/proc/self/cmdline", O_RDONLY);
        if (fd < 0) return false;
        ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
        if (n <= 0) return false;
        return strstr(cmdline, "pubgmhd") != nullptr;
    };
    Clock::time_point lastAliveCheck;

    // 渲染主循环
    while (true) {
        // 每 2 秒检查游戏进程是否存活, 不存活则发送空帧后退出
        if (shouldLogEvery(lastAliveCheck, std::chrono::milliseconds(2000))) {
            if (!isGameProcessAlive()) {
                GLOG("RenderClient: 游戏进程已退出, 发送空帧清除 overlay");
                inputThreadRunning.store(false, std::memory_order_release);
                for (int i = 0; i < 3; i++) {
                    imgui->BeginFrame();
                    imgui->EndFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                GLOG("RenderClient: 渲染循环退出");
                break;
            }
        }

        imgui->BeginFrame();

        if (shouldLogEvery(lastDisplayRefresh, std::chrono::milliseconds(1000))) {
            JavaDisplayInfo refreshedDisplayInfo;
            if (resolveDisplayInfo(refreshedDisplayInfo)) {
                normalizePublicOverlayDisplayInfo(refreshedDisplayInfo);
                if (refreshedDisplayInfo.width != activeDisplayInfo.width
                    || refreshedDisplayInfo.height != activeDisplayInfo.height
                    || refreshedDisplayInfo.rotateTheta != activeDisplayInfo.rotateTheta) {
                    GLOG("RenderClient 显示尺寸刷新: %dx%d r%d -> %dx%d r%d",
                         activeDisplayInfo.width,
                         activeDisplayInfo.height,
                         activeDisplayInfo.rotateTheta,
                         refreshedDisplayInfo.width,
                         refreshedDisplayInfo.height,
                         refreshedDisplayInfo.rotateTheta);
                    activeDisplayInfo = refreshedDisplayInfo;
                }
            }
        }

        {
            ImGuiIO& io = ImGui::GetIO();
            if (activeDisplayInfo.width > 0 && activeDisplayInfo.height > 0) {
                io.DisplaySize = ImVec2(static_cast<float>(activeDisplayInfo.width), static_cast<float>(activeDisplayInfo.height));
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            }
        }

        ue4draw::SharedUE4Data::getInstance().getData(gameData);
        overlay.drawOverlay(gameData);

        if (shouldLogEvery(lastHeartbeatLog, std::chrono::milliseconds(3000))) {
            const ImGuiIO& io = ImGui::GetIO();
            GLOG("RenderClient 心跳: display=%.0fx%.0f fps=%.1f inMatch=%d alive=%d/%d tracked=%zu",
                 io.DisplaySize.x,
                 io.DisplaySize.y,
                 io.Framerate,
                 gameData.inMatch ? 1 : 0,
                 gameData.aliveCount,
                 gameData.totalCount,
                 gameData.players.size());
        }

        imgui->EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    inputThreadRunning.store(false, std::memory_order_release);
    if (inputThread.joinable()) inputThread.join();
}

static void UE4WorkerThread(void* plibUE4ModeBase, void* pGNames,
                            void* pGWorld, void* pGUObjectArray, uint64_t moduleSize, void* pData) {
    LOG(LOG_LEVEL_INFO, "[UE4Worker] 工作线程启动");
    LOG(LOG_LEVEL_INFO, "[UE4Worker] libUE4Base=%p GNames=%p GWorld=%p GUObjectArray=%p moduleSize=0x%llX",
        plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray, (unsigned long long)moduleSize);

    // 读取配置文件中的日志开关
    g_runtimeLogEnabled = readLogEnabled();

    // 对局状态监控 + 玩家坐标采集 (额外线程自动启动)
    std::thread([=]() {
        LOG(LOG_LEVEL_INFO, "[UE4Worker] 初始化 UE4Interface 并启动对局监控");

        // 等待游戏引擎完成初始化, GNames/GWorld/GUObjectArray 尚未稳定时直接访问会崩溃
        // 通过 /proc/self/mem 安全探测 (不安装信号处理器, 不与 UE4Dumper 冲突)
        LOG(LOG_LEVEL_INFO, "[UE4Worker] 等待游戏引擎就绪...");
        {
            uintptr_t gNamesAddr = reinterpret_cast<uintptr_t>(pGNames);
            uintptr_t gWorldAddr = reinterpret_cast<uintptr_t>(pGWorld);
            constexpr int kMaxWaitSeconds = 120;
            int memFd = open("/proc/self/mem", O_RDONLY);
            bool ready = false;
            for (int i = 0; i < kMaxWaitSeconds * 2; i++) {
                int32_t numNames = 0;
                uintptr_t worldPtr = 0;
                bool namesOk = false, worldOk = false;
                if (memFd >= 0 && gNamesAddr >= 0x10000) {
                    // TNameEntryArray.NumElements @ +0x1400
                    if (pread(memFd, &numNames, sizeof(numNames), static_cast<off_t>(gNamesAddr + 0x1400)) == sizeof(numNames)) {
                        namesOk = (numNames > 100);
                    }
                }
                if (memFd >= 0 && gWorldAddr >= 0x10000) {
                    if (pread(memFd, &worldPtr, sizeof(worldPtr), static_cast<off_t>(gWorldAddr)) == sizeof(worldPtr)) {
                        worldOk = (worldPtr >= 0x10000);
                    }
                }
                if (namesOk && worldOk) {
                    LOG(LOG_LEVEL_INFO, "[UE4Worker] 引擎就绪! numNames=%d worldPtr=%p (等待了 %.1fs)",
                        numNames, (void*)worldPtr, i * 0.5f);
                    ready = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (memFd >= 0) close(memFd);
            if (!ready) {
                LOG(LOG_LEVEL_ERROR, "[UE4Worker] 引擎等待超时 (%ds), 放弃启动监控", kMaxWaitSeconds);
                return;
            }
        }

        // 额外等待 5 秒让引擎完全稳定 (避免 GUObjectArray 正在扩容)
        std::this_thread::sleep_for(std::chrono::seconds(5));

        // ---- UE4Dumper 导出 / UE4Header 生成 (复用引擎等待, 不另起线程) ----
        if (readUeDumperEnabled()) {
            LOG(LOG_LEVEL_INFO, "[UE4Worker] ue_dumper 已启用, 开始 dump");
            ue4::UE4Dumper dumpOnly(
                reinterpret_cast<uintptr_t>(plibUE4ModeBase),
                reinterpret_cast<uint64_t>(pGNames),
                reinterpret_cast<uint64_t>(pGUObjectArray),
                reinterpret_cast<uint64_t>(pGWorld),
                static_cast<uintptr_t>(moduleSize),
                "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/"
            );
            if (!dumpOnly.init()) {
                LOG(LOG_LEVEL_ERROR, "[UE4Worker] UE4Dumper 初始化失败");
                toast_util::showToast("UE4Dumper 初始化失败");
            } else {
                LOG(LOG_LEVEL_INFO, "[UE4Worker] UE4Dumper 初始化成功, NumNames=%d", dumpOnly.getNumNames());
                if (dumpOnly.dumpAll()) {
                    LOG(LOG_LEVEL_INFO, "[UE4Worker] dump 全部完成");
                    toast_util::showToast("UE4 Dump 完成");
                } else {
                    LOG(LOG_LEVEL_ERROR, "[UE4Worker] dump 部分失败");
                    toast_util::showToast("UE4 Dump 部分失败");
                }
            }
        }
        if (readUeHeaderEnabled()) {
            LOG(LOG_LEVEL_INFO, "[UE4Worker] ue_header 已启用, 开始生成");
            ue4::UE4Dumper headerDumper(
                reinterpret_cast<uintptr_t>(plibUE4ModeBase),
                reinterpret_cast<uint64_t>(pGNames),
                reinterpret_cast<uint64_t>(pGUObjectArray),
                reinterpret_cast<uint64_t>(pGWorld),
                static_cast<uintptr_t>(moduleSize),
                "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/"
            );
            if (!headerDumper.init()) {
                LOG(LOG_LEVEL_ERROR, "[UE4Worker] UE4Header 初始化失败");
                toast_util::showToast("UE4Header 初始化失败");
            } else {
                ue4::UE4Header header(headerDumper, "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/");
                header.start();
                toast_util::showToast("UE4 Header 生成完成");
            }
        }

        // 1. 创建 UE4Dumper 并初始化 (用于对局监控的反射解析)
        auto* dumper = new ue4::UE4Dumper(
            reinterpret_cast<uintptr_t>(plibUE4ModeBase),
            reinterpret_cast<uint64_t>(pGNames),
            reinterpret_cast<uint64_t>(pGUObjectArray),
            reinterpret_cast<uint64_t>(pGWorld),
            static_cast<uintptr_t>(moduleSize),
            "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/"
        );
        if (!dumper->init()) {
            LOG(LOG_LEVEL_ERROR, "[UE4Worker] UE4Dumper 初始化失败, 无法启动对局监控");
            toast_util::showToast("UE4Dumper 初始化失败");
            delete dumper;
            return;
        }

        // 2. 创建 UE4Interface 并采集所有类信息
        auto* interface = new ue4inf::UE4Interface(*dumper);
        interface->fillClassInfo();
        LOG(LOG_LEVEL_INFO, "[UE4Worker] UE4Interface 采集完成, 共 %d 个类", interface->getClassCount());

        // 3. 创建 MatchMonitor, 通过 interface 动态解析偏移
        // pGWorld 即 GWorld 全局变量地址 (由 Injector 传入 base+offset)
        auto* monitor = new pubgmhd::MatchMonitor(
            reinterpret_cast<uintptr_t>(plibUE4ModeBase),
            reinterpret_cast<uintptr_t>(pGNames),
            reinterpret_cast<uintptr_t>(pGWorld),
            reinterpret_cast<uintptr_t>(pGUObjectArray),
            moduleSize,
            *interface
        );
        if (!monitor->start()) {
            LOG(LOG_LEVEL_ERROR, "[UE4Worker] MatchMonitor 启动失败");
            toast_util::showToast("MatchMonitor 启动失败");
            delete monitor;
            delete interface;
            delete dumper;
        } else {
            toast_util::showToast("对局监控已启动");
        }
    }).detach();

    LOG(LOG_LEVEL_INFO, "[UE4Worker] 进入主循环");
}

extern "C" __attribute__((visibility("default")))
bool MyStartPointPUBG(void* plibUE4ModeBase, void* pGNames,
                     void* pGWorld, void* pGUObjectArray, uint64_t moduleSize, void* pData) {
    if (!plibUE4ModeBase || !pGNames || !pGWorld || !pGUObjectArray) {
        GERR("MyStartPointPUBG: 参数为空 base=%p GNames=%p GWorld=%p GUObjectArray=%p",
             plibUE4ModeBase,
             pGNames,
             pGWorld,
             pGUObjectArray);
        LOG(LOG_LEVEL_ERROR, "[MyStartPointPUBG] 参数为空: base=%p GNames=%p GWorld=%p GUObjectArray=%p",
            plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray);
        return false;
    }

    // 在启动任何线程之前读取日志开关, 确保 GUI 线程启动时日志已开启
    g_runtimeLogEnabled = readLogEnabled();

    GLOG("MyStartPointPUBG: base=%p GNames=%p GWorld=%p GUObjectArray=%p moduleSize=0x%llX log=%d",
        plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray, (unsigned long long)moduleSize, g_runtimeLogEnabled ? 1 : 0);

    LOG(LOG_LEVEL_INFO, "[MyStartPointPUBG] 启动 UE4 工作线程");

    bool expected = false;
    if (g_ue4GuiThreadStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        GLOG("MyStartPointPUBG: 启动 UE4 GUI 线程");
        LOG(LOG_LEVEL_INFO, "[MyStartPointPUBG] 启动 UE4 GUI 线程");
        std::thread(UE4GuiThread).detach();
    } else {
        GLOG("MyStartPointPUBG: UE4 GUI 线程已存在, 跳过重复启动");
        LOG(LOG_LEVEL_INFO, "[MyStartPointPUBG] UE4 GUI 线程已存在, 跳过重复启动");
    }

    std::thread(UE4WorkerThread, plibUE4ModeBase, pGNames,
                pGWorld, pGUObjectArray, moduleSize, pData).detach();

    return true;
}

OBFU_ATTRS_END
