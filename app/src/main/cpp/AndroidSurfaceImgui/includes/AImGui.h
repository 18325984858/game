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

        void SetupWindowInfo(void *windowInfo);

        bool IsClientConnected() const { return m_clientConnected.load(std::memory_order_acquire); }

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
        size_t m_maxPacketSize = 8 * 1024 * 1024; // 8MB, enough for Chinese font atlas exchange
        sockaddr_in m_transportAddress{};
        int m_serverFd = -1, m_clientFd = -1;
        std::atomic<bool> m_clientConnected{false};  // 渲染线程安全的连接状态
        std::unique_ptr<std::thread> m_serverWorkerThread;
        std::vector<uint8_t> m_serverFontData;
        std::vector<uint8_t> m_serverRenderData, m_serverRenderDataBack;
        std::mutex m_renderDataMutex;
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
} // namespace android

#endif // !A_IMGUI_H