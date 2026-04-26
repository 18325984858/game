// AimAssistJni.cpp
#include <jni.h>
#include "AimAssist.h"
using namespace ai_overlay;

extern "C" {

JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeAimSetEnabled(JNIEnv*, jclass, jboolean v) {
    AimAssist::getInstance().setEnabled(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeAimSetVisualOnly(JNIEnv*, jclass, jboolean v) {
    AimAssist::getInstance().setVisualOnly(v == JNI_TRUE);
}
JNIEXPORT void JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeAimSetTrigger(JNIEnv*, jclass, jboolean v) {
    AimAssist::getInstance().setTrigger(v == JNI_TRUE);
}
JNIEXPORT jboolean JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeAimInjectorReady(JNIEnv*, jclass) {
    return AimAssist::getInstance().injectorReady() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jstring JNICALL
Java_com_example_dobbyproject_AIScreenDetect_nativeAimInjectorError(JNIEnv* env, jclass) {
    auto s = AimAssist::getInstance().injectorError();
    return env->NewStringUTF(s.c_str());
}

} // extern "C"
