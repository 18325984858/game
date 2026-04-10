#include "AImGui.h"

#define TAG "UE4-GUI"
#include "Global.h"
#include "ANativeWindowCreator.h"
#include "ATouchEvent.h"

#include <ImGui-SharedDrawData/modules/ImGuiSharedDrawData.h>
#include <zstd.h>
#include <netinet/tcp.h>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

size_t android::anative_window_creator::detail::compat::SystemVersion = 13;

namespace
{
    bool TryLoadChineseFontFromFile(ImGuiIO &imguiIO, const char *path, float fontSizePixels, int fontNo = 0)
    {
        struct stat st{};
        if (nullptr == path || 0 != stat(path, &st) || !S_ISREG(st.st_mode))
            return false;

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (0 > fd)
            return false;

        int fileSize = static_cast<int>(st.st_size);
        void *data = IM_ALLOC(fileSize);
        if (nullptr == data)
        {
            close(fd);
            return false;
        }

        size_t totalRead = 0;
        while (totalRead < static_cast<size_t>(fileSize))
        {
            auto readSize = read(fd, reinterpret_cast<char *>(data) + totalRead, fileSize - totalRead);
            if (0 >= readSize)
                break;
            totalRead += static_cast<size_t>(readSize);
        }
        close(fd);

        if (totalRead != static_cast<size_t>(fileSize))
        {
            IM_FREE(data);
            return false;
        }

        ImFontConfig fontConfig;
        fontConfig.FontDataOwnedByAtlas = true;
        fontConfig.OversampleH = 1;
        fontConfig.OversampleV = 1;
        fontConfig.PixelSnapH = true;
        fontConfig.FontNo = fontNo;
        fontConfig.SizePixels = fontSizePixels;

        ImFont *font = imguiIO.Fonts->AddFontFromMemoryTTF(
                data,
                fileSize,
                fontSizePixels,
                &fontConfig,
                imguiIO.Fonts->GetGlyphRangesChineseSimplifiedCommon());
        if (nullptr == font)
        {
            IM_FREE(data);
            return false;
        }

        if (!imguiIO.Fonts->Build())
        {
            LogDebug("[AImGui] Font build skipped: %s", path);
            imguiIO.Fonts->Clear();
            return false;
        }

        if (nullptr == font->FindGlyphNoFallback(static_cast<ImWchar>(0x4E2D)))
        {
            LogDebug("[AImGui] Font missing Chinese glyphs: %s", path);
            imguiIO.Fonts->Clear();
            return false;
        }

        LogInfo("[AImGui] Loaded Chinese font: %s (%d bytes, atlas %dx%d)",
                path,
                fileSize,
                imguiIO.Fonts->TexWidth,
                imguiIO.Fonts->TexHeight);
        return true;
    }

    bool IsFontFileName(const char *name)
    {
        if (nullptr == name)
            return false;

        auto len = strlen(name);
        if (len < 4)
            return false;

        const char *ext = name + len - 4;
        return 0 == strcasecmp(ext, ".ttf")
                || 0 == strcasecmp(ext, ".otf")
                || 0 == strcasecmp(ext, ".ttc");
    }

    void LoadPreferredImGuiFont(ImGuiIO &imguiIO, float fontSizePixels)
    {
        imguiIO.Fonts->TexDesiredWidth = 4096;

        static const char *fontPaths[] = {
                "/data/local/tmp/chinese.ttf",
                "/system/fonts/DroidSansFallback.ttf",
                "/system/fonts/NotoSansSC-Regular.ttf",
                "/system/fonts/NotoSansCJKsc-Regular.ttf",
                "/system/fonts/MiLanProVF.ttf",
                "/system/fonts/HarmonyOS_Sans_SC.ttf",
                "/system/fonts/OPPOSans-Regular.ttf",
                "/system/fonts/VivoSans-Regular.ttf",
                "/system/fonts/RobotoFallback-Regular.ttf",
                "/system/fonts/NotoSansSC-Regular.otf",
                "/system/fonts/NotoSansHans-Regular.otf",
                "/system/fonts/NotoSansCJKsc-Regular.otf",
                "/system/fonts/NotoSansSC-Regular.ttc",
                "/system/fonts/NotoSansCJKsc-Regular.ttc",
                "/system/fonts/NotoSansCJK-Regular.ttc",
        };

        for (const char *path : fontPaths)
        {
            if (TryLoadChineseFontFromFile(imguiIO, path, fontSizePixels))
                return;
        }

        DIR *dir = opendir("/system/fonts");
        if (nullptr != dir)
        {
            dirent *entry = nullptr;
            while (nullptr != (entry = readdir(dir)))
            {
                if (!IsFontFileName(entry->d_name))
                    continue;

                char path[512] = {};
                snprintf(path, sizeof(path), "/system/fonts/%s", entry->d_name);
                if (TryLoadChineseFontFromFile(imguiIO, path, fontSizePixels))
                {
                    closedir(dir);
                    return;
                }
            }
            closedir(dir);
        }

        imguiIO.Fonts->Clear();
        ImFontConfig fallbackConfig;
        fallbackConfig.SizePixels = fontSizePixels;
        imguiIO.Fonts->AddFontDefault(&fallbackConfig);
        imguiIO.Fonts->Build();
        LogInfo("[AImGui] Chinese font unavailable, fallback to default atlas %dx%d",
                imguiIO.Fonts->TexWidth,
                imguiIO.Fonts->TexHeight);
    }
}

static ImGuiKey KeyCodeToImGuiKey(int32_t keyCode)
{
    switch (keyCode)
    {
    case AKEYCODE_TAB:
        return ImGuiKey_Tab;
    case AKEYCODE_DPAD_LEFT:
        return ImGuiKey_LeftArrow;
    case AKEYCODE_DPAD_RIGHT:
        return ImGuiKey_RightArrow;
    case AKEYCODE_DPAD_UP:
        return ImGuiKey_UpArrow;
    case AKEYCODE_DPAD_DOWN:
        return ImGuiKey_DownArrow;
    case AKEYCODE_PAGE_UP:
        return ImGuiKey_PageUp;
    case AKEYCODE_PAGE_DOWN:
        return ImGuiKey_PageDown;
    case AKEYCODE_MOVE_HOME:
        return ImGuiKey_Home;
    case AKEYCODE_MOVE_END:
        return ImGuiKey_End;
    case AKEYCODE_INSERT:
        return ImGuiKey_Insert;
    case AKEYCODE_FORWARD_DEL:
        return ImGuiKey_Delete;
    case AKEYCODE_DEL:
        return ImGuiKey_Backspace;
    case AKEYCODE_SPACE:
        return ImGuiKey_Space;
    case AKEYCODE_ENTER:
        return ImGuiKey_Enter;
    case AKEYCODE_ESCAPE:
        return ImGuiKey_Escape;
    case AKEYCODE_APOSTROPHE:
        return ImGuiKey_Apostrophe;
    case AKEYCODE_COMMA:
        return ImGuiKey_Comma;
    case AKEYCODE_MINUS:
        return ImGuiKey_Minus;
    case AKEYCODE_PERIOD:
        return ImGuiKey_Period;
    case AKEYCODE_SLASH:
        return ImGuiKey_Slash;
    case AKEYCODE_SEMICOLON:
        return ImGuiKey_Semicolon;
    case AKEYCODE_EQUALS:
        return ImGuiKey_Equal;
    case AKEYCODE_LEFT_BRACKET:
        return ImGuiKey_LeftBracket;
    case AKEYCODE_BACKSLASH:
        return ImGuiKey_Backslash;
    case AKEYCODE_RIGHT_BRACKET:
        return ImGuiKey_RightBracket;
    case AKEYCODE_GRAVE:
        return ImGuiKey_GraveAccent;
    case AKEYCODE_CAPS_LOCK:
        return ImGuiKey_CapsLock;
    case AKEYCODE_SCROLL_LOCK:
        return ImGuiKey_ScrollLock;
    case AKEYCODE_NUM_LOCK:
        return ImGuiKey_NumLock;
    case AKEYCODE_SYSRQ:
        return ImGuiKey_PrintScreen;
    case AKEYCODE_BREAK:
        return ImGuiKey_Pause;
    case AKEYCODE_NUMPAD_0:
        return ImGuiKey_Keypad0;
    case AKEYCODE_NUMPAD_1:
        return ImGuiKey_Keypad1;
    case AKEYCODE_NUMPAD_2:
        return ImGuiKey_Keypad2;
    case AKEYCODE_NUMPAD_3:
        return ImGuiKey_Keypad3;
    case AKEYCODE_NUMPAD_4:
        return ImGuiKey_Keypad4;
    case AKEYCODE_NUMPAD_5:
        return ImGuiKey_Keypad5;
    case AKEYCODE_NUMPAD_6:
        return ImGuiKey_Keypad6;
    case AKEYCODE_NUMPAD_7:
        return ImGuiKey_Keypad7;
    case AKEYCODE_NUMPAD_8:
        return ImGuiKey_Keypad8;
    case AKEYCODE_NUMPAD_9:
        return ImGuiKey_Keypad9;
    case AKEYCODE_NUMPAD_DOT:
        return ImGuiKey_KeypadDecimal;
    case AKEYCODE_NUMPAD_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case AKEYCODE_NUMPAD_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case AKEYCODE_NUMPAD_SUBTRACT:
        return ImGuiKey_KeypadSubtract;
    case AKEYCODE_NUMPAD_ADD:
        return ImGuiKey_KeypadAdd;
    case AKEYCODE_NUMPAD_ENTER:
        return ImGuiKey_KeypadEnter;
    case AKEYCODE_NUMPAD_EQUALS:
        return ImGuiKey_KeypadEqual;
    case AKEYCODE_CTRL_LEFT:
        return ImGuiKey_LeftCtrl;
    case AKEYCODE_SHIFT_LEFT:
        return ImGuiKey_LeftShift;
    case AKEYCODE_ALT_LEFT:
        return ImGuiKey_LeftAlt;
    case AKEYCODE_META_LEFT:
        return ImGuiKey_LeftSuper;
    case AKEYCODE_CTRL_RIGHT:
        return ImGuiKey_RightCtrl;
    case AKEYCODE_SHIFT_RIGHT:
        return ImGuiKey_RightShift;
    case AKEYCODE_ALT_RIGHT:
        return ImGuiKey_RightAlt;
    case AKEYCODE_META_RIGHT:
        return ImGuiKey_RightSuper;
    case AKEYCODE_MENU:
        return ImGuiKey_Menu;
    case AKEYCODE_0:
        return ImGuiKey_0;
    case AKEYCODE_1:
        return ImGuiKey_1;
    case AKEYCODE_2:
        return ImGuiKey_2;
    case AKEYCODE_3:
        return ImGuiKey_3;
    case AKEYCODE_4:
        return ImGuiKey_4;
    case AKEYCODE_5:
        return ImGuiKey_5;
    case AKEYCODE_6:
        return ImGuiKey_6;
    case AKEYCODE_7:
        return ImGuiKey_7;
    case AKEYCODE_8:
        return ImGuiKey_8;
    case AKEYCODE_9:
        return ImGuiKey_9;
    case AKEYCODE_A:
        return ImGuiKey_A;
    case AKEYCODE_B:
        return ImGuiKey_B;
    case AKEYCODE_C:
        return ImGuiKey_C;
    case AKEYCODE_D:
        return ImGuiKey_D;
    case AKEYCODE_E:
        return ImGuiKey_E;
    case AKEYCODE_F:
        return ImGuiKey_F;
    case AKEYCODE_G:
        return ImGuiKey_G;
    case AKEYCODE_H:
        return ImGuiKey_H;
    case AKEYCODE_I:
        return ImGuiKey_I;
    case AKEYCODE_J:
        return ImGuiKey_J;
    case AKEYCODE_K:
        return ImGuiKey_K;
    case AKEYCODE_L:
        return ImGuiKey_L;
    case AKEYCODE_M:
        return ImGuiKey_M;
    case AKEYCODE_N:
        return ImGuiKey_N;
    case AKEYCODE_O:
        return ImGuiKey_O;
    case AKEYCODE_P:
        return ImGuiKey_P;
    case AKEYCODE_Q:
        return ImGuiKey_Q;
    case AKEYCODE_R:
        return ImGuiKey_R;
    case AKEYCODE_S:
        return ImGuiKey_S;
    case AKEYCODE_T:
        return ImGuiKey_T;
    case AKEYCODE_U:
        return ImGuiKey_U;
    case AKEYCODE_V:
        return ImGuiKey_V;
    case AKEYCODE_W:
        return ImGuiKey_W;
    case AKEYCODE_X:
        return ImGuiKey_X;
    case AKEYCODE_Y:
        return ImGuiKey_Y;
    case AKEYCODE_Z:
        return ImGuiKey_Z;
    case AKEYCODE_F1:
        return ImGuiKey_F1;
    case AKEYCODE_F2:
        return ImGuiKey_F2;
    case AKEYCODE_F3:
        return ImGuiKey_F3;
    case AKEYCODE_F4:
        return ImGuiKey_F4;
    case AKEYCODE_F5:
        return ImGuiKey_F5;
    case AKEYCODE_F6:
        return ImGuiKey_F6;
    case AKEYCODE_F7:
        return ImGuiKey_F7;
    case AKEYCODE_F8:
        return ImGuiKey_F8;
    case AKEYCODE_F9:
        return ImGuiKey_F9;
    case AKEYCODE_F10:
        return ImGuiKey_F10;
    case AKEYCODE_F11:
        return ImGuiKey_F11;
    case AKEYCODE_F12:
        return ImGuiKey_F12;
    default:
        return ImGuiKey_None;
    }
}

static unsigned int KeyCodeToCharacter(int32_t keyCode, bool upperCase)
{
    switch (keyCode)
    {
    case AKEYCODE_SPACE:
        return ' ';
    // case AKEYCODE_ENTER:
    //     return '\n';
    case AKEYCODE_APOSTROPHE:
        return upperCase ? '"' : '\'';
    case AKEYCODE_COMMA:
        return upperCase ? '<' : ',';
    case AKEYCODE_MINUS:
        return upperCase ? '_' : '-';
    case AKEYCODE_PERIOD:
        return upperCase ? '>' : '.';
    case AKEYCODE_SLASH:
        return upperCase ? '?' : '/';
    case AKEYCODE_SEMICOLON:
        return upperCase ? ':' : ';';
    case AKEYCODE_EQUALS:
        return upperCase ? '+' : '=';
    case AKEYCODE_LEFT_BRACKET:
        return upperCase ? '{' : '[';
    case AKEYCODE_BACKSLASH:
        return upperCase ? '|' : '\\';
    case AKEYCODE_RIGHT_BRACKET:
        return upperCase ? '}' : ']';
    case AKEYCODE_GRAVE:
        return upperCase ? '~' : '`';
    case AKEYCODE_NUMPAD_0:
        return '0';
    case AKEYCODE_NUMPAD_1:
        return '1';
    case AKEYCODE_NUMPAD_2:
        return '2';
    case AKEYCODE_NUMPAD_3:
        return '3';
    case AKEYCODE_NUMPAD_4:
        return '4';
    case AKEYCODE_NUMPAD_5:
        return '5';
    case AKEYCODE_NUMPAD_6:
        return '6';
    case AKEYCODE_NUMPAD_7:
        return '7';
    case AKEYCODE_NUMPAD_8:
        return '8';
    case AKEYCODE_NUMPAD_9:
        return '9';
    case AKEYCODE_NUMPAD_DOT:
        return '.';
    case AKEYCODE_NUMPAD_DIVIDE:
        return '/';
    case AKEYCODE_NUMPAD_MULTIPLY:
        return '*';
    case AKEYCODE_NUMPAD_SUBTRACT:
        return '-';
    case AKEYCODE_NUMPAD_ADD:
        return '+';
    // case AKEYCODE_NUMPAD_ENTER:
    //     return '\n';
    case AKEYCODE_NUMPAD_EQUALS:
        return '=';
    case AKEYCODE_0:
        return upperCase ? ')' : '0';
    case AKEYCODE_1:
        return upperCase ? '!' : '1';
    case AKEYCODE_2:
        return upperCase ? '@' : '2';
    case AKEYCODE_3:
        return upperCase ? '#' : '3';
    case AKEYCODE_4:
        return upperCase ? '$' : '4';
    case AKEYCODE_5:
        return upperCase ? '%' : '5';
    case AKEYCODE_6:
        return upperCase ? '^' : '6';
    case AKEYCODE_7:
        return upperCase ? '&' : '7';
    case AKEYCODE_8:
        return upperCase ? '*' : '8';
    case AKEYCODE_9:
        return upperCase ? '(' : '9';
    case AKEYCODE_A:
        return upperCase ? 'A' : 'a';
    case AKEYCODE_B:
        return upperCase ? 'B' : 'b';
    case AKEYCODE_C:
        return upperCase ? 'C' : 'c';
    case AKEYCODE_D:
        return upperCase ? 'D' : 'd';
    case AKEYCODE_E:
        return upperCase ? 'E' : 'e';
    case AKEYCODE_F:
        return upperCase ? 'F' : 'f';
    case AKEYCODE_G:
        return upperCase ? 'G' : 'g';
    case AKEYCODE_H:
        return upperCase ? 'H' : 'h';
    case AKEYCODE_I:
        return upperCase ? 'I' : 'i';
    case AKEYCODE_J:
        return upperCase ? 'J' : 'j';
    case AKEYCODE_K:
        return upperCase ? 'K' : 'k';
    case AKEYCODE_L:
        return upperCase ? 'L' : 'l';
    case AKEYCODE_M:
        return upperCase ? 'M' : 'm';
    case AKEYCODE_N:
        return upperCase ? 'N' : 'n';
    case AKEYCODE_O:
        return upperCase ? 'O' : 'o';
    case AKEYCODE_P:
        return upperCase ? 'P' : 'p';
    case AKEYCODE_Q:
        return upperCase ? 'Q' : 'q';
    case AKEYCODE_R:
        return upperCase ? 'R' : 'r';
    case AKEYCODE_S:
        return upperCase ? 'S' : 's';
    case AKEYCODE_T:
        return upperCase ? 'T' : 't';
    case AKEYCODE_U:
        return upperCase ? 'U' : 'u';
    case AKEYCODE_V:
        return upperCase ? 'V' : 'v';
    case AKEYCODE_W:
        return upperCase ? 'W' : 'w';
    case AKEYCODE_X:
        return upperCase ? 'X' : 'x';
    case AKEYCODE_Y:
        return upperCase ? 'Y' : 'y';
    case AKEYCODE_Z:
        return upperCase ? 'Z' : 'z';
    default:
        return 0;
    }
}

namespace android
{
    AImGui::AImGui(const Options &options)
        : m_options(options)
    {
        LogInfo("[AImGui] ctor begin renderType=%d autoUpdateOrientation=%d screenWidth=%d screenHeight=%d rotateTheta=%d",
                static_cast<int>(m_options.renderType),
                m_options.autoUpdateOrientation ? 1 : 0,
                m_options.screenWidth,
                m_options.screenHeight,
                m_options.rotateTheta);
        InitEnvironment();
        LogInfo("[AImGui] ctor end state=%d nativeWindow=%p eglDisplay=%p eglSurface=%p eglContext=%p",
                m_state ? 1 : 0,
                m_nativeWindow,
                m_defaultDisplay,
                m_eglSurface,
                m_eglContext);
    }

    AImGui::~AImGui()
    {
        UnInitEnvironment();
    }

    void AImGui::BeginFrame()
    {
        if (!m_state)
            return;

        if (RenderType::RenderClient != m_options.renderType && nullptr != m_nativeWindow)
        {
            int currentWidth = ANativeWindow_getWidth(m_nativeWindow);
            int currentHeight = ANativeWindow_getHeight(m_nativeWindow);
            if (0 < currentWidth && 0 < currentHeight)
            {
                m_screenWidth = currentWidth;
                m_screenHeight = currentHeight;
            }
        }

        if (m_options.autoUpdateOrientation)
        {
            auto displayInfo = ANativeWindowCreator::GetDisplayInfo();

            // Check if display orientation is changed
            if (m_rotateTheta != displayInfo.theta)
            {
                UnInitEnvironment();
                InitEnvironment();
            }
        }

        ANativeWindowCreator::ProcessMirrorDisplay();

        ImGui_ImplOpenGL3_NewFrame();
        if (RenderType::RenderClient != m_options.renderType)
            ImGui_ImplAndroid_NewFrame();
        else
        {
            // Copy from imgui_impl_android.cpp
            timespec currentTimeSpec{};
            auto &imguiIO = ImGui::GetIO();

            imguiIO.DisplaySize = {static_cast<float>(m_screenWidth), static_cast<float>(m_screenHeight)};
            imguiIO.DisplayFramebufferScale = {1.0f, 1.0f};

            clock_gettime(CLOCK_MONOTONIC, &currentTimeSpec);
            double currentTime = static_cast<double>(currentTimeSpec.tv_sec) + currentTimeSpec.tv_nsec / 1000000000.0;
            imguiIO.DeltaTime = 0.0 < m_lastTime ? static_cast<float>(currentTime - m_lastTime) : static_cast<float>(1.0f / 60.0f);
            m_lastTime = currentTime;
        }
        if (RenderType::RenderClient == m_options.renderType || RenderType::RenderNative == m_options.renderType)
            ImGui::NewFrame();
    }
    void AImGui::EndFrame()
    {
        if (!m_state)
            return;

        if (RenderType::RenderClient == m_options.renderType)
        {
            ImGui::Render();
            const auto &sharedData = ImGui::GetSharedDrawData();
            if (!sharedData.empty())
            {
                if (!m_options.compressionFrameData)
                {
                    uint32_t packetSize = static_cast<uint32_t>(sharedData.size());
                    WriteData(&packetSize, sizeof(packetSize));
                    WriteData(const_cast<uint8_t *>(sharedData.data()), sharedData.size());
                    m_renderPacketCount++;
                    m_lastRenderPacketSize = packetSize;
                    m_lastRenderDecodedSize = sharedData.size();
                }
                else
                {
                    static std::vector<uint8_t> compressBuffer;
                    static std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> compressContext(ZSTD_createCCtx(), &ZSTD_freeCCtx);
                    size_t compressBufferSize = 7 + ZSTD_compressBound(sharedData.size());

                    if (nullptr == compressContext)
                    {
                        LogDebug("[-] Client can not create compress context");
                        m_state = false;
                        return;
                    }
                    if (compressBuffer.empty()) // First compression
                    {
                        ZSTD_CCtx_setParameter(compressContext.get(), ZSTD_c_compressionLevel, ZSTD_defaultCLevel());
                        ZSTD_CCtx_setParameter(compressContext.get(), ZSTD_c_checksumFlag, 1);
                    }
                    if (compressBuffer.size() < compressBufferSize)
                        compressBuffer.resize(compressBufferSize);

                    ZSTD_inBuffer input = {sharedData.data(), sharedData.size(), 0};
                    ZSTD_outBuffer output = {compressBuffer.data(), compressBuffer.size(), 0};
                    if (0 == ZSTD_compressStream2(compressContext.get(), &output, &input, ZSTD_e_end))
                    {
                        uint32_t packetSize = sizeof(uint32_t) + output.pos;
                        WriteData(&packetSize, sizeof(packetSize));
                        uint32_t sharedDataSize = sharedData.size();
                        WriteData(&sharedDataSize, sizeof(sharedDataSize));
                        WriteData(compressBuffer.data(), output.pos);
                        m_renderPacketCount++;
                        m_lastRenderPacketSize = packetSize;
                        m_lastRenderDecodedSize = sharedDataSize;
                    }
                    else
                        LogDebug("[-] Client compression frame data error");
                }

                if (0 == (m_renderPacketCount % 180))
                {
                    LogInfo("[AImGui] Client sent render packets=%llu lastPacket=%zu decoded=%zu",
                            static_cast<unsigned long long>(m_renderPacketCount),
                            m_lastRenderPacketSize,
                            m_lastRenderDecodedSize);
                }
            }
        }
        else if (RenderType::RenderServer == m_options.renderType)
        {
            switch (m_renderState)
            {
            case RenderState::ReadData:
            {
                // 空闲状态: 如果有之前渲染过的帧, 刷新为透明 (防止退出后残留)
                if (m_renderFrameCount > 0 && m_clientFd < 0) {
                    glClear(GL_COLOR_BUFFER_BIT);
                    eglSwapBuffers(m_defaultDisplay, m_eglSurface);
                    m_renderFrameCount = 0;
                }
                break;
            }
            case RenderState::SetFont:
            {
                if (!m_options.exchangeFontData)
                {
                    m_renderState = RenderState::ReadData;
                    break;
                }

                {
                    std::lock_guard<std::mutex> lock(m_renderDataMutex);
                    ImGui_ImplOpenGL3_DestroyFontsTexture();
                    ImGui::SetSharedFontData(m_serverFontData);
                    ImGui_ImplOpenGL3_CreateFontsTexture();
                    LogInfo("[AImGui] Server applied font packet count=%llu size=%zu",
                            static_cast<unsigned long long>(m_fontPacketCount),
                            m_lastFontPacketSize);
                }
                m_renderState = RenderState::ReadData;

                break;
            }
            case RenderState::Rendering:
            {
                std::lock_guard<std::mutex> lock(m_renderDataMutex);
                auto drawData = ImGui::RenderSharedDrawData(m_serverRenderData);
                if (nullptr != drawData)
                {
                    if (m_options.exchangeFontData)
                    {
                        for (const auto &cmdList : drawData->CmdLists)
                        {
                            for (auto &cmd : cmdList->CmdBuffer)
                                cmd.TextureId = ImGui::GetIO().Fonts->TexID;
                        }
                    }

                    int totalCmdCount = 0;
                    for (int cmdListIndex = 0; cmdListIndex < drawData->CmdListsCount; ++cmdListIndex)
                        totalCmdCount += drawData->CmdLists[cmdListIndex]->CmdBuffer.Size;

                    if (0 == drawData->CmdListsCount || 0 == totalCmdCount)
                    {
                        m_renderState = RenderState::ReadData;
                        break;
                    }

                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(drawData);
                    eglSwapBuffers(m_defaultDisplay, m_eglSurface);
                    m_renderFrameCount++;
                    if (0 == (m_renderFrameCount % 180))
                    {
                        LogInfo("[AImGui] Server rendered frames=%llu packets=%llu drawLists=%d cmds=%d decoded=%zu",
                                static_cast<unsigned long long>(m_renderFrameCount),
                                static_cast<unsigned long long>(m_renderPacketCount),
                                drawData->CmdListsCount,
                                totalCmdCount,
                                m_lastRenderDecodedSize);
                    }
                }
                else
                {
                    LogError("[AImGui] Server RenderSharedDrawData returned null decoded=%zu packetCount=%llu",
                             m_lastRenderDecodedSize,
                             static_cast<unsigned long long>(m_renderPacketCount));
                }
                m_renderState = RenderState::ReadData;

                break;
            }
            default:
                break;
            }
        }
        else if (RenderType::RenderNative == m_options.renderType)
        {
            ImGui::Render();
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            eglSwapBuffers(m_defaultDisplay, m_eglSurface);
        }
    }

    void AImGui::ProcessInputEvent()
    {
        ATouchEvent::TouchEvent event{};

        if (!m_state)
            return;

        if (RenderType::RenderServer == m_options.renderType || RenderType::RenderNative == m_options.renderType)
        {
            static ATouchEvent touchEvent;

            if (!touchEvent.GetTouchEvent(&event))
                return;
            event.TransformToScreen(m_screenWidth, m_screenHeight, m_rotateTheta);

            if (RenderType::RenderServer == m_options.renderType)
            {
                if (-1 == m_clientFd)
                    return;
                WriteData(&event, sizeof(event));
            }
        }
        else
        {
            auto readResult = ReadData(&event, sizeof(event));
            if (0 >= readResult)
            {
                // LogDebug("[-] Client can not read input event, readResult:%d  %d:%s", readResult, errno, strerror(errno));
                return;
            }
        }

        if (RenderType::RenderClient == m_options.renderType || RenderType::RenderNative == m_options.renderType)
        {
            auto &imguiIO = ImGui::GetIO();
            switch (event.type)
            {
            case ATouchEvent::EventType::Move:
            {
                imguiIO.AddMousePosEvent(event.x, event.y);
                break;
            }
            case ATouchEvent::EventType::TouchDown:
            case ATouchEvent::EventType::TouchUp:
            {
                imguiIO.AddMousePosEvent(event.x, event.y);
                imguiIO.AddMouseButtonEvent(0, ATouchEvent::EventType::TouchDown == event.type);
                break;
            }
            case ATouchEvent::EventType::KeyDown:
            case ATouchEvent::EventType::KeyUp:
            {
                auto imguiKey = KeyCodeToImGuiKey(event.keyCode);
                if (ImGuiKey_None == imguiKey)
                    break;

                switch (imguiKey)
                {
                case ImGuiKey_LeftCtrl:
                case ImGuiKey_RightCtrl:
                    imguiIO.AddKeyEvent(ImGuiMod_Ctrl, ATouchEvent::EventType::KeyDown == event.type);
                    break;
                case ImGuiKey_LeftShift:
                case ImGuiKey_RightShift:
                    imguiIO.AddKeyEvent(ImGuiMod_Shift, ATouchEvent::EventType::KeyDown == event.type);
                    break;
                case ImGuiKey_LeftAlt:
                case ImGuiKey_RightAlt:
                    imguiIO.AddKeyEvent(ImGuiMod_Alt, ATouchEvent::EventType::KeyDown == event.type);
                    break;
                default:
                    break;
                }
                imguiIO.AddKeyEvent(imguiKey, ATouchEvent::EventType::KeyDown == event.type);
                imguiIO.SetKeyEventNativeData(imguiKey, event.keyCode, event.scanCode);

                if (ATouchEvent::EventType::KeyDown != event.type)
                    break;
                unsigned int character = KeyCodeToCharacter(event.keyCode, ImGui::IsKeyDown(ImGuiMod_Shift));
                if (imguiIO.WantTextInput && 0 != character)
                    imguiIO.AddInputCharacter(character);
                break;
            }
            case ATouchEvent::EventType::Wheel:
            {
                imguiIO.AddMousePosEvent(std::abs(event.x), event.y);
                imguiIO.AddMouseWheelEvent(0, 0 > event.x ? -1 : 1);
                break;
            }
            default:
                break;
            }
        }
    }

    void AImGui::SetupWindowInfo(void *windowInfo)
    {
        ANativeWindowCreator::UpdateWindowInfo(m_nativeWindow, windowInfo);
    }

    bool AImGui::InitEnvironment()
    {
        LogInfo("[AImGui] InitEnvironment begin renderType=%d", static_cast<int>(m_options.renderType));

        m_usesExternalNativeWindow = nullptr != m_options.externalNativeWindow;

        // Initialize rpc
        m_transportAddress.sin_family = AF_INET;
        m_transportAddress.sin_port = htons(16888);
        if (RenderType::RenderClient == m_options.renderType)
        {
            inet_pton(AF_INET, m_options.clientConnectAddress.data(), &m_transportAddress.sin_addr);

            m_clientFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (0 > m_clientFd)
            {
                LogError("[-] Client fd create failed, m_clientFd:%d errno=%d:%s", m_clientFd, errno, strerror(errno));
                return false;
            }

            if (m_options.tcpNoDelay)
            {
                int optionValue = 1;
                setsockopt(m_clientFd, IPPROTO_TCP, TCP_NODELAY, &optionValue, sizeof(optionValue));
            }

            if (0 > connect(m_clientFd, reinterpret_cast<sockaddr *>(&m_transportAddress), sizeof(m_transportAddress)))
            {
                LogError("[-] Client connect to %s:%d failed, %d:%s",
                         m_options.clientConnectAddress.c_str(),
                         ntohs(m_transportAddress.sin_port),
                         errno,
                         strerror(errno));
                return false;
            }

            LogInfo("[AImGui] Client connected to %s:%d fd=%d",
                    m_options.clientConnectAddress.c_str(),
                    ntohs(m_transportAddress.sin_port),
                    m_clientFd);
        }
        else if (RenderType::RenderServer == m_options.renderType)
        {
            inet_pton(AF_INET, m_options.serverListenAddress.data(), &m_transportAddress.sin_addr);

            m_serverFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (0 > m_serverFd)
            {
                LogError("[-] Server fd create failed, m_serverFd:%d errno=%d:%s", m_serverFd, errno, strerror(errno));
                return false;
            }
            int optionValue = 1;
            if (0 > setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &optionValue, sizeof(int)))
            {
                LogError("[-] Server fd set reuseaddr failed, m_serverFd:%d errno=%d:%s", m_serverFd, errno, strerror(errno));
                return false;
            }

            if (0 > bind(m_serverFd, reinterpret_cast<sockaddr *>(&m_transportAddress), sizeof(m_transportAddress)))
            {
                LogDebug("[-] Server bind fd failed, %d:%s", errno, strerror(errno));
                return false;
            }

            if (0 > listen(m_serverFd, 1))
            {
                LogDebug("[-] Server listen fd failed, %d:%s", errno, strerror(errno));
                return false;
            }

            LogInfo("[AImGui] Server listening on %s:%d fd=%d",
                    m_options.serverListenAddress.c_str(),
                    ntohs(m_transportAddress.sin_port),
                    m_serverFd);
        }

        int displayTheta = m_options.rotateTheta;
        int displayWidth = m_options.screenWidth;
        int displayHeight = m_options.screenHeight;
        if (m_usesExternalNativeWindow)
        {
            if (0 >= displayWidth)
                displayWidth = ANativeWindow_getWidth(m_options.externalNativeWindow);
            if (0 >= displayHeight)
                displayHeight = ANativeWindow_getHeight(m_options.externalNativeWindow);
        }
        if (0 >= displayWidth || 0 >= displayHeight)
        {
            LogInfo("[AImGui] display size missing, query GetDisplayInfo");
            auto displayInfo = ANativeWindowCreator::GetDisplayInfo();
            displayTheta = displayInfo.theta;
            displayWidth = displayInfo.width;
            displayHeight = displayInfo.height;
        }
        LogInfo("[AImGui] display angle:%d width:%d height:%d", displayTheta, displayWidth, displayHeight);

        if (RenderType::RenderClient != m_options.renderType)
        {
            if (m_usesExternalNativeWindow)
            {
                m_nativeWindow = m_options.externalNativeWindow;
                LogInfo("[AImGui] using external native window=%p size=%dx%d", m_nativeWindow, displayWidth, displayHeight);
            }
            else
            {
                // Create native window
                LogInfo("[AImGui] creating native window");
                m_nativeWindow = ANativeWindowCreator::Create({.name = "AImGui", .width = displayWidth, .height = displayHeight, .skipScreenshot = false});
                if (nullptr == m_nativeWindow)
                {
                    LogError("[AImGui] ANativeWindow create failed");
                    return false;
                }

                // Acquire native window created by the private compositor path.
                LogInfo("[AImGui] acquiring native window=%p", m_nativeWindow);
                ANativeWindow_acquire(m_nativeWindow);
            }
            LogInfo("[AImGui] native window acquired size=%dx%d", ANativeWindow_getWidth(m_nativeWindow), ANativeWindow_getHeight(m_nativeWindow));
        }

        // EGL initialization
        LogInfo("[AImGui] eglGetDisplay begin");
        m_defaultDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (EGL_NO_DISPLAY == m_defaultDisplay)
        {
            LogError("[AImGui] EGL get default display failed: %d", eglGetError());
            return false;
        }

        LogInfo("[AImGui] eglInitialize begin");
        if (EGL_TRUE != eglInitialize(m_defaultDisplay, 0, 0))
        {
            LogError("[AImGui] EGL initialize failed: %d", eglGetError());
            return false;
        }

        EGLint numEglConfig = 0;
        EGLConfig eglConfig{};
        std::pair<EGLint, EGLint> eglConfigAttributeList[] = {
            {
                EGL_SURFACE_TYPE,
                RenderType::RenderClient != m_options.renderType ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
            },                                         // 根据服务类型选择渲染表面类型为窗口或像素缓冲区
            {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT}, // 使用OpenGL ES 2.0
            {EGL_RED_SIZE, 8},                         // 红色分量位数为8位
            {EGL_GREEN_SIZE, 8},                       // 绿色分量位数为8位
            {EGL_BLUE_SIZE, 8},                        // 蓝色分量位数为8位
            {EGL_ALPHA_SIZE, 8},                       // Alpha 位数为8位
            {EGL_DEPTH_SIZE, 24},                      // 深度缓冲位数为24位
            {EGL_STENCIL_SIZE, 8},                     // 模板缓冲位数为8位
            {EGL_SAMPLE_BUFFERS, 0},                   // 多重采样抗锯齿缓冲禁用
            {EGL_NONE, EGL_NONE},
        };
        LogInfo("[AImGui] eglChooseConfig begin");
        if (EGL_TRUE != eglChooseConfig(m_defaultDisplay, reinterpret_cast<const EGLint *>(eglConfigAttributeList), &eglConfig, 1, &numEglConfig))
        {
            LogError("[AImGui] EGL choose config failed: %d", eglGetError());
            return false;
        }
        if (0 == numEglConfig)
        {
            LogError("[AImGui] EGL choose config failed: unsupported config attribute list");
            return false;
        }

        if (RenderType::RenderClient != m_options.renderType)
        {
            EGLint eglBufferFormat;
            LogInfo("[AImGui] eglGetConfigAttrib begin");
            if (EGL_TRUE != eglGetConfigAttrib(m_defaultDisplay, eglConfig, EGL_NATIVE_VISUAL_ID, &eglBufferFormat))
            {
                LogError("[AImGui] EGL get config attribute failed: %d", eglGetError());
                return false;
            }
            LogInfo("[AImGui] ANativeWindow_setBuffersGeometry begin format=%d", eglBufferFormat);
            ANativeWindow_setBuffersGeometry(m_nativeWindow, 0, 0, eglBufferFormat);
            LogInfo("[AImGui] eglCreateWindowSurface begin nativeWindow=%p", m_nativeWindow);
            m_eglSurface = eglCreateWindowSurface(m_defaultDisplay, eglConfig, m_nativeWindow, nullptr);
        }
        else
        {
            std::pair<EGLint, EGLint> bufferAttribute[] = {
                {EGL_WIDTH, displayWidth},
                {EGL_HEIGHT, displayHeight},
                {EGL_NONE, EGL_NONE},
            };
            LogInfo("[AImGui] eglCreatePbufferSurface begin");
            m_eglSurface = eglCreatePbufferSurface(m_defaultDisplay, eglConfig, reinterpret_cast<const EGLint *>(bufferAttribute));
        }
        if (EGL_NO_SURFACE == m_eglSurface)
        {
            LogError("[AImGui] EGL create surface failed: %d", eglGetError());
            return false;
        }

        const char* glslVersion = "#version 300 es";
        int eglContextClientVersion = 3;

        const EGLint eglContextAttribListGles3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        const EGLint eglContextAttribListGles2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};

        LogInfo("[AImGui] eglCreateContext begin prefer=GLES3");
        m_eglContext = eglCreateContext(m_defaultDisplay, eglConfig, EGL_NO_CONTEXT, eglContextAttribListGles3);
        if (EGL_NO_CONTEXT == m_eglContext)
        {
            auto eglError = eglGetError();
            LogInfo("[AImGui] EGL create GLES3 context failed: %d, fallback to GLES2", eglError);
            m_eglContext = eglCreateContext(m_defaultDisplay, eglConfig, EGL_NO_CONTEXT, eglContextAttribListGles2);
            if (EGL_NO_CONTEXT == m_eglContext)
            {
                LogError("[AImGui] EGL create context failed after GLES2 fallback: %d", eglGetError());
                return false;
            }

            eglContextClientVersion = 2;
            glslVersion = "#version 100";
        }

        LogInfo("[AImGui] eglMakeCurrent begin");
        if (EGL_TRUE != eglMakeCurrent(m_defaultDisplay, m_eglSurface, m_eglSurface, m_eglContext))
        {
            LogError("[AImGui] EGL make current failed: %d", eglGetError());
            return false;
        }

        if (RenderType::RenderClient != m_options.renderType && m_options.disableVsync)
        {
            eglSwapInterval(m_defaultDisplay, 0);
            LogInfo("[AImGui] eglSwapInterval set to 0 for low latency");
        }

        const char* glVersion = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* glslVersionRuntime = reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        LogInfo("[AImGui] GL runtime context=GLES%d version=%s glsl=%s",
                eglContextClientVersion,
                glVersion ? glVersion : "(null)",
                glslVersionRuntime ? glslVersionRuntime : "(null)");

        // ImGui initialization
        LogInfo("[AImGui] ImGui init begin");
        IMGUI_CHECKVERSION();

        m_imguiContext = ImGui::CreateContext();
        if (nullptr == m_imguiContext)
        {
            LogError("[AImGui] ImGui create context failed");
            return false;
        }

        auto &imguiIO = ImGui::GetIO();

        imguiIO.IniFilename = nullptr;
        imguiIO.ConfigInputTrickleEventQueue = false;
        ImGui::StyleColorsDark();
        if (m_options.styleScale > 0.0f && m_options.styleScale != 1.0f)
            ImGui::GetStyle().ScaleAllSizes(m_options.styleScale);

        LoadPreferredImGuiFont(imguiIO, m_options.fontSizePixels > 0.0f ? m_options.fontSizePixels : 18.0f);
        if (RenderType::RenderClient == m_options.renderType && m_options.exchangeFontData)
        {
            auto sharedFontData = ImGui::GetSharedFontData();
            uint32_t packetSize = static_cast<uint32_t>(sharedFontData.size());
            // First packet
            WriteData(&packetSize, sizeof(packetSize));
            WriteData(sharedFontData.data(), sharedFontData.size());
            m_fontPacketCount = 1;
            m_lastFontPacketSize = sharedFontData.size();
            LogInfo("[AImGui] Client sent initial font packet size=%u", packetSize);
        }

        if (RenderType::RenderClient != m_options.renderType)
        {
            if (!ImGui_ImplAndroid_Init(m_nativeWindow))
            {
                LogError("[AImGui] ImGui init android implement failed");
                return false;
            }
        }
        else
        {
            imguiIO.BackendPlatformName = "imgui_impl_aimgui";
        }
        LogInfo("[AImGui] ImGui OpenGL backend init glsl=%s", glslVersion);
        if (!ImGui_ImplOpenGL3_Init(glslVersion))
        {
            LogError("[AImGui] ImGui init OpenGL3 failed");
            return false;
        }

        glViewport(0, 0, displayWidth, displayHeight);
        glClearColor(0.f, 0.f, 0.f, 0.f);

        m_rotateTheta = displayTheta;
        m_screenWidth = displayWidth;
        m_screenHeight = displayHeight;

        LogInfo("[AImGui] InitEnvironment success screen=%dx%d rotate=%d", m_screenWidth, m_screenHeight, m_rotateTheta);

        m_state = true;
        if (RenderType::RenderServer == m_options.renderType)
        {
            m_serverWorkerThread = std::make_unique<std::thread>(&AImGui::ServerWorker, this);
            LogInfo("[AImGui] Server worker thread started");
        }

        return true;
    }
    void AImGui::UnInitEnvironment()
    {
        m_state = false;

        int clientFd = m_clientFd;
        int serverFd = m_serverFd;
        m_clientFd = -1;
        m_serverFd = -1;

        if (-1 != clientFd)
        {
            shutdown(clientFd, SHUT_RDWR);
            close(clientFd);
        }
        if (-1 != serverFd)
        {
            shutdown(serverFd, SHUT_RDWR);
            close(serverFd);
        }

        if (nullptr != m_imguiContext)
        {
            ImGui_ImplOpenGL3_Shutdown();

            if (RenderType::RenderClient != m_options.renderType)
                ImGui_ImplAndroid_Shutdown();
            else
                ImGui::GetIO().BackendPlatformName = nullptr;
            ImGui::DestroyContext(m_imguiContext);
        }

        if (EGL_NO_DISPLAY != m_defaultDisplay)
        {
            eglMakeCurrent(m_defaultDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

            if (EGL_NO_CONTEXT != m_eglContext)
                eglDestroyContext(m_defaultDisplay, m_eglContext);
            if (EGL_NO_SURFACE != m_eglSurface)
                eglDestroySurface(m_defaultDisplay, m_eglSurface);

            eglTerminate(m_defaultDisplay);
        }

        if (RenderType::RenderClient != m_options.renderType)
        {
            if (nullptr != m_nativeWindow)
            {
                ANativeWindow_release(m_nativeWindow);
                if (!m_usesExternalNativeWindow)
                    android::ANativeWindowCreator::Destroy(m_nativeWindow);
            }
        }

        if (m_serverWorkerThread && m_serverWorkerThread->joinable())
            m_serverWorkerThread->join();
        m_serverWorkerThread.reset();

        m_imguiContext = nullptr;
        m_eglContext = EGL_NO_CONTEXT;
        m_eglSurface = EGL_NO_SURFACE;
        m_defaultDisplay = EGL_NO_DISPLAY;
        m_nativeWindow = nullptr;
        m_usesExternalNativeWindow = false;
    }

    void AImGui::ServerWorker()
    {
        while (m_state)
        {
            m_clientFd = accept(m_serverFd, nullptr, nullptr);
            if (0 > m_clientFd)
            {
                if (m_state)
                {
                    LogDebug("[-] Server accept client connect failed, %d:%s", errno, strerror(errno));
                    m_state = false;
                }
                return;
            }

            if (m_options.tcpNoDelay)
            {
                int optionValue = 1;
                setsockopt(m_clientFd, IPPROTO_TCP, TCP_NODELAY, &optionValue, sizeof(optionValue));
            }

            {
                std::lock_guard<std::mutex> lock(m_renderDataMutex);
                m_serverFontData.clear();
                m_serverRenderData.clear();
                m_serverRenderDataBack.clear();
                m_renderState = RenderState::ReadData;
                m_fontPacketCount = 0;
                m_renderPacketCount = 0;
                m_renderFrameCount = 0;
                m_lastFontPacketSize = 0;
                m_lastRenderPacketSize = 0;
                m_lastRenderDecodedSize = 0;
                m_serverFontPacketReceived = false;
            }
            LogInfo("[AImGui] Server accepted client fd=%d", m_clientFd);

            uint32_t packetSize = 0;
            while (m_state)
            {
                if (static_cast<int>(sizeof(packetSize)) > ReadData(&packetSize, sizeof(packetSize)))
                {
                    if (m_state)
                        LogDebug("[-] Server can not read packet size, %d:%s", errno, strerror(errno));
                    break;
                }
                if (packetSize > m_maxPacketSize)
                {
                    LogDebug("[-] Packet is too large: %2.f", packetSize / 1024.f / 1024.f);
                    break;
                }

                if (m_serverRenderDataBack.size() < packetSize)
                    m_serverRenderDataBack.resize(packetSize);
                auto readResult = ReadData(m_serverRenderDataBack.data(), packetSize);
                if (0 >= readResult)
                {
                    if (m_state)
                        LogDebug("[-] Client disconnect or read failed, readResult:%d  %d:%s", readResult, errno, strerror(errno));
                    break;
                }

                if (RenderState::ReadData != m_renderState)
                    continue;
                if (m_options.exchangeFontData && !m_serverFontPacketReceived) // NOTE: First packet is font data
                {
                    std::lock_guard<std::mutex> lock(m_renderDataMutex);
                    m_serverFontData.swap(m_serverRenderDataBack);
                    m_serverFontPacketReceived = true;
                    m_fontPacketCount++;
                    m_lastFontPacketSize = packetSize;
                    LogInfo("[AImGui] Server received font packet size=%u", packetSize);
                    m_renderState = RenderState::SetFont;
                }
                else
                {
                    std::lock_guard<std::mutex> lock(m_renderDataMutex);
                    if (!m_options.compressionFrameData)
                        m_serverRenderData.swap(m_serverRenderDataBack);
                    else
                    {
                        static std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> decompressContext(ZSTD_createDCtx(), &ZSTD_freeDCtx);

                        if (nullptr == decompressContext)
                        {
                            LogDebug("[-] Server can not create decompress context");
                            m_state = false;
                            break;
                        }

                        uint32_t sharedDataSize = 0;
                        memcpy(&sharedDataSize, m_serverRenderDataBack.data(), sizeof(sharedDataSize));
                        if (m_serverRenderData.size() < sharedDataSize)
                            m_serverRenderData.resize(sharedDataSize);

                        ZSTD_inBuffer input{m_serverRenderDataBack.data() + sizeof(sharedDataSize), packetSize - sizeof(sharedDataSize), 0};
                        ZSTD_outBuffer output{m_serverRenderData.data(), m_serverRenderData.size(), 0};
                        if (0 != ZSTD_decompressStream(decompressContext.get(), &output, &input))
                            LogDebug("[-] Server decompression frame data error");

                        m_lastRenderDecodedSize = output.pos;
                    }
                    m_renderPacketCount++;
                    m_lastRenderPacketSize = packetSize;
                    if (0 == (m_renderPacketCount % 180) || 1 == m_renderPacketCount)
                    {
                        LogInfo("[AImGui] Server received render packet count=%llu packet=%zu decoded=%zu",
                                static_cast<unsigned long long>(m_renderPacketCount),
                                m_lastRenderPacketSize,
                                m_lastRenderDecodedSize);
                    }
                    m_renderState = RenderState::Rendering;
                }
            }

            int clientFd = m_clientFd;
            m_clientFd = -1;
            if (-1 != clientFd)
            {
                shutdown(clientFd, SHUT_RDWR);
                close(clientFd);
            }

            // 客户端断开后清空渲染数据, 渲染线程会在 ReadData 状态做 glClear
            {
                std::lock_guard<std::mutex> lock(m_renderDataMutex);
                m_serverRenderData.clear();
                m_renderState = RenderState::ReadData;
            }

            if (m_state)
                LogInfo("[AImGui] Server client disconnected, waiting next client");
        }
    }

    int AImGui::ReadData(void *buffer, size_t readSize)
    {
        size_t packetReaded = 0;
        pollfd pfd{
            .fd = m_clientFd,
            .events = POLLIN,
        };

        while (packetReaded < readSize)
        {
            auto pollResult = poll(&pfd, 1, 1000); // Wait for 1s
            if (0 >= pollResult)
                return pollResult;

            auto readResult = read(m_clientFd, reinterpret_cast<char *>(buffer) + packetReaded, readSize - packetReaded);
            if (0 >= readResult)
                return readResult;
            packetReaded += readResult;
        }

        return static_cast<int>(packetReaded);
    }
    void AImGui::WriteData(void *data, size_t size)
    {
        if (RenderType::RenderServer == m_options.renderType && -1 == m_clientFd)
            return;

        size_t totalWritten = 0;
        while (totalWritten < size)
        {
            auto result = write(m_clientFd, reinterpret_cast<char *>(data) + totalWritten, size - totalWritten);
            if (0 >= result)
            {
                LogDebug("[-] WriteData failed, result:%zd  %d:%s", result, errno, strerror(errno));
                if (RenderType::RenderServer == m_options.renderType)
                {
                    int clientFd = m_clientFd;
                    m_clientFd = -1;
                    if (-1 != clientFd)
                    {
                        shutdown(clientFd, SHUT_RDWR);
                        close(clientFd);
                    }
                    return;
                }
                m_state = false;
                return;
            }
            totalWritten += result;
        }
    }
} // namespace android