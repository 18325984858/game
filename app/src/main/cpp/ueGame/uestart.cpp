#include "uestart.h"
#include "../Log/log.h"
#include "libUE4Dumper/UE4Dumper.h"
#include "libUE4Header/UE4Header.h"
#include "interface/interface.h"
#include "pubgmhd/pubgmhd.h"
#include "Draw/UE4Draw.h"
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <dlfcn.h>
#include <jni.h>
#include <stdexcept>
#include <sys/stat.h>
#include <dirent.h>

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

// =====================================================================
//  UE4 GUI 线程 — 使用 AImGui 独立 Surface 绘制 (无 Hook)
// =====================================================================

// 直接日志输出 (绕过可能被 OBFU 覆盖的 LOG 宏)
#define GLOG(...) __android_log_print(ANDROID_LOG_INFO, "UE4-GUI", __VA_ARGS__)
#define GERR(...) __android_log_print(ANDROID_LOG_ERROR, "UE4-GUI", __VA_ARGS__)

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_opengl3.h>
#include <imgui/backends/imgui_impl_android.h>

// =====================================================================
//  通过 JNI 创建悬浮窗覆盖层 (不依赖 libgui.so)
//  使用 WindowManager + SurfaceView 获取 ANativeWindow
// =====================================================================
static ANativeWindow* createOverlayWindow(JNIEnv* env) {
    // 获取 Application context
    jclass atClass = env->FindClass("android/app/ActivityThread");
    if (!atClass) return nullptr;
    jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication", "()Landroid/app/Application;");
    jobject ctx = curApp ? env->CallStaticObjectMethod(atClass, curApp) : nullptr;
    env->DeleteLocalRef(atClass);
    if (!ctx) return nullptr;

    // 获取 WindowManager
    jclass ctxClass = env->GetObjectClass(ctx);
    jmethodID getSysSvc = env->GetMethodID(ctxClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring wmName = env->NewStringUTF("window");
    jobject wm = env->CallObjectMethod(ctx, getSysSvc, wmName);
    env->DeleteLocalRef(wmName);
    env->DeleteLocalRef(ctxClass);
    if (!wm) { env->DeleteLocalRef(ctx); return nullptr; }

    // 获取屏幕尺寸
    jclass displayMetricsClass = env->FindClass("android/util/DisplayMetrics");
    jobject dm = env->NewObject(displayMetricsClass, env->GetMethodID(displayMetricsClass, "<init>", "()V"));
    jclass wmClass = env->FindClass("android/view/WindowManager");
    jmethodID getDisplay = env->GetMethodID(wmClass, "getDefaultDisplay", "()Landroid/view/Display;");
    jobject display = env->CallObjectMethod(wm, getDisplay);
    jclass displayClass = env->GetObjectClass(display);
    jmethodID getRealMetrics = env->GetMethodID(displayClass, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V");
    env->CallVoidMethod(display, getRealMetrics, dm);
    int screenW = env->GetIntField(dm, env->GetFieldID(displayMetricsClass, "widthPixels", "I"));
    int screenH = env->GetIntField(dm, env->GetFieldID(displayMetricsClass, "heightPixels", "I"));
    env->DeleteLocalRef(dm); env->DeleteLocalRef(display); env->DeleteLocalRef(displayClass);
    env->DeleteLocalRef(displayMetricsClass); env->DeleteLocalRef(wmClass);
    GLOG("屏幕尺寸: %dx%d", screenW, screenH);

    // 创建 SurfaceView
    jclass surfaceViewClass = env->FindClass("android/view/SurfaceView");
    jmethodID svInit = env->GetMethodID(surfaceViewClass, "<init>", "(Landroid/content/Context;)V");
    jobject surfaceView = env->NewObject(surfaceViewClass, svInit, ctx);

    // 设置透明背景
    jmethodID setZOrderOnTop = env->GetMethodID(surfaceViewClass, "setZOrderOnTop", "(Z)V");
    env->CallVoidMethod(surfaceView, setZOrderOnTop, JNI_TRUE);
    jmethodID getHolder = env->GetMethodID(surfaceViewClass, "getHolder", "()Landroid/view/SurfaceHolder;");
    jobject holder = env->CallObjectMethod(surfaceView, getHolder);
    jclass holderClass = env->GetObjectClass(holder);
    jmethodID setFormat = env->GetMethodID(holderClass, "setFormat", "(I)V");
    env->CallVoidMethod(holder, setFormat, -3); // PixelFormat.TRANSLUCENT

    // 创建 LayoutParams (TYPE_APPLICATION_OVERLAY)
    jclass lpClass = env->FindClass("android/view/WindowManager$LayoutParams");
    jmethodID lpInit = env->GetMethodID(lpClass, "<init>", "(IIIII)V");
    // TYPE_APPLICATION_OVERLAY = 2038, FLAG_NOT_FOCUSABLE|FLAG_NOT_TOUCHABLE|FLAG_LAYOUT_IN_SCREEN = 0x8|0x10|0x100
    jobject lp = env->NewObject(lpClass, lpInit, screenW, screenH, 2038, 0x8 | 0x10 | 0x100, -3);

    // 设置 gravity = TOP | LEFT
    jfieldID gravityField = env->GetFieldID(lpClass, "gravity", "I");
    env->SetIntField(lp, gravityField, 0x30 | 0x03); // Gravity.TOP | Gravity.LEFT

    // 添加到 WindowManager
    jclass wmIfClass = env->FindClass("android/view/ViewManager");
    jmethodID addView = env->GetMethodID(wmIfClass, "addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    env->CallVoidMethod(wm, addView, surfaceView, lp);

    if (env->ExceptionCheck()) {
        GERR("创建悬浮窗异常");
        env->ExceptionDescribe();
        env->ExceptionClear();
        env->DeleteLocalRef(surfaceView); env->DeleteLocalRef(holder);
        env->DeleteLocalRef(lp); env->DeleteLocalRef(lpClass);
        env->DeleteLocalRef(wm); env->DeleteLocalRef(ctx);
        return nullptr;
    }

    // 等待 Surface 就绪
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 获取 Surface -> ANativeWindow
    jmethodID getSurface = env->GetMethodID(holderClass, "getSurface", "()Landroid/view/Surface;");
    jobject surface = env->CallObjectMethod(holder, getSurface);
    if (!surface) {
        GERR("getSurface 返回 null");
        env->DeleteLocalRef(holder); env->DeleteLocalRef(holderClass);
        env->DeleteLocalRef(surfaceView); env->DeleteLocalRef(lp);
        env->DeleteLocalRef(wm); env->DeleteLocalRef(ctx);
        return nullptr;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    GLOG("ANativeWindow: %p", window);

    env->DeleteLocalRef(surface); env->DeleteLocalRef(holder);
    env->DeleteLocalRef(holderClass); env->DeleteLocalRef(surfaceView);
    env->DeleteLocalRef(lp); env->DeleteLocalRef(lpClass);
    env->DeleteLocalRef(wmIfClass); env->DeleteLocalRef(wm); env->DeleteLocalRef(ctx);
    return window;
}

static void UE4GuiThread() {
    GLOG("GUI 线程已启动, 等待 10 秒...");
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 获取 JVM (直接调用 JNI_GetCreatedJavaVMs)
    JavaVM* vm = nullptr;
    jsize vmCount = 0;
    // 先用 toast_util 的缓存
    vm = toast_util::getJavaVM();
    if (!vm) {
        // 直接从 libart.so 获取 (尝试多个路径)
        const char* artPaths[] = {
            "libart.so",
            "/apex/com.android.art/lib64/libart.so",
            "/system/lib64/libart.so",
        };
        void* art = nullptr;
        for (auto* p : artPaths) {
            art = dlopen(p, RTLD_NOW | RTLD_NOLOAD);
            if (art) break;
            art = dlopen(p, RTLD_NOW);
            if (art) break;
        }
        if (art) {
            auto fn = reinterpret_cast<jint(*)(JavaVM**, jsize, jsize*)>(dlsym(art, "JNI_GetCreatedJavaVMs"));
            if (fn) {
                JavaVM* vms[2] = {};
                fn(vms, 2, &vmCount);
                if (vmCount > 0) vm = vms[0];
            }
            GLOG("从 libart.so 获取 JVM: fn=%p vmCount=%d vm=%p", (void*)fn, vmCount, vm);
        } else {
            GLOG("dlopen libart.so 失败: %s", dlerror());
        }
    }
    if (!vm) { GERR("无法获取 JavaVM"); return; }

    JNIEnv* env = nullptr;
    bool attached = false;
    jint stat = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (stat == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) { GERR("AttachCurrentThread 失败"); return; }
        attached = true;
    }
    if (!env) { GERR("JNIEnv 为空"); return; }

    GLOG("正在通过 JNI 创建悬浮窗...");
    ANativeWindow* window = createOverlayWindow(env);
    if (!window) {
        GERR("创建悬浮窗失败, 绘制功能不可用");
        if (attached) vm->DetachCurrentThread();
        return;
    }
    GLOG("悬浮窗创建成功");

    // 初始化 EGL
    EGLDisplay eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDisplay, nullptr, nullptr);

    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig eglConfig; EGLint numConfig;
    eglChooseConfig(eglDisplay, configAttribs, &eglConfig, 1, &numConfig);

    EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext eglCtx = eglCreateContext(eglDisplay, eglConfig, EGL_NO_CONTEXT, ctxAttribs);

    EGLint bufFormat;
    eglGetConfigAttrib(eglDisplay, eglConfig, EGL_NATIVE_VISUAL_ID, &bufFormat);
    ANativeWindow_setBuffersGeometry(window, 0, 0, bufFormat);
    EGLSurface eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig, window, nullptr);

    eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglCtx);

    int screenW = ANativeWindow_getWidth(window);
    int screenH = ANativeWindow_getHeight(window);
    GLOG("EGL 初始化完成: %dx%d", screenW, screenH);

    // 初始化 ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)screenW, (float)screenH);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(3.f);

    // 加载字体 (默认)
    io.Fonts->AddFontDefault();
    io.Fonts->Build();

    ImGui_ImplOpenGL3_Init("#version 300 es");
    GLOG("ImGui 初始化完成");

    ue4draw::DrawGameData gameData;
    ue4draw::UE4Overlay overlay;
    double lastTime = 0.0;

    // 渲染主循环
    while (true) {
        // 时间
        timespec ts{};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
        io.DeltaTime = lastTime > 0.0 ? (float)(now - lastTime) : (1.0f / 60.0f);
        lastTime = now;

        // 检查窗口尺寸变化
        int curW = ANativeWindow_getWidth(window);
        int curH = ANativeWindow_getHeight(window);
        if (curW > 0 && curH > 0) {
            io.DisplaySize = ImVec2((float)curW, (float)curH);
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();

        ue4draw::SharedUE4Data::getInstance().getData(gameData);
        overlay.drawOverlay(gameData);

        ImGui::Render();
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        eglSwapBuffers(eglDisplay, eglSurface);

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

static void UE4WorkerThread(void* plibUE4ModeBase, void* pGNames,
                            void* pGWorld, void* pGUObjectArray, uint64_t moduleSize, void* pData) {
    LOG(LOG_LEVEL_INFO, "[UE4Worker] 工作线程启动");
    LOG(LOG_LEVEL_INFO, "[UE4Worker] libUE4Base=%p GNames=%p GWorld=%p GUObjectArray=%p moduleSize=0x%llX",
        plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray, (unsigned long long)moduleSize);

    // 检查界面上的 "启用 ueDumper (导出 dump)" 按钮状态
    if (readUeDumperEnabled()) {
        LOG(LOG_LEVEL_INFO, "[UE4Worker] ue_dumper 已启用, 创建 dump 线程");
        std::thread([=]() {
            ue4::UE4Dumper dumper(
                reinterpret_cast<uintptr_t>(plibUE4ModeBase),
                reinterpret_cast<uint64_t>(pGNames),
                reinterpret_cast<uint64_t>(pGUObjectArray),
                reinterpret_cast<uint64_t>(pGWorld),
                static_cast<uintptr_t>(moduleSize),
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

    // 检查 ue_header 开关: 生成 IDA 头文件和脚本
    if (readUeHeaderEnabled()) {
        LOG(LOG_LEVEL_INFO, "[UE4Worker] ue_header 已启用, 创建 header 生成线程");
        std::thread([=]() {
            ue4::UE4Dumper dumper(
                reinterpret_cast<uintptr_t>(plibUE4ModeBase),
                reinterpret_cast<uint64_t>(pGNames),
                reinterpret_cast<uint64_t>(pGUObjectArray),
                reinterpret_cast<uint64_t>(pGWorld),
                static_cast<uintptr_t>(moduleSize),
                "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/"
            );
            if (!dumper.init()) {
                LOG(LOG_LEVEL_ERROR, "[UE4Worker] UE4Header 初始化失败");
                toast_util::showToast("UE4Header 初始化失败");
            } else {
                ue4::UE4Header header(dumper, "/data/data/com.tencent.tmgp.pubgmhd/cache/ue4_dump/");
                header.start();
                toast_util::showToast("UE4 Header 生成完成");
            }
        }).detach();
    } else {
        LOG(LOG_LEVEL_INFO, "[UE4Worker] ue_header 未启用, 跳过");
    }

    // 对局状态监控 + 玩家坐标采集 (额外线程自动启动)
    std::thread([=]() {
        LOG(LOG_LEVEL_INFO, "[UE4Worker] 初始化 UE4Interface 并启动对局监控");

        // 1. 创建 UE4Dumper 并初始化
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
        // pGWorld 已在 MyStartPointUE4 入口处计算为 GWorld 全局变量地址
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
bool MyStartPointUE4(void* plibUE4ModeBase, void* pGNames,
                     void* pGWorld, void* pGUObjectArray, uint64_t moduleSize, void* pData) {
    if (!plibUE4ModeBase || !pGNames || !pGWorld || !pGUObjectArray) {
        LOG(LOG_LEVEL_ERROR, "[MyStartPointUE4] 参数为空: base=%p GNames=%p GWorld=%p GUObjectArray=%p",
            plibUE4ModeBase, pGNames, pGWorld, pGUObjectArray);
        return false;
    }

    // 在入口处计算 GWorld 全局变量地址 (偏移只在这里使用)
    // pGWorld 是注入时的 UWorld* 快照, 会随地图切换失效
    // GWorld 全局变量地址 = base + 0x14988578, 每次读取都能获徖当前 UWorld*
    uintptr_t base = reinterpret_cast<uintptr_t>(plibUE4ModeBase);
    void* pGWorldGlobal = reinterpret_cast<void*>(base + 0x14988578);

    LOG(LOG_LEVEL_INFO, "MatchMonitor [MyStartPointUE4] 参数: base=%p GNames=%p GWorld=%p(全局=%p) GUObjectArray=%p moduleSize=0x%llX",
            plibUE4ModeBase, pGNames, pGWorld, pGWorldGlobal, pGUObjectArray, (unsigned long long)moduleSize);

    LOG(LOG_LEVEL_INFO, "[MyStartPointUE4] 启动 UE4 工作线程");

    // 启动 GUI 线程 (独立 Surface 绘制, 无 Hook)
    std::thread(UE4GuiThread).detach();

    // 传递 pGWorldGlobal (全局变量地址) 而非 pGWorld (快照值)
    std::thread(UE4WorkerThread, plibUE4ModeBase, pGNames,
                pGWorldGlobal, pGUObjectArray, moduleSize, pData).detach();

    return true;
}

OBFU_ATTRS_END
