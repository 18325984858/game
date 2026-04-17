/**
 * @file    inject_hide_ctl.cpp
 * @brief   直接通过 KernelPatch 的 SUPERCALL_KPM_CONTROL 与 inject-hide KPM 对话。
 *
 *  唯一稳定的控制通道（新版 APatch 不再提供 `kpatch` CLI，只能走 syscall）。
 *
 *  superkey 来源优先级：
 *    1) InjectHideCtl::setSuperkey() 显式注入
 *    2) /data/local/tmp/.kp_key        (adb shell 写入，重启不丢)
 *    3) /sdcard/kpkey.txt              (文件管理器放置)
 *    4) /data/adb/kp/superkey          (老版 KernelPatch)
 *    5) 回退到 "su"（仅当 app uid 已在 APatch su 白名单）
 *
 *  注意：APatch 默认 `skip_store_super_key=1`，不会把 key 落盘，
 *  之前解析 /data/adb/ap/log/*.log 里 `truncate <key> event ...` 拿到的
 *  只是首次安装期写入的历史 key，运行时 key 多半已被换掉。
 */
#include "inject_hide_ctl.h"
#include "../Log/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <unistd.h>
#include <sys/syscall.h>
#include <android/log.h>
#include <string>
#include <vector>

#define HTAG "[InjectHideCtl]"
#define MODULE_NAME "kpm-inject-hide"

// ──────── KernelPatch supercall 常量（来自 kernel/patch/include/uapi/scdefs.h） ────────
#define KP_NR_SUPERCALL          45
#define KP_SUPERCALL_HELLO       0x1000
#define KP_SUPERCALL_HELLO_MAGIC 0x11581158
#define KP_SUPERCALL_KPM_CONTROL 0x1022

// ver_and_cmd: 上 32 位 KernelPatch userland 版本（kernel 侧暂未校验）,
// 中间 16 位固定 0x1158，低 16 位是命令号。
static inline long ver_and_cmd(long cmd) {
    uint32_t vc = (0u << 16) | (10u << 8) | 5u;  // 任意，内核不校验
    return ((long)vc << 32) | ((long)0x1158 << 16) | (cmd & 0xFFFF);
}

namespace {

// 缓存 superkey（探测到后常驻）
static std::string g_superkey;
// 是否已通过 sc_hello 探测到 KernelPatch 存在
static int g_kp_ready = -1;

// 读取一段 root shell 命令输出
static std::string popen_su(const std::string& cmd) {
    std::string full = "su -c '" + cmd + "' 2>/dev/null";
    FILE* fp = popen(full.c_str(), "r");
    if (!fp) return "";
    char buf[512];
    std::string acc;
    while (fgets(buf, sizeof(buf), fp)) acc += buf;
    pclose(fp);
    return acc;
}

// 从 APatch 初始化日志里提取 superkey —— 已废弃。
// APatch 默认 `skip_store_super_key=1`，日志中的 key 多为首次安装期写入的历史 key，
// 与运行时内核里的 superkey 大概率已经不一致，XOR 校验会失败，
// 表现为 sc_hello 返回 -1 (ENOENT，因为 hook 未命中，真 truncate 被执行)。
// 所以不再尝试从日志探测。

// 尝试从文件里读 superkey。优先用户/adb 可控的路径。
static std::string detect_superkey_from_files() {
    static const char* files[] = {
        "/data/local/tmp/.kp_key",   // adb shell 可写，重启不丢
        "/sdcard/kpkey.txt",         // 文件管理器放置
        "/data/adb/kp/superkey",     // 老版 KernelPatch
        "/data/adb/ap/superkey",
        "/data/adb/.superkey",
        nullptr,
    };
    for (int i = 0; files[i]; i++) {
        std::string s = popen_su(std::string("cat ") + files[i]);
        // 去掉末尾换行空白
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                              s.back() == ' '  || s.back() == '\t'))
            s.pop_back();
        if (!s.empty() && s.size() <= 64) {
            __android_log_print(ANDROID_LOG_INFO, HTAG,
                "superkey from %s (len=%zu)", files[i], s.size());
            return s;
        }
    }
    return "";
}

// 获取可用的 key：优先缓存 → 文件 → 回退 "su"
static const std::string& get_key() {
    if (!g_superkey.empty()) return g_superkey;
    std::string k = detect_superkey_from_files();
    if (k.empty()) {
        __android_log_print(ANDROID_LOG_WARN, HTAG,
            "未能获取 superkey（APatch 默认不存 key）。"
            "请调用 InjectHideCtl::setSuperkey() 或把 key 写入 "
            "/data/local/tmp/.kp_key；回退使用 \"su\"（需 app uid 在 APatch su 白名单）");
        k = "su";
    }
    g_superkey = std::move(k);
    return g_superkey;
}

// sc_hello: 确认 KernelPatch 已安装且 key 有效
static bool sc_hello_ok() {
    const std::string& key = get_key();
    long ret = syscall(KP_NR_SUPERCALL, key.c_str(), ver_and_cmd(KP_SUPERCALL_HELLO));
    __android_log_print(ANDROID_LOG_INFO, HTAG,
        "sc_hello ret=0x%lx (expect 0x%x) key_len=%zu",
        (long)ret, (unsigned)KP_SUPERCALL_HELLO_MAGIC, key.size());
    return ret == (long)KP_SUPERCALL_HELLO_MAGIC;
}

// 发送一条 KPM control 命令
static bool sc_kpm_ctl(const std::string& cmd, std::string* out) {
    const std::string& key = get_key();
    char resp[256] = {0};
    long ret = syscall(KP_NR_SUPERCALL, 
                       key.c_str(),
                       ver_and_cmd(KP_SUPERCALL_KPM_CONTROL),
                       MODULE_NAME,
                       cmd.c_str(),
                       resp,
                       (long)sizeof(resp));
    if (out) {
        resp[sizeof(resp) - 1] = '\0';
        *out = resp;
    }
    __android_log_print(ANDROID_LOG_INFO, HTAG,
        "ctl <- %s | ret=%ld out=%s",
        cmd.c_str(), ret, resp);
    return ret == 0;
}

static std::string join_int(const std::vector<int>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ",";
        s += std::to_string(v[i]);
    }
    return s;
}

static std::string join_str(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) { if (i) s += ","; s += v[i]; }
    return s;
}

} // anonymous namespace


namespace InjectHideCtl {

bool isModuleLoaded() {
    if (g_kp_ready < 0) g_kp_ready = sc_hello_ok() ? 1 : 0;
    if (!g_kp_ready) {
        __android_log_print(ANDROID_LOG_WARN, HTAG,
            "KernelPatch 未就绪 (sc_hello 失败 — 可能未 root 或未授权当前 app)");
        return false;
    }
    std::string out;
    bool ok = sc_kpm_ctl("list_hide_pid", &out);
    // 模块已加载时会返回 "total: N\n..." 字符串
    bool loaded = ok && out.find("total:") != std::string::npos;
    __android_log_print(ANDROID_LOG_INFO, HTAG,
        "isModuleLoaded loaded=%d out='%s'",
        (int)loaded, out.c_str());
    return loaded;
}

bool rawCtl(const std::string& cmd, std::string* out) {
    return sc_kpm_ctl(cmd, out);
}

bool enableProcHide()  { return sc_kpm_ctl("enable_proc_hide",  nullptr); }
bool disableProcHide() { return sc_kpm_ctl("disable_proc_hide", nullptr); }

bool addHidePid(int pid) {
    if (pid <= 0) return false;
    return sc_kpm_ctl("add_hide_pid:" + std::to_string(pid), nullptr);
}
bool addHidePids(const std::vector<int>& pids) {
    if (pids.empty()) return true;
    return sc_kpm_ctl("add_hide_pid:" + join_int(pids), nullptr);
}
bool removeHidePid(int pid) {
    if (pid <= 0) return false;
    return sc_kpm_ctl("remove_hide_pid:" + std::to_string(pid), nullptr);
}
bool clearHidePid() { return sc_kpm_ctl("clear_hide_pid", nullptr); }

bool enableFileHide()  { return sc_kpm_ctl("enable_file_hide",  nullptr); }
bool disableFileHide() { return sc_kpm_ctl("disable_file_hide", nullptr); }

bool addHideSo(const std::string& name) {
    if (name.empty()) return false;
    return sc_kpm_ctl("add_hide_so:" + name, nullptr);
}
bool addHideSos(const std::vector<std::string>& names) {
    if (names.empty()) return true;
    return sc_kpm_ctl("add_hide_so:" + join_str(names), nullptr);
}
bool removeHideSo(const std::string& name) {
    if (name.empty()) return false;
    return sc_kpm_ctl("remove_hide_so:" + name, nullptr);
}
bool clearHideSo() { return sc_kpm_ctl("clear_hide_so", nullptr); }

bool hideSelf() {
    int pid = (int)getpid();
    bool ok1 = addHidePid(pid);
    bool ok2 = enableProcHide();
    __android_log_print(ANDROID_LOG_INFO, HTAG,
        "hideSelf pid=%d addPid=%d enable=%d", pid, ok1, ok2);
    return ok1 && ok2;
}

bool unhideSelf() {
    int pid = (int)getpid();
    bool ok = removeHidePid(pid);
    std::string out;
    if (sc_kpm_ctl("list_hide_pid", &out)) {
        if (out.find("total: 0") != std::string::npos) {
            disableProcHide();
        }
    }
    return ok;
}

void setSuperkey(const std::string& key) {
    g_superkey = key;
    g_kp_ready = -1;   // 让下次 isModuleLoaded() 重新跑 sc_hello
    __android_log_print(ANDROID_LOG_INFO, HTAG,
        "setSuperkey len=%zu (cached)", key.size());
}

bool verifyKey(const std::string& key) {
    // 空串: 清缓存，走文件探测；非空: 直接用该 key
    g_superkey.clear();
    g_kp_ready = -1;
    if (!key.empty()) g_superkey = key;
    bool ok = sc_hello_ok();
    if (ok) {
        g_kp_ready = 1;
    } else {
        // 验证失败：清掉缓存避免污染后续流程
        g_superkey.clear();
        g_kp_ready = 0;
    }
    return ok;
}

} // namespace InjectHideCtl
