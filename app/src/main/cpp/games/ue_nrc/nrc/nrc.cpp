#include "nrc.h"
#include "../../../core/log/log.h"

namespace nrc {

// =====================================================================
//  SharedNrcData
// =====================================================================
SharedNrcData& SharedNrcData::getInstance() {
    static SharedNrcData inst;
    return inst;
}

void SharedNrcData::pushData(const DrawNrcData& d) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_data = d;
    m_lastPush = std::chrono::steady_clock::now();
    m_hasData = true;
}

void SharedNrcData::getData(DrawNrcData& out) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    out = m_data;
}

int64_t SharedNrcData::getMsSinceLastPush() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_hasData) return -1;
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastPush).count();
}

// =====================================================================
//  NrcMatchMonitor (占位 — 仅推送空帧维持 GUI 心跳)
// =====================================================================
NrcMatchMonitor::NrcMatchMonitor(uintptr_t moduleBase, uintptr_t moduleSize,
                                 uint32_t offNamePool,
                                 uint32_t offGUObjectArrayNum,
                                 uint32_t offGUObjectArrayChunks,
                                 uint32_t offGWorld)
    : m_moduleBase(moduleBase)
    , m_moduleSize(moduleSize)
    , m_offNamePool(offNamePool)
    , m_offGUObjectArrayNum(offGUObjectArrayNum)
    , m_offGUObjectArrayChunks(offGUObjectArrayChunks)
    , m_offGWorld(offGWorld)
{
}

NrcMatchMonitor::~NrcMatchMonitor() {
    stop();
}

bool NrcMatchMonitor::start() {
    bool expected = false;
    if (!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    m_thread = std::thread(&NrcMatchMonitor::threadMain, this);
    return true;
}

void NrcMatchMonitor::stop() {
    m_running.store(false, std::memory_order_release);
    if (m_thread.joinable()) m_thread.join();
}

void NrcMatchMonitor::threadMain() {
    LOG(LOG_LEVEL_INFO, "[NrcMatchMonitor] 占位线程启动 (本版本未适配实战字段)");
    while (m_running.load(std::memory_order_acquire)) {
        DrawNrcData empty;
        empty.worldName = "(尚未适配)";
        SharedNrcData::getInstance().pushData(empty);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LOG(LOG_LEVEL_INFO, "[NrcMatchMonitor] 占位线程退出");
}

} // namespace nrc

OBFU_ATTRS_END
