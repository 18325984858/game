// NanoDetInfer.h - 加载 nanodet-plus-m_320 ncnn 模型并推理
#pragma once

#include "AIDetection.h"
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

namespace ncnn { class Net; class Mat; }

namespace ai_overlay {

class NanoDetInfer {
public:
    NanoDetInfer();
    ~NanoDetInfer();
    NanoDetInfer(const NanoDetInfer&) = delete;
    NanoDetInfer& operator=(const NanoDetInfer&) = delete;

    // 加载 .param + .bin (从外部存储路径). useGpu=true 时尝试 Vulkan
    bool load(const std::string& paramPath,
              const std::string& binPath,
              bool useGpu);

    bool isLoaded() const { return m_loaded; }

    // 输入 RGBA8888 整帧, 返回检测结果(已按原图坐标缩放).
    // scoreThr  : 置信度阈值
    // classFilter: -1 表示返回所有类, 否则只返回该类
    bool detect(const uint8_t* rgba,
                int srcW,
                int srcH,
                std::vector<DetectionBox>& out,
                float scoreThr,
                int classFilter);

    int inputSize() const { return m_inputSize; }

private:
    std::unique_ptr<ncnn::Net> m_net;
    bool m_loaded = false;
    int  m_inputSize = 320;
    int  m_numClasses = 80;        // COCO
    int  m_regMax = 7;             // reg_max (输出 dis_pred 通道 = 4*(reg_max+1) = 32)
    std::vector<int> m_strides{8, 16, 32};   // nanodet-m
};

} // namespace ai_overlay
