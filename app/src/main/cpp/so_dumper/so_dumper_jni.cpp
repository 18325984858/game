/**
 * @file    so_dumper_jni.cpp
 * @brief   JNI 桥接层 - 将 C++ SoDumper 功能暴露给 Java
 */
#include <jni.h>
#include <string>
#include <vector>
#include "so_dumper.h"
#include "../core/log/log.h"

#define DTAG "[SoDumperJNI]"

extern "C" {

/**
 * 获取匹配的运行中应用列表
 * 返回 String[] : ["pid:packageName", ...]
 */
JNIEXPORT jobjectArray JNICALL
Java_com_example_dobbyproject_SoDumperActivity_nativeListRunningApps(
        JNIEnv* env, jobject /* this */, jstring jFilter) {

    const char* filter = env->GetStringUTFChars(jFilter, nullptr);
    std::string filterStr(filter ? filter : "");
    env->ReleaseStringUTFChars(jFilter, filter);

    auto apps = SoDumper::listRunningApps(filterStr);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)apps.size(), stringClass, nullptr);

    for (int i = 0; i < (int)apps.size(); i++) {
        std::string item = std::to_string(apps[i].pid) + ":" + apps[i].packageName;
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(item.c_str()));
    }
    return arr;
}

/**
 * 获取指定进程加载的内存映射模块列表
 * 返回 String[] : ["baseAddr:endAddr:size:fileOffset:perms:name:path", ...]
 */
JNIEXPORT jobjectArray JNICALL
Java_com_example_dobbyproject_SoDumperActivity_nativeListModules(
        JNIEnv* env, jobject /* this */, jint pid) {

    auto modules = SoDumper::listModules((int)pid);

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray((jsize)modules.size(), stringClass, nullptr);

    for (int i = 0; i < (int)modules.size(); i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "0x%lx:0x%lx:%zu:0x%lx:%s:",
                 (unsigned long)modules[i].baseAddr,
                 (unsigned long)modules[i].endAddr,
                 modules[i].size,
                 (unsigned long)modules[i].fileOffset,
                 modules[i].perms.c_str());
        std::string item = std::string(buf) + modules[i].name + ":" + modules[i].path;
        env->SetObjectArrayElement(arr, i, env->NewStringUTF(item.c_str()));
    }
    return arr;
}

/**
 * Dump 指定 SO 并修复 ELF
 * @param pid       目标进程 PID
 * @param baseAddr  模块基地址 (十六进制字符串 "0x...")
 * @param endAddr   模块结束地址
 * @param moduleName 模块名
 * @param outPath   输出文件路径
 * @return 0 成功, 负数失败
 */
JNIEXPORT jstring JNICALL
Java_com_example_dobbyproject_SoDumperActivity_nativeDumpSo(
        JNIEnv* env, jobject /* this */,
        jint pid, jlong baseAddr, jlong endAddr, jstring jModuleName, jstring jOutPath) {

    const char* moduleName = env->GetStringUTFChars(jModuleName, nullptr);
    const char* outPath = env->GetStringUTFChars(jOutPath, nullptr);

    SoDumper::ModuleInfo mod;
    mod.name = moduleName ? moduleName : "";
    mod.path = "";
    mod.baseAddr = (uintptr_t)baseAddr;
    mod.endAddr = (uintptr_t)endAddr;
    mod.fileOffset = 0;
    mod.perms = "";
    mod.size = mod.endAddr - mod.baseAddr;

    std::string outPathStr(outPath ? outPath : "");

    env->ReleaseStringUTFChars(jModuleName, moduleName);
    env->ReleaseStringUTFChars(jOutPath, outPath);

    LOG(LOG_LEVEL_INFO, DTAG " JNI dumpSo: pid=%d, module=%s, base=0x%lx, end=0x%lx, out=%s",
                        (int)pid, mod.name.c_str(),
                        (unsigned long)mod.baseAddr, (unsigned long)mod.endAddr,
                        outPathStr.c_str());

    std::string actualPath;
    int ret = SoDumper::dumpAndFixSo((int)pid, mod, outPathStr, &actualPath);
    if (ret == 0) {
        return env->NewStringUTF(actualPath.empty() ? outPathStr.c_str() : actualPath.c_str());
    } else {
        char errBuf[32];
        snprintf(errBuf, sizeof(errBuf), "ERR:%d", ret);
        return env->NewStringUTF(errBuf);
    }
}

} // extern "C"
