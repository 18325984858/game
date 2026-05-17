package com.example.dobbyproject;

/**
 * 用 `pm hide` / `pm unhide` 把 root 管理器从 PackageManager 默认结果里临时摘掉。
 *
 * 起因:
 *   reveny Native Root Detector v7.7.0 等用 pm.getInstalledPackages(0) 走 Binder
 *   到 system_server 查表, 拿到 "me.bmax.apatch" 即报 "Detected Risky App"。
 *   KPM (kernel) 拦不到 Binder; 改包名 (apktool 重打包) 又破坏性大、不可逆。
 *
 *   `pm hide PKG` 调用的是 IPackageManager.setApplicationHiddenSettingAsUser(),
 *   把 ApplicationInfo.hidden 设为 true ⇒
 *     - pm list packages / getInstalledPackages(0) 不返回该包;
 *     - pm path 返回空;
 *     - launcher 上的图标消失 (App 仍在磁盘, 数据完整);
 *     - 但是, 已 mmap 该包资源 / 已运行进程不受影响;
 *   `pm unhide PKG` 反向一行恢复, 完全可逆, 不丢 superkey, 不影响 APatch 内核补丁。
 *
 * 与 Root 痕迹隐藏开关联动:
 *   - apply()   : 启用 Root 隐藏时调用, 对每个 root 管理器 pm hide;
 *   - restore() : 关闭 Root 隐藏时调用, pm unhide 还原。
 *
 * 注意事项:
 *   - hide 期间 APatch Manager 桌面图标会消失, 用户想再进 Manager UI 必须
 *     先关闭 game 里的 Root 隐藏 (会自动 unhide), 或手动跑 `su -c "pm unhide me.bmax.apatch"`。
 *   - 本机制只针对 PackageManager 默认查询。如果检测器调用 getInstalledPackages
 *     时传了 MATCH_UNINSTALLED_PACKAGES / MATCH_DISABLED_COMPONENTS, 仍会看见;
 *     此种情况需要 HMA / Shamiko。
 *   - apd / kpd 这些 root 后台守护进程不受 pm hide 影响 (它们不在 system_server
 *     的 PackageManager 体系内), 所以 root 功能 100% 不受影响。
 */
public final class ApatchHide {
    private ApatchHide() {}

    /** 需要从 PackageManager 默认结果里隐藏的 root 管理器包名列表。 */
    private static final String[] PKGS = new String[] {
            "me.bmax.apatch",                  // APatch
            "com.topjohnwu.magisk",            // Magisk
            "io.github.huskydg.magisk",        // Magisk Delta
            "io.github.vvb2060.magisk",        // Magisk Alpha
            "me.weishu.kernelsu",              // KernelSU
            "com.rifsxd.ksunext",              // KernelSU Next
            "com.sukisu.ultra",                // SukiSU
    };

    /**
     * pm hide 全部 root 管理器包。返回成功隐藏的包数。
     * 没装的包 pm 会报 Failure, 但我们忽略 (返回 false 不计数)。
     */
    public static int apply() {
        return forEach(true);
    }

    /** pm unhide 全部 root 管理器包。返回成功还原的包数。 */
    public static int restore() {
        return forEach(false);
    }

    private static int forEach(boolean hide) {
        StringBuilder sh = new StringBuilder();
        sh.append("OK=0\n");
        String op = hide ? "hide" : "unhide";
        for (String p : PKGS) {
            // pm path 检查是否真的存在该包 (含已 hidden 的); 不存在则跳过
            sh.append("if pm list packages -u ").append(p)
              .append(" 2>/dev/null | grep -q \"^package:").append(p).append("$\"; then\n");
            sh.append("  pm ").append(op).append(" ").append(p)
              .append(" >/dev/null 2>&1 && OK=$((OK+1))\n");
            sh.append("fi\n");
        }
        sh.append("echo $OK\n");

        final int[] count = {0};
        SuShell.runWithLines(sh.toString(), line -> {
            line = line == null ? "" : line.trim();
            if (line.matches("\\d+")) count[0] = Integer.parseInt(line);
        });
        return count[0];
    }
}
