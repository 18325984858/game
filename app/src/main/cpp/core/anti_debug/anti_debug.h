#pragma once

// 启动一个后台线程, 周期性检测调试器/Frida/Xposed 等痕迹, 命中后 abort().
// 由 CMake 选项 ENABLE_ANTI_DEBUG 控制是否编译进来 (默认 ON).
// 推荐在 JNI_OnLoad 调用一次. 重复调用是幂等的.
void StartAntiDebugWatcher();
