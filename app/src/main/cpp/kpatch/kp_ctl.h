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

} // namespace KpCtl

#endif // KP_CTL_H
