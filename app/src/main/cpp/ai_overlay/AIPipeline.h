// AIPipeline.h - 后台线程: 截屏 -> 推理 -> 推送结果
#pragma once
#include <string>
#include <thread>
#include <atomic>
#include <memory>

namespace ai_overlay {

class NanoDetInfer;

class AIPipeline {
public:
    static AIPipeline& getInstance();

    // 设置模型路径(.param/.bin) — 应用 onCreate / 第一次启用前调用
    void setModelPaths(const std::string& paramPath,
                       const std::string& binPath);

    // 启动 / 停止 后台线程; 非线程安全多次调用安全(带锁)
    void start();
    void stop();

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }

    // 配置
    void setUseGpu(bool v)       { m_useGpu = v; }
    void setUseSu(bool v)        { m_useSu = v; }
    void setIntervalMs(int v)    { m_intervalMs.store(v, std::memory_order_release); }
    int  intervalMs() const      { return m_intervalMs.load(std::memory_order_acquire); }

private:
    AIPipeline() = default;
    ~AIPipeline();
    void threadMain();

    std::unique_ptr<NanoDetInfer> m_infer;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopFlag{false};
    std::atomic<int>  m_intervalMs{120};   // ~8 fps 默认
    std::string m_paramPath;
    std::string m_binPath;
    bool m_useGpu = true;
    bool m_useSu  = false;
};

} // namespace ai_overlay
