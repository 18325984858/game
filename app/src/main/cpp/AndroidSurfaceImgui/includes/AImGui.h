#ifndef A_IMGUI_H // !A_IMGUI_H
#define A_IMGUI_H

#include <imgui/imgui.h>
#include <imgui/backends/imgui_impl_android.h>
#include <imgui/backends/imgui_impl_opengl3.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <sys/socket.h>
#include <poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <android/keycodes.h>
#include <linux/time.h>

#include <thread>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>
#include <condition_variable>

namespace android
{
    class AImGui
    {
    public:
        enum class RenderType
        {
            RenderNative,
            RenderServer,
            RenderClient,
        };

        enum class RenderState
        {
            SetFont,
            Rendering,
            ReadData,
        };

        struct Options
        {
            RenderType renderType = RenderType::RenderNative;
            bool compressionFrameData = true;
            bool autoUpdateOrientation = false;
            bool exchangeFontData = false;
            bool tcpNoDelay = true;
            bool disableVsync = true;
            float styleScale = 1.75f;
            float fontSizePixels = 18.0f;
            int screenWidth = 0;
            int screenHeight = 0;
            int rotateTheta = 0;
            ANativeWindow *externalNativeWindow = nullptr;
            std::string serverListenAddress = "127.0.0.1";
            std::string clientConnectAddress = "127.0.0.1";
            // RenderServer 监听端口 / RenderClient 连接端口
            // 默认 16888 (PUBG); DFM=16889; NRC=16890; 不同游戏使用不同端口避免占用冲突
            int port = 16888;
        };

    public:
        AImGui() : AImGui(Options{})
        {
        }
        AImGui(const Options &options);
        ~AImGui();

        void BeginFrame();
        void EndFrame();

        void ProcessInputEvent();

        // 注入外部 (Java MotionEvent / 投屏) 触摸事件到 ImGui 输入流.
        //   action: 0 = TouchDown, 1 = Move, 2 = TouchUp, 3 = Cancel
        //   x, y  : 已经位于屏幕 (display) 坐标系, 不再做旋转/缩放变换.
        // RenderServer 模式下会通过 socket 转发给 RenderClient;
        // RenderNative 模式直接 AddMousePosEvent / AddMouseButtonEvent.
        // 线程安全: 可在任意线程调用 (典型: Java UI 线程通过 JNI).
        void InjectExternalTouch(int action, float x, float y);

        // 非阻塞检查是否有待处理的输入事件 (用于 RenderClient 同线程调用)
        bool PollInputReady(int timeoutMs = 0) const;

        void SetupWindowInfo(void *windowInfo);

        bool IsClientConnected() const { return m_clientConnected.load(std::memory_order_acquire); }

        // ---- 旁路: 菜单 rect 发布 (RenderClient -> RenderServer) ----

        // 发送给 server 供 Java 侧触摸捕获窗跟随. 不需要业务代码手动调用.
        // 最多同时跟随 kMaxMenuRects 个菜单.
        static constexpr int kMaxMenuRects = 16;
        // RenderServer 侧查询: 输入最多可容纳 capacity 个 rect 的数组 (每个 4 个 float),
        // 返回实际填充的数量. 0 表示尚未收到任何 rect.
        int  QueryMenuRects(float* outXywh, int capacity) const;

        // 兼容: 老的单 rect API (现已被 EndFrame 自动 publish 取代, 保留 noop).
        void PublishMenuRect(float x, float y, float w, float h);
        constexpr operator bool() const
        {
            return m_state;
        }

    private:
        bool InitEnvironment();
        void UnInitEnvironment();

        void ServerWorker();

        int ReadData(void *buffer, size_t readSize);
        void WriteData(void *data, size_t size);

    private:
        bool m_state = false;

        int m_rotateTheta = 0;
        int m_screenWidth = -1, m_screenHeight = -1;
        double m_lastTime = 0.0;

        Options m_options;
        // 16MB. Chinese font atlas (4096x2048 RGBA = 32MB?) is sent via separate font packet path,
        // but font packet header carries size 8388616 (8MB+8B) that previously exceeded 8MB cap and
        // caused server to disconnect DFM client immediately ("Packet is too large: 8").
        size_t m_maxPacketSize = 16 * 1024 * 1024;
        sockaddr_in m_transportAddress{};
        int m_serverFd = -1, m_clientFd = -1;
        std::atomic<bool> m_clientConnected{false};  // 渲染线程安全的连接状态
        // 旁路: server 端缓存最近收到的菜单 rect 列表.
        mutable std::mutex m_menuRectsMutex;
        std::vector<float> m_menuRects;  // [x,y,w,h, x,y,w,h, ...] flat
        std::atomic<int>   m_menuRectsCount{0};
        // 客户端节流: 上一次发送的 rect 集 (hash) + 时间戳
        std::chrono::steady_clock::time_point m_menuRectLastSendTime{};
        uint64_t m_menuRectLastSentHash = 0;
        std::unique_ptr<std::thread> m_serverWorkerThread;
        std::vector<uint8_t> m_serverFontData;
        std::vector<uint8_t> m_serverRenderData, m_serverRenderDataBack;
        std::mutex m_renderDataMutex;
        std::mutex m_writeMutex;   // 串行化 server -> client 的 socket write
                                   // (evdev 触点 + Java MotionEvent 注入两路并发)
        std::atomic<RenderState> m_renderState = RenderState::ReadData;
        uint64_t m_fontPacketCount = 0;
        uint64_t m_renderPacketCount = 0;
        uint64_t m_renderFrameCount = 0;
        std::chrono::steady_clock::time_point m_clientDisconnectTime{};
        size_t m_lastFontPacketSize = 0;
        size_t m_lastRenderPacketSize = 0;
        size_t m_lastRenderDecodedSize = 0;
        bool m_serverFontPacketReceived = false;

        ANativeWindow *m_nativeWindow = nullptr;
        bool m_usesExternalNativeWindow = false;
        EGLDisplay m_defaultDisplay = EGL_NO_DISPLAY;
        EGLSurface m_eglSurface = EGL_NO_SURFACE;
        EGLContext m_eglContext = EGL_NO_CONTEXT;
        ImGuiContext *m_imguiContext = nullptr;
    };

    // (旧 单 rect API 已被废弃; 现在由 EndFrame 自动枚举多个菜单.)
} // namespace android

#endif // !A_IMGUI_H