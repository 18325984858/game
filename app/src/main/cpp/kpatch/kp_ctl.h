/**
 * @file    kp_ctl.h
 * @brief   KernelPatch KPM 控制接口 (reader 端调用)
 *
 * 通过 supercall 与内核模块通信。需要设备已 root 并已加载对应 KPM。
 *
 * 注意: 内部协议字符串 (MODULE_NAME / "add_hide_pid:" 等) 由内核侧定义,
 *       不能在用户态重命名。本头文件只对 C++ 命名空间和日志 tag 做了
 *       中性化处理, 减少静态扫描时直接命中关键词的概率。
 */
#ifndef KP_CTL_H
#define KP_CTL_H

#include <string>
#include <vector>

namespace KpCtl {

    // ─── 基础开关 ────────────────────────────────────────
    bool enableProcHide();
    bool disableProcHide();

    // ─── PID 列表管理 ────────────────────────────────────
    bool addHidePid(int pid);
    bool addHidePids(const std::vector<int>& pids);
    bool removeHidePid(int pid);
    bool clearHidePid();

    // ─── SO 关键词隐藏（可选）───────────────────────────
    bool enableFileHide();
    bool disableFileHide();
    bool addHideSo(const std::string& name);
    bool addHideSos(const std::vector<std::string>& names);
    bool removeHideSo(const std::string& name);
    bool clearHideSo();

    // ─── 便捷函数 ────────────────────────────────────────
    bool hideSelf();
    bool unhideSelf();
    bool isModuleLoaded();
    bool rawCtl(const std::string& cmd, std::string* out = nullptr);

    void setSuperkey(const std::string& key);
    bool verifyKey(const std::string& key);

    /**
     * 订阅“KPM 已可用”事件。
     *
     * 背景: libdobbyproject.so 被 Java VM 加载时 (JNI_OnLoad), 用户还没
     * 输入 superkey, sc_hello 必然失败。后续 Java 层调用 verifyKey() 成功
     * 后, kp_ctl 内部会将 g_kp_ready 置 1, 此时会回调这里注册的所有 hook,
     * 让调用方 (如 kpm_inject_hide_jni) 补做 JNI_OnLoad 阶段未能完成的
     * 初始化 (add_hide_pkg / add_exempt_self 等)。
     *
     * 幂等: 同一 hook 反复注册会追加多份; 调用方负责在自己侧防护幂等。
     * 线程: 回调在调用 verifyKey() 的线程上同步执行。
     */
    using ReadyHook = void (*)();
    void onReady(ReadyHook fn);

} // namespace KpCtl

#endif // KP_CTL_H
