#include "Global.h"
#include "AImGui.h"

#include <jni.h>

#include <thread>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

static bool shouldStartDemoGui()
{
    char cmdline[256] = {};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    if (n <= 0) return false;
    cmdline[n] = '\0';
    // overlay 服务进程: 由 PublicOverlayRenderer 驱动, 不能启动 demo Gui
    if (strstr(cmdline, ":overlay") != nullptr) return false;
    // 游戏进程 (注入): 由 uestart.cpp 的 UE4GuiThread 驱动, 不能启动 demo Gui
    if (strstr(cmdline, "pubgmhd") != nullptr) return false;
    if (strstr(cmdline, "tmgp.dfm") != nullptr) return false;
    // 仅在 dobbyproject 主进程中启动 demo Gui (调试用途)
    return strstr(cmdline, "dobbyproject") != nullptr && strstr(cmdline, ":") == nullptr;
}

void Gui()
{
    android::AImGui imgui;
    bool state = true, showDemoWindow = false, showAnotherWindow = false;
    ImVec4 clearColor(0.45f, 0.55f, 0.60f, 1.00f);

    if (!imgui)
    {
        LogInfo("[-] ImGui initialization failed");
        return;
    }

    std::thread processInputEventThread(
        [&]
        {
            while (state)
            {
                imgui.ProcessInputEvent();
                std::this_thread::sleep_for(std::chrono::microseconds(1));
            }
        });

    while (state)
    {
        imgui.BeginFrame();

        // 1. Show the big demo window (Most of the sample code is in ImGui::ShowDemoWindow()! You can browse its code to learn more about Dear ImGui!).
        if (showDemoWindow)
            ImGui::ShowDemoWindow(&showDemoWindow);

        // 2. Show a simple window that we create ourselves. We use a Begin/End pair to create a named window.
        {
            static float f = 0.0f;
            static int counter = 0;

            ImGui::Begin("Hello, world!", &state); // Create a window called "Hello, world!" and append into it.

            ImGui::Text("This is some useful text.");        // Display some text (you can use a format strings too)
            ImGui::Checkbox("Demo Window", &showDemoWindow); // Edit bools storing our window open/close state
            ImGui::Checkbox("Another Window", &showAnotherWindow);

            ImGui::SliderFloat("float", &f, 0.0f, 1.0f);            // Edit 1 float using a slider from 0.0f to 1.0f
            ImGui::ColorEdit3("clear color", (float *)&clearColor); // Edit 3 floats representing a color

            if (ImGui::Button("Button")) // Buttons return true when clicked (most widgets return true when edited/activated)
                counter++;
            ImGui::SameLine();
            ImGui::Text("counter = %d", counter);

            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
            ImGui::End();
        }

        // 3. Show another simple window.
        if (showAnotherWindow)
        {
            ImGui::Begin("Another Window", &showAnotherWindow); // Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
            ImGui::Text("Hello from another window!");
            if (ImGui::Button("Close Me"))
                showAnotherWindow = false;
            ImGui::End();
        }

        imgui.EndFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (processInputEventThread.joinable())
        processInputEventThread.join();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    LogInfo("[=] =============================================Injected so has been loaded.=============================================");

    // demo Gui 仅在 dobbyproject 主进程中启动 (调试用途)
    // overlay 进程由 PublicOverlayRenderer 驱动, 游戏进程由 UE4GuiThread 驱动
    if (shouldStartDemoGui())
    {
        std::thread(Gui).detach();
    }
    else
    {
        LogInfo("[=] Non-main process or injected target, skipping demo Gui thread");
    }

    return JNI_VERSION_1_6;
}