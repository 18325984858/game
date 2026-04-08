#include <jni.h>
#include <android/native_window_jni.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#include "../Log/log.h"
#include "../AndroidSurfaceImgui/includes/AImGui.h"

#define OLOG(level, fmt, ...) LOG(level, fmt, ##__VA_ARGS__)

namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_overlayMutex;
std::thread g_overlayThread;
std::atomic<bool> g_overlayStopRequested{false};
std::atomic<bool> g_overlayRunning{false};

bool shouldLogEvery(Clock::time_point& lastLogTime, std::chrono::milliseconds interval) {
    const auto now = Clock::now();
    if (lastLogTime.time_since_epoch().count() != 0 && now - lastLogTime < interval) {
        return false;
    }
    lastLogTime = now;
    return true;
}

void stopRendererLocked() {
    g_overlayStopRequested.store(true, std::memory_order_release);
    if (g_overlayThread.joinable()) {
        g_overlayThread.join();
    }
    g_overlayRunning.store(false, std::memory_order_release);
}

void overlayThreadMain(ANativeWindow* window, int width, int height, int rotateTheta) {
    struct RunningReset {
        ~RunningReset() {
            g_overlayRunning.store(false, std::memory_order_release);
        }
    } runningReset;

    try {
        android::AImGui::Options options{
            .renderType = android::AImGui::RenderType::RenderServer,
            .compressionFrameData = false,
            .autoUpdateOrientation = false,
            .exchangeFontData = true,
            .tcpNoDelay = true,
            .disableVsync = true,
            .styleScale = 1.75f,
            .fontSizePixels = 18.0f,
            .screenWidth = width,
            .screenHeight = height,
            .rotateTheta = rotateTheta,
            .externalNativeWindow = window,
            .serverListenAddress = "127.0.0.1",
        };

        OLOG(LOG_LEVEL_INFO, "公开 Overlay 渲染线程启动 width=%d height=%d rotate=%d window=%p", width, height, rotateTheta, window);

        std::unique_ptr<android::AImGui> imgui;
        imgui = std::make_unique<android::AImGui>(options);
        if (!imgui || !(*imgui)) {
            OLOG(LOG_LEVEL_ERROR, "公开 Overlay 初始化失败");
            return;
        }

        std::thread inputThread([imguiPtr = imgui.get()]() {
            OLOG(LOG_LEVEL_INFO, "公开 Overlay 输入线程启动");
            while (!g_overlayStopRequested.load(std::memory_order_acquire)) {
                imguiPtr->ProcessInputEvent();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            OLOG(LOG_LEVEL_INFO, "公开 Overlay 输入线程退出");
        });

        Clock::time_point lastHeartbeatLog;
        while (!g_overlayStopRequested.load(std::memory_order_acquire)) {
            imgui->BeginFrame();
            imgui->EndFrame();

            if (shouldLogEvery(lastHeartbeatLog, std::chrono::milliseconds(3000))) {
                OLOG(LOG_LEVEL_INFO, "公开 Overlay 心跳 width=%d height=%d", width, height);
            }

            std::this_thread::yield();
        }

        if (inputThread.joinable()) {
            inputThread.join();
        }

        OLOG(LOG_LEVEL_INFO, "公开 Overlay 渲染线程退出");
    } catch (const std::exception& exception) {
        OLOG(LOG_LEVEL_ERROR, "公开 Overlay 异常: %s", exception.what());
    } catch (...) {
        OLOG(LOG_LEVEL_ERROR, "公开 Overlay 未知异常");
    }
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeStartRenderer(
        JNIEnv* env,
        jclass,
        jobject surface,
        jint width,
    jint height,
    jint rotateTheta) {
    std::lock_guard<std::mutex> lock(g_overlayMutex);

    stopRendererLocked();

    if (nullptr == surface) {
        OLOG(LOG_LEVEL_ERROR, "nativeStartRenderer: surface 为空");
        return JNI_FALSE;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (nullptr == window) {
        OLOG(LOG_LEVEL_ERROR, "nativeStartRenderer: ANativeWindow_fromSurface 失败");
        return JNI_FALSE;
    }

    g_overlayStopRequested.store(false, std::memory_order_release);
    g_overlayRunning.store(true, std::memory_order_release);
    g_overlayThread = std::thread(overlayThreadMain,
                                  window,
                                  static_cast<int>(width),
                                  static_cast<int>(height),
                                  static_cast<int>(rotateTheta));

    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeStopRenderer(
        JNIEnv*,
        jclass) {
    std::lock_guard<std::mutex> lock(g_overlayMutex);
    stopRendererLocked();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeIsRendererRunning(
        JNIEnv*,
        jclass) {
    return g_overlayRunning.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}