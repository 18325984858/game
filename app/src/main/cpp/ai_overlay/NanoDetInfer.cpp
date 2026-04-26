// NanoDetInfer.cpp - NanoDet-Plus 推理 + GFL 解码 + NMS
// 参考: https://github.com/RangiLyu/nanodet 的 ncnn-android demo
#include "NanoDetInfer.h"
#include "../core/log/log.h"

#include <net.h>
#include <mat.h>
#include <cpu.h>
#include <gpu.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ai_overlay {

struct GridStride { int gx, gy, stride; };

static inline float fastExp(float x) { return std::exp(x); }

static void softmax(const float* in, float* out, int n) {
    float m = in[0];
    for (int i = 1; i < n; ++i) if (in[i] > m) m = in[i];
    float sum = 0.f;
    for (int i = 0; i < n; ++i) { out[i] = fastExp(in[i] - m); sum += out[i]; }
    if (sum > 0.f) for (int i = 0; i < n; ++i) out[i] /= sum;
}

static float iou(const DetectionBox& a, const DetectionBox& b) {
    float xx1 = std::max(a.x, b.x);
    float yy1 = std::max(a.y, b.y);
    float xx2 = std::min(a.x + a.w, b.x + b.w);
    float yy2 = std::min(a.y + a.h, b.y + b.h);
    float w = std::max(0.f, xx2 - xx1);
    float h = std::max(0.f, yy2 - yy1);
    float inter = w * h;
    float uni = a.w * a.h + b.w * b.h - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

static void nms(std::vector<DetectionBox>& boxes, float iouThr) {
    std::sort(boxes.begin(), boxes.end(),
              [](const DetectionBox& A, const DetectionBox& B) { return A.score > B.score; });
    std::vector<DetectionBox> out;
    out.reserve(boxes.size());
    std::vector<bool> dead(boxes.size(), false);
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (dead[i]) continue;
        out.push_back(boxes[i]);
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            if (dead[j]) continue;
            if (boxes[i].classId != boxes[j].classId) continue;
            if (iou(boxes[i], boxes[j]) > iouThr) dead[j] = true;
        }
    }
    boxes.swap(out);
}

NanoDetInfer::NanoDetInfer()
    : m_net(std::make_unique<ncnn::Net>()) {}

NanoDetInfer::~NanoDetInfer() = default;

bool NanoDetInfer::load(const std::string& paramPath,
                        const std::string& binPath,
                        bool useGpu) {
    m_loaded = false;
    m_net->clear();

    m_net->opt.num_threads = std::max(2, ncnn::get_big_cpu_count());
    m_net->opt.use_packing_layout = true;
    m_net->opt.use_fp16_arithmetic = true;
    m_net->opt.use_fp16_storage = true;
    m_net->opt.use_fp16_packed = true;

    if (useGpu) {
#if NCNN_VULKAN
        if (ncnn::get_gpu_count() > 0) {
            m_net->opt.use_vulkan_compute = true;
            LOG(LOG_LEVEL_INFO, "[AI/NanoDet] Vulkan enabled (gpu_count=%d)", ncnn::get_gpu_count());
        } else {
            LOG(LOG_LEVEL_WARN, "[AI/NanoDet] Vulkan unavailable, fallback CPU");
        }
#endif
    }

    if (m_net->load_param(paramPath.c_str()) != 0) {
        LOG(LOG_LEVEL_ERROR, "[AI/NanoDet] load_param failed: %s", paramPath.c_str());
        return false;
    }
    if (m_net->load_model(binPath.c_str()) != 0) {
        LOG(LOG_LEVEL_ERROR, "[AI/NanoDet] load_model failed: %s", binPath.c_str());
        return false;
    }

    m_loaded = true;
    LOG(LOG_LEVEL_INFO, "[AI/NanoDet] model loaded ok inputSize=%d numClasses=%d",
        m_inputSize, m_numClasses);
    return true;
}

bool NanoDetInfer::detect(const uint8_t* rgba, int srcW, int srcH,
                          std::vector<DetectionBox>& out,
                          float scoreThr, int classFilter) {
    out.clear();
    if (!m_loaded || !rgba || srcW <= 0 || srcH <= 0) return false;

    // letterbox -> inputSize x inputSize
    const int target = m_inputSize;
    float scale = std::min((float)target / srcW, (float)target / srcH);
    int newW = std::round(srcW * scale);
    int newH = std::round(srcH * scale);
    int padW = target - newW;
    int padH = target - newH;
    int padLeft = padW / 2;
    int padTop  = padH / 2;

    // RGBA -> RGB resize
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(
        rgba, ncnn::Mat::PIXEL_RGBA2RGB, srcW, srcH, newW, newH);

    ncnn::Mat inPad;
    ncnn::copy_make_border(in, inPad,
                           padTop, padH - padTop,
                           padLeft, padW - padLeft,
                           ncnn::BORDER_CONSTANT, 0.f);

    const float meanVals[3] = {103.53f, 116.28f, 123.675f};
    const float normVals[3] = {1.f / 57.375f, 1.f / 57.12f, 1.f / 58.395f};
    inPad.substract_mean_normalize(meanVals, normVals);

    ncnn::Extractor ex = m_net->create_extractor();
    ex.input("input.1", inPad);

    std::vector<DetectionBox> raw;
    raw.reserve(128);
    std::vector<float> dis(m_regMax + 1);

    // nanodet-m: 每个 stride 有 cls_pred_stride_X (80 channels) 和 dis_pred_stride_X (32 channels)
    // Permute 后 shape: cls = [num_anchors, 80], dis = [num_anchors, 32]
    for (int s : m_strides) {
        char clsName[64], disName[64];
        snprintf(clsName, sizeof(clsName), "cls_pred_stride_%d", s);
        snprintf(disName, sizeof(disName), "dis_pred_stride_%d", s);
        ncnn::Mat clsMat, disMat;
        if (ex.extract(clsName, clsMat) != 0 ||
            ex.extract(disName, disMat) != 0) {
            LOG(LOG_LEVEL_WARN, "[AI/NanoDet] extract %s/%s failed", clsName, disName);
            return false;
        }
        const int featW = target / s;
        const int featH = target / s;
        const int numAnc = featW * featH;
        if (clsMat.w != m_numClasses || disMat.w != 4 * (m_regMax + 1) ||
            clsMat.h != numAnc || disMat.h != numAnc) {
            LOG(LOG_LEVEL_WARN, "[AI/NanoDet] stride=%d shape mismatch cls=%dx%d dis=%dx%d expect anchors=%d",
                s, clsMat.w, clsMat.h, disMat.w, disMat.h, numAnc);
            return false;
        }
        for (int gy = 0; gy < featH; ++gy) {
            for (int gx = 0; gx < featW; ++gx) {
                int idx = gy * featW + gx;
                const float* clsRow = clsMat.row(idx);
                int bestC = -1;
                float bestS = 0.f;
                for (int c = 0; c < m_numClasses; ++c) {
                    float v = clsRow[c];
                    if (v > bestS) { bestS = v; bestC = c; }
                }
                if (bestS < scoreThr) continue;
                if (classFilter >= 0 && bestC != classFilter) continue;

                const float* disRow = disMat.row(idx);
                float pred[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    softmax(disRow + k * (m_regMax + 1), dis.data(), m_regMax + 1);
                    float v = 0.f;
                    for (int j = 0; j <= m_regMax; ++j) v += dis[j] * j;
                    pred[k] = v * s;
                }
                float cx = (gx + 0.5f) * s;
                float cy = (gy + 0.5f) * s;
                float x0 = cx - pred[0];
                float y0 = cy - pred[1];
                float x1 = cx + pred[2];
                float y1 = cy + pred[3];
                x0 = (x0 - padLeft) / scale;
                y0 = (y0 - padTop)  / scale;
                x1 = (x1 - padLeft) / scale;
                y1 = (y1 - padTop)  / scale;
                x0 = std::max(0.f, std::min((float)srcW, x0));
                y0 = std::max(0.f, std::min((float)srcH, y0));
                x1 = std::max(0.f, std::min((float)srcW, x1));
                y1 = std::max(0.f, std::min((float)srcH, y1));
                if (x1 <= x0 || y1 <= y0) continue;

                DetectionBox b{};
                b.x = x0; b.y = y0;
                b.w = x1 - x0; b.h = y1 - y0;
                b.score = bestS;
                b.classId = bestC;
                raw.push_back(b);
            }
        }
    }

    nms(raw, 0.5f);
    out.swap(raw);
    return true;
}

} // namespace ai_overlay
