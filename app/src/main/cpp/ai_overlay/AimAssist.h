// AimAssist.h - 基于 AI 屏幕检测结果的辅助瞄准
//   - 选择最近 / 最优目标 (头部位置 = box_top + h * headRatio)
//   - 视觉模式: 仅返回锁定坐标用于 ImGui 绘制
//   - 触屏注入模式: 通过 /dev/uinput 创建虚拟触屏, 在右半屏注入小幅拖动
// 完全不使用任何 Hook / 不读取目标进程内存.
#pragma once
#include "AIDetection.h"
#include <atomic>
#include <thread>
#include <mutex>

namespace ai_overlay {

struct AimTarget {
    bool  valid = false;
    float screenX = 0.f;     // 头部目标点 (按推理时的源帧坐标系)
    float screenY = 0.f;
    float boxX = 0.f;        // 整框
    float boxY = 0.f;
    float boxW = 0.f;
    float boxH = 0.f;
    float score = 0.f;
    int   classId = 0;
    int   srcW = 0;          // 推理源帧大小 (用来转 ImGui 坐标)
    int   srcH = 0;
};

class AimAssist {
public:
    static AimAssist& getInstance();

    // ===== 配置 (UI 线程调用) =====
    void setEnabled(bool v);
    bool enabled() const { return m_enabled.load(std::memory_order_acquire); }

    // 仅视觉指示, 不注入触屏
    void setVisualOnly(bool v) { m_visualOnly.store(v, std::memory_order_release); }
    bool visualOnly() const    { return m_visualOnly.load(std::memory_order_acquire); }

    void setFovRadius(float v) { m_fovRadius.store(v, std::memory_order_release); }
    float fovRadius() const    { return m_fovRadius.load(std::memory_order_acquire); }

    void setHeadRatio(float v) { m_headRatio.store(v, std::memory_order_release); }
    float headRatio() const    { return m_headRatio.load(std::memory_order_acquire); }

    void setMinScore(float v)  { m_minScore.store(v, std::memory_order_release); }
    float minScore() const     { return m_minScore.load(std::memory_order_acquire); }

    void setSensitivity(float sx, float sy) {
        m_sensX.store(sx, std::memory_order_release);
        m_sensY.store(sy, std::memory_order_release);
    }
    void setSmoothing(float v) { m_smooth.store(v, std::memory_order_release); }

    // 触发(若 requireTrigger=true 则只在 trigger=true 期间注入触屏拖动)
    void setRequireTrigger(bool v) { m_requireTrigger.store(v, std::memory_order_release); }
    void setTrigger(bool v)        { m_triggerHeld.store(v, std::memory_order_release); }
    bool trigger() const           { return m_triggerHeld.load(std::memory_order_acquire); }
    bool requireTrigger() const    { return m_requireTrigger.load(std::memory_order_acquire); }

    void setOnlyPerson(bool v) { m_onlyPerson.store(v, std::memory_order_release); }

    // ===== 目标选择 =====
    // 在 AI 检测结果中找最优目标(距 (cx,cy) 最近且在 FOV 内, 满足 score/class 过滤).
    // cx,cy 与 srcW,srcH 都是 AI 推理源帧坐标系(屏幕像素).
    // 返回 false 表示无目标.
    bool pickTarget(float cx, float cy, AimTarget& out) const;

    // 触屏注入设备状态 (UI 显示)
    bool injectorReady() const { return m_injectorReady.load(std::memory_order_acquire); }
    std::string injectorError() const;

private:
    AimAssist() = default;
    ~AimAssist();
    AimAssist(const AimAssist&) = delete;
    AimAssist& operator=(const AimAssist&) = delete;

    // 触屏注入线程
    void injectorThreadMain();
    bool openUinputDevice(int screenW, int screenH);
    void closeUinputDevice();
    void writeAbsMove(int x, int y);
    void writeTouchDown(int x, int y);
    void writeTouchUp();

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_visualOnly{true};
    std::atomic<float> m_fovRadius{250.0f};
    std::atomic<float> m_headRatio{0.18f};
    std::atomic<float> m_minScore{0.30f};
    std::atomic<float> m_sensX{1.0f};
    std::atomic<float> m_sensY{1.0f};
    std::atomic<float> m_smooth{0.55f};
    std::atomic<bool>  m_onlyPerson{true};
    std::atomic<bool>  m_requireTrigger{true};
    std::atomic<bool>  m_triggerHeld{false};

    // 注入器线程
    std::thread m_injThread;
    std::atomic<bool> m_injStop{false};
    std::atomic<bool> m_injectorReady{false};
    int m_uinputFd = -1;
    int m_devW = 0;   // 虚拟设备坐标空间(=屏幕像素)
    int m_devH = 0;
    bool m_touchActive = false;
    int m_touchAnchorX = 0;     // 当前虚拟手指位置 (右半屏中心起始)
    int m_touchAnchorY = 0;
    mutable std::mutex m_errMu;
    std::string m_lastError;
};

} // namespace ai_overlay
