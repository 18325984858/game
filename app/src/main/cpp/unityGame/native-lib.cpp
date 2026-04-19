#include <jni.h>
#include <string>
#include <android/log.h>

#include "../Log/log.h"
#include "unitystart.h"
#include "../Injector/Injector.h"
#include "../ReadProcessMemory/kp_ctl.h"

struct CommandResult {
    int exitCode;
    std::string stdoutStr;
};

CommandResult run_as_root(const char* cmd) {
    CommandResult res;
    // 使用 su -c 'cmd'
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

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif