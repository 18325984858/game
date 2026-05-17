package com.example.dobbyproject;

import android.util.Log;

/**
 * Root 痕迹隐藏的统一编排器: 一次性把所有相关层 enable/disable, 让"开"和"关"
 * 永远走同一份代码, 避免散落在 toggleRootHide / doUninstallKpmSvcOnly /
 * RootHideGuard.doRestore 三处行为不一致。
 *
 * 层次 (apply 顺序 == 用户态依赖, restore 顺序反过来):
 *   ① KPM 内核 root_hide       (enable_root_hide / disable_root_hide)
 *   ② KPM hide_pkg 加入根管理器 (add_hide_pkg : / remove_hide_pkg :)
 *   ③ SpoofProps              (resetprop verifiedboot / oem_unlock / debuggable...)
 *   ④ ApatchHide              (pm hide / pm unhide root 管理器包名)
 *   ⑤ 脏标记                  (写 /data/local/tmp/dobby_roothide.dirty 让
 *                              下次启动 RootHideGuard 能识别 "未干净退出")
 *
 * 调用点:
 *   - InjectHideActivity.toggleRootHide → applyAll() / restoreAll()
 *   - InjectHideActivity.doUninstallKpmSvcOnly / doCascadeUninstall →
 *     restoreAll() 先跑, 再 nativeKpmUnload (否则 KPM 卸了之后 hide_pkg /
 *     root_hide 的 raw ctl 都没法发了, 用户态会留下不一致状态)
 *   - RootHideGuard.doRestore → 异常守护路径直接调 restoreAll()
 *
 * 所有内部步骤独立 try/catch, 任一步骤失败不影响其他步骤继续做完。
 */
public final class RootHideOrchestrator {
    private RootHideOrchestrator() {}

    private static final String TAG = "RootHideOrch";

    /**
     * 启用整套 Root 隐藏 (内核 + 属性 + 包管理器)。
     * @return 是否全部子步骤都成功
     */
    public static boolean applyAll() {
        Log.i(TAG, "applyAll begin");
        // 开账本: 此次 apply 做了哪些动作都记到 /data/local/tmp/dobby_roothide.cfg,
        // restoreAll 直接按账本反向操作, 不会盲遍历默认列表造成误删别人的状态。
        try { RootHideConfig.begin(); } catch (Throwable t) { Log.w(TAG, "cfg.begin", t); }
        boolean ok = true;

        // ① KPM 内核 root_hide on
        try {
            String r = RootHideGuard.nativeRawCtl("enable_root_hide");
            Log.i(TAG, "applyAll step1 enable_root_hide -> " + r);
            if (r != null && r.startsWith("OK")) {
                RootHideConfig.record("ROOT_HIDE", "ON");
            } else {
                // KPM 可能默认就是 on (kw=162 启动), 不算失败。但若 raw ctl 自身
                // 返回 FAIL (例如 superkey 还没灌入), 这一步算软失败, 继续后续。
                Log.w(TAG, "applyAll step1: enable_root_hide soft-fail, kernel probably already on");
            }
        } catch (Throwable t) { Log.w(TAG, "enable_root_hide", t); ok = false; }

        // ② KPM hide_pkg + auto hide_pid 联动
        try {
            int added = 0;
            for (String p : InjectHideActivity.ROOT_MGR_PKGS) {
                try {
                    String r = RootHideGuard.nativeRawCtl("add_hide_pkg:" + p);
                    if (r != null && r.startsWith("OK")) {
                        RootHideConfig.record("HIDE_PKG", p);
                        added++;
                    }
                } catch (Throwable ignored) {}
            }
            Log.i(TAG, "applyAll step2 add_hide_pkg done (added=" + added + ")");
        } catch (Throwable t) { Log.w(TAG, "add_hide_pkg", t); ok = false; }

        // ③ SpoofProps (verifiedboot / oem_unlock / debuggable / build.type ...)
        try {
            boolean propOk = SpoofProps.apply();
            Log.i(TAG, "applyAll step3 SpoofProps.apply -> " + propOk);
            if (propOk) {
                RootHideConfig.record("SPOOF_SNAPSHOT", "/data/local/tmp/spoof_props.snapshot");
            } else {
                ok = false;
            }
        } catch (Throwable t) { Log.w(TAG, "SpoofProps.apply", t); ok = false; }

        // ④ ApatchHide (pm hide root 管理器, 让 reveny 这类 binder 检测看不到)
        try {
            for (String p : InjectHideActivity.ROOT_MGR_PKGS) {
                try {
                    if (ApatchHide.applyOne(p)) RootHideConfig.record("PM_HIDE", p);
                } catch (Throwable ignored) {}
            }
            Log.i(TAG, "applyAll step4 ApatchHide.apply done");
        } catch (Throwable t) { Log.w(TAG, "ApatchHide.apply", t); ok = false; }

        // ⑤ 写脏标记, 让异常守护下次启动时能识别"应该是开着的"
        try { RootHideGuard.armDirtyFlag(); Log.i(TAG, "applyAll step5 armDirtyFlag done"); }
        catch (Throwable t) { Log.w(TAG, "armDirtyFlag", t); ok = false; }

        Log.i(TAG, "applyAll end ok=" + ok);
        return ok;
    }

    /**
     * 关闭整套 Root 隐藏 (按 apply 逆序还原)。卸 KPM 之前必须先调本方法,
     * 否则 KPM 卸了之后 raw ctl 没法 disable_root_hide / remove_hide_pkg,
     * 系统进入 "userspace 仍 spoof / pm hide, 但内核已无 hide" 的不一致态。
     *
     * @return 是否全部子步骤都成功
     */
    public static boolean restoreAll() {
        Log.i(TAG, "restoreAll begin");
        boolean ok = true;

        // 优先从账本读取 "上次 apply 实际做了什么", 严格反向; 账本不存在时退化为
        // 默认 ROOT_MGR_PKGS 遍历, 保留对旧版本部署的兼容。
        boolean hasCfg = RootHideConfig.exists();
        java.util.List<String> pmHidden = hasCfg ? RootHideConfig.pmHidePkgs() : null;
        java.util.List<String> kpmHidden = hasCfg ? RootHideConfig.hidePkgs() : null;

        if (pmHidden == null || pmHidden.isEmpty()) {
            pmHidden = java.util.Arrays.asList(InjectHideActivity.ROOT_MGR_PKGS);
        }
        if (kpmHidden == null || kpmHidden.isEmpty()) {
            kpmHidden = java.util.Arrays.asList(InjectHideActivity.ROOT_MGR_PKGS);
        }
        Log.i(TAG, "restoreAll source=" + (hasCfg ? "cfg" : "default")
                + " pmHide=" + pmHidden.size() + " kpmHide=" + kpmHidden.size());

        // 反向 ④ ApatchHide.restore — 严格只 unhide cfg 里记的包, 不动用户手动 hide 的
        try {
            int n = 0;
            for (String p : pmHidden) {
                try { if (ApatchHide.restoreOne(p)) n++; } catch (Throwable ignored) {}
            }
            Log.i(TAG, "restoreAll step-4 ApatchHide.restore -> " + n);
        } catch (Throwable t) { Log.w(TAG, "ApatchHide.restore", t); ok = false; }

        // 反向 ③ SpoofProps.restore (按 snapshot 还原 verified-boot 等真实值)
        try {
            boolean propOk = SpoofProps.restore();
            Log.i(TAG, "restoreAll step-3 SpoofProps.restore -> " + propOk);
            if (!propOk) ok = false;
        } catch (Throwable t) { Log.w(TAG, "SpoofProps.restore", t); ok = false; }

        // 反向 ② hide_pkg 撤销 — 同理, 只撤 cfg 记的, 不动用户自加的
        try {
            for (String p : kpmHidden) {
                try { RootHideGuard.nativeRawCtl("remove_hide_pkg:" + p); } catch (Throwable ignored) {}
            }
            Log.i(TAG, "restoreAll step-2 remove_hide_pkg done");
        } catch (Throwable t) { Log.w(TAG, "remove_hide_pkg", t); ok = false; }

        // 反向 ① KPM 内核 root_hide off
        try {
            String r = RootHideGuard.nativeRawCtl("disable_root_hide");
            Log.i(TAG, "restoreAll step-1 disable_root_hide -> " + r);
        } catch (Throwable t) { Log.w(TAG, "disable_root_hide", t); ok = false; }

        // 反向 ⑤ 清脏标记 + 清账本
        try { RootHideGuard.disarmDirtyFlag(); Log.i(TAG, "restoreAll step-5 disarmDirtyFlag done"); }
        catch (Throwable t) { Log.w(TAG, "disarmDirtyFlag", t); ok = false; }
        try { RootHideConfig.clear(); } catch (Throwable t) { Log.w(TAG, "cfg.clear", t); }

        Log.i(TAG, "restoreAll end ok=" + ok);
        return ok;
    }
}
