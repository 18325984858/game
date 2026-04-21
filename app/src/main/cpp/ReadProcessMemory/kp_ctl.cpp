/**
 * @file    kp_ctl.cpp
 * @brief   通过 KernelPatch SUPERCALL_KPM_CONTROL 与 KPM 对话。
 *
 *  注意: MODULE_NAME 与命令字符串 ("add_hide_pid:" 等) 是内核模块的协议,
 *  不能在用户态修改。仅命名空间 / 日志 tag / 文件名做了中性化。
 */
#include "kp_ctl.h"
#include "../Log/log.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>                                            
#include <sys/syscall.h>
#include <android/log.h>
#include <string>
#include <vector>

#define HTAG "[KCtl]"
// 内核模块名: 必须与 kpms/inject-hide/inject-hide.c 中 KPM_NAME 保持一致
#define MODULE_NAME "kpm-svc"

// ──────── KernelPatch supercall 常量 ────────
#define KP_NR_SUPERCALL          45
#define KP_SUPERCALL_HELLO       0x1000
#define KP_SUPERCALL_HELLO_MAGIC 0x11581158
#define KP_SUPERCALL_KPM_CONTROL 0x1022

static inline long ver_and_cmd(long cmd) {
    uint32_t vc = (0u << 16) | (10u << 8) | 5u;
    return ((long)vc << 32) | ((long)0x1158 << 16) | (cmd & 0xFFFF);
}

namespace {

static std::string g_superkey;
static int g_kp_ready = -1;

static std::string popen_su(const std::string& cmd) {
    // 用 timeout(1) 包裹避免 su 未授权时永久挂起.
    // 2s 足够读几个小文件, 且不会拖住 UI.
    // 注意: 这里不能加 `-g 3009` (readproc) — 因为 detect_superkey_from_files 要读
    //       /data/adb/ksu/.tmp/superkey 等 chmod 600 root:root 文件,
    //       一旦 gid 被切到 readproc 反而失去访问权限, 导致 sc_hello 一直失败.
    //       hidepid 限制只发生在跨进程读 /proc/PID/* 上, 这里读的是 /data/adb,
    //       保留默认 gid=0 即可.
    std::string full = "timeout 2 su -c '" + cmd + "' 2>/dev/null";
    FILE* fp = popen(full.c_str(), "r");
    if (!fp) return "";
    char buf[512];
    std::string acc;
    while (fgets(buf, sizeof(buf), fp)) acc += buf;
    pclose(fp);
    return acc;
}

static std::string detect_superkey_from_files() {
    static const char* files[] = {
        "/data/local/tmp/.kp_key",
        "/sdcard/kpkey.txt",
        "/data/adb/kp/superkey",
        "/data/adb/ap/superkey",
        "/data/adb/.superkey",
        nullptr,
    };
    // 一次 popen 合并读取所有候选路径, 避免 5 次串行启动 su 运行时 5x 开销.
    std::string cmd = "for f in";
    for (int i = 0; files[i]; i++) { cmd += " "; cmd += files[i]; }
    cmd += "; do if [ -f \"$f\" ]; then echo \"==$f==\"; cat \"$f\"; echo; fi; done";
    std::string s = popen_su(cmd);
    if (s.empty()) return "";
    // 按顺序取第一个非空块
    size_t pos = 0;
    while ((pos = s.find("==", pos)) != std::string::npos) {
        size_t end = s.find("==", pos + 2);
        if (end == std::string::npos) break;
        size_t lineStart = s.find('\n', end);
        if (lineStart == std::string::npos) break;
        size_t blockEnd = s.find("==", lineStart);
        if (blockEnd == std::string::npos) blockEnd = s.size();
        std::string val = s.substr(lineStart + 1, blockEnd - lineStart - 1);
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' ||
                                val.back() == ' '  || val.back() == '\t'))
            val.pop_back();
        if (!val.empty() && val.size() <= 64) {
            std::string fname = s.substr(pos + 2, end - pos - 2);
            LOG(LOG_LEVEL_INFO, HTAG " superkey from %s (len=%zu)", fname.c_str(), val.size());
            return val;
        }
        pos = blockEnd;
    }
    return "";
}

static const std::string& get_key() {
    if (!g_superkey.empty()) return g_superkey;
    std::string k = detect_superkey_from_files();
    if (k.empty()) {
        LOG(LOG_LEVEL_WARN, HTAG " no superkey, fallback to \"su\"");
        k = "su";
    }
    g_superkey = std::move(k);
    return g_superkey;
}

static bool sc_hello_ok() {
    const std::string& key = get_key();
    long ret = syscall(KP_NR_SUPERCALL, key.c_str(), ver_and_cmd(KP_SUPERCALL_HELLO));
    LOG(LOG_LEVEL_INFO, HTAG " sc_hello ret=0x%lx (expect 0x%x) key_len=%zu",
        (long)ret, (unsigned)KP_SUPERCALL_HELLO_MAGIC, key.size());
    return ret == (long)KP_SUPERCALL_HELLO_MAGIC;
}

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
    LOG(LOG_LEVEL_INFO, HTAG " ctl <- %s | ret=%ld out=%s",
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


namespace KpCtl {

bool isModuleLoaded() {
    if (g_kp_ready < 0) g_kp_ready = sc_hello_ok() ? 1 : 0;
    if (!g_kp_ready) {
        LOG(LOG_LEVEL_WARN, HTAG " kp not ready (sc_hello failed)");
        return false;
    }
    std::string out;
    bool ok = sc_kpm_ctl("list_hide_pid", &out);
    bool loaded = ok && out.find("total:") != std::string::npos;
    LOG(LOG_LEVEL_INFO, HTAG " isModuleLoaded loaded=%d out='%s'",
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
    LOG(LOG_LEVEL_INFO, HTAG " hideSelf pid=%d addPid=%d enable=%d", pid, ok1, ok2);
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
    g_kp_ready = -1;
    LOG(LOG_LEVEL_INFO, HTAG " setSuperkey len=%zu (cached)", key.size());
}

bool verifyKey(const std::string& key) {
    g_superkey.clear();
    g_kp_ready = -1;
    if (!key.empty()) g_superkey = key;
    bool ok = sc_hello_ok();
    if (ok) {
        g_kp_ready = 1;
    } else {
        g_superkey.clear();
        g_kp_ready = 0;
    }
    return ok;
}

} // namespace KpCtl
