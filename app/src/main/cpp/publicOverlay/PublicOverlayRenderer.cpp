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
        bool clientWasConnected = false;          // 追踪 RenderClient 是否曾连接过
        Clock::time_point lastClientSeenTime;     // 最后一次见到 client 连接的时刻
        const auto kClientLostTimeout = std::chrono::seconds(2);   // client 失联超时
        const auto kNoClientStartTimeout = std::chrono::seconds(0); // 启动后从未连接的超时(0=禁用)
        const auto startTime = Clock::now();
        bool exitDueToClientLost = false;
        while (!g_overlayStopRequested.load(std::memory_order_acquire)) {
            imgui->ProcessInputEvent();
            imgui->BeginFrame();
            imgui->EndFrame();

            const auto now = Clock::now();
            const bool clientNow = imgui->IsClientConnected();
            if (clientNow) {
                clientWasConnected = true;
                lastClientSeenTime = now;
            } else if (clientWasConnected) {
                // 曾连接过, 如果失联超时, 主动退出并清屏
                if (lastClientSeenTime.time_since_epoch().count() != 0 &&
                    now - lastClientSeenTime > kClientLostTimeout) {
                    OLOG(LOG_LEVEL_INFO, "公开 Overlay: RenderClient 失联 %lldms, 清除画面并退出",
                         (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - lastClientSeenTime).count());
                    exitDueToClientLost = true;
                    break;
                }
            } else if (kNoClientStartTimeout.count() > 0 &&
                       now - startTime > kNoClientStartTimeout) {
                // 启动后一直没有 client 连接, 超时退出 (默认禁用)
                OLOG(LOG_LEVEL_INFO, "公开 Overlay: 启动后无 client 连接超时, 退出");
                break;
            }

            if (shouldLogEvery(lastHeartbeatLog, std::chrono::milliseconds(3000))) {
                OLOG(LOG_LEVEL_INFO, "公开 Overlay 心跳 width=%d height=%d client=%d", width, height, clientNow ? 1 : 0);
            }

            // 帧节拍: 避免 yield() 导致 CPU 满载空转, 降低系统调度压力
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // 渲染多帧空帧, 确保 Surface 上之前 RenderClient 残留的内容被清除
        // (单帧可能因双缓冲/三缓冲未真正提交到屏幕)
        const int kClearFrames = exitDueToClientLost ? 5 : 2;
        for (int i = 0; i < kClearFrames; ++i) {
            imgui->BeginFrame();
            imgui->EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        OLOG(LOG_LEVEL_INFO, "公开 Overlay 已渲染 %d 帧空帧清除画面", kClearFrames);

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