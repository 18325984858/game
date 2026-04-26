// AIDetection.h - 跨进程共享检测数据 (mmap on /data/local/tmp/ai_dets.bin)
// 关键: AIPipeline 在 dobbyproject 进程跑, UE4Draw 在 PUBG 进程跑.
// 必须用 shared mmap 才能互通, C++ 单例对每个进程都是独立实例.
#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <chrono>
#include <array>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <android/log.h>

namespace ai_overlay {

struct DetectionBox {
    float x;      // 屏幕坐标 (左上)
    float y;
    float w;
    float h;
    float score;  // 置信度 0..1
    int classId;  // 类别 ID (COCO 0=person)
};

namespace shm_detail {
constexpr uint32_t kMagic        = 0x41494435; // 'AID5'
constexpr int      kMaxBoxes     = 128;
constexpr const char* kShmPath   = "/data/local/tmp/ai_dets.bin";

struct DetSlot {
    uint32_t count;
    uint32_t pad;
    DetectionBox boxes[kMaxBoxes];
};

struct Header {
    uint32_t magic;
    uint32_t version;
    int32_t  enabled;
    int32_t  ready;
    int32_t  classFilter;
    float    scoreThreshold;
    int32_t  srcW;
    int32_t  srcH;
    int64_t  inferUs;
    int64_t  lastPushNs;
    int32_t  frontSlot;
    int32_t  pad;
    char     lastError[128];
    DetSlot  slots[2];
};
} // shm_detail

class AISharedData {
    using Clock = std::chrono::steady_clock;
public:
    static AISharedData& getInstance() {
        static AISharedData inst;
        return inst;
    }
    AISharedData(const AISharedData&) = delete;
    AISharedData& operator=(const AISharedData&) = delete;

    void pushDetections(std::vector<DetectionBox>&& dets,
                        int srcW,
                        int srcH,
                        int64_t inferUs) {
        if (!m_hdr) return;
        const int wi = 1 - __atomic_load_n(&m_hdr->frontSlot, __ATOMIC_ACQUIRE);
        auto& slot = m_hdr->slots[wi];
        const uint32_t n = static_cast<uint32_t>(std::min(
            dets.size(), static_cast<size_t>(shm_detail::kMaxBoxes)));
        slot.count = n;
        for (uint32_t i = 0; i < n; ++i) slot.boxes[i] = dets[i];
        m_hdr->srcW = srcW;
        m_hdr->srcH = srcH;
        m_hdr->inferUs = inferUs;
        m_hdr->lastPushNs = Clock::now().time_since_epoch().count();
        __atomic_store_n(&m_hdr->frontSlot, wi, __ATOMIC_RELEASE);
    }

    void getDetections(std::vector<DetectionBox>& out, int& srcW, int& srcH) const {
        out.clear();
        if (!m_hdr) { srcW = srcH = 0; return; }
        const int ri = __atomic_load_n(&m_hdr->frontSlot, __ATOMIC_ACQUIRE);
        const auto& slot = m_hdr->slots[ri];
        const uint32_t n = std::min(slot.count, (uint32_t)shm_detail::kMaxBoxes);
        out.resize(n);
        for (uint32_t i = 0; i < n; ++i) out[i] = slot.boxes[i];
        srcW = m_hdr->srcW;
        srcH = m_hdr->srcH;
    }

    int64_t lastInferUs() const { return m_hdr ? m_hdr->inferUs : 0; }

    int64_t msSinceLastPush() const {
        if (!m_hdr) return -1;
        int64_t v = m_hdr->lastPushNs;
        if (v == 0) return -1;
        auto now = Clock::now().time_since_epoch().count();
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::duration(now - v)).count();
    }

    bool enabled() const { return m_hdr && __atomic_load_n(&m_hdr->enabled, __ATOMIC_ACQUIRE) != 0; }
    void setEnabled(bool v) {
        if (!m_hdr) return;
        int32_t prev = __atomic_exchange_n(&m_hdr->enabled, v ? 1 : 0, __ATOMIC_ACQ_REL);
        if (prev != (v ? 1 : 0)) {
            extern void __ai_log_enabled_changed(bool);
            __ai_log_enabled_changed(v);
        }
    }

    float scoreThreshold() const { return m_hdr ? m_hdr->scoreThreshold : 0.4f; }
    void setScoreThreshold(float v) { if (m_hdr) m_hdr->scoreThreshold = v; }

    bool ready() const { return m_hdr && __atomic_load_n(&m_hdr->ready, __ATOMIC_ACQUIRE) != 0; }
    void setReady(bool v) { if (m_hdr) __atomic_store_n(&m_hdr->ready, v ? 1 : 0, __ATOMIC_RELEASE); }

    std::string lastError() const {
        if (!m_hdr) return {};
        return std::string(m_hdr->lastError);
    }
    void setLastError(const std::string& e) {
        if (!m_hdr) return;
        std::strncpy(m_hdr->lastError, e.c_str(), sizeof(m_hdr->lastError) - 1);
        m_hdr->lastError[sizeof(m_hdr->lastError) - 1] = 0;
    }

    int targetClassFilter() const { return m_hdr ? m_hdr->classFilter : 0; }
    void setTargetClassFilter(int v) { if (m_hdr) m_hdr->classFilter = v; }

    bool valid() const { return m_hdr != nullptr; }

private:
    AISharedData() {
        const char* path = shm_detail::kShmPath;
        const size_t sz = sizeof(shm_detail::Header);
        int fd = ::open(path, O_RDWR | O_CREAT, 0666);
        if (fd < 0) {
            __android_log_print(ANDROID_LOG_ERROR, "AISharedData",
                "open(%s) failed errno=%d (%s) pid=%d uid=%d",
                path, errno, strerror(errno), (int)getpid(), (int)getuid());
            return;
        }
        struct stat st{};
        ::fstat(fd, &st);
        bool needInit = (st.st_size < (off_t)sz);
        if (needInit) {
            if (::ftruncate(fd, (off_t)sz) != 0) {
                __android_log_print(ANDROID_LOG_ERROR, "AISharedData",
                    "ftruncate failed errno=%d (%s)", errno, strerror(errno));
            }
        }
        ::fchmod(fd, 0666);
        void* p = ::mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) {
            __android_log_print(ANDROID_LOG_ERROR, "AISharedData",
                "mmap failed errno=%d (%s) sz=%zu pid=%d uid=%d",
                errno, strerror(errno), sz, (int)getpid(), (int)getuid());
            return;
        }
        m_hdr = reinterpret_cast<shm_detail::Header*>(p);
        if (m_hdr->magic != shm_detail::kMagic) {
            std::memset(m_hdr, 0, sz);
            m_hdr->magic = shm_detail::kMagic;
            m_hdr->version = 1;
            m_hdr->scoreThreshold = 0.40f;
            m_hdr->classFilter = 0;
        }
        __android_log_print(ANDROID_LOG_INFO, "AISharedData",
            "mmap ok pid=%d uid=%d magic=0x%x sz=%zu",
            (int)getpid(), (int)getuid(), m_hdr->magic, sz);
    }
    shm_detail::Header* m_hdr = nullptr;
};

} // namespace ai_overlay
