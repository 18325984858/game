/**
 * @file    inject_hide_ctl.h
 * @brief   inject-hide KPM 控制接口 (reader 端调用)
 *
 * 通过 `kpatch ctl kpm-inject-hide "<cmd>"` 与内核模块通信。
 * 需要设备已 root 并已加载 inject-hide.kpm。
 *
 * 典型用法 (在 app 启动时调用一次即可):
 *   InjectHideCtl::hideSelf();            // 把自己 pid 加入列表并启用 proc_hide
 *
 * 结束时 (可选):
 *   InjectHideCtl::unhideSelf();          // 从列表移除并关闭 proc_hide
 */
#ifndef INJECT_HIDE_CTL_H
#define INJECT_HIDE_CTL_H

#include <string>
#include <vector>

namespace InjectHideCtl {

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

    /**
     * 一键隐身：
     *   1) add_hide_pid:<getpid()>
     *   2) enable_proc_hide
     * 如果此前已经启用过，会是幂等的 (add 返回已存在/enable 重复置位均不失败)。
     * @return 两步都成功返回 true
     */
    bool hideSelf();

    /**
     * 一键取消：
     *   1) remove_hide_pid:<getpid()>
     *   2) 若移除后列表为空，disable_proc_hide
     */
    bool unhideSelf();

    /**
     * 查询 inject-hide 模块是否已加载。
     */
    bool isModuleLoaded();

    /**
     * 原始命令通道 (供高级用户直接下命令)。
     * @param cmd    例如 "list_hide_pid"
     * @param out    模块返回的字符串 (可为 nullptr)
     * @return 命令投递成功返回 true
     */
    bool rawCtl(const std::string& cmd, std::string* out = nullptr);

    /**
     * 手动提供 APatch / KernelPatch 的 superkey。
     *
     * APatch 默认 `skip_store_super_key=1` 不会把 key 落盘，
     * 因此没法自动探测 —— 调用方需要在 app 初始化时把 key 喂进来。
     * 如果没调用本函数，下列文件也会被尝试读取（按顺序）：
     *   /data/local/tmp/.kp_key       (adb shell 可写；重启不丢)
     *   /sdcard/kpkey.txt             (用户用文件管理器放)
     *   /data/adb/kp/superkey
     * 都没有时回退到 "su"（仅当本 app uid 在 APatch su 白名单里才有效）。
     *
     * @param key  明文 superkey。为空字符串表示清除缓存、强制重新探测。
     */
    void setSuperkey(const std::string& key);

    /**
     * 用指定 key 尝试一次 sc_hello，仅做密钥鉴权验证，不管模块是否加载。
     * 成功时，key 会被缓存为当前 superkey。
     *
     * @param key  明文 superkey；空串表示用当前缓存 / 已探测文件 key 再试一次。
     * @return     true = key 正确（sc_hello 返回 0x11581158）
     */
    bool verifyKey(const std::string& key);

} // namespace InjectHideCtl

#endif // INJECT_HIDE_CTL_H
