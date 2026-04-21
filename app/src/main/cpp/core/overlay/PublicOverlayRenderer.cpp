#include <jni.h>
#include <android/native_window_jni.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>

#include "../../core/log/log.h"
#include "../../AndroidSurfaceImgui/includes/AImGui.h"

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
            .fontSizePixels = 24.0f,
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

        // 注意: ProcessInputEvent 必须与 BeginFrame/EndFrame 在同一线程调用,
        // 否则 ImGui::IO 的 InputEventsQueue 会多线程竞争导致 ImVector 越界崩溃

        Clock::time_point lastHeartbeatLog;
        Clock::time_point lastClientLostLog;
        bool clientWasConnected = false;
        // 渲染主循环 — 只在 Java 端显式 stop 时退出
        // 客户端断开后不退出, 继续 BeginFrame/EndFrame 让 AImGui 内部清屏路径
        // 持续刷掉所有 EGL 后备缓冲区里的旧画面, 同时支持游戏重启后客户端重连
        while (!g_overlayStopRequested.load(std::memory_order_acquire)) {
            // ProcessInputEvent 每次只从 /dev/input 读 1 个 input_event (非阻塞);
            // 一帧触摸事件需要消化 4-10 个 input_event (X/Y/BTN/SYN_REPORT 等),
            // 手指快速滑动时设备会以 >1000 events/s 的速率产生事件。每帧只调一次
            // 会让事件在内核缓冲堆积, 导致点击/拖动响应延迟几十到上百毫秒。
            // 在 RenderServer 模式下 ProcessInputEvent 只做 read+forward, 不触
            // ImGui IO, 多调安全; 单次调用没数据时立即返回 (EAGAIN), 几乎无开销。
            for (int i = 0; i < 256; ++i) {
                imgui->ProcessInputEvent();
            }
            imgui->BeginFrame();
            imgui->EndFrame();

            const bool clientNow = imgui->IsClientConnected();
            if (clientNow) {
                clientWasConnected = true;
            } else if (clientWasConnected) {
                // 客户端断线: 不主动退出, 让 AImGui 持续清屏并等待重连
                if (shouldLogEvery(lastClientLostLog, std::chrono::milliseconds(5000))) {
                    OLOG(LOG_LEVEL_INFO, "公开 Overlay: 客户端已断开, 持续清屏等待重连");
                }
            }

            if (shouldLogEvery(lastHeartbeatLog, std::chrono::milliseconds(3000))) {
                OLOG(LOG_LEVEL_INFO, "公开 Overlay 心跳 width=%d height=%d client=%d", width, height, clientNow ? 1 : 0);
            }

            // 帧节拍: 避免 yield() 导致 CPU 满载空转, 降低系统调度压力
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // 退出前再多渲染几帧空帧, 确保 EGL 多缓冲全部被清空
        for (int i = 0; i < 6; ++i) {
            imgui->BeginFrame();
            imgui->EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
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