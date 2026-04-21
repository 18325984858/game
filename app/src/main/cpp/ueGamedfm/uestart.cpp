#include "uestart.h"
#include "../core/log/log.h"
#include "UE5DfmDumper.h"
#include "UE5DfmStruct.h"
#include "libUE5Header/UE5Header.h"
#include "dfm/dfm.h"
#include "Draw/DfmDraw.h"
#include "AImGui.h"
#include "ANativeWindowCreator.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include <cstddef>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <string>
#include <dlfcn.h>
#include <jni.h>
#include <sys/stat.h>
#include <stdexcept>

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
static bool readHeaderEnabled() { return readConfigFlag("ue_header=1"); }
static bool readLogEnabled()    { return readConfigFlag("log=1"); }

// =====================================================================
//  JNI Toast 工具
// =====================================================================
namespace toast_util {
    using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);

    static JavaVM* getJavaVM() {
        static JavaVM* s_vm = nullptr;
        static bool s_tried = false;
        if (s_tried) return s_vm;
        s_tried = true;

        JavaVM* vmBuf[2] = {nullptr, nullptr};
        jsize vmCount = 0;

        auto fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
        if (fn && fn(vmBuf, 2, &vmCount) == JNI_OK && vmCount > 0) {
            s_vm = vmBuf[0];
        }

        if (!s_vm) {
            void* libArt = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
            if (libArt) {
                fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(libArt, "JNI_GetCreatedJavaVMs"));
                if (fn && fn(vmBuf, 2, &vmCount) == JNI_OK && vmCount > 0) s_vm = vmBuf[0];
                dlclose(libArt);
            }
        }

        LOG(LOG_LEVEL_INFO, "getJavaVM: resolved vm=%p", s_vm);
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

        jclass atClass = env->FindClass("android/app/ActivityThread");
        if (!atClass || env->ExceptionCheck()) { env->ExceptionClear(); if (attached) vm->DetachCurrentThread(); return; }
        jmethodID curApp = env->GetStaticMethodID(atClass, "currentApplication", "()Landroid/app/Application;");
        jobject ctx = curApp ? env->CallStaticObjectMethod(atClass, curApp) : nullptr;
        env->DeleteLocalRef(atClass);
        if (!ctx || env->ExceptionCheck()) { env->ExceptionClear(); if (attached) vm->DetachCurrentThread(); return; }

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
        env->DeleteLocalRef(ctx);
        if (attached) vm->DetachCurrentThread();
    }
} // namespace toast_util

} // namespace

// =====================================================================
//  注入偏移参数包 (从 MyStartPointDFM 计算并传递到工作线程)
// =====================================================================
struct DfmInjectParams {
    uintptr_t base;
    uint64_t  moduleSize;
    uint32_t  offNamePool;
    uint32_t  offGUObjectArrayNum;
    uint32_t  offGUObjectArrayChunks;
    uint32_t  offGWorld;
};

// =====================================================================
//  显示信息查询 (Java WindowManager / shell 回退)
// =====================================================================
namespace {

struct DisplayInfo {
    int width = 0, height = 0, rotateTheta = 0;
};

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
    // 横屏归一化
    if (info.width > 0 && info.height > 0 && info.width < info.height) {
        int tmp = info.width; info.width = info.height; info.height = tmp;
    }
    if (info.width <= 0) { info.width = 2400; info.height = 1080; }
    LOG(LOG_LEVEL_INFO, "queryShellDisplayInfo: %dx%d r%d", info.width, info.height, info.rotateTheta);
    return info;
}

} // namespace

// =====================================================================
//  DFM GUI 线程 — AImGui RenderClient 绘制 (与 PUBG UE4GuiThread 相同模式)
// =====================================================================

static std::atomic<bool> g_dfmGuiThreadStarted{false};

static void DfmGuiThread() {
    struct GuiResetGuard {
        ~GuiResetGuard() {
            g_dfmGuiThreadStarted.store(false, std::memory_order_release);
            LOG(LOG_LEVEL_INFO, "DFM GUI 线程已退出, 释放单例锁");
        }
    } resetGuard;

    LOG(LOG_LEVEL_INFO, "DFM GUI 线程启动, 无限重试连接 Overlay 服务 (每 3 秒)...");

    DisplayInfo displayInfo = queryShellDisplayInfo();
    LOG(LOG_LEVEL_INFO, "AImGui RenderClient: %dx%d", displayInfo.width, displayInfo.height);

    android::AImGui::Options opts{
        .renderType = android::AImGui::RenderType::RenderClient,
        .compressionFrameData = false,
        .autoUpdateOrientation = false,
        .exchangeFontData = true,
        .tcpNoDelay = true,
        .disableVsync = true,
        .styleScale = 1.75f,
        .fontSizePixels = 24.0f,
        .screenWidth = displayInfo.width,
        .screenHeight = displayInfo.height,
        .rotateTheta = displayInfo.rotateTheta,
        .clientConnectAddress = "127.0.0.1",
    };

    // 检测游戏进程存活 (用于退出重试循环)
    auto isGameAlive = []() -> bool {
        char cmdline[256] = {};
        int fd = open("/proc/self/cmdline", O_RDONLY);
        if (fd < 0) return false;
        ssize_t n = read(fd, cmdline, sizeof(cmdline) - 1);
        close(fd);
        return n > 0 && strstr(cmdline, "tmgp.dfm") != nullptr;
    };

    std::unique_ptr<android::AImGui> imgui;
    // 无限重试连接 — 用户可能先启动游戏再启动 app overlay 服务
    for (int attempt = 1; ; ++attempt) {
        if (!isGameAlive()) {
            LOG(LOG_LEVEL_INFO, "RenderClient: 游戏进程已退出, 放弃连接");
            return;
        }
        try {
            imgui = std::make_unique<android::AImGui>(opts);
            LOG(LOG_LEVEL_INFO, "AImGui 构造完成: attempt=%d state=%d", attempt, *imgui ? 1 : 0);
        } catch (const std::exception& ex) {
            LOG(LOG_LEVEL_ERROR, "AImGui 构造异常: attempt=%d error=%s", attempt, ex.what());
            imgui.reset();
        } catch (...) {
            LOG(LOG_LEVEL_ERROR, "AImGui 构造异常: attempt=%d error=unknown", attempt);
            imgui.reset();
        }
        if (imgui && *imgui) break;
        if (attempt % 10 == 1) {
            LOG(LOG_LEVEL_INFO, "等待公开 Overlay 服务: attempt=%d (每 3 秒重试)", attempt);
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }

    if (!imgui || !(*imgui)) {
        LOG(LOG_LEVEL_ERROR, "AImGui RenderClient 初始化失败: 无法连接公开 Overlay 服务");
        return;
    }

    LOG(LOG_LEVEL_INFO, "AImGui RenderClient 初始化完成, 开始渲染循环");

    // 注意: ProcessInputEvent 必须与 BeginFrame/EndFrame 在同一线程调用,
    // 否则 ImGui::IO 的 InputEventsQueue 会多线程竞争导致 ImVector 越界崩溃

    dfmdraw::DfmOverlay overlay;
    dfm::DrawDfmData gameData;

    auto lastAliveCheck = std::chrono::steady_clock::now();

    // 数据陈旧/游戏退出阈值 (毫秒)
    //  - kStaleSkipDrawMs : 超过此时间未收到新数据 → 停止绘制 UI, 仅送空帧 (清屏)
    //  - kStaleExitMs     : 超过此时间未收到新数据 → 主动退出 GUI 线程, 断开 RenderClient
    constexpr int64_t kStaleSkipDrawMs = 3000;
    constexpr int64_t kStaleExitMs     = 10000;

    // 渲染主循环
    while (true) {
        // 每 2 秒检查游戏进程存活
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastAliveCheck).count() > 2000) {
            lastAliveCheck = now;
            if (!isGameAlive()) {
                LOG(LOG_LEVEL_INFO, "RenderClient: 游戏进程已退出");
                for (int i = 0; i < 5; i++) {
                    imgui->BeginFrame(); imgui->EndFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                break;
            }
        }

        // 检查 DfmMatchMonitor 是否还在推送数据 (游戏假死/对局监控停止时停止绘制)
        int64_t staleMs = dfm::SharedDfmData::getInstance().getMsSinceLastPush();
        if (staleMs >= 0 && staleMs > kStaleExitMs) {
            LOG(LOG_LEVEL_INFO, "RenderClient: 数据已陈旧 %lldms, 清屏并退出", (long long)staleMs);
            for (int i = 0; i < 5; i++) {
                imgui->BeginFrame(); imgui->EndFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            break;
        }

        // 非阻塞地处理所有待处理输入事件 (同线程, 避免竞态)
        while (imgui->PollInputReady(0)) {
            imgui->ProcessInputEvent();
        }

        imgui->BeginFrame();

        // 数据较新 → 正常绘制 overlay; 数据陈旧 → 送空帧让服务端清屏
        if (staleMs < 0 || staleMs <= kStaleSkipDrawMs) {
            // 从 SharedDfmData 读取最新数据
            dfm::SharedDfmData::getInstance().getData(gameData);

            // 绘制 DFM overlay
            overlay.drawOverlay(gameData);
        }

        imgui->EndFrame();
        // 不额外 sleep, 由 TCP 传输和 GPU 自然限速实现最低延迟
    }
}

// =====================================================================
//  工作线程
// =====================================================================
static void DfmWorkerThread(DfmInjectParams* params) {
    // 取出参数并释放堆内存
    DfmInjectParams p = *params;
    delete params;

    uintptr_t base = p.base;
    uint64_t moduleSize = p.moduleSize;

    LOG(LOG_LEVEL_INFO, "[DfmWorker] 工作线程启动");
    LOG(LOG_LEVEL_INFO, "[DfmWorker] base=%p moduleSize=0x%llX offNP=0x%X offNum=0x%X offChunks=0x%X offGW=0x%X",
        (void*)base, (unsigned long long)moduleSize,
        p.offNamePool, p.offGUObjectArrayNum, p.offGUObjectArrayChunks, p.offGWorld);

    // 等待游戏引擎完成初始化
    LOG(LOG_LEVEL_INFO, "[DfmWorker] 等待游戏引擎就绪...");
    {
        constexpr int kMaxWaitSeconds = 120;
        int memFd = open("/proc/self/mem", O_RDONLY);
        bool ready = false;

        for (int i = 0; i < kMaxWaitSeconds * 2; i++) {
            // 验证 NamePool 第一个块指针
            uintptr_t poolBlockAddr = base + p.offNamePool + offsetof(ue5dfm::FNameEntryAllocator, Blocks);
            uintptr_t block0 = 0;
            bool namePoolOk = false;
            if (memFd >= 0 && poolBlockAddr >= 0x10000) {
                if (pread(memFd, &block0, sizeof(block0), static_cast<off_t>(poolBlockAddr)) == sizeof(block0)) {
                    namePoolOk = (block0 > 0x10000);
                }
            }

            // 验证 GUObjectArray numElements
            uint32_t numElements = 0;
            bool objArrayOk = false;
            uintptr_t numAddr = base + p.offGUObjectArrayNum;
            if (memFd >= 0 && numAddr >= 0x10000) {
                if (pread(memFd, &numElements, sizeof(numElements), static_cast<off_t>(numAddr)) == sizeof(numElements)) {
                    objArrayOk = (numElements > 100);
                }
            }

            // 验证 GWorld
            uintptr_t gworldAddr = base + p.offGWorld;
            uintptr_t worldPtr = 0;
            bool worldOk = false;
            if (memFd >= 0 && gworldAddr >= 0x10000) {
                if (pread(memFd, &worldPtr, sizeof(worldPtr), static_cast<off_t>(gworldAddr)) == sizeof(worldPtr)) {
                    worldOk = (worldPtr >= 0x10000);
                }
            }

            if (namePoolOk && objArrayOk && worldOk) {
                LOG(LOG_LEVEL_INFO, "[DfmWorker] 引擎就绪! block0=%p numElements=%u worldPtr=%p (等待了 %.1fs)",
                    (void*)block0, numElements, (void*)worldPtr, i * 0.5f);
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (memFd >= 0) close(memFd);
        if (!ready) {
            LOG(LOG_LEVEL_ERROR, "[DfmWorker] 引擎等待超时 (%ds), 放弃启动", kMaxWaitSeconds);
            return;
        }
    }

    // 额外等待 5 秒让引擎完全稳定
    std::this_thread::sleep_for(std::chrono::seconds(5));

    const bool wantDumper = readDumperEnabled();
    const bool wantHeader = readHeaderEnabled();
    LOG(LOG_LEVEL_INFO, "[DfmWorker] 配置: ue_dumper=%d ue_header=%d", wantDumper, wantHeader);

    // 若启用 dumper 或 header, 构造一个共享的 UE5DfmDumper (避免重复 init)
    std::unique_ptr<ue5dfm::UE5DfmDumper> sharedDumper;
    if (wantDumper || wantHeader) {
        sharedDumper = std::make_unique<ue5dfm::UE5DfmDumper>(
            base,
            static_cast<uintptr_t>(moduleSize),
            p.offNamePool,
            p.offGUObjectArrayNum,
            p.offGUObjectArrayChunks,
            p.offGWorld,
            "/data/data/com.tencent.tmgp.dfm/cache/ue5_dump/"
        );
        if (!sharedDumper->init()) {
            LOG(LOG_LEVEL_ERROR, "[DfmWorker] UE5DfmDumper 初始化失败, 跳过 dump/header");
            toast_util::showToast("DFM Dumper 初始化失败");
            sharedDumper.reset();
        } else {
            LOG(LOG_LEVEL_INFO, "[DfmWorker] UE5DfmDumper 初始化成功");
        }
    }

    // ---- UE5 IDA 头文件 / script.json 生成 (优先执行, SDK dump 慢) ----
    if (wantHeader) {
        if (!sharedDumper) {
            LOG(LOG_LEVEL_ERROR, "[DfmWorker] ue_header 已启用但 Dumper 不可用, 跳过");
        } else {
            LOG(LOG_LEVEL_INFO, "[DfmWorker] ue_header 已启用, 开始生成 IDA 头文件/script.json");
            ue5dfm::UE5Header header(*sharedDumper, "/data/data/com.tencent.tmgp.dfm/cache/ue5_dump/");
            header.start();
            toast_util::showToast("UE5 Header 生成完成");
            LOG(LOG_LEVEL_INFO, "[DfmWorker] ue_header 生成完成");
        }
    } else {
        LOG(LOG_LEVEL_INFO, "[DfmWorker] ue_header 未启用, 跳过头文件生成");
    }

    // ---- SDK Dump ----
    if (wantDumper) {
        if (!sharedDumper) {
            LOG(LOG_LEVEL_ERROR, "[DfmWorker] ue_dumper 已启用但 Dumper 不可用, 跳过");
        } else {
            LOG(LOG_LEVEL_INFO, "[DfmWorker] ue_dumper 已启用, 开始 dump");
            if (sharedDumper->dumpAll()) {
                LOG(LOG_LEVEL_INFO, "[DfmWorker] dump 全部完成");
                toast_util::showToast("DFM SDK Dump 完成");
            } else {
                LOG(LOG_LEVEL_ERROR, "[DfmWorker] dump 部分失败");
                toast_util::showToast("DFM SDK Dump 部分失败");
            }
        }
    } else {
        LOG(LOG_LEVEL_INFO, "[DfmWorker] ue_dumper 未启用, 跳过 dump");
    }

    // 释放共享 dumper (后续对局监控用自己的)
    sharedDumper.reset();

    // ---- 对局监控 (物资/玩家/相机 采集 → SharedDfmData → GUI) ----
    LOG(LOG_LEVEL_INFO, "[DfmWorker] 启动对局监控...");
    auto* monitor = new dfm::DfmMatchMonitor(
        base,
        static_cast<uintptr_t>(moduleSize),
        p.offNamePool,
        p.offGUObjectArrayNum,
        p.offGUObjectArrayChunks,
        p.offGWorld
    );
    if (!monitor->start()) {
        LOG(LOG_LEVEL_ERROR, "[DfmWorker] DfmMatchMonitor 启动失败");
        toast_util::showToast("DFM 对局监控启动失败");
        delete monitor;
    } else {
        LOG(LOG_LEVEL_INFO, "[DfmWorker] DfmMatchMonitor 已启动");
        toast_util::showToast("DFM 对局监控已启动");
    }

    LOG(LOG_LEVEL_INFO, "[DfmWorker] 工作线程退出");
}

// =====================================================================
//  入口函数
// =====================================================================
static std::atomic<bool> g_dfmStarted{false};

extern "C" __attribute__((visibility("default")))
bool MyStartPointDFM(void* plibUE5ModeBase, void* pGNames,
                     void* pGWorld, void* pGUObjectArray, 
                     void *pGUObjectArrayChunks,uint64_t moduleSize, void* pData) {
    // 防重入: 注入器可能多次调用, 只启动一次工作线程
    bool expected = false;
    if (!g_dfmStarted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        LOG(LOG_LEVEL_INFO, "[MyStartPointDFM] 工作线程已存在, 跳过重复启动");
        return true;
    }

    if (!plibUE5ModeBase || !pGUObjectArray) {
        LOG(LOG_LEVEL_ERROR, "[MyStartPointDFM] 参数为空: base=%p GUObjectArray=%p",
            plibUE5ModeBase, pGUObjectArray);
        return false;
    }

    LOG(LOG_LEVEL_INFO, "[MyStartPointDFM] 启动 DFM 工作线程: base=%p GNames=%p GWorld=%p GUObjectArray=%p moduleSize=0x%llX",
        plibUE5ModeBase, pGNames, pGWorld, pGUObjectArray, (unsigned long long)moduleSize);

    // 注入器直接传入偏移值 (不是绝对地址, 无需减基址)
    uintptr_t base = reinterpret_cast<uintptr_t>(plibUE5ModeBase);
    auto* params = new DfmInjectParams{
        .base = base,
        .moduleSize = moduleSize,
        .offNamePool            = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pGNames)),
        .offGUObjectArrayNum    = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pGUObjectArray)),
        .offGUObjectArrayChunks = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pGUObjectArrayChunks)),
        .offGWorld              = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pGWorld)),
    };

    LOG(LOG_LEVEL_INFO, "[MyStartPointDFM] 偏移: NP=0x%X Num=0x%X Chunks=0x%X GW=0x%X",
        params->offNamePool, params->offGUObjectArrayNum,
        params->offGUObjectArrayChunks, params->offGWorld);

    // 启动 GUI 线程 (AImGui RenderClient, 连接公开 Overlay 服务)
    bool guiExpected = false;
    if (g_dfmGuiThreadStarted.compare_exchange_strong(guiExpected, true, std::memory_order_acq_rel)) {
        LOG(LOG_LEVEL_INFO, "[MyStartPointDFM] 启动 DFM GUI 线程");
        std::thread(DfmGuiThread).detach();
    } else {
        LOG(LOG_LEVEL_INFO, "[MyStartPointDFM] DFM GUI 线程已存在, 跳过");
    }

    std::thread(DfmWorkerThread, params).detach();

    return true;
}
