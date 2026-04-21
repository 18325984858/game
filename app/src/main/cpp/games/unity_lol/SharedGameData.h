/**
 * @file    SharedGameData.h
 * @brief   线程安全的游戏数据桥接器 —— 在数据采集线程和 ImGui 渲染线程之间共享数据
 * @author  Song
 * @date    2026/03/12
 *
 * @details 单例模式。数据���集线程调用 pushData() 写入最新快照，
 *          ImGui 渲染线程调用 pullData() 获取最新快照并绘制覆盖层。
 */

#ifndef SHARED_GAME_DATA_H
#define SHARED_GAME_DATA_H

#include <condition_variable>
#include <chrono>
#include <mutex>
#include <atomic>
#include "lol/lolm.h"

class SharedGameData {
public:
    static SharedGameData& getInstance() {
        static SharedGameData instance;
        return instance;
    }

    SharedGameData(const SharedGameData&) = delete;
    SharedGameData& operator=(const SharedGameData&) = delete;

    /**
     * @brief 由数据采集线程调用 —— 写入最新的小地图数据快照
     * @param data 最新的 MiniMapData 快照
     */
    void pushData(const lol::MiniMapData& data) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_data = data;
        m_hasNewData.store(true, std::memory_order_release);
    }

    /**
     * @brief 由 ImGui 渲染线程调用 —— 拉取最新数据（消费型：拉取后标记为已读）
     * @param outData [传出] 最新的 MiniMapData
     * @return true=有新数据, false=��更新
     */
    bool pullData(lol::MiniMapData& outData) {
        if (!m_hasNewData.load(std::memory_order_acquire))
            return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        outData = m_data;
        m_hasNewData.store(false, std::memory_order_release);
        return true;
    }

    /** @brief 获取最新数据（非消费型：不修改 hasNewData 标志） */
    lol::MiniMapData getLatestData() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_data;
    }

    /** @brief 设置对局是否进行中 */
    void setBattleActive(bool active) {
        m_battleActive.store(active, std::memory_order_release);
    }

    /** @brief 查询对局是否进行中 */
    bool isBattleActive() const {
        return m_battleActive.load(std::memory_order_acquire);
    }

    /** @brief 设置自动清兵开关 */
    void setAutoClearMinionsEnabled(bool enabled) {
        m_autoClearMinionsEnabled.store(enabled, std::memory_order_release);
    }

    /** @brief 查询自动清兵是否开启 */
    bool isAutoClearMinionsEnabled() const {
        return m_autoClearMinionsEnabled.load(std::memory_order_acquire);
    }

    /** @brief UI 线程请求触发普攻（单次边沿请求） */
    void requestNormalAttack() {
        m_normalAttackRequestSeq.fetch_add(1, std::memory_order_acq_rel);
        m_normalAttackRequested.store(true, std::memory_order_release);
        m_attackCv.notify_one();
    }

    /** @brief 获取当前普攻请求序号 */
    uint64_t getNormalAttackRequestSeq() const {
        return m_normalAttackRequestSeq.load(std::memory_order_acquire);
    }

    /** @brief 等待普攻请求到来或超时（用于输入线程降低空转） */
    void waitForNormalAttackWork(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_attackMutex);
        m_attackCv.wait_for(lock, timeout, [&]() {
            return m_normalAttackRequested.load(std::memory_order_acquire) > 0;
        });
    }

    /** @brief 查询是否存在待处理的普攻请求 */
    bool hasNormalAttackRequest() const {
        return m_normalAttackRequested.load(std::memory_order_acquire);
    }

    /** @brief 数据线程消费当前普攻请求 */
    bool consumeNormalAttackRequest() {
        return m_normalAttackRequested.exchange(false, std::memory_order_acq_rel);
    }

private:
    SharedGameData() = default;
    ~SharedGameData() = default;

    std::mutex m_mutex;
    std::mutex m_attackMutex;
    std::condition_variable m_attackCv;
    lol::MiniMapData m_data;
    std::atomic<bool> m_hasNewData{false};
    std::atomic<bool> m_battleActive{false};
    std::atomic<bool> m_autoClearMinionsEnabled{false};
    std::atomic<bool> m_normalAttackRequested{false};
    std::atomic<uint64_t> m_normalAttackRequestSeq{0};
};

#endif // SHARED_GAME_DATA_H



