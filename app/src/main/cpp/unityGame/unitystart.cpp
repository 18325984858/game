//
// Created by Song on 2025/11/10.
//

#include "unitystart.h"
#include "../Log/log.h"
#include "./lol/lolm.h"
#include "./il2cppHeader/il2cppHeader.h"
#include "./li2cppDumper/li2cppdumper.h"
#include "Draw/Draw.h"
#include <chrono>
#include <thread>
#include <csignal>
#include <csetjmp>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <cerrno>
#include <limits>
#include <dlfcn.h>
#include <jni.h>

// 线程安全游戏数据桥接
#include "SharedGameData.h"

// Dobby — inline hook (仅用于数据采集 hook, 不再用于绘制)
#include "../Dobby/include/dobby.h"

// ImGui (AImGui RenderClient 使用)
#include <imgui/imgui.h>

// 触摸输入 (读取 /dev/input/event* — 保留用于数据采集中的按键模拟)
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <GLES3/gl3.h>

// ═══════════════════════════════════════════════════════════════════════════════
// SIGSEGV/SIGBUS 信号安全网
// ═══════════════════════════════════════════════════════════════════════════════

static thread_local sigjmp_buf t_jumpBuf;
static thread_local volatile sig_atomic_t t_guardActive = 0;

static struct sigaction s_oldSigsegv;
static struct sigaction s_oldSigbus;

static void crashSignalHandler(int sig, siginfo_t* info, void* ctx) {
    if (t_guardActive) {
        t_guardActive = 0;
        siglongjmp(t_jumpBuf, sig);
    }
    struct sigaction* old = (sig == SIGSEGV) ? &s_oldSigsegv : &s_oldSigbus;
    if (old->sa_flags & SA_SIGINFO) {
        old->sa_sigaction(sig, info, ctx);
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN) {
        old->sa_handler(sig);
    } else {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

static std::once_flag s_crashGuardOnce;
static void installCrashGuard() {
    std::call_once(s_crashGuardOnce, []() {
        struct sigaction sa{};
        sa.sa_sigaction = crashSignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &s_oldSigsegv);
        sigaction(SIGBUS,  &sa, &s_oldSigbus);
        LOG(LOG_LEVEL_INFO, "[CrashGuard] 信号安全网已安装 (SIGSEGV + SIGBUS)");
    });
}



// ═══════════════════════════════════════════════════════════════════════════════
// 触摸输入 — 读取 /dev/input/event* 原始触摸事件, 喂给 ImGui IO
//
// 游戏进程无权直接读 /dev/input/event* (SELinux + DAC)。
// 需要用户通过 ADB 手动提权:
//   adb shell su -c "chmod 666 /dev/input/event*"
// init() 会每隔几秒自动重试, chmod 生效后即可使用。
// ═══════════════════════════════════════════════════════════════════════════════

namespace touch_input {

    using JniGetCreatedJavaVMsFn = jint (*)(JavaVM**, jsize, jsize*);

    static int   s_fd       = -2;     // -2=未初始化/重试中, -1=永久失败, >=0=有效
    static char  s_devPath[64] = {0};
    static char  s_devName[128] = {0};
    static float s_absMaxX  = 1.0f;   // 数字化仪 X 轴最大值 (物理竖屏坐标系)
    static float s_absMaxY  = 1.0f;   // 数字化仪 Y 轴最大值
    static float s_rawX     = -1.0f;  // 归一化原始触摸坐标 (0..1, 数字化仪坐标系)
    static float s_rawY     = -1.0f;
    static float s_curX     = -1.0f;  // 转换后的屏幕坐标 (已应用旋转)
    static float s_curY     = -1.0f;
    static bool  s_touching = false;
    static int   s_retryCount = 0;
    static std::chrono::steady_clock::time_point s_lastRetry{};
    static bool  s_loggedOpenPermissionError = false;
    static bool  s_lockToSurfaceOrientation = false;
    static bool  s_lastTouchingState = false;

    // ── 触摸坐标旋转模式 ──
    //
    // /dev/input/event* 原始触摸始终在物理数字化仪坐标系 (通常是竖屏)
    // 当游戏横屏运行时, 需要旋转坐标映射:
    //   -1 = 自动检测 (对比数字化仪方向 vs 屏幕方向)
    //    0 = 不旋转 (竖屏游戏 或 内核已旋转)
    //    1 = ROTATION_90  (逆时针90°, 最常见横屏)
    //    2 = ROTATION_180
    //    3 = ROTATION_270 (顺时针90°, 反向横屏)
    //
    static int s_rotation = -1;
    static int s_lastAutoRotation = 0;
    static int s_lastJavaTouchRotation = -1;
    static int s_activeTouchRotation = -1;
    static int s_lockedCorrectionRotation = -1;
    static bool s_pendingMirrorResolve = false;
    static bool s_rotationCorrectionLocked = false;
    static float s_touchStartRawX = -1.0f;
    static float s_touchStartRawY = -1.0f;

    static int displayRotationToTouchRotation(int displayRotation) {
        displayRotation &= 3;
        return displayRotation;
    }

    static int getAutoRotation(float screenW, float screenH);

    static int getEffectiveRotation(float screenW, float screenH) {
        if (s_rotationCorrectionLocked && s_lockedCorrectionRotation >= 0) {
            return s_lockedCorrectionRotation;
        }

        if (s_rotation >= 0) {
            return s_rotation;
        }

        return getAutoRotation(screenW, screenH);
    }

    static int captureCurrentRotationForLock() {
        if (s_activeTouchRotation >= 0) return s_activeTouchRotation;
        if (s_rotation >= 0) return s_rotation;
        if (s_lastAutoRotation >= 0) return s_lastAutoRotation;
        return 0;
    }

    static bool isLandscapeRotation(int rotation) {
        return rotation == 1 || rotation == 3;
    }

    static int inferRotationFromSurface(float screenW, float screenH, int fallbackRotation) {
        const bool surfaceLandscape = (screenW > screenH);
        const bool fallbackMatchesSurface = surfaceLandscape
            ? isLandscapeRotation(fallbackRotation)
            : !isLandscapeRotation(fallbackRotation);
        if (fallbackMatchesSurface) return fallbackRotation;

        const bool lastMatchesSurface = surfaceLandscape
            ? isLandscapeRotation(s_lastAutoRotation)
            : !isLandscapeRotation(s_lastAutoRotation);
        if (lastMatchesSurface) return s_lastAutoRotation;

        return surfaceLandscape ? 3 : 0;
    }

    static bool hasLandscapeMirrorAmbiguity(float screenW, float screenH) {
        if (!s_lockToSurfaceOrientation) return false;
        if (screenW <= screenH) return false;
        if (s_lastJavaTouchRotation < 0) return false;
        int surfaceRotation = inferRotationFromSurface(screenW, screenH, s_lastJavaTouchRotation);
        return isLandscapeRotation(surfaceRotation) &&
               isLandscapeRotation(s_lastJavaTouchRotation) &&
               surfaceRotation != s_lastJavaTouchRotation;
    }

    static int resolveAutoRotationForTouchStart(float screenW, float screenH) {
        int autoRotation = getAutoRotation(screenW, screenH);
        s_pendingMirrorResolve = hasLandscapeMirrorAmbiguity(screenW, screenH);
        return autoRotation;
    }

    static void tryResolveMirrorFromFirstDrag(float screenW, float screenH) {
        if (!s_pendingMirrorResolve) return;
        if (s_lastJavaTouchRotation < 0) {
            s_pendingMirrorResolve = false;
            return;
        }

        float dx = s_rawX - s_touchStartRawX;
        float dy = s_rawY - s_touchStartRawY;
        float dragDistanceSq = dx * dx + dy * dy;
        if (dragDistanceSq < 0.0025f) return;

        if (std::fabs(dx) < 0.03f && std::fabs(dy) < 0.03f) return;

        int surfaceRotation = inferRotationFromSurface(screenW, screenH, s_lastJavaTouchRotation);
        if (surfaceRotation != s_lastJavaTouchRotation) {
            LOG(LOG_LEVEL_INFO,
                "[Touch] 首次拖动触发镜像自校正: surface=%d -> java=%d drag=(%.3f,%.3f)",
                surfaceRotation, s_lastJavaTouchRotation, dx, dy);
            s_activeTouchRotation = s_lastJavaTouchRotation;
            s_lastAutoRotation = s_lastJavaTouchRotation;
        }
        s_pendingMirrorResolve = false;
    }

    static JavaVM* getJavaVM() {
        static JavaVM* s_javaVm = nullptr;
        static bool s_resolved = false;
        if (s_resolved) return s_javaVm;
        s_resolved = true;

        auto resolveFromHandle = [&](void* handle) -> JavaVM* {
            if (!handle) return nullptr;
            auto fn = reinterpret_cast<JniGetCreatedJavaVMsFn>(dlsym(handle, "JNI_GetCreatedJavaVMs"));
            if (!fn) return nullptr;

            JavaVM* vmBuf[2] = {nullptr, nullptr};
            jsize vmCount = 0;
            if (fn(vmBuf, 2, &vmCount) != JNI_OK || vmCount <= 0) return nullptr;
            return vmBuf[0];
        };

        s_javaVm = resolveFromHandle(RTLD_DEFAULT);
        if (!s_javaVm) {
            void* libArt = dlopen("libart.so", RTLD_NOW | RTLD_NOLOAD);
            s_javaVm = resolveFromHandle(libArt);
            if (libArt) dlclose(libArt);
        }
        if (!s_javaVm) {
            void* libAndroidRuntime = dlopen("libandroid_runtime.so", RTLD_NOW | RTLD_NOLOAD);
            s_javaVm = resolveFromHandle(libAndroidRuntime);
            if (libAndroidRuntime) dlclose(libAndroidRuntime);
        }

        LOG(LOG_LEVEL_INFO, "[Touch] JavaVM resolve: %p", s_javaVm);
        return s_javaVm;
    }

    static int queryDisplayRotationFromContext(JNIEnv* env, jobject context) {
        if (!env || !context) return -1;

        int rotation = -1;
        jclass contextClass = env->FindClass("android/content/Context");
        jfieldID windowServiceField = contextClass
            ? env->GetStaticFieldID(contextClass, "WINDOW_SERVICE", "Ljava/lang/String;")
            : nullptr;
        jstring windowService = windowServiceField
            ? reinterpret_cast<jstring>(env->GetStaticObjectField(contextClass, windowServiceField))
            : nullptr;

        jclass objectClass = env->GetObjectClass(context);
        jmethodID getSystemService = objectClass
            ? env->GetMethodID(objectClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;")
            : nullptr;
        jobject windowManager = (getSystemService && windowService)
            ? env->CallObjectMethod(context, getSystemService, windowService)
            : nullptr;
        if (windowManager) {
            jclass wmClass = env->GetObjectClass(windowManager);
            jmethodID getDefaultDisplay = wmClass
                ? env->GetMethodID(wmClass, "getDefaultDisplay", "()Landroid/view/Display;")
                : nullptr;
            jobject display = getDefaultDisplay
                ? env->CallObjectMethod(windowManager, getDefaultDisplay)
                : nullptr;
            if (display) {
                jclass displayClass = env->GetObjectClass(display);
                jmethodID getRotation = displayClass
                    ? env->GetMethodID(displayClass, "getRotation", "()I")
                    : nullptr;
                if (getRotation) {
                    rotation = (int)env->CallIntMethod(display, getRotation);
                }
                if (displayClass) env->DeleteLocalRef(displayClass);
                env->DeleteLocalRef(display);
            }
            if (wmClass) env->DeleteLocalRef(wmClass);
            env->DeleteLocalRef(windowManager);
        }

        if (windowService) env->DeleteLocalRef(windowService);
        if (objectClass) env->DeleteLocalRef(objectClass);
        if (contextClass) env->DeleteLocalRef(contextClass);
        return rotation;
    }

    static jobject queryCurrentActivityFromActivityThread(JNIEnv* env) {
        if (!env) return nullptr;

        jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
        if (!activityThreadClass || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return nullptr;
        }

        jmethodID currentActivityThread = env->GetStaticMethodID(
            activityThreadClass, "currentActivityThread", "()Landroid/app/ActivityThread;");
        jobject activityThread = currentActivityThread
            ? env->CallStaticObjectMethod(activityThreadClass, currentActivityThread)
            : nullptr;
        if (!activityThread || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (activityThread) env->DeleteLocalRef(activityThread);
            env->DeleteLocalRef(activityThreadClass);
            return nullptr;
        }

        jfieldID activitiesField = env->GetFieldID(
            activityThreadClass, "mActivities", "Landroid/util/ArrayMap;");
        jobject activities = activitiesField
            ? env->GetObjectField(activityThread, activitiesField)
            : nullptr;
        if (!activities || env->ExceptionCheck()) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            if (activities) env->DeleteLocalRef(activities);
            env->DeleteLocalRef(activityThread);
            env->DeleteLocalRef(activityThreadClass);
            return nullptr;
        }

        jobject activity = nullptr;
        jclass arrayMapClass = env->GetObjectClass(activities);
        jmethodID sizeMethod = arrayMapClass
            ? env->GetMethodID(arrayMapClass, "size", "()I")
            : nullptr;
        jmethodID valueAtMethod = arrayMapClass
            ? env->GetMethodID(arrayMapClass, "valueAt", "(I)Ljava/lang/Object;")
            : nullptr;
        jint size = (sizeMethod && valueAtMethod) ? env->CallIntMethod(activities, sizeMethod) : 0;
        if (!env->ExceptionCheck()) {
            for (jint index = 0; index < size && !activity; ++index) {
                jobject record = env->CallObjectMethod(activities, valueAtMethod, index);
                if (!record || env->ExceptionCheck()) {
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    if (record) env->DeleteLocalRef(record);
                    continue;
                }

                jclass recordClass = env->GetObjectClass(record);
                jfieldID pausedField = recordClass
                    ? env->GetFieldID(recordClass, "paused", "Z")
                    : nullptr;
                jfieldID activityField = recordClass
                    ? env->GetFieldID(recordClass, "activity", "Landroid/app/Activity;")
                    : nullptr;
                jboolean paused = pausedField ? env->GetBooleanField(record, pausedField) : JNI_FALSE;
                jobject candidate = activityField ? env->GetObjectField(record, activityField) : nullptr;
                if (env->ExceptionCheck()) {
                    env->ExceptionClear();
                    if (candidate) env->DeleteLocalRef(candidate);
                    if (recordClass) env->DeleteLocalRef(recordClass);
                    env->DeleteLocalRef(record);
                    continue;
                }

                if (candidate && !paused) {
                    activity = candidate;
                } else if (candidate) {
                    env->DeleteLocalRef(candidate);
                }

                if (recordClass) env->DeleteLocalRef(recordClass);
                env->DeleteLocalRef(record);
            }
        }

        if (env->ExceptionCheck()) env->ExceptionClear();
        if (arrayMapClass) env->DeleteLocalRef(arrayMapClass);
        env->DeleteLocalRef(activities);
        env->DeleteLocalRef(activityThread);
        env->DeleteLocalRef(activityThreadClass);
        return activity;
    }

    static int queryDisplayRotationFromJava() {
        JavaVM* javaVm = getJavaVM();
        if (!javaVm) return -1;

        JNIEnv* env = nullptr;
        bool attachedHere = false;
        jint envStat = javaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (envStat == JNI_EDETACHED) {
            if (javaVm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) {
                return -1;
            }
            attachedHere = true;
        } else if (envStat != JNI_OK || !env) {
            return -1;
        }

        int rotation = -1;
        jclass unityPlayerClass = env->FindClass("com/unity3d/player/UnityPlayer");
        if (unityPlayerClass && !env->ExceptionCheck()) {
            jfieldID currentActivityField = env->GetStaticFieldID(
                unityPlayerClass, "currentActivity", "Landroid/app/Activity;");
            if (currentActivityField) {
                jobject activity = env->GetStaticObjectField(unityPlayerClass, currentActivityField);
                if (activity) {
                    rotation = queryDisplayRotationFromContext(env, activity);
                    env->DeleteLocalRef(activity);
                }
            }
            env->DeleteLocalRef(unityPlayerClass);
        }

        if (rotation < 0 && env->ExceptionCheck()) {
            env->ExceptionClear();
        }

        if (rotation < 0) {
            jobject activity = queryCurrentActivityFromActivityThread(env);
            if (activity) {
                rotation = queryDisplayRotationFromContext(env, activity);
                env->DeleteLocalRef(activity);
            }
        }

        if (rotation < 0 && env->ExceptionCheck()) {
            env->ExceptionClear();
        }

        if (rotation < 0) {
            jclass activityThreadClass = env->FindClass("android/app/ActivityThread");
            if (activityThreadClass && !env->ExceptionCheck()) {
                jmethodID currentApplication = env->GetStaticMethodID(
                    activityThreadClass, "currentApplication", "()Landroid/app/Application;");
                jobject application = currentApplication
                    ? env->CallStaticObjectMethod(activityThreadClass, currentApplication)
                    : nullptr;
                if (application) {
                    rotation = queryDisplayRotationFromContext(env, application);
                    env->DeleteLocalRef(application);
                }
                env->DeleteLocalRef(activityThreadClass);
            }
        }

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            rotation = -1;
        }

        if (attachedHere) javaVm->DetachCurrentThread();
        return rotation;
    }

    static int getAutoRotation(float screenW, float screenH) {
        static auto s_lastQuery = std::chrono::steady_clock::time_point{};
        auto now = std::chrono::steady_clock::now();
        if (s_lastQuery.time_since_epoch().count() == 0 ||
            now - s_lastQuery >= std::chrono::milliseconds(500)) {
            s_lastQuery = now;
            int queriedRotation = queryDisplayRotationFromJava();
            if (queriedRotation >= 0 && queriedRotation <= 3) {
                int touchRotation = displayRotationToTouchRotation(queriedRotation);
                s_lastJavaTouchRotation = touchRotation;
                s_lastAutoRotation = s_lockToSurfaceOrientation
                    ? inferRotationFromSurface(screenW, screenH, touchRotation)
                    : touchRotation;
            } else {
                bool surfaceLandscape = (screenW > screenH);
                bool digitizerPortrait = (s_absMaxY > s_absMaxX * 1.1f);
                if (s_lockToSurfaceOrientation) {
                    s_lastAutoRotation = inferRotationFromSurface(screenW, screenH, s_lastAutoRotation);
                } else if (surfaceLandscape && digitizerPortrait) {
                    s_lastAutoRotation = 3;
                } else if (!surfaceLandscape && s_absMaxX > s_absMaxY * 1.1f) {
                    s_lastAutoRotation = 1;
                } else {
                    s_lastAutoRotation = 0;
                }
            }
        }
        return s_lastAutoRotation;
    }

    /** @brief 循环切换旋转模式 (供 UI 按钮调用) */
    void cycleRotation() {
        if (s_rotationCorrectionLocked) {
            LOG(LOG_LEVEL_INFO, "[Touch] 旋转矫正已锁定，忽略 Rot 按钮");
            return;
        }
        if (s_rotation < 0) {
            s_rotation = 0;
        } else if (s_rotation >= 3) {
            s_rotation = -1;
        } else {
            s_rotation += 1;
        }
        LOG(LOG_LEVEL_INFO, "[Touch] 旋转模式切换 → %s", getRotationLabel());
    }

    /** @brief 获取当前旋转值 */
    int getRotation() { return s_rotation; }

    void toggleSurfaceRotationLock() {
        s_lockToSurfaceOrientation = !s_lockToSurfaceOrientation;
        LOG(LOG_LEVEL_INFO, "[Touch] Surface 方向锁定 → %d", s_lockToSurfaceOrientation ? 1 : 0);
    }

    bool isSurfaceRotationLockEnabled() {
        return s_lockToSurfaceOrientation;
    }

    void toggleRotationCorrectionLock() {
        if (!s_rotationCorrectionLocked) {
            s_lockedCorrectionRotation = captureCurrentRotationForLock();
            s_rotationCorrectionLocked = true;
            LOG(LOG_LEVEL_INFO, "[Touch] 旋转矫正锁定 → ON (rot=%d)", s_lockedCorrectionRotation);
            return;
        }

        s_rotationCorrectionLocked = false;
        s_lockedCorrectionRotation = -1;
        LOG(LOG_LEVEL_INFO, "[Touch] 旋转矫正锁定 → OFF");
    }

    bool isRotationCorrectionLocked() {
        return s_rotationCorrectionLocked;
    }

    /** @brief 获取当前旋转模式描述 */
    const char* getRotationLabel() {
        static thread_local char label[32];
        switch (s_rotation) {
            case -1:
                snprintf(label, sizeof(label), "%s%s:%d",
                         s_lockToSurfaceOrientation ? "AutoS" : "AutoD",
                         s_rotationCorrectionLocked ? "L" : "",
                         (s_rotationCorrectionLocked && s_lockedCorrectionRotation >= 0
                            ? s_lockedCorrectionRotation
                            : s_lastAutoRotation) * 90);
                return label;
            case 0:  return "0";
            case 1:  return "90";
            case 2:  return "180";
            case 3:  return "270";
            default: return "?";
        }
    }

    int getFd() { return s_fd; }
    const char* getDevicePath() { return s_devPath[0] ? s_devPath : "<none>"; }
    const char* getDeviceName() { return s_devName[0] ? s_devName : "<unknown>"; }
    float getRawX() { return s_rawX; }
    float getRawY() { return s_rawY; }
    float getCurX() { return s_curX; }
    float getCurY() { return s_curY; }
    bool isTouching() { return s_touching; }

    struct TouchDeviceCandidate {
        int fd = -1;
        int score = -1;
        bool hasMt = false;
        bool hasSingleTouch = false;
        bool hasBtnTouch = false;
        bool hasDirect = false;
        float absMaxX = 1.0f;
        float absMaxY = 1.0f;
        char path[64] = {0};
        char name[128] = {0};
    };

    static void toLowerCopy(const char* src, char* dst, size_t dstSize) {
        if (!dst || dstSize == 0) return;
        dst[0] = '\0';
        if (!src) return;
        size_t i = 0;
        for (; src[i] != '\0' && i + 1 < dstSize; ++i) {
            dst[i] = (char)std::tolower((unsigned char)src[i]);
        }
        dst[i] = '\0';
    }

    static bool containsKeyword(const char* text, const char* keyword) {
        return text && keyword && std::strstr(text, keyword) != nullptr;
    }

    static bool buildTouchDeviceCandidate(const char* path, TouchDeviceCandidate& out) {
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            if (!s_loggedOpenPermissionError && (errno == EACCES || errno == EPERM)) {
                s_loggedOpenPermissionError = true;
                LOG(LOG_LEVEL_WARN,
                    "[Touch] 无法打开 %s: errno=%d (%s). 通常是 /dev/input/event* 权限不足, 执行: adb shell su -c \"chmod 666 /dev/input/event*\"",
                    path, errno, strerror(errno));
            }
            return false;
        }

        constexpr size_t kBitsPerLong = sizeof(unsigned long) * 8;
        constexpr auto bitArrayWords = [](size_t maxBit) constexpr {
            return (maxBit / kBitsPerLong) + 1;
        };
        auto hasBit = [&](const unsigned long* bits, size_t bit) -> bool {
            return ((bits[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1UL) != 0;
        };

        unsigned long evBits[bitArrayWords(EV_MAX)] = {};
        unsigned long absBits[bitArrayWords(ABS_MAX)] = {};
        unsigned long keyBits[bitArrayWords(KEY_MAX)] = {};
        unsigned long propBits[bitArrayWords(INPUT_PROP_MAX)] = {};
        if (ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits) < 0 ||
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0) {
            close(fd);
            return false;
        }

        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
        ioctl(fd, EVIOCGPROP(sizeof(propBits)), propBits);

        char devName[128] = {0};
        ioctl(fd, EVIOCGNAME(sizeof(devName)), devName);

        bool hasMtX = hasBit(absBits, ABS_MT_POSITION_X);
        bool hasMtY = hasBit(absBits, ABS_MT_POSITION_Y);
        bool hasAbsX = hasBit(absBits, ABS_X);
        bool hasAbsY = hasBit(absBits, ABS_Y);
        bool hasBtnTouch = hasBit(keyBits, BTN_TOUCH);
        bool hasDirect = hasBit(propBits, INPUT_PROP_DIRECT);

        if ((!hasMtX || !hasMtY) && (!hasAbsX || !hasAbsY)) {
            close(fd);
            return false;
        }

        struct input_absinfo absX{}, absY{};
        int absCodeX = (hasMtX && hasMtY) ? ABS_MT_POSITION_X : ABS_X;
        int absCodeY = (hasMtX && hasMtY) ? ABS_MT_POSITION_Y : ABS_Y;
        if (ioctl(fd, EVIOCGABS(absCodeX), &absX) < 0 ||
            ioctl(fd, EVIOCGABS(absCodeY), &absY) < 0) {
            close(fd);
            return false;
        }

        char nameLower[128] = {0};
        toLowerCopy(devName, nameLower, sizeof(nameLower));

        int score = 0;
        if (hasMtX && hasMtY) score += 40;
        if (hasAbsX && hasAbsY) score += 10;
        if (hasBtnTouch) score += 20;
        if (hasDirect) score += 25;
        if (containsKeyword(nameLower, "touchscreen")) score += 80;
        if (containsKeyword(nameLower, "touch screen")) score += 80;
        if (containsKeyword(nameLower, "touch panel")) score += 70;
        if (containsKeyword(nameLower, "touchpanel")) score += 70;
        if (containsKeyword(nameLower, "_ts") || containsKeyword(nameLower, " ts")) score += 45;
        if (containsKeyword(nameLower, "sec_touchscreen")) score += 80;
        if (containsKeyword(nameLower, "fts") || containsKeyword(nameLower, "goodix") ||
            containsKeyword(nameLower, "synaptics") || containsKeyword(nameLower, "focaltech") ||
            containsKeyword(nameLower, "elan") || containsKeyword(nameLower, "novatek")) {
            score += 35;
        }
        if (containsKeyword(nameLower, "key") || containsKeyword(nameLower, "keyboard") ||
            containsKeyword(nameLower, "mouse") || containsKeyword(nameLower, "stylus") ||
            containsKeyword(nameLower, "fingerprint") || containsKeyword(nameLower, "sensor")) {
            score -= 40;
        }

        out.fd = fd;
        out.score = score;
        out.hasMt = hasMtX && hasMtY;
        out.hasSingleTouch = hasAbsX && hasAbsY;
        out.hasBtnTouch = hasBtnTouch;
        out.hasDirect = hasDirect;
        out.absMaxX = absX.maximum > 0 ? (float)absX.maximum : 1.0f;
        out.absMaxY = absY.maximum > 0 ? (float)absY.maximum : 1.0f;
        snprintf(out.path, sizeof(out.path), "%s", path);
        snprintf(out.name, sizeof(out.name), "%s", devName[0] ? devName : "<unnamed>");
        return true;
    }

    /** @brief 尝试打开触摸设备, 失败则每 5 秒自动重试 */
    void init() {
        // 已成功打开
        if (s_fd >= 0) return;

        // 限制重试频率: 每 5 秒重试一次
        auto now = std::chrono::steady_clock::now();
        if (s_fd == -2 && s_retryCount > 0) {
            if (now - s_lastRetry < std::chrono::seconds(5)) return;
        }
        s_lastRetry = now;
        s_retryCount++;

        // 首次运行时打印提示
        if (s_retryCount == 1) {
            LOG(LOG_LEVEL_INFO, "[Touch] ══════ 初始化触摸输入 ══════");
            LOG(LOG_LEVEL_INFO, "[Touch] 如果触摸不可用, 请在 PC 上执行:");
            LOG(LOG_LEVEL_INFO, "[Touch]   adb shell su -c \"chmod 666 /dev/input/event*\"");

            // 尝试自动提权 (需要设备已 root)
            system("su -c 'setenforce 0' 2>/dev/null");
            int ret = system("su -c 'chmod 666 /dev/input/event*' 2>/dev/null");
            if (ret == 0) {
                LOG(LOG_LEVEL_INFO, "[Touch] 自动 chmod 成功");
            } else {
                LOG(LOG_LEVEL_WARN, "[Touch] 自动 chmod 失败(ret=%d), 需手动执行", ret);
            }
        }

        TouchDeviceCandidate best{};
        auto considerPath = [&](const char* path) {
            TouchDeviceCandidate candidate{};
            if (!buildTouchDeviceCandidate(path, candidate)) return;

            LOG(LOG_LEVEL_INFO,
                "[Touch] 候选设备: %s name=%s score=%d mt=%d abs=%d btnTouch=%d direct=%d range=(%.0f,%.0f)",
                candidate.path, candidate.name, candidate.score,
                candidate.hasMt ? 1 : 0,
                candidate.hasSingleTouch ? 1 : 0,
                candidate.hasBtnTouch ? 1 : 0,
                candidate.hasDirect ? 1 : 0,
                candidate.absMaxX, candidate.absMaxY);

            if (best.fd < 0 || candidate.score > best.score) {
                if (best.fd >= 0) close(best.fd);
                best = candidate;
            } else {
                close(candidate.fd);
            }
        };

        for (int i = 0; i < 32; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            considerPath(path);
        }

        DIR* dir = opendir("/dev/input");
        if (dir) {
            struct dirent* entry = nullptr;
            while ((entry = readdir(dir)) != nullptr) {
                if (strncmp(entry->d_name, "event", 5) != 0) continue;
                char path[128];
                snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
                considerPath(path);
            }
            closedir(dir);
        }

        if (best.fd >= 0) {
            s_fd = best.fd;
            s_absMaxX = best.absMaxX;
            s_absMaxY = best.absMaxY;
            if (s_rotation < 0) {
                bool digitizerPortrait = (s_absMaxY > s_absMaxX * 1.1f);
                s_lastAutoRotation = digitizerPortrait ? 3 : 0;
                s_lastJavaTouchRotation = s_lastAutoRotation;
            }
            snprintf(s_devPath, sizeof(s_devPath), "%s", best.path);
            snprintf(s_devName, sizeof(s_devName), "%s", best.name);
            LOG(LOG_LEVEL_INFO, "[Touch] ✓ 选中触摸设备: %s (%s) score=%d (重试 %d 次)",
                s_devPath, s_devName, best.score, s_retryCount);
            LOG(LOG_LEVEL_INFO, "[Touch]   数字化仪范围: X=0..%.0f  Y=0..%.0f  (宽高比=%.2f)",
                s_absMaxX, s_absMaxY, s_absMaxX / s_absMaxY);
            return;
        }

        // 仍然失败, 保持 s_fd = -2 以便下次重试
        if (s_retryCount <= 3) {
            LOG(LOG_LEVEL_INFO, "[Touch] 等待权限或设备匹配... (第 %d 次尝试, 扫描 /dev/input/event0~31)", s_retryCount);
        }
    }

    /**
     * @brief 处理原始触摸事件, 应用旋转变换后喂给 ImGui IO
     *
     * 旋转变换公式 (rawX/rawY 是归一化的数字化仪竖屏坐标 0..1):
     *   ROTATION_0:   screenX = rawX * W,           screenY = rawY * H
     *   ROTATION_90:  screenX = (1-rawY) * W,       screenY = rawX * H     (逆时针90°)
     *   ROTATION_180: screenX = (1-rawX) * W,       screenY = (1-rawY) * H
    *   ROTATION_270: screenX = rawY * W,            screenY = (1-rawX) * H (顺时针90°)
     */
    void processEvents(float screenW, float screenH) {
        if (s_fd < 0) return;
        struct input_event ev{};
        bool posUpdated = false;  // 本轮 SYN_REPORT 是否收到位置更新
        while (read(s_fd, &ev, sizeof(ev)) == sizeof(ev)) {
            switch (ev.type) {
                case EV_ABS:
                    // 多点触控协议 B (优先)
                    if (ev.code == ABS_MT_POSITION_X) {
                        s_rawX = std::clamp((float)ev.value / s_absMaxX, 0.0f, 1.0f);
                        posUpdated = true;
                    } else if (ev.code == ABS_MT_POSITION_Y) {
                        s_rawY = std::clamp((float)ev.value / s_absMaxY, 0.0f, 1.0f);
                        posUpdated = true;
                    }
                    // 单点触控协议 (fallback)
                    else if (ev.code == ABS_X) {
                        s_rawX = std::clamp((float)ev.value / s_absMaxX, 0.0f, 1.0f);
                        posUpdated = true;
                    } else if (ev.code == ABS_Y) {
                        s_rawY = std::clamp((float)ev.value / s_absMaxY, 0.0f, 1.0f);
                        posUpdated = true;
                    }
                    // 触摸状态
                    else if (ev.code == ABS_MT_TRACKING_ID) {
                        s_touching = (ev.value >= 0);
                    }
                    break;
                case EV_KEY:
                    if (ev.code == BTN_TOUCH) s_touching = (ev.value != 0);
                    break;
                case EV_SYN:
                    if (ev.code == SYN_REPORT) {
                        auto& io = ImGui::GetIO();
                        if (s_rawX >= 0.0f && s_rawY >= 0.0f) {

                            if (s_touching && !s_lastTouchingState) {
                                s_touchStartRawX = s_rawX;
                                s_touchStartRawY = s_rawY;
                                s_activeTouchRotation = s_rotationCorrectionLocked
                                    ? s_lockedCorrectionRotation
                                    : ((s_rotation < 0)
                                        ? resolveAutoRotationForTouchStart(screenW, screenH)
                                        : s_rotation);
                            } else if (!s_touching && s_lastTouchingState) {
                                s_activeTouchRotation = -1;
                                s_pendingMirrorResolve = false;
                                s_touchStartRawX = -1.0f;
                                s_touchStartRawY = -1.0f;
                            }

                            // ── 确定旋转模式 ──
                            int rot = s_rotationCorrectionLocked
                                ? s_lockedCorrectionRotation
                                : s_rotation;
                            if (rot < 0) {
                                if (s_activeTouchRotation >= 0) {
                                    rot = s_activeTouchRotation;
                                    tryResolveMirrorFromFirstDrag(screenW, screenH);
                                    rot = s_activeTouchRotation;
                                } else {
                                    rot = getEffectiveRotation(screenW, screenH);
                                }
                            }

                            // ── 应用旋转变换 ──
                            float fx, fy;
                            switch (rot) {
                                default:
                                case 0:  // 不旋转
                                    fx = s_rawX * screenW;
                                    fy = s_rawY * screenH;
                                    break;
                                case 1:  // ROTATION_90 (逆时针, 最常见横屏)
                                    fx = (1.0f - s_rawY) * screenW;
                                    fy = s_rawX * screenH;
                                    break;
                                case 2:  // ROTATION_180
                                    fx = (1.0f - s_rawX) * screenW;
                                    fy = (1.0f - s_rawY) * screenH;
                                    break;
                                case 3:  // ROTATION_270 (顺时针, 反向横屏)
                                    fx = s_rawY * screenW;
                                    fy = (1.0f - s_rawX) * screenH;
                                    break;
                            }

                            s_curX = fx;
                            s_curY = fy;
                            io.MousePos = ImVec2(fx, fy);
                        }
                        // 如果设备不发送 BTN_TOUCH/TRACKING_ID，通过位置更新推断触摸
                        if (posUpdated && !s_touching) {
                            s_touching = true;
                        }
                        io.MouseDown[0] = s_touching;
                        s_lastTouchingState = s_touching;
                        posUpdated = false;
                    }
                    break;
            }
        }
    }

    void drawDebugIndicator() {
        if (s_fd < 0 || s_curX < 0.0f) return;
        if (!s_touching) return;  // 仅在触摸时显示
        auto* dl = ImGui::GetForegroundDrawList();
        ImVec2 pos(s_curX, s_curY);
        // 外圈白色环 + 内圈红色实心，更醒目
        dl->AddCircleFilled(pos, 22.0f, IM_COL32(255, 255, 255, 120));
        dl->AddCircleFilled(pos, 14.0f, IM_COL32(255, 50, 50, 220));
        dl->AddCircle(pos, 22.0f, IM_COL32(255, 255, 255, 200), 0, 2.0f);
    }

} // namespace touch_input


// ═══════════════════════════════════════════════════════════════════════════════
// AImGui RenderClient 绘制方案 (无 Hook)
//
// 独立 GUI 线程通过 AImGui RenderClient 连接到 PublicOverlayRenderer (RenderServer),
// 通过 TCP 传输 ImGui 绘制数据, 在独立 Surface 上渲染覆盖层。
// 完全不 hook 游戏线程, 零性能影响。
// ═══════════════════════════════════════════════════════════════════════════════

#include "../AndroidSurfaceImgui/includes/AImGui.h"

namespace {

struct ShellDisplayInfo {
    int width = 0;
    int height = 0;
    int rotateTheta = 0;
};

static bool queryShellDisplay(ShellDisplayInfo& outInfo) {
    FILE* fp = popen("wm size 2>/dev/null", "r");
    if (!fp) return false;
    char buf[128] = {};
    while (fgets(buf, sizeof(buf), fp)) {
        int w = 0, h = 0;
        if (sscanf(buf, "Physical size: %dx%d", &w, &h) == 2 ||
            sscanf(buf, "Override size: %dx%d", &w, &h) == 2) {
            if (w > 0 && h > 0) { outInfo.width = w; outInfo.height = h; }
        }
    }
    pclose(fp);

    // 横屏归一化
    if (outInfo.width > 0 && outInfo.height > 0) {
        if (outInfo.width < outInfo.height) {
            std::swap(outInfo.width, outInfo.height);
            outInfo.rotateTheta = 0;
        }
        return true;
    }
    return false;
}

} // namespace

static std::atomic<bool> g_unityGuiThreadStarted{false};

static void UnityGuiThread() {
    struct GuiResetGuard {
        ~GuiResetGuard() { g_unityGuiThreadStarted.store(false, std::memory_order_release); }
    } resetGuard;

    LOG(LOG_LEVEL_INFO, "[UnityGui] GUI 线程启动, 等待 10 秒...");
    std::this_thread::sleep_for(std::chrono::seconds(10));

    // 获取显示尺寸
    ShellDisplayInfo displayInfo;
    if (!queryShellDisplay(displayInfo)) {
        displayInfo.width = 2400;
        displayInfo.height = 1080;
        displayInfo.rotateTheta = 0;
        LOG(LOG_LEVEL_WARN, "[UnityGui] 显示信息获取失败, 使用兜底尺寸 %dx%d", displayInfo.width, displayInfo.height);
    }
    LOG(LOG_LEVEL_INFO, "[UnityGui] 显示尺寸: %dx%d rotate=%d", displayInfo.width, displayInfo.height, displayInfo.rotateTheta);

    // 创建 AImGui RenderClient
    android::AImGui::Options imguiOptions{
        .renderType = android::AImGui::RenderType::RenderClient,
        .exchangeFontData = true,
        .tcpNoDelay = true,
        .disableVsync = true,
        .styleScale = 1.75f,
        .fontSizePixels = 18.0f,
        .screenWidth = displayInfo.width,
        .screenHeight = displayInfo.height,
        .rotateTheta = displayInfo.rotateTheta,
        .clientConnectAddress = "127.0.0.1",
    };

    LOG(LOG_LEVEL_INFO, "[UnityGui] AImGui RenderClient 连接中...");
    std::unique_ptr<android::AImGui> imgui;
    for (int attempt = 1; attempt <= 20; ++attempt) {
        try {
            imgui = std::make_unique<android::AImGui>(imguiOptions);
        } catch (const std::exception& e) {
            LOG(LOG_LEVEL_ERROR, "[UnityGui] AImGui 初始化异常: attempt=%d error=%s", attempt, e.what());
            imgui.reset();
        } catch (...) {
            LOG(LOG_LEVEL_ERROR, "[UnityGui] AImGui 初始化异常: attempt=%d", attempt);
            imgui.reset();
        }
        if (imgui && *imgui) break;
        LOG(LOG_LEVEL_INFO, "[UnityGui] 等待 Overlay 服务就绪: %d/20", attempt);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!imgui || !(*imgui)) {
        LOG(LOG_LEVEL_ERROR, "[UnityGui] AImGui RenderClient 连接失败");
        return;
    }
    LOG(LOG_LEVEL_INFO, "[UnityGui] AImGui RenderClient 连接成功");

    // 输入处理线程
    std::thread([imguiPtr = imgui.get()]() {
        LOG(LOG_LEVEL_INFO, "[UnityGui] 输入线程启动");
        while (true) {
            imguiPtr->ProcessInputEvent();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }).detach();

    // 主渲染循环
    lol::MiniMapData gameData;
    draw::GameOverlay overlay;

    while (true) {
        imgui->BeginFrame();

        {
            ImGuiIO& io = ImGui::GetIO();
            if (displayInfo.width > 0 && displayInfo.height > 0) {
                io.DisplaySize = ImVec2(static_cast<float>(displayInfo.width),
                                        static_cast<float>(displayInfo.height));
                io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            }
        }

        SharedGameData::getInstance().pullData(gameData);
        bool inBattle = SharedGameData::getInstance().isBattleActive();
        overlay.drawOverlay(gameData, inBattle);

        imgui->EndFrame();
        std::this_thread::yield();
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// TestFunction — 数据采集主入口 (实时刷新)
// ═══════════════════════════════════════════════════════════════════════════════

namespace {

struct AutoClearTargetSet {
    const lol::MiniMapMinionInfo* nearest = nullptr;
    const lol::MiniMapMinionInfo* lowestHpInRange = nullptr;
};

bool isFiniteAutoFarmValue(float value) {
    return std::isfinite(value);
}

bool hasValidAutoFarmWorldPos(const lol::UnityVector3& pos) {
    return isFiniteAutoFarmValue(pos.x) &&
           isFiniteAutoFarmValue(pos.y) &&
           isFiniteAutoFarmValue(pos.z);
}

AutoClearTargetSet selectAutoClearTargets(const lol::MiniMapData& data) {
    AutoClearTargetSet result;
    if (data.minions.empty()) return result;
    if (!hasValidAutoFarmWorldPos(data.myWorldPos)) return result;

    const float attackRangeSq = data.mySkillRange > 0.0f
        ? (data.mySkillRange * data.mySkillRange)
        : -1.0f;
    float nearestDistanceSq = std::numeric_limits<float>::infinity();
    float lowestHp = std::numeric_limits<float>::infinity();
    float lowestHpDistanceSq = std::numeric_limits<float>::infinity();

    for (const auto& minion : data.minions) {
        if (!minion.isEnemy || !minion.hasWorldPos || minion.curHp <= 0.0f) continue;
        if (!hasValidAutoFarmWorldPos(minion.worldPos)) continue;

        const float dx = minion.worldPos.x - data.myWorldPos.x;
        const float dz = minion.worldPos.z - data.myWorldPos.z;
        const float distanceSq = dx * dx + dz * dz;

        if (distanceSq < nearestDistanceSq) {
            nearestDistanceSq = distanceSq;
            result.nearest = &minion;
        }

        if (attackRangeSq > 0.0f && distanceSq <= attackRangeSq) {
            if (minion.curHp < lowestHp ||
                (std::fabs(minion.curHp - lowestHp) < 0.001f && distanceSq < lowestHpDistanceSq)) {
                lowestHp = minion.curHp;
                lowestHpDistanceSq = distanceSq;
                result.lowestHpInRange = &minion;
            }
        }
    }

    return result;
}

void processAutoClearMinions(lol::lol& game,
                             const lol::MiniMapData& data,
                             std::chrono::steady_clock::time_point now) {
    static bool s_autoFarmMoving = false;
    static auto s_lastAttackAt = std::chrono::steady_clock::time_point{};
    static auto s_lastMoveCommandAt = std::chrono::steady_clock::time_point{};
    static auto s_lastStopMoveAt = std::chrono::steady_clock::time_point{};
    static float s_lastMoveDirX = 0.0f;
    static float s_lastMoveDirY = 0.0f;

    auto stopAutoMove = [&]() {
        if (!s_autoFarmMoving) return;
        game.simulateMovement(0.0f, 0.0f);
        s_autoFarmMoving = false;
        s_lastMoveDirX = 0.0f;
        s_lastMoveDirY = 0.0f;
        s_lastStopMoveAt = now;
    };

    if (!SharedGameData::getInstance().isAutoClearMinionsEnabled()) {
        stopAutoMove();
        return;
    }

    if (data.mySkillRange <= 0.0f || !hasValidAutoFarmWorldPos(data.myWorldPos)) {
        stopAutoMove();
        return;
    }

    const auto targets = selectAutoClearTargets(data);
    if (targets.lowestHpInRange) {
        stopAutoMove();

        LOG(LOG_LEVEL_INFO,
            "[AutoFarm] attack target objId=%u camp=%d enemy=%d hp=%.0f/%.0f pos=(%.1f,%.1f,%.1f)",
            targets.lowestHpInRange->objId,
            targets.lowestHpInRange->camp,
            targets.lowestHpInRange->isEnemy,
            targets.lowestHpInRange->curHp,
            targets.lowestHpInRange->maxHp,
            targets.lowestHpInRange->worldPos.x,
            targets.lowestHpInRange->worldPos.y,
            targets.lowestHpInRange->worldPos.z);

        constexpr auto kStopBeforeAttackDelay = std::chrono::milliseconds(120);
        if (s_lastStopMoveAt.time_since_epoch().count() != 0 &&
            now - s_lastStopMoveAt < kStopBeforeAttackDelay) {
            return;
        }

        constexpr auto kAttackCooldown = std::chrono::milliseconds(350);
        if (s_lastAttackAt.time_since_epoch().count() == 0 || now - s_lastAttackAt >= kAttackCooldown) {
            if (game.simulateNormalAttack()) {
                s_lastAttackAt = now;
            }
        }
        return;
    }

    if (!targets.nearest || !targets.nearest->hasWorldPos) {
        stopAutoMove();
        return;
    }

    const float dx = targets.nearest->worldPos.x - data.myWorldPos.x;
    const float dz = targets.nearest->worldPos.z - data.myWorldPos.z;
    const float distanceSq = dx * dx + dz * dz;

    LOG(LOG_LEVEL_INFO,
        "[AutoFarm] move target objId=%u camp=%d enemy=%d hp=%.0f/%.0f pos=(%.1f,%.1f,%.1f) dist=%.2f",
        targets.nearest->objId,
        targets.nearest->camp,
        targets.nearest->isEnemy,
        targets.nearest->curHp,
        targets.nearest->maxHp,
        targets.nearest->worldPos.x,
        targets.nearest->worldPos.y,
        targets.nearest->worldPos.z,
        std::sqrt(distanceSq));

    if (distanceSq <= 0.0001f) {
        stopAutoMove();
        return;
    }

    if (data.mySkillRange > 0.0f) {
        const float stopDistance = std::max(0.5f, data.mySkillRange - 0.35f);
        if (distanceSq <= stopDistance * stopDistance) {
            stopAutoMove();
            return;
        }
    }

    const float distance = std::sqrt(distanceSq);
    const float dirX = dx / distance;
    const float dirY = dz / distance;

    constexpr auto kMoveCommandInterval = std::chrono::milliseconds(90);
    const bool moveIntervalElapsed =
        s_lastMoveCommandAt.time_since_epoch().count() == 0 ||
        now - s_lastMoveCommandAt >= kMoveCommandInterval;
    const bool directionChanged =
        !s_autoFarmMoving ||
        std::fabs(dirX - s_lastMoveDirX) >= 0.08f ||
        std::fabs(dirY - s_lastMoveDirY) >= 0.08f;
    if (!moveIntervalElapsed && !directionChanged) {
        return;
    }

    if (game.simulateMovement(dirX, dirY)) {
        s_autoFarmMoving = true;
        s_lastMoveDirX = dirX;
        s_lastMoveDirY = dirY;
        s_lastMoveCommandAt = now;
    }
}

} // namespace

static void TestFunction(void *pli2cppModeBase, void *pCodeRegistration,
                         void *pMetadataRegistration, void *pGlobalMetadataHeader,
                         void *pMetadataImagesTable) {

    LOG(LOG_LEVEL_INFO, "[TestFunction] ══════ 启动 ══════");
    installCrashGuard();

    lol::lol lol(pli2cppModeBase, pCodeRegistration,
                 pMetadataRegistration, pGlobalMetadataHeader, pMetadataImagesTable);
    LOG(LOG_LEVEL_INFO, "[TestFunction] 游戏数据采集初始化完成 ✓");

    constexpr int kCollectMs = 50;
    constexpr int kPrintMs   = 5000;
    auto lastCollect = std::chrono::steady_clock::now();
    auto lastPrint   = lastCollect;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastCollect >= std::chrono::milliseconds(kCollectMs)) {

            int crashSig = sigsetjmp(t_jumpBuf, 1);
            if (crashSig != 0) {
                LOG(LOG_LEVEL_ERROR, "[TestFunction] ⚠ 捕获信号 %d，跳过本次采集", crashSig);
                t_guardActive = 0;
                // 崩溃时不改变状态，保留上一帧数据，避免闪烁
            } else {
                t_guardActive = 1;
                bool isBattle = lol.get_BattleStarted();
                SharedGameData::getInstance().setBattleActive(isBattle);
                if (isBattle) {
                    lol.updateMiniMapData();
                    lol.updateMinionData();
                    lol.tickPendingAttack();
                    lol.detectMyHeroSurroundingObstacles(5.0f); // 5单位步长实时检测己方英雄四方向障碍

                    const auto& miniMapData = lol.getMiniMapData();
                    processAutoClearMinions(lol, miniMapData, now);
                    SharedGameData::getInstance().pushData(miniMapData);

                    if (now - lastPrint >= std::chrono::milliseconds(kPrintMs)) {
                        lol.printMiniMapData();
                        lastPrint = now;
                    }
                }
                t_guardActive = 0;
            }
            lastCollect = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// 读取 /data/local/tmp/dobby_config.txt 配置文件
// ═══════════════════════════════════════════════════════════════════════════════

struct DobbyConfig {
    bool enableDumper = false;
    bool enableHeader = false;
    bool enableLog = true;
};

static DobbyConfig readDobbyConfig() {
    DobbyConfig cfg;
    int fd = open("/data/local/tmp/dobby_config.txt", O_RDONLY);
    if (fd < 0) return cfg;
    char buf[256] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return cfg;
    buf[n] = '\0';
    if (strstr(buf, "dumper=1")) cfg.enableDumper = true;
    if (strstr(buf, "header=1")) cfg.enableHeader = true;
    if (strstr(buf, "log=0")) cfg.enableLog = false;
    // 应用日志开关
    g_runtimeLogEnabled = cfg.enableLog;
    LOG(LOG_LEVEL_INFO, "[Config] dumper=%d header=%d log=%d", cfg.enableDumper ? 1 : 0, cfg.enableHeader ? 1 : 0, cfg.enableLog ? 1 : 0);
    return cfg;
}

// ═══════════════════════════════════════════════════════════════════════════════
//  JNI Toast 工具 — 在安卓主线程显示下方弹框
// ═══════════════════════════════════════════════════════════════════════════════
namespace toast_util {
    static void showToast(const char* msg) {
        JavaVM* vm = touch_input::getJavaVM();
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

// ═══════════════════════════════════════════════════════════════════════════════
// MyStartPointLOL — 注入入口
// ═══════════════════════════════════════════════════════════════════════════════

extern "C" __attribute__((visibility("default")))
bool MyStartPointLOL(void *pli2cppModeBase, void *pCodeRegistration, void *pMetadataRegistration,
                  void *pGlobalMetadataHeader, void *pMetadataImagesTable) {
    try {
        if (pli2cppModeBase == nullptr) {
            LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] 独立应用模式 — 跳过");
            return false;
        }

        // 参数现在是偏移值 (不是已解引用的指针), 需要 base + offset 解引用
        uintptr_t base = reinterpret_cast<uintptr_t>(pli2cppModeBase);
        uintptr_t offCodeReg    = reinterpret_cast<uintptr_t>(pCodeRegistration);
        uintptr_t offMetaReg    = reinterpret_cast<uintptr_t>(pMetadataRegistration);
        uintptr_t offGlobalMeta = reinterpret_cast<uintptr_t>(pGlobalMetadataHeader);
        uintptr_t offMetaImages = reinterpret_cast<uintptr_t>(pMetadataImagesTable);

        // 解引用: 从 base + offset 处读取实际指针值
        pCodeRegistration     = *reinterpret_cast<void**>(base + offCodeReg);
        pMetadataRegistration = *reinterpret_cast<void**>(base + offMetaReg);
        pGlobalMetadataHeader = *reinterpret_cast<void**>(base + offGlobalMeta);
        pMetadataImagesTable  = *reinterpret_cast<void**>(base + offMetaImages);

        LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] base=%p offsets: CR=0x%lX MR=0x%lX GM=0x%lX MI=0x%lX",
            pli2cppModeBase, (unsigned long)offCodeReg, (unsigned long)offMetaReg,
            (unsigned long)offGlobalMeta, (unsigned long)offMetaImages);
        LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] resolved: CR=%p MR=%p GM=%p MI=%p",
            pCodeRegistration, pMetadataRegistration, pGlobalMetadataHeader, pMetadataImagesTable);

        LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] 注入模式 — 启动数据采集 + eglSwapBuffers Hook 绘制");
        installCrashGuard();

        // 读取配置
        DobbyConfig config = readDobbyConfig();

        // 按配置在独立线程中执行 Dumper / Header（不阻塞主注入流程）
        if (config.enableHeader) {
            std::thread([=]() {
                LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] [线程] 启用 il2cppHeader — 导出头文件");
                try {
                    li2cppHeader::li2cppHeader il2cppH(pli2cppModeBase, pCodeRegistration,
                                                       pMetadataRegistration, pGlobalMetadataHeader, pMetadataImagesTable);
                    il2cppH.start();
                    LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] [线程] il2cppHeader 完成");
                    toast_util::showToast("il2cppHeader 导出完成");
                    int fd = open("/data/local/tmp/dobby_header_done", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) { write(fd, "done", 4); close(fd); }
                } catch (const std::exception& e) {
                    LOG(LOG_LEVEL_ERROR, "[MyStartPointLOL] il2cppHeader 异常: %s", e.what());
                    toast_util::showToast("il2cppHeader 导出失败");
                } catch (...) {
                    LOG(LOG_LEVEL_ERROR, "[MyStartPointLOL] il2cppHeader 未知异常");
                    toast_util::showToast("il2cppHeader 导出失败");
                }
            }).detach();
        }
        if (config.enableDumper) {
            std::thread([=]() {
                LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] [线程] 启用 il2cppDumper — 导出 dump");
                try {
                    li2cpp::li2cppDumper il2cppD(pli2cppModeBase, pCodeRegistration,
                                                 pMetadataRegistration, pGlobalMetadataHeader, pMetadataImagesTable);
                    il2cppD.initInfo();
                    LOG(LOG_LEVEL_INFO, "[MyStartPointLOL] [线程] il2cppDumper 完成");
                    toast_util::showToast("il2cppDumper 导出完成");
                    int fd = open("/data/local/tmp/dobby_dumper_done", O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) { write(fd, "done", 4); close(fd); }
                } catch (const std::exception& e) {
                    LOG(LOG_LEVEL_ERROR, "[MyStartPointLOL] il2cppDumper 异常: %s", e.what());
                    toast_util::showToast("il2cppDumper 导出失败");
                } catch (...) {
                    LOG(LOG_LEVEL_ERROR, "[MyStartPointLOL] il2cppDumper 未知异常");
                    toast_util::showToast("il2cppDumper 导出失败");
                }
            }).detach();
        }

        bool guiExpected = false;
        if (g_unityGuiThreadStarted.compare_exchange_strong(guiExpected, true, std::memory_order_acq_rel)) {
            std::thread(UnityGuiThread).detach();
        }

        std::thread([=]() {
            std::this_thread::sleep_for(std::chrono::seconds(2));

           TestFunction(pli2cppModeBase, pCodeRegistration,
                    pMetadataRegistration, pGlobalMetadataHeader, pMetadataImagesTable);

        }).detach();

        return true;
    }
    catch (const std::exception& e) {
        LOG(LOG_LEVEL_ERROR, "[MyStartPointLOL] 异常: %s", e.what());
        return false;
    }
    catch (...) {
        LOG(LOG_LEVEL_ERROR, "[MyStartPointLOL] 未知异常");
        return false;
    }
}



/*

  li2cppHeader::li2cppHeader il2cpp1(pli2cppModeBase, pCodeRegistration,
                                     pMetadataRegistration, pGlobalMetadataHeader, pMetadataImagesTable);
            il2cpp1.start();

           li2cpp::li2cppDumper il2cpp2(pli2cppModeBase, pCodeRegistration,
                                     pMetadataRegistration, pGlobalMetadataHeader, pMetadataImagesTable);
            il2cpp2.initInfo();

*/

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif
