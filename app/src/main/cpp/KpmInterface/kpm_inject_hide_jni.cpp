/**
 * @file    kpm_inject_hide_jni.cpp
 * @brief   InjectHideActivity 的 JNI 桥接层。
 *          通过 KpCtl (ReadProcessMemory/kp_ctl.cpp) 把
 *          KernelPatch/kpms 模块的 control0 命令映射为 JNI 方法：
 *            - nativeEnable/Disable ProcHide / FileHide / CommHide : 三个总开关
 *            - nativeAdd/Remove/Clear/List HidePid / HideSo / HidePkg / HideComm
 *            - nativeListRunningApps : 复用 SoDumper root ps -A 枚举进程
 *            - nativeHideSelf / nativeUnhideSelf : 隐藏/显示自身进程
 *            - nativeRawCtl / nativeGetStatus : 调试用透传
 *
 *          同时实现 JNI_OnLoad / JNI_OnUnload：
 *            - 加载时读取 /proc/self/cmdline 取主包名；
 *            - 在 nativeSetSuperkey 拿到密钥后自动 add_hide_pkg 自身，
 *              保证进程启动即被 KPM 跟踪并在匹配 comm 时自动加入 hide_pid；
 *            - 卸载时尝试 remove_hide_pkg（ART 进程被 kill 时一般不会回调）。
 *
 *  对应 Java 类：com.example.dobbyproject.InjectHideActivity
 */
#include <jni.h>
#include <string>
#include <vector>
#include <cstdio>
#include <android/log.h>

#include "../ReadProcessMemory/kp_ctl.h"
#include "../soDumper/so_dumper.h"

#define KTAG "[KpmInjectHideJNI]"

namespace {

std::string jstr(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* p = env->GetStringUTFChars(s, nullptr);
    std::string out = p ? p : "";
    if (p) env->ReleaseStringUTFChars(s, p);
    return out;
}

// �?/proc/<pid>/cmdline，若为空退回到 /proc/<pid>/comm
std::string proc_name_of(int pid) {
    if (pid <= 0) return "?";
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE* fp = fopen(path, "r");
    if (fp) {
        char buf[256] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        if (n > 0) {
            for (size_t i = 0; i < n; i++) if (buf[i] == '\0') buf[i] = ' ';
            std::string s(buf, n);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
            if (!s.empty()) return s;
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    fp = fopen(path, "r");
    if (fp) {
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), fp)) {
            fclose(fp);
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
            if (!s.empty()) return s;
        } else {
            fclose(fp);
        }
    }
    return "<dead>";
}

} // namespace

// ─── SO 加载/卸载时自动把自己包名注册/注销�?kernel hide_pkg ──
// Android �?JNI_OnUnload �?app 进程�?kill 时通常不会回调，这里主要依�?// JNI_OnLoad 注册；后�?InjectHideActivity 的刷新会按运行快照自动清理掉
// 已经消亡的包名（�?refreshAll 中的失效检测）�?static std::string self_cached_pkg;

static std::string read_self_pkg() {
    FILE* fp = fopen("/proc/self/cmdline", "r");
    if (!fp) return {};
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return {};
    // cmdline �?\0 分隔，第一段即完整进程名（可能�?':xxx' 子进程后缀�?    std::string full(buf);
    // 取主包名：去�?':xxx' 后缀
    size_t colon = full.find(':');
    if (colon != std::string::npos) full = full.substr(0, colon);
    return full;
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        // 拿不�?env 也不影响后续逻辑
    }
    self_cached_pkg = read_self_pkg();
    if (!self_cached_pkg.empty() && KpCtl::isModuleLoaded()) {
        std::string out;
        bool ok = KpCtl::rawCtl("add_hide_pkg:" + self_cached_pkg, &out);
        __android_log_print(ANDROID_LOG_INFO, KTAG,
            "auto add_hide_pkg '%s' ok=%d resp='%s'",
            self_cached_pkg.c_str(), (int)ok, out.c_str());
    } else {
        __android_log_print(ANDROID_LOG_INFO, KTAG,
            "JNI_OnLoad: skip auto hide (pkg='%s', kpm_loaded=%d)",
            self_cached_pkg.c_str(), (int)KpCtl::isModuleLoaded());
    }
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM*, void*) {
    if (!self_cached_pkg.empty() && KpCtl::isModuleLoaded()) {
        std::string out;
        bool ok = KpCtl::rawCtl("remove_hide_pkg:" + self_cached_pkg, &out);
        __android_log_print(ANDROID_LOG_INFO, KTAG,
            "auto remove_hide_pkg '%s' ok=%d",
            self_cached_pkg.c_str(), (int)ok);
    }
}

extern "C" {

#define JNI_METHOD(ret, name) \
    JNIEXPORT ret JNICALL Java_com_example_dobbyproject_InjectHideActivity_##name

// ─── 基础能力 ─────────────────────────────────────────────
JNI_METHOD(jboolean, nativeIsModuleLoaded)(JNIEnv*, jobject) {
    return KpCtl::isModuleLoaded() ? JNI_TRUE : JNI_FALSE;
}

JNI_METHOD(void, nativeSetSuperkey)(JNIEnv* env, jobject, jstring jKey) {
    KpCtl::setSuperkey(jstr(env, jKey));
    // 设置 superkey 之后立刻尝试自动登记自身包名（此�?JNI_OnLoad 阶段
    // 多半没有 key，rawCtl 会静默失败）
    if (self_cached_pkg.empty()) self_cached_pkg = read_self_pkg();
    if (!self_cached_pkg.empty() && KpCtl::isModuleLoaded()) {
        std::string out;
        bool ok = KpCtl::rawCtl("add_hide_pkg:" + self_cached_pkg, &out);
        __android_log_print(ANDROID_LOG_INFO, KTAG,
            "setSuperkey �?auto add_hide_pkg '%s' ok=%d resp='%s'",
            self_cached_pkg.c_str(), (int)ok, out.c_str());
    }
}

JNI_METHOD(jstring, nativeRawCtl)(JNIEnv* env, jobject, jstring jCmd) {
    std::string out;
    bool ok = KpCtl::rawCtl(jstr(env, jCmd), &out);
    std::string r = (ok ? "OK: " : "FAIL: ") + out;
    return env->NewStringUTF(r.c_str());
}

// ─── proc_hide (PID �? ──────────────────────────────────
JNI_METHOD(jboolean, nativeEnableProcHide)(JNIEnv*, jobject) {
    return KpCtl::enableProcHide() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeDisableProcHide)(JNIEnv*, jobject) {
    return KpCtl::disableProcHide() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeAddHidePid)(JNIEnv*, jobject, jint pid) {
    return KpCtl::addHidePid((int)pid) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeRemoveHidePid)(JNIEnv*, jobject, jint pid) {
    return KpCtl::removeHidePid((int)pid) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeClearHidePid)(JNIEnv*, jobject) {
    return KpCtl::clearHidePid() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jstring, nativeListHidePid)(JNIEnv* env, jobject) {
    std::string out;
    KpCtl::rawCtl("list_hide_pid", &out);
    // 输出格式: "total: N\npid1\npid2\n..."
    // 我们把每�?pid 附上进程名：  "<pid>  (<cmdline>)"
    std::string result;
    size_t i = 0;
    while (i < out.size()) {
        size_t nl = out.find('\n', i);
        std::string line = out.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        i = (nl == std::string::npos) ? out.size() : nl + 1;
        if (line.empty()) continue;
        if (line.rfind("total:", 0) == 0) {
            result += line + "\n";
            continue;
        }
        // 尝试�?line 解析�?PID
        int pid = 0;
        bool ok = true;
        for (char c : line) {
            if (c >= '0' && c <= '9') { pid = pid * 10 + (c - '0'); }
            else if (c == ' ' || c == '\t' || c == '\r') { continue; }
            else { ok = false; break; }
        }
        if (ok && pid > 0) {
            result += std::to_string(pid) + "  " + proc_name_of(pid) + "\n";
        } else {
            result += line + "\n";
        }
    }
    return env->NewStringUTF(result.c_str());
}

JNI_METHOD(jstring, nativeGetStatus)(JNIEnv* env, jobject) {
    std::string out;
    bool ok = KpCtl::rawCtl("status", &out);
    if (!ok) out = "unreachable";
    return env->NewStringUTF(out.c_str());
}

// ─── file_hide (SO 关键�? ───────────────────────────────
JNI_METHOD(jboolean, nativeEnableFileHide)(JNIEnv*, jobject) {
    return KpCtl::enableFileHide() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeDisableFileHide)(JNIEnv*, jobject) {
    return KpCtl::disableFileHide() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeAddHideSo)(JNIEnv* env, jobject, jstring jName) {
    return KpCtl::addHideSo(jstr(env, jName)) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeRemoveHideSo)(JNIEnv* env, jobject, jstring jName) {
    return KpCtl::removeHideSo(jstr(env, jName)) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeClearHideSo)(JNIEnv*, jobject) {
    return KpCtl::clearHideSo() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jstring, nativeListHideSo)(JNIEnv* env, jobject) {
    std::string out;
    KpCtl::rawCtl("list_hide_so", &out);
    return env->NewStringUTF(out.c_str());
}

// ─── 包名级监控列�?(hide_pkg) ───────────────────────────
JNI_METHOD(jstring, nativeListHidePkg)(JNIEnv* env, jobject) {
    std::string out;
    KpCtl::rawCtl("list_hide_pkg", &out);
    return env->NewStringUTF(out.c_str());
}
JNI_METHOD(jboolean, nativeAddHidePkg)(JNIEnv* env, jobject, jstring jName) {
    std::string name = jstr(env, jName);
    if (name.empty()) return JNI_FALSE;
    std::string out;
    return KpCtl::rawCtl("add_hide_pkg:" + name, &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeRemoveHidePkg)(JNIEnv* env, jobject, jstring jName) {
    std::string name = jstr(env, jName);
    if (name.empty()) return JNI_FALSE;
    std::string out;
    return KpCtl::rawCtl("remove_hide_pkg:" + name, &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeClearHidePkg)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("clear_hide_pkg", &out) ? JNI_TRUE : JNI_FALSE;
}

// ─── 线程名级隐藏列表 (hide_comm) ────────────────────────
JNI_METHOD(jstring, nativeListHideComm)(JNIEnv* env, jobject) {
    std::string out;
    KpCtl::rawCtl("list_hide_comm", &out);
    return env->NewStringUTF(out.c_str());
}
JNI_METHOD(jboolean, nativeAddHideComm)(JNIEnv* env, jobject, jstring jName) {
    std::string name = jstr(env, jName);
    if (name.empty()) return JNI_FALSE;
    std::string out;
    return KpCtl::rawCtl("add_hide_comm:" + name, &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeRemoveHideComm)(JNIEnv* env, jobject, jstring jName) {
    std::string name = jstr(env, jName);
    if (name.empty()) return JNI_FALSE;
    std::string out;
    return KpCtl::rawCtl("remove_hide_comm:" + name, &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeClearHideComm)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("clear_hide_comm", &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeEnableCommHide)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("enable_comm_hide", &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeDisableCommHide)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("disable_comm_hide", &out) ? JNI_TRUE : JNI_FALSE;
}

// ─── 进程枚举 (root �?/proc，复�?SoDumper 实现) ──────
JNI_METHOD(jobjectArray, nativeListRunningApps)(JNIEnv* env, jobject, jstring jFilter) {
    std::string filter = jstr(env, jFilter);
    auto apps = SoDumper::listRunningApps(filter);
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)apps.size(), stringClass, nullptr);
    for (int i = 0; i < (int)apps.size(); i++) {
        std::string item = std::to_string(apps[i].pid) + ":" + apps[i].packageName;
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(item.c_str()));
    }
    return arr;
}

// ─── 便捷：隐�?取消隐藏自身 ──────────────────────────────
JNI_METHOD(jboolean, nativeHideSelf)(JNIEnv*, jobject) {
    return KpCtl::hideSelf() ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeUnhideSelf)(JNIEnv*, jobject) {
    return KpCtl::unhideSelf() ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
