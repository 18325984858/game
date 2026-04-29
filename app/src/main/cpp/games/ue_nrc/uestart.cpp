#include "uestart.h"
#include "../../core/log/log.h"
#include "../../core/stealth/stealth_hooks.h"
#include "engine/UE4NrcDumper.h"
#include "engine/UE4NrcStruct.h"
#include "nrc/nrc.h"
#include "AImGui.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdexcept>
#include <cstddef>

// =====================================================================
//  配置读取
// =====================================================================
namespace {

static bool readConfigFlag(const char* key) {
    int fd = open("/data/local/tmp/dobby_config.txt", O_RDONLY);
    if (fd < 0) return false;
    char buf[256] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    return strstr(buf, key) != nullptr;
}

static bool readDumperEnabled() { return readConfigFlag("ue_dumper=1"); }

// =====================================================================
//  显示信息
// =====================================================================
struct DisplayInfo { int width = 0, height = 0, rotateTheta = 0; };

static DisplayInfo queryShellDisplayInfo() {
    DisplayInfo info;
    FILE* pipe = popen("/system/bin/wm size 2>/dev/null", "r");
    if (pipe) {
        char buf[256] = {};
        while (fgets(buf, sizeof(buf), pipe)) {
            int w = 0, h = 0;
            if (sscanf(buf, "%*[^0-9]%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                info.width = w; info.height = h;
            }
        }
        pclose(pipe);
    }
    if (info.width > 0 && info.height > 0 && info.width < info.height) {
        std::swap(info.width, info.height);
    }
    if (info.width <= 0) { info.width = 2400; info.height = 1080; }
    return info;
}

} // namespace

// =====================================================================
//  注入参数
// =====================================================================
struct NrcInjectParams {
    uintptr_t base;
    uint64_t  moduleSize;
    uint32_t  offNamePool;
    uint32_t  offGUObjectArrayNum;
    uint32_t  offGUObjectArrayChunks;
    uint32_t  offGWorld;
};

// =====================================================================
//  GUI 线程
// =====================================================================
static std::atomic<bool> g_nrcGuiThreadStarted{false};

static bool nrcIsGameAlive() {
    char cmdline[256] = {};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) return false;
    ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    return n > 0 && strstr(cmdline, "tencent.nrc") != nullptr;
}

static void NrcGuiThread() {
    struct ResetGuard {
        ~ResetGuard() {
            g_nrcGuiThreadStarted.store(false, std::memory_order_release);
            LOG(LOG_LEVEL_INFO, "[NRC GUI] 线程退出, 释放单例锁");
        }
    } guard;

    LOG(LOG_LEVEL_INFO, "[NRC GUI] 线程启动, 连接 Overlay 服务...");

    DisplayInfo dpy = queryShellDisplayInfo();
    LOG(LOG_LEVEL_INFO, "[NRC GUI] Display %dx%d", dpy.width, dpy.height);

    android::AImGui::Options opts{
        .renderType = android::AImGui::RenderType::RenderClient,
        .compressionFrameData = false,
        .autoUpdateOrientation = false,
        .exchangeFontData = true,
        .tcpNoDelay = true,
        .disableVsync = true,
        .styleScale = 1.75f,
        .fontSizePixels = 24.0f,
        .screenWidth = dpy.width,
        .screenHeight = dpy.height,
        .rotateTheta = dpy.rotateTheta,
        .clientConnectAddress = "127.0.0.1",
        .port = 16890,  // NRC 专用端口 (PUBG=16888, DFM=16889, NRC=16890)
    };

    std::unique_ptr<android::AImGui> imgui;
    for (int attempt = 1; ; ++attempt) {
        if (!nrcIsGameAlive()) {
            LOG(LOG_LEVEL_INFO, "[NRC GUI] 游戏退出, 放弃");
            return;
        }
        try { imgui = std::make_unique<android::AImGui>(opts); }
        catch (const std::exception& ex) {
            LOG(LOG_LEVEL_ERROR, "[NRC GUI] AImGui 异常: %s", ex.what());
            imgui.reset();
        } catch (...) { imgui.reset(); }
        if (imgui && *imgui) break;
        if (attempt % 10 == 1) {
            LOG(LOG_LEVEL_INFO, "[NRC GUI] 等待 Overlay 服务: attempt=%d", attempt);
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    LOG(LOG_LEVEL_INFO, "[NRC GUI] AImGui 初始化完成");

    auto lastAlive = std::chrono::steady_clock::now();

    constexpr int64_t kStaleExitMs     = 10000;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAlive).count() > 2000) {
            lastAlive = now;
            if (!nrcIsGameAlive()) {
                LOG(LOG_LEVEL_INFO, "[NRC GUI] 游戏退出");
                for (int i = 0; i < 5; ++i) { imgui->BeginFrame(); imgui->EndFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
                break;
            }
        }
        int64_t staleMs = nrc::SharedNrcData::getInstance().getMsSinceLastPush();
        if (staleMs >= 0 && staleMs > kStaleExitMs) {
            LOG(LOG_LEVEL_INFO, "[NRC GUI] 数据陈旧 %lldms, 退出", (long long)staleMs);
            for (int i = 0; i < 5; ++i) { imgui->BeginFrame(); imgui->EndFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); }
            break;
        }

        while (imgui->PollInputReady(0)) imgui->ProcessInputEvent();

        imgui->BeginFrame();
        // 绘制功能已移除; 仅保留帧心跳以维持 Overlay 连接
        imgui->EndFrame();
    }
}

// =====================================================================
//  工作线程
// =====================================================================
static void NrcWorkerThread(NrcInjectParams* params) {
    NrcInjectParams p = *params;
    delete params;

    LOG(LOG_LEVEL_INFO, "[NrcWorker] 启动 base=%p moduleSize=0x%llX NP=0x%X Num=0x%X Chunks=0x%X GW=0x%X",
        (void*)p.base, (unsigned long long)p.moduleSize,
        p.offNamePool, p.offGUObjectArrayNum, p.offGUObjectArrayChunks, p.offGWorld);

    // 等待引擎就绪 (60s 内每 0.5s 探测一次)
    constexpr int kMaxWait = 60;
    int memFd = open("/proc/self/mem", O_RDONLY);
    bool ready = false;
    for (int i = 0; i < kMaxWait * 2; ++i) {
        uint32_t num = 0;
        uintptr_t blk0 = 0, world = 0;
        bool ok = (memFd >= 0);
        if (ok) {
            uintptr_t aBlk = p.base + p.offNamePool + offsetof(ue4nrc::FNamePool, Blocks);
            ok = (pread(memFd, &blk0, sizeof(blk0), (off_t)aBlk) == (ssize_t)sizeof(blk0)) && (blk0 > 0x10000);
        }
        if (ok) {
            ok = (pread(memFd, &num, sizeof(num), (off_t)(p.base + p.offGUObjectArrayNum)) == (ssize_t)sizeof(num)) && (num > 100);
        }
        if (ok) {
            ok = (pread(memFd, &world, sizeof(world), (off_t)(p.base + p.offGWorld)) == (ssize_t)sizeof(world));
            // GWorld 启动时可为空, 不强制
        }
        if (ok) {
            LOG(LOG_LEVEL_INFO, "[NrcWorker] 引擎就绪 num=%u block0=%p (%.1fs)", num, (void*)blk0, i * 0.5f);
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (memFd >= 0) close(memFd);
    if (!ready) {
        LOG(LOG_LEVEL_ERROR, "[NrcWorker] 引擎等待超时, 放弃");
        return;
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));

    const bool wantDumper = readDumperEnabled();
    LOG(LOG_LEVEL_INFO, "[NrcWorker] 配置: ue_dumper=%d", wantDumper);

    if (wantDumper) {
        auto dumper = std::make_unique<ue4nrc::UE4NrcDumper>(
            p.base,
            (uintptr_t)p.moduleSize,
            p.offNamePool,
            p.offGUObjectArrayNum,
            p.offGUObjectArrayChunks,
            p.offGWorld,
            "/data/data/com.tencent.nrc/cache/dumps/"
        );
        if (!dumper->init()) {
            LOG(LOG_LEVEL_ERROR, "[NrcWorker] UE4NrcDumper init 失败");
        } else {
            LOG(LOG_LEVEL_INFO, "[NrcWorker] 开始 dumpAll");
            dumper->dumpAll();
            LOG(LOG_LEVEL_INFO, "[NrcWorker] dumpAll 完成");
        }
    } else {
        LOG(LOG_LEVEL_INFO, "[NrcWorker] ue_dumper 未启用, 跳过 dump");
    }

    // 启动占位 Monitor (持续 push 空帧, 维持 GUI 心跳)
    auto* mon = new nrc::NrcMatchMonitor(
        p.base, (uintptr_t)p.moduleSize,
        p.offNamePool, p.offGUObjectArrayNum, p.offGUObjectArrayChunks, p.offGWorld);
    if (!mon->start()) {
        LOG(LOG_LEVEL_ERROR, "[NrcWorker] NrcMatchMonitor 启动失败");
        delete mon;
    } else {
        LOG(LOG_LEVEL_INFO, "[NrcWorker] NrcMatchMonitor 已启动 (占位)");
    }

    LOG(LOG_LEVEL_INFO, "[NrcWorker] 工作线程退出");
}

// =====================================================================
//  入口
// =====================================================================
static std::atomic<bool> g_nrcStarted{false};

extern "C" __attribute__((visibility("default")))
bool MyStartPointNRC(void* plibUE4ModeBase, void* pNamePool, void* pGWorld,
                     void* pGUObjectArrayNum, void* pGUObjectArrayChunks,
                     uint64_t moduleSize, void* /*pData*/) {
    // 安装反检测 hook: 隐藏自身 .so (dl_iterate_phdr/dladdr) + 伪造 root/解锁相关系统属性
    // (ro.secure / ro.debuggable / ro.boot.verifiedbootstate 等)。幂等, 多次调用安全。
    installStealthHooks();
    bool expected = false;
    if (!g_nrcStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        LOG(LOG_LEVEL_INFO, "[MyStartPointNRC] 已启动, 跳过");
        return true;
    }
    if (!plibUE4ModeBase || !pGUObjectArrayNum) {
        LOG(LOG_LEVEL_ERROR, "[MyStartPointNRC] 参数为空");
        return false;
    }

    LOG(LOG_LEVEL_INFO, "[MyStartPointNRC] base=%p NP=%p GW=%p Num=%p Chunks=%p moduleSize=0x%llX",
        plibUE4ModeBase, pNamePool, pGWorld, pGUObjectArrayNum, pGUObjectArrayChunks,
        (unsigned long long)moduleSize);

    auto* params = new NrcInjectParams{
        .base                   = (uintptr_t)plibUE4ModeBase,
        .moduleSize             = moduleSize,
        .offNamePool            = (uint32_t)(uintptr_t)pNamePool,
        .offGUObjectArrayNum    = (uint32_t)(uintptr_t)pGUObjectArrayNum,
        .offGUObjectArrayChunks = (uint32_t)(uintptr_t)pGUObjectArrayChunks,
        .offGWorld              = (uint32_t)(uintptr_t)pGWorld,
    };

    bool guiExpected = false;
    if (g_nrcGuiThreadStarted.compare_exchange_strong(guiExpected, true, std::memory_order_acq_rel)) {
        std::thread(NrcGuiThread).detach();
    }
    std::thread(NrcWorkerThread, params).detach();
    return true;
}

OBFU_ATTRS_END
