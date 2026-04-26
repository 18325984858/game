// ScreenCapture.h - root 模式下用 /system/bin/screencap 截屏
#pragma once
#include <cstdint>
#include <vector>

namespace ai_overlay {

struct CaptureFrame {
    int width = 0;
    int height = 0;
    int stride = 0;          // 每行字节数 (== width*4 for RGBA_8888)
    int format = 0;          // 1 = RGBA_8888 (Android PixelFormat)
    std::vector<uint8_t> pixels;  // RGBA8888
};

// 调用 `/system/bin/screencap` (Android 服务自带), 返回 RAW 帧.
// screencap 二进制 RAW 头: 4*int32 = (width, height, format, ...) 然后 pixel data
// 参考: frameworks/base/cmds/screencap/screencap.cpp
// 该函数 fork+exec 子进程, 通过 pipe 读取输出, 返回 false 表示失败.
// suMode=true 时通过 `su -c screencap` 执行(部分设备需要 root 才能截 SurfaceFlinger).
bool captureScreen(CaptureFrame& out, bool suMode = false);

} // namespace ai_overlay
