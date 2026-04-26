// AIPipelineJni.cpp - Kotlin/Java 控制接口
#include <jni.h>
#include <string>
#include "AIPipeline.h"
#include "AIDetection.h"

using namespace ai_overlay;

extern "C" {

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeSetModelPaths(
        JNIEnv* env, jclass, jstring jParam, jstring jBin) {
    const char* p = env->GetStringUTFChars(jParam, nullptr);
    const char* b = env->GetStringUTFChars(jBin,   nullptr);
    AIPipeline::getInstance().setModelPaths(p ? p : "", b ? b : "");
    if (p) env->ReleaseStringUTFChars(jParam, p);
    if (b) env->ReleaseStringUTFChars(jBin,   b);
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeSetUseGpu(JNIEnv*, jclass, jboolean v) {
    AIPipeline::getInstance().setUseGpu(v == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeSetUseSu(JNIEnv*, jclass, jboolean v) {
    AIPipeline::getInstance().setUseSu(v == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeSetIntervalMs(JNIEnv*, jclass, jint v) {
    if (v < 33) v = 33;
    if (v > 2000) v = 2000;
    AIPipeline::getInstance().setIntervalMs(v);
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeStart(JNIEnv*, jclass) {
    AIPipeline::getInstance().start();
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeStop(JNIEnv*, jclass) {
    AIPipeline::getInstance().stop();
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeSetEnabled(JNIEnv*, jclass, jboolean v) {
    AISharedData::getInstance().setEnabled(v == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeIsRunning(JNIEnv*, jclass) {
    return AIPipeline::getInstance().isRunning() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeIsReady(JNIEnv*, jclass) {
    return AISharedData::getInstance().ready() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeGetLastError(JNIEnv* env, jclass) {
    auto s = AISharedData::getInstance().lastError();
    return env->NewStringUTF(s.c_str());
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeSetScoreThreshold(JNIEnv*, jclass, jfloat v) {
    if (v < 0.05f) v = 0.05f;
    if (v > 0.95f) v = 0.95f;
    AISharedData::getInstance().setScoreThreshold(v);
}

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeSetClassFilter(JNIEnv*, jclass, jint v) {
    AISharedData::getInstance().setTargetClassFilter(v);
}

} // extern "C"
