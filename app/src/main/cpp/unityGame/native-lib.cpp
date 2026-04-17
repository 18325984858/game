#include <jni.h>
#include <string>
#include <android/log.h>

#include "../Log/log.h"
#include "unitystart.h"
#include "../Injector/Injector.h"
#include "../ReadProcessMemory/inject_hide_ctl.h"

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
    bool ok = InjectHideCtl::verifyKey(key);
    return ok ? JNI_TRUE : JNI_FALSE;
}

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif