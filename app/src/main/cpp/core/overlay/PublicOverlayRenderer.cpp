#include <jni.h>
#include <android/native_window_jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "../../core/log/log.h"
#include "../../AndroidSurfaceImgui/includes/AImGui.h"

#define OLOG(level, fmt, ...) LOG(level, fmt, ##__VA_ARGS__)

namespace {

using Clock = std::chrono::steady_clock;

// 每个端口对应一个独立的 RenderServer 实例 (PUBG=16888, DFM=16889, NRC=16890)
struct RendererInstance {
    std::thread thread;
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> running{false};
    // 渲染线程在创建 AImGui 后会赋值, 退出前会清空.
    // JNI 注入触摸通过这个指针调用 InjectExternalTouch.
    // 注: 用 atomic 指针, 读端先 load 再用; stop 路径会先 join 渲染线程,
    //    保证在指针失效前不会再有 inject 调用通过 g_instancesMutex 进入.
    std::atomic<android::AImGui*> imgui{nullptr};
    int port = 0;
};

std::mutex g_instancesMutex;
std::unordered_map<int, std::unique_ptr<RendererInstance>> g_instances;

bool shouldLogEvery(Clock::time_point& lastLogTime, std::chrono::milliseconds interval) {
    const auto now = Clock::now();
    if (lastLogTime.time_since_epoch().count() != 0 && now - lastLogTime < interval) {
        return false;
    }
    lastLogTime = now;
    return true;
}

void overlayThreadMain(RendererInstance* inst, ANativeWindow* window, int width, int height, int rotateTheta) {
    struct RunningReset {
        RendererInstance* inst;
        ~RunningReset() {
            inst->running.store(false, std::memory_order_release);
        }
    } runningReset{inst};

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
            .port = inst->port,
        };

        OLOG(LOG_LEVEL_INFO, "公开 Overlay 渲染线程启动 port=%d width=%d height=%d rotate=%d window=%p",
             inst->port, width, height, rotateTheta, window);

        std::unique_ptr<android::AImGui> imgui;
        imgui = std::make_unique<android::AImGui>(options);
        if (!imgui || !(*imgui)) {
            OLOG(LOG_LEVEL_ERROR, "公开 Overlay 初始化失败 port=%d", inst->port);
            return;
        }
        inst->imgui.store(imgui.get(), std::memory_order_release);
        // RAII: 退出前清零, 避免 JNI 访问已销毁的 AImGui.
        struct ImguiPtrReset {
            RendererInstance* inst;
            ~ImguiPtrReset() { inst->imgui.store(nullptr, std::memory_order_release); }
        } imguiPtrReset{inst};

        Clock::time_point lastHeartbeatLog;
        Clock::time_point lastClientLostLog;
        bool clientWasConnected = false;

        while (!inst->stopRequested.load(std::memory_order_acquire)) {
            for (int i = 0; i < 256; ++i) {
                imgui->ProcessInputEvent();
            }
            imgui->BeginFrame();
            imgui->EndFrame();

            const bool clientNow = imgui->IsClientConnected();
            if (clientNow) {
                clientWasConnected = true;
            } else if (clientWasConnected) {
                if (shouldLogEvery(lastClientLostLog, std::chrono::milliseconds(5000))) {
                    OLOG(LOG_LEVEL_INFO, "公开 Overlay port=%d 客户端已断开, 持续清屏等待重连", inst->port);
                }
            }

            if (shouldLogEvery(lastHeartbeatLog, std::chrono::milliseconds(3000))) {
                OLOG(LOG_LEVEL_INFO, "公开 Overlay 心跳 port=%d width=%d height=%d client=%d",
                     inst->port, width, height, clientNow ? 1 : 0);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        for (int i = 0; i < 6; ++i) {
            imgui->BeginFrame();
            imgui->EndFrame();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        OLOG(LOG_LEVEL_INFO, "公开 Overlay 渲染线程退出 port=%d", inst->port);
    } catch (const std::exception& exception) {
        OLOG(LOG_LEVEL_ERROR, "公开 Overlay 异常 port=%d: %s", inst->port, exception.what());
    } catch (...) {
        OLOG(LOG_LEVEL_ERROR, "公开 Overlay 未知异常 port=%d", inst->port);
    }
}

// 调用方必须持有 g_instancesMutex
void stopInstanceLocked(RendererInstance* inst) {
    if (!inst) return;
    inst->stopRequested.store(true, std::memory_order_release);
    if (inst->thread.joinable()) {
        inst->thread.join();
    }
    inst->running.store(false, std::memory_order_release);
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeStartRenderer(
        JNIEnv* env,
        jclass,
        jobject surface,
        jint width,
        jint height,
        jint rotateTheta,
        jint port) {
    if (port <= 0) {
        OLOG(LOG_LEVEL_ERROR, "nativeStartRenderer: 非法 port=%d", port);
        return JNI_FALSE;
    }

    std::lock_guard<std::mutex> lock(g_instancesMutex);

    // 同端口已存在则先停掉
    auto it = g_instances.find(port);
    if (it != g_instances.end()) {
        stopInstanceLocked(it->second.get());
        g_instances.erase(it);
    }

    if (nullptr == surface) {
        OLOG(LOG_LEVEL_ERROR, "nativeStartRenderer: surface 为空 port=%d", port);
        return JNI_FALSE;
    }

    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    if (nullptr == window) {
        OLOG(LOG_LEVEL_ERROR, "nativeStartRenderer: ANativeWindow_fromSurface 失败 port=%d", port);
        return JNI_FALSE;
    }

    auto inst = std::make_unique<RendererInstance>();
    inst->port = port;
    inst->stopRequested.store(false, std::memory_order_release);
    inst->running.store(true, std::memory_order_release);

    RendererInstance* rawInst = inst.get();
    inst->thread = std::thread(overlayThreadMain,
                               rawInst,
                               window,
                               static_cast<int>(width),
                               static_cast<int>(height),
                               static_cast<int>(rotateTheta));

    g_instances.emplace(port, std::move(inst));
    OLOG(LOG_LEVEL_INFO, "nativeStartRenderer: 已启动 port=%d size=%dx%d", port, (int)width, (int)height);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeStopRenderer(
        JNIEnv*,
        jclass,
        jint port) {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    auto it = g_instances.find(port);
    if (it == g_instances.end()) return;
    stopInstanceLocked(it->second.get());
    g_instances.erase(it);
    OLOG(LOG_LEVEL_INFO, "nativeStopRenderer: port=%d 已停止", port);
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeStopAllRenderers(
        JNIEnv*,
        jclass) {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    for (auto& kv : g_instances) {
        stopInstanceLocked(kv.second.get());
    }
    g_instances.clear();
    OLOG(LOG_LEVEL_INFO, "nativeStopAllRenderers: 全部已停止");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeIsRendererRunning(
        JNIEnv*,
        jclass,
        jint port) {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    auto it = g_instances.find(port);
    if (it == g_instances.end()) return JNI_FALSE;
    return it->second->running.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

// 注入投屏 / Java MotionEvent 触摸事件到指定端口的 RenderServer.
//   action: 0=Down, 1=Move, 2=Up, 3=Cancel
//   x, y  : 已经位于 display 像素坐标系 (与 TextureView 相同)
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeInjectTouch(
        JNIEnv*,
        jclass,
        jint port,
        jint action,
        jfloat x,
        jfloat y) {
    std::lock_guard<std::mutex> lock(g_instancesMutex);
    auto it = g_instances.find(port);
    if (it == g_instances.end()) return JNI_FALSE;
    android::AImGui* imgui = it->second->imgui.load(std::memory_order_acquire);
    if (!imgui) return JNI_FALSE;
    imgui->InjectExternalTouch(static_cast<int>(action),
                               static_cast<float>(x),
                               static_cast<float>(y));
    return JNI_TRUE;
}

// 查询 RenderServer 缓存的最近一次菜单 rect 列表 (RenderClient 侧每帧自动上报).
//   out[0]    = 实际 rect 数量 (0..AImGui::kMaxMenuRects)
//   out[1..]  = 每个 rect 占 4 个 int: x, y, w, h (display 像素)
// 返回 false: 端口未运行 / out 为 null / 长度不足.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_PublicOverlayBridge_nativeGetMenuRect(
        JNIEnv* env,
        jclass,
        jint port,
        jintArray out) {
    if (env == nullptr || out == nullptr) return JNI_FALSE;
    const jsize len = env->GetArrayLength(out);
    if (len < 1) return JNI_FALSE;

    std::lock_guard<std::mutex> lock(g_instancesMutex);
    auto it = g_instances.find(port);
    if (it == g_instances.end()) return JNI_FALSE;
    android::AImGui* imgui = it->second->imgui.load(std::memory_order_acquire);
    if (!imgui) return JNI_FALSE;

    const int capacity = std::min<int>(android::AImGui::kMaxMenuRects, (len - 1) / 4);
    float buf[android::AImGui::kMaxMenuRects * 4];
    int n = imgui->QueryMenuRects(buf, capacity);
    if (n < 0) n = 0;

    // 输出布局: [count, x0,y0,w0,h0, x1,y1,w1,h1, ...]
    jint header = static_cast<jint>(n);
    env->SetIntArrayRegion(out, 0, 1, &header);
    if (n > 0) {
        jint vals[android::AImGui::kMaxMenuRects * 4];
        for (int i = 0; i < n * 4; ++i)
            vals[i] = static_cast<jint>(buf[i]);
        env->SetIntArrayRegion(out, 1, n * 4, vals);
    }
    return JNI_TRUE;
}

OBFU_ATTRS_END
