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

#include "../kpatch/kp_ctl.h"
#include "../so_dumper/so_dumper.h"
#include "../core/log/log.h"
#include "../core/stealth/stealth_hooks.h"
#if defined(ENABLE_ANTI_DEBUG)
#include "../core/anti_debug/anti_debug.h"
#endif

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

// SO load/unload auto-register self pkg to kernel hide_pkg
static std::string self_cached_pkg;

static std::string read_self_pkg() {
    FILE* fp = fopen("/proc/self/cmdline", "r");
    if (!fp) return {};
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return {};
    // cmdline is \0 separated; take first segment as process name
    std::string full(buf);
    // strip ':xxx' subprocess suffix
    size_t colon = full.find(':');
    if (colon != std::string::npos) full = full.substr(0, colon);
    return full;
}

// KPM "首次可用" 回调: 由 KpCtl::verifyKey() 成功时同步触发 (一般是
// MainActivity 的 nativeValidateKpKey 路径). 此时 g_superkey 已被 Java 喂入,
// 可以安全地调 sc_kpm_ctl. 这里把 add_hide_pkg / add_exempt_self 这些原本
// 想在 JNI_OnLoad 里做的事 "迁移" 到这条延迟通路, 避免 SO 加载时同步 popen("su")
// 阻塞 UI 主线程, 也避免在没有 key 的情况下静默失败.
static void onKpmReady_AutoHideSelf() {
    if (self_cached_pkg.empty()) self_cached_pkg = read_self_pkg();
    if (self_cached_pkg.empty()) {
        LOG(LOG_LEVEL_WARN, KTAG " onKpmReady: self_cached_pkg empty, skip");
        return;
    }
    std::string out;
    bool ok = KpCtl::rawCtl("add_hide_pkg:" + self_cached_pkg, &out);
    LOG(LOG_LEVEL_INFO, KTAG " onKpmReady auto add_hide_pkg '%s' ok=%d resp='%s'",
        self_cached_pkg.c_str(), (int)ok, out.c_str());

    // 同时把自己加入 RootHide UID 豁免名单, 防止 root_hide 启用后自家进程被
    // 168 个 root 关键字误伤导致 su 等检测失败.
    std::string out2;
    bool ok2 = KpCtl::rawCtl("add_exempt_self", &out2);
    LOG(LOG_LEVEL_INFO, KTAG " onKpmReady auto add_exempt_self ok=%d resp='%s'",
        (int)ok2, out2.c_str());
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm && vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        // 拿不到 env 也不影响后续逻辑
    }
#if defined(ENABLE_ANTI_DEBUG)
    StartAntiDebugWatcher();
#endif
    // 安装反检测 inline hook (幂等, 仅装一次):
    //  1) hook dl_iterate_phdr / dladdr -> 从模块枚举中过滤掉 libdobbyproject / Dobby / frida 等 .so;
    //  2) hook __system_property_get / read / read_callback -> 伪造 ro.boot.verifiedbootstate=green、
    //     ro.secure=1、ro.debuggable=0、ro.build.tags=release-keys 等, 让游戏/反作弊看不到 root 与解锁迹象。
    installStealthHooks();

    // 只缓存包名, 不在这里调用 KpCtl::isModuleLoaded() —— 该函数会触发
    // sc_hello / get_key / popen("su -c cat ...") 链路, 而此时 Java 还未把
    // superkey 喂给 native, 必然失败, 还会同步阻塞 SO 加载耗时数秒.
    //
    // 真正的 "首次可用" 时机由 KpCtl::verifyKey() 成功后通过 onReady 回调
    // 通知, 见 onKpmReady_AutoHideSelf().
    self_cached_pkg = read_self_pkg();
    KpCtl::onReady(&onKpmReady_AutoHideSelf);
    LOG(LOG_LEVEL_INFO, KTAG " JNI_OnLoad: pkg='%s', deferred auto-hide via onReady",
        self_cached_pkg.c_str());

    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL JNI_OnUnload(JavaVM*, void*) {
    if (!self_cached_pkg.empty() && KpCtl::isModuleLoaded()) {
        std::string out;
        bool ok = KpCtl::rawCtl("remove_hide_pkg:" + self_cached_pkg, &out);
        LOG(LOG_LEVEL_INFO, KTAG " auto remove_hide_pkg '%s' ok=%d",
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
        LOG(LOG_LEVEL_INFO, KTAG " setSuperkey �?auto add_hide_pkg '%s' ok=%d resp='%s'",
            self_cached_pkg.c_str(), (int)ok, out.c_str());

        // 同时把自己加入 RootHide UID 豁免名单，避免 root_hide 启用后
        // 自家进程被 168 个 root 关键字误伤导致 su 等检测失败。
        // KPM 在内核态读 current->cred->uid，所以这里不用传 uid。
        std::string out2;
        bool ok2 = KpCtl::rawCtl("add_exempt_self", &out2);
        LOG(LOG_LEVEL_INFO, KTAG " setSuperkey -> auto add_exempt_self ok=%d resp='%s'",
            (int)ok2, out2.c_str());
    }
}

JNI_METHOD(jstring, nativeRawCtl)(JNIEnv* env, jobject, jstring jCmd) {
    std::string out;
    bool ok = KpCtl::rawCtl(jstr(env, jCmd), &out);
    std::string r = (ok ? "OK: " : "FAIL: ") + out;
    return env->NewStringUTF(r.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_dobbyproject_MainActivity_nativeKpmRawCtl(JNIEnv* env, jobject, jstring jCmd) {
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

// ─── Root 痕迹隐藏 (RootHide 模块) ──────────────────────────
// 对应 FrideHide-kpm/kpms/inject-hide/Root/RootHide.{h,c}
// 所有命令通过 control0 rawCtl 透传，无需新增 KpCtl 成员函数
JNI_METHOD(jboolean, nativeEnableRootHide)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("enable_root_hide", &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeDisableRootHide)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("disable_root_hide", &out) ? JNI_TRUE : JNI_FALSE;
}
// 返回 root_hide 子状态: "root_hide=0/1\nfile_hide=0/1\nroot_kw_count=N\n"
JNI_METHOD(jstring, nativeGetStatusRoot)(JNIEnv* env, jobject) {
    std::string out;
    bool ok = KpCtl::rawCtl("status_root", &out);
    if (!ok) out = "unreachable";
    return env->NewStringUTF(out.c_str());
}
JNI_METHOD(jstring, nativeListHideRoot)(JNIEnv* env, jobject) {
    std::string out;
    KpCtl::rawCtl("list_hide_root", &out);
    return env->NewStringUTF(out.c_str());
}
JNI_METHOD(jboolean, nativeAddHideRoot)(JNIEnv* env, jobject, jstring jName) {
    std::string name = jstr(env, jName);
    if (name.empty()) return JNI_FALSE;
    std::string out;
    return KpCtl::rawCtl("add_hide_root:" + name, &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeRemoveHideRoot)(JNIEnv* env, jobject, jstring jName) {
    std::string name = jstr(env, jName);
    if (name.empty()) return JNI_FALSE;
    std::string out;
    return KpCtl::rawCtl("remove_hide_root:" + name, &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeClearHideRoot)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("clear_hide_root", &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeResetHideRoot)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("reset_hide_root", &out) ? JNI_TRUE : JNI_FALSE;
}

// ─── 系统进程豁免 (sys_exempt) ─────────────────────────────
// 默认启用：UID < sys_exempt_uid_max (缺省 10000) 的调用方被 KPM
// 视为 trusted，避免 installd / system_server 等系统链路被误拦
// (例如启用 root_hide/file_hide 时 adb install/am start 卡住)。
JNI_METHOD(jboolean, nativeEnableSysExempt)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("enable_sys_exempt", &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeDisableSysExempt)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("disable_sys_exempt", &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeSetSysExemptUid)(JNIEnv*, jobject, jint uidMax) {
    std::string out;
    std::string cmd = "set_sys_exempt_uid:" + std::to_string((int)uidMax);
    return KpCtl::rawCtl(cmd, &out) ? JNI_TRUE : JNI_FALSE;
}

// ─── 日志总开关 (kpm_log) ────────────────────────────────
// 控制 KPM 里所有 klog()/klog_dbg() 的输出，klog_err 与 klog_always
// 不受影响。默认开启；生产环境可关闭以降低 dmesg 噪声。
JNI_METHOD(jboolean, nativeEnableLog)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("enable_log", &out) ? JNI_TRUE : JNI_FALSE;
}
JNI_METHOD(jboolean, nativeDisableLog)(JNIEnv*, jobject) {
    std::string out;
    return KpCtl::rawCtl("disable_log", &out) ? JNI_TRUE : JNI_FALSE;
}
// 返回 "log_enabled=<0/1>\n"
JNI_METHOD(jstring, nativeGetStatusLog)(JNIEnv* env, jobject) {
    std::string out;
    bool ok = KpCtl::rawCtl("status_log", &out);
    if (!ok) out = "unreachable";
    return env->NewStringUTF(out.c_str());
}

} // extern "C"

OBFU_ATTRS_END
