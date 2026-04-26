// AIPipeline.cpp
#include "AIPipeline.h"
#include "AIDetection.h"
#include "ScreenCapture.h"
#include "NanoDetInfer.h"
#include "../core/log/log.h"

#include <chrono>
#include <thread>

namespace ai_overlay {

void __ai_log_enabled_changed(bool v) {
    LOG(LOG_LEVEL_INFO, "[AI/Shared] enabled -> %d", v ? 1 : 0);
}

AIPipeline& AIPipeline::getInstance() {
    static AIPipeline inst;
    return inst;
}

AIPipeline::~AIPipeline() { stop(); }

void AIPipeline::setModelPaths(const std::string& paramPath, const std::string& binPath) {
    m_paramPath = paramPath;
    m_binPath = binPath;
}

void AIPipeline::start() {
    if (m_running.load(std::memory_order_acquire)) return;
    m_stopFlag.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_thread = std::thread(&AIPipeline::threadMain, this);
    LOG(LOG_LEVEL_INFO, "[AI/Pipe] started");
}

void AIPipeline::stop() {
    if (!m_running.load(std::memory_order_acquire)) return;
    m_stopFlag.store(true, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
    m_running.store(false, std::memory_order_release);
    AISharedData::getInstance().setReady(false);
    LOG(LOG_LEVEL_INFO, "[AI/Pipe] stopped");
}

void AIPipeline::threadMain() {
    auto& shared = AISharedData::getInstance();
    LOG(LOG_LEVEL_INFO, "[AI/Pipe] threadMain shared.valid()=%d", shared.valid() ? 1 : 0);

    // 加载模型
    if (m_paramPath.empty() || m_binPath.empty()) {
        shared.setLastError("model paths not set");
        LOG(LOG_LEVEL_ERROR, "[AI/Pipe] model paths empty");
        return;
    }
    m_infer = std::make_unique<NanoDetInfer>();
    if (!m_infer->load(m_paramPath, m_binPath, m_useGpu)) {
        shared.setLastError("model load failed");
        return;
    }
    shared.setReady(true);
    shared.setLastError("");

    using Clock = std::chrono::steady_clock;
    auto nextDeadline = Clock::now();
    while (!m_stopFlag.load(std::memory_order_acquire)) {
        if (!shared.enabled()) {
            static auto s_lastHb = Clock::now();
            auto nowHb = Clock::now();
            if (nowHb - s_lastHb > std::chrono::seconds(5)) {
                LOG(LOG_LEVEL_INFO, "[AI/Pipe] heartbeat: enabled=0 (waiting for UI checkbox)");
                s_lastHb = nowHb;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        auto now = Clock::now();
        if (now < nextDeadline) {
            std::this_thread::sleep_for(std::min(
                std::chrono::milliseconds(20),
                std::chrono::duration_cast<std::chrono::milliseconds>(nextDeadline - now)));
            continue;
        }
        int interval = m_intervalMs.load(std::memory_order_acquire);
        nextDeadline = now + std::chrono::milliseconds(interval);

        // 1. 截屏
        CaptureFrame frame;
        auto t0 = Clock::now();
        if (!captureScreen(frame, m_useSu)) {
            shared.setLastError("capture failed");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // 2. 推理
        std::vector<DetectionBox> dets;
        float thr = shared.scoreThreshold();
        int classFilter = shared.targetClassFilter();
        auto t1 = Clock::now();
        if (!m_infer->detect(frame.pixels.data(), frame.width, frame.height,
                             dets, thr, classFilter)) {
            shared.setLastError("infer failed");
            continue;
        }
        auto t2 = Clock::now();
        int64_t inferUs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        int64_t capUs   = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

        // 周期统计 (每 30 帧打印一次, 避免日志洪水)
        static int s_frameCnt = 0;
        if ((++s_frameCnt % 30) == 0) {
            LOG(LOG_LEVEL_INFO, "[AI/Pipe] frame#%d size=%dx%d cap=%.1fms infer=%.1fms dets=%zu",
                s_frameCnt, frame.width, frame.height,
                capUs / 1000.0, inferUs / 1000.0, dets.size());
            // 打印前 5 个检测的 classId/score/box
            char buf[512]; int off = 0;
            for (size_t i = 0; i < dets.size() && i < 5 && off < 480; ++i) {
                off += snprintf(buf + off, sizeof(buf) - off,
                    " [%zu cls=%d s=%.2f xy=%.0f,%.0f wh=%.0fx%.0f]",
                    i, dets[i].classId, dets[i].score,
                    dets[i].x, dets[i].y, dets[i].w, dets[i].h);
            }
            LOG(LOG_LEVEL_INFO, "[AI/Pipe] dets:%s", buf);
        }

        shared.pushDetections(std::move(dets), frame.width, frame.height, inferUs);
    }

    m_infer.reset();
}

} // namespace ai_overlay
