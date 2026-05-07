#include <jni.h>
#include <string>
#include <android/log.h>

#include <fstream>
#include <vector>
#include <sys/system_properties.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#include "../../core/log/log.h"
#include "unitystart.h"
#include "../../injector/Injector.h"
#include "../../kpatch/kp_ctl.h"

struct CommandResult {
    int exitCode;
    std::string stdoutStr;
};

CommandResult run_as_root(const char* cmd) {
    CommandResult res;
    // APatch 兼容: 裸 `su -c '...'` 不加 -G/-Z, 三家都收.
    // 主要用于启动阶段调试探测 root, 业务路径已迁移到
    // MemReader::runRootShellCapture (持久 RootShell + sh 探测 -G).
    std::string full = "su -c '";
    full += cmd;
    full += "'";

    std::array<char, 128> buffer;
    std::string result;
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        res.exitCode = -1;
        res.stdoutStr = "";
        return res;
    }
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    int rc = pclose(pipe);
    res.exitCode = rc;
    res.stdoutStr = result;
    return res;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_dobbyproject_MainActivity_stringFromJNI(
        JNIEnv* env,
        jobject /* this */) {
    std::string hello = "Hello from C++";


    CommandResult res = run_as_root("ls /data");
    LOG(LOG_LEVEL_INFO,"exitCode=%08X\nstdout=%s\n",res.exitCode,res.stdoutStr.c_str());




    MyStartPointLOL();


    return env->NewStringUTF(hello.c_str());
}

extern "C" JNIEXPORT jint JNICALL
Java_com_example_dobbyproject_MainActivity_injectSoToTarget(
        JNIEnv* env,
        jobject /* this */,
        jstring packageName,
        jstring soPath) {
    const char* pkg = env->GetStringUTFChars(packageName, nullptr);
    const char* so  = env->GetStringUTFChars(soPath, nullptr);

    int ret = Injector::injectByPackageName(pkg, so);

    env->ReleaseStringUTFChars(packageName, pkg);
    env->ReleaseStringUTFChars(soPath, so);
    return ret;
}

// ─── KernelPatch superkey 校验 / 保存 ──────────────────────────────
// 由 MainActivity 在 Root 校验通过后调用：
//   nativeValidateKpKey("")          → 用文件里的 key（/data/local/tmp/.kp_key 等）试
//   nativeValidateKpKey("xxxx")      → 用用户输入的 key 试
// 返回 JNI_TRUE 表示 sc_hello 成功（key 正确）。
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_MainActivity_nativeValidateKpKey(
        JNIEnv* env, jobject /*thiz*/, jstring jKey) {
    std::string key;
    if (jKey) {
        const char* p = env->GetStringUTFChars(jKey, nullptr);
        if (p) { key = p; env->ReleaseStringUTFChars(jKey, p); }
    }
    bool ok = KpCtl::verifyKey(key);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// 探测 APatch / KernelPatch 是否安装. 不依赖 superkey, 但需要设备已 root.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_MainActivity_nativeIsApatchAvailable(
        JNIEnv* /*env*/, jclass /*clazz*/) {
    return KpCtl::isAvailable() ? JNI_TRUE : JNI_FALSE;
}

// ─── 日志运行时开关 (Java <-> C++ 双向同步) ────────────────────────
// Java UI 上的 CheckBox 与 C++ 的 g_runtimeLogEnabled 共享同一状态:
//   - Java 改动 → 调用 nativeSetLogEnabled() 同步到 C++
//   - 启动时   → Java 调 nativeIsLogEnabled() 读取 C++ 默认值, 用其初始化 UI
//   - C++ 改动 → 通过 nativeIsLogEnabled() 暴露给 Java 主动轮询/查询
extern "C" JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_MainActivity_nativeIsLogEnabled(
        JNIEnv* /*env*/, jclass /*clazz*/) {
    return g_runtimeLogEnabled ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_example_dobbyproject_MainActivity_nativeSetLogEnabled(
        JNIEnv* /*env*/, jclass /*clazz*/, jboolean enabled) {
    const bool newValue = (enabled == JNI_TRUE);
    if (g_runtimeLogEnabled != newValue) {
        g_runtimeLogEnabled = newValue;
        // 强制写一行(暂时打开输出, 以便能看到关闭/开启事件本身)
        const bool prev = g_runtimeLogEnabled;
        g_runtimeLogEnabled = true;
        LOG(LOG_LEVEL_INFO, "[LogSwitch] g_runtimeLogEnabled -> %d (Java)", newValue ? 1 : 0);
        g_runtimeLogEnabled = prev;
    }
}

// ─── Native 模拟器检测 ─────────────────────────────────────────────
// 与 Java 层 DeviceCheck 互补: 这里读 system_property/uname/proc, 比 Build.*
// 字段更难被 Java Hook 改写. 返回字符串:
//   "REAL"               → 真机
//   "EMU|<hit>|<hit>..." → 命中信号清单 (UI 端按 '|' 切分展示)
namespace {
    inline bool file_exists(const char* p) {
        struct stat st{}; return ::stat(p, &st) == 0;
    }
    inline std::string get_prop(const char* k) {
        char buf[PROP_VALUE_MAX] = {0};
        __system_property_get(k, buf);
        return std::string(buf);
    }
    inline bool contains_ic(const std::string& hay, const char* needle) {
        std::string h = hay; for (auto& c : h) c = (char)std::tolower((unsigned char)c);
        std::string n = needle; for (auto& c : n) c = (char)std::tolower((unsigned char)c);
        return h.find(n) != std::string::npos;
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_example_dobbyproject_MainActivity_nativeDetectEmulator(
        JNIEnv* env, jclass /*clazz*/) {
    std::vector<std::string> hits;

    // 1) system properties (__system_property_get, 不走 Build.*)
    struct PropRule { const char* key; const char* needle; };
    const PropRule rules[] = {
        {"ro.kernel.qemu",   "1"},
        {"ro.boot.qemu",     "1"},
        {"ro.hardware",      "ranchu"},
        {"ro.hardware",      "goldfish"},
        {"ro.hardware",      "vbox86"},
        {"ro.hardware",      "ttvm_x86"},
        {"ro.hardware",      "nox"},
        {"ro.hardware",      "ldgrt"},
        {"ro.product.cpu.abi",   "x86"},
        {"ro.product.cpu.abilist","x86"},
        {"ro.bootloader",    "unknown"},
        {"ro.bootloader",    "nox"},
        {"ro.product.name",  "sdk"},
        {"ro.product.name",  "vbox"},
        {"ro.product.model", "Emulator"},
        {"ro.product.model", "sdk_gphone"},
    };
    for (const auto& r : rules) {
        std::string v = get_prop(r.key);
        if (!v.empty() && contains_ic(v, r.needle)) {
            std::string h = "PROP "; h += r.key; h += "="; h += v;
            hits.push_back(h);
        }
    }

    // 2) uname -m, ARM 真机应是 aarch64 / armv7l, 不应是 x86_64 / i686
    {
        struct utsname u{};
        if (uname(&u) == 0) {
            std::string m = u.machine;
            if (contains_ic(m, "x86") || contains_ic(m, "i686") || contains_ic(m, "amd64")) {
                hits.push_back(std::string("UNAME machine=") + m);
            }
        }
    }

    // 3) /proc/cpuinfo 关键词
    {
        std::ifstream ifs("/proc/cpuinfo");
        std::string line, all;
        while (std::getline(ifs, line)) all += line + "\n";
        const char* kws[] = {"Intel", "AMD", "QEMU", "Hypervisor", "VirtualBox"};
        for (auto* k : kws) {
            if (contains_ic(all, k)) { hits.push_back(std::string("CPUINFO ") + k); break; }
        }
    }

    // 4) /proc/self/maps 检查 ARM 翻译层 / 模拟器特有 so
    {
        std::ifstream ifs("/proc/self/maps");
        std::string line;
        const char* sos[] = {
            "libhoudini.so", "libndk_translation.so",
            "libdroid4x.so", "libnoxd.so", "libldutils.so"
        };
        bool found[5] = {false,false,false,false,false};
        while (std::getline(ifs, line)) {
            for (size_t i = 0; i < sizeof(sos)/sizeof(sos[0]); ++i) {
                if (!found[i] && line.find(sos[i]) != std::string::npos) {
                    found[i] = true;
                    hits.push_back(std::string("MAPS ") + sos[i]);
                }
            }
        }
    }

    // 5) 模拟器特有设备节点
    const char* nodes[] = {
        "/dev/qemu_pipe",
        "/dev/socket/qemud",
        "/dev/socket/genyd",
        "/dev/socket/baseband_genyd",
        "/system/bin/nox-prop",
        "/system/bin/ldinit",
        "/data/.bluestacks.prop",
    };
    for (auto* p : nodes) {
        if (file_exists(p)) hits.push_back(std::string("FILE ") + p);
    }

    std::string out;
    if (hits.empty()) {
        out = "REAL";
    } else {
        out = "EMU";
        for (auto& h : hits) { out += '|'; out += h; }
    }
    LOG(LOG_LEVEL_INFO, "[DeviceCheck/native] %s", out.c_str());
    return env->NewStringUTF(out.c_str());
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif