#include <jni.h>
#include <string>
#include <android/log.h>

#include "../Log/log.h"
#include "unitystart.h"
#include "../Injector/Injector.h"

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




    MyStartPoint();


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

#ifdef OBFU_ATTRS_END
OBFU_ATTRS_END
#endif