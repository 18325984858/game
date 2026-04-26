// AimAssist.cpp - 视觉锁定 + uinput 虚拟触屏注入 (旁路, 无 Hook)
#include "AimAssist.h"
#include "../core/log/log.h"

#include <linux/input.h>
#include <linux/uinput.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <thread>

#ifndef ABS_MT_SLOT
#define ABS_MT_SLOT 0x2f
#endif

namespace ai_overlay {

AimAssist& AimAssist::getInstance() {
    static AimAssist inst;
    return inst;
}

AimAssist::~AimAssist() {
    setEnabled(false);
}

std::string AimAssist::injectorError() const {
    std::lock_guard<std::mutex> lk(m_errMu);
    return m_lastError;
}

static void setError(std::mutex& m, std::string& slot, const std::string& s) {
    std::lock_guard<std::mutex> lk(m);
    slot = s;
}

void AimAssist::setEnabled(bool v) {
    bool prev = m_enabled.exchange(v, std::memory_order_acq_rel);
    if (prev == v) return;
    if (v) {
        // 启动注入器线程 (即使 visualOnly=true 也启动? 仅在需要触屏时启动更安全)
        if (!m_visualOnly.load(std::memory_order_acquire)) {
            m_injStop.store(false, std::memory_order_release);
            m_injThread = std::thread(&AimAssist::injectorThreadMain, this);
            LOG(LOG_LEVEL_INFO, "[AI/Aim] inject thread started");
        }
    } else {
        m_injStop.store(true, std::memory_order_release);
        if (m_injThread.joinable()) m_injThread.join();
        m_injectorReady.store(false, std::memory_order_release);
        LOG(LOG_LEVEL_INFO, "[AI/Aim] disabled");
    }
}

bool AimAssist::pickTarget(float cx, float cy, AimTarget& out) const {
    auto& shared = AISharedData::getInstance();
    std::vector<DetectionBox> dets;
    int srcW = 0, srcH = 0;
    shared.getDetections(dets, srcW, srcH);
    if (dets.empty() || srcW <= 0 || srcH <= 0) return false;

    const float minScore   = m_minScore.load(std::memory_order_acquire);
    const float fov        = m_fovRadius.load(std::memory_order_acquire);
    const float headRatio  = m_headRatio.load(std::memory_order_acquire);
    const bool  onlyPerson = m_onlyPerson.load(std::memory_order_acquire);
    const float fovSqr     = fov * fov;

    float bestDistSqr = fovSqr + 1.f;
    int bestIdx = -1;

    for (size_t i = 0; i < dets.size(); ++i) {
        const auto& b = dets[i];
        if (b.score < minScore) continue;
        if (onlyPerson && b.classId != 0) continue;
        float hx = b.x + b.w * 0.5f;
        float hy = b.y + b.h * headRatio;
        float dx = hx - cx;
        float dy = hy - cy;
        float d2 = dx * dx + dy * dy;
        if (d2 > fovSqr) continue;
        if (d2 < bestDistSqr) {
            bestDistSqr = d2;
            bestIdx = static_cast<int>(i);
        }
    }
    if (bestIdx < 0) return false;
    const auto& b = dets[bestIdx];
    out.valid = true;
    out.screenX = b.x + b.w * 0.5f;
    out.screenY = b.y + b.h * headRatio;
    out.boxX = b.x; out.boxY = b.y;
    out.boxW = b.w; out.boxH = b.h;
    out.score = b.score;
    out.classId = b.classId;
    out.srcW = srcW;
    out.srcH = srcH;
    return true;
}

// =====================================================================
//  uinput 虚拟触屏注入
//  - 创建一个 ABS_MT 多点触屏设备, 屏幕坐标空间 = 物理屏幕
//  - 在右半屏中心 "按下" 一个虚拟手指, 然后通过 ABS_MT_POSITION_X/Y 微移
//    游戏的 InputManager 会把这视为玩家在拖动右摇杆 → 转为相机旋转
//  - 释放时上送 MT_TRACKING_ID = -1 + SYN_REPORT
//  注意: 写入 /dev/uinput 通常需要 CAP_SYS_ADMIN 或 root.
//        本工程已通过 KernelPatch + magisk su 提权, 启动前可在 Java 层
//        chmod 666 /dev/uinput. 这里仅做错误兜底.
// =====================================================================

static int safeIoctl(int fd, unsigned long req, unsigned long arg) {
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

bool AimAssist::openUinputDevice(int screenW, int screenH) {
    if (screenW <= 0 || screenH <= 0) return false;
    int fd = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "open /dev/uinput failed: %s", strerror(errno));
        setError(m_errMu, m_lastError, buf);
        LOG(LOG_LEVEL_ERROR, "[AI/Aim] %s", buf);
        return false;
    }

    // 启用事件类型
    if (safeIoctl(fd, UI_SET_EVBIT, EV_SYN) < 0 ||
        safeIoctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        safeIoctl(fd, UI_SET_EVBIT, EV_ABS) < 0) {
        setError(m_errMu, m_lastError, "UI_SET_EVBIT failed");
        ::close(fd); return false;
    }
    safeIoctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
    safeIoctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    safeIoctl(fd, UI_SET_ABSBIT, ABS_MT_SLOT);
    safeIoctl(fd, UI_SET_ABSBIT, ABS_MT_TRACKING_ID);
    safeIoctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_X);
    safeIoctl(fd, UI_SET_ABSBIT, ABS_MT_POSITION_Y);
    safeIoctl(fd, UI_SET_ABSBIT, ABS_MT_TOUCH_MAJOR);
    safeIoctl(fd, UI_SET_ABSBIT, ABS_MT_PRESSURE);

    struct uinput_user_dev uidev{};
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "ai-aim-vts");
    uidev.id.bustype = BUS_VIRTUAL;
    uidev.id.vendor  = 0xC0DE;
    uidev.id.product = 0xA1A1;
    uidev.id.version = 1;
    uidev.absmin[ABS_MT_SLOT] = 0;
    uidev.absmax[ABS_MT_SLOT] = 9;
    uidev.absmin[ABS_MT_TRACKING_ID] = 0;
    uidev.absmax[ABS_MT_TRACKING_ID] = 65535;
    uidev.absmin[ABS_MT_POSITION_X] = 0;
    uidev.absmax[ABS_MT_POSITION_X] = screenW - 1;
    uidev.absmin[ABS_MT_POSITION_Y] = 0;
    uidev.absmax[ABS_MT_POSITION_Y] = screenH - 1;
    uidev.absmin[ABS_MT_TOUCH_MAJOR] = 0;
    uidev.absmax[ABS_MT_TOUCH_MAJOR] = 32;
    uidev.absmin[ABS_MT_PRESSURE] = 0;
    uidev.absmax[ABS_MT_PRESSURE] = 255;

    if (::write(fd, &uidev, sizeof(uidev)) != static_cast<ssize_t>(sizeof(uidev))) {
        setError(m_errMu, m_lastError, "uinput write dev failed");
        ::close(fd); return false;
    }
    if (safeIoctl(fd, UI_DEV_CREATE, 0) < 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "UI_DEV_CREATE failed: %s", strerror(errno));
        setError(m_errMu, m_lastError, buf);
        ::close(fd); return false;
    }
    m_uinputFd = fd;
    m_devW = screenW;
    m_devH = screenH;
    setError(m_errMu, m_lastError, "");
    LOG(LOG_LEVEL_INFO, "[AI/Aim] uinput device created %dx%d", screenW, screenH);
    return true;
}

void AimAssist::closeUinputDevice() {
    if (m_uinputFd >= 0) {
        if (m_touchActive) writeTouchUp();
        safeIoctl(m_uinputFd, UI_DEV_DESTROY, 0);
        ::close(m_uinputFd);
        m_uinputFd = -1;
    }
    m_touchActive = false;
}

static void emit(int fd, uint16_t type, uint16_t code, int32_t value) {
    if (fd < 0) return;
    struct input_event ev{};
    ev.type = type;
    ev.code = code;
    ev.value = value;
    (void)::write(fd, &ev, sizeof(ev));
}

void AimAssist::writeTouchDown(int x, int y) {
    if (m_uinputFd < 0) return;
    static int trackingId = 0x10000;
    int slot = 9;
    int tid = ++trackingId;
    emit(m_uinputFd, EV_ABS, ABS_MT_SLOT, slot);
    emit(m_uinputFd, EV_ABS, ABS_MT_TRACKING_ID, tid);
    emit(m_uinputFd, EV_KEY, BTN_TOUCH, 1);
    emit(m_uinputFd, EV_ABS, ABS_MT_POSITION_X, x);
    emit(m_uinputFd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit(m_uinputFd, EV_ABS, ABS_MT_TOUCH_MAJOR, 8);
    emit(m_uinputFd, EV_ABS, ABS_MT_PRESSURE, 100);
    emit(m_uinputFd, EV_SYN, SYN_REPORT, 0);
    m_touchActive = true;
    m_touchAnchorX = x;
    m_touchAnchorY = y;
}

void AimAssist::writeAbsMove(int x, int y) {
    if (m_uinputFd < 0 || !m_touchActive) return;
    emit(m_uinputFd, EV_ABS, ABS_MT_SLOT, 9);
    emit(m_uinputFd, EV_ABS, ABS_MT_POSITION_X, x);
    emit(m_uinputFd, EV_ABS, ABS_MT_POSITION_Y, y);
    emit(m_uinputFd, EV_SYN, SYN_REPORT, 0);
    m_touchAnchorX = x;
    m_touchAnchorY = y;
}

void AimAssist::writeTouchUp() {
    if (m_uinputFd < 0 || !m_touchActive) return;
    emit(m_uinputFd, EV_ABS, ABS_MT_SLOT, 9);
    emit(m_uinputFd, EV_ABS, ABS_MT_TRACKING_ID, -1);
    emit(m_uinputFd, EV_KEY, BTN_TOUCH, 0);
    emit(m_uinputFd, EV_SYN, SYN_REPORT, 0);
    m_touchActive = false;
}

void AimAssist::injectorThreadMain() {
    using Clock = std::chrono::steady_clock;
    // 初始化时屏幕尺寸未知, 取 AI 当前帧 srcW/srcH 作为虚拟设备坐标空间
    auto& shared = AISharedData::getInstance();
    int wantW = 0, wantH = 0;
    while (!m_injStop.load(std::memory_order_acquire)) {
        std::vector<DetectionBox> tmp; int sw=0, sh=0;
        shared.getDetections(tmp, sw, sh);
        if (sw > 0 && sh > 0) { wantW = sw; wantH = sh; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (m_injStop.load(std::memory_order_acquire)) return;
    if (!openUinputDevice(wantW, wantH)) return;
    m_injectorReady.store(true, std::memory_order_release);

    int curX = wantW * 3 / 4;   // 右半屏中心
    int curY = wantH / 2;
    auto lastTick = Clock::now();
    constexpr int kTickMs = 12;     // ~80 Hz 注入

    while (!m_injStop.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick).count();
        if (dt < kTickMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs - dt));
            continue;
        }
        lastTick = now;

        const bool needTrig = m_requireTrigger.load(std::memory_order_acquire);
        const bool trigger  = m_triggerHeld.load(std::memory_order_acquire);
        if (needTrig && !trigger) {
            if (m_touchActive) writeTouchUp();
            continue;
        }

        AimTarget tgt;
        if (!pickTarget(wantW * 0.5f, wantH * 0.5f, tgt)) {
            if (m_touchActive && (now - lastTick) > std::chrono::milliseconds(120)) {
                writeTouchUp();
            }
            continue;
        }
        // 计算屏幕中心到目标的像素 delta
        const float sx = m_sensX.load(std::memory_order_acquire);
        const float sy = m_sensY.load(std::memory_order_acquire);
        const float smooth = m_smooth.load(std::memory_order_acquire);
        float dx = (tgt.screenX - wantW * 0.5f) * sx;
        float dy = (tgt.screenY - wantH * 0.5f) * sy;
        // 平滑: 每 tick 只移动 (1-smooth) 比例
        float step = std::max(0.05f, 1.0f - smooth);
        int targetX = curX + static_cast<int>(dx * step);
        int targetY = curY + static_cast<int>(dy * step);
        // 限幅在虚拟设备右半屏内
        targetX = std::max(wantW / 2 + 4, std::min(wantW - 4, targetX));
        targetY = std::max(4, std::min(wantH - 4, targetY));

        if (!m_touchActive) writeTouchDown(targetX, targetY);
        else                writeAbsMove(targetX, targetY);
        curX = targetX; curY = targetY;
    }
    closeUinputDevice();
}

} // namespace ai_overlay
