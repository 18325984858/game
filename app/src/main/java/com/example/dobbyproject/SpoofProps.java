package com.example.dobbyproject;

/**
 * 引导属性伪装 (spoof_props) 客户端。
 *
 * 与 KPM Root 痕迹隐藏总开关 (RootHide) 联动:
 *   - apply()   : 启用 Root 隐藏时调用; 先把当前 getprop 值写到 SNAPSHOT
 *                 (仅首次, 后续覆盖不写, 避免把 "已伪装" 当原值保存),
 *                 然后用 resetprop 改写 verified-boot / OEM unlock 链。
 *   - restore() : 关闭 Root 隐藏时调用; 按 SNAPSHOT 把每条属性还原回去,
 *                 不存在的条目用 resetprop --delete 删掉。
 *
 * 设计要点:
 *   - 只依赖 su + resetprop (APatch / Magisk / KSU 通用);
 *   - 整段 shell 一次性灌给 su, 出错只在 logcat 留痕, 不阻塞 UI;
 *   - SNAPSHOT 文件路径: /data/local/tmp/spoof_props.snapshot
 *     格式: "KEY=BASE64(VALUE)" 每行一条; VALUE 为空表示属性不存在。
 *   - resetprop 用 -n (no-restart) 强写 ro.* readonly 属性;
 *     还原时若原值非空也用 -n 写回; 若原值缺失则 --delete。
 *
 * 解决的检测:
 *   - reveny Native Root Detector v7.7.0 的 "Bootloader Unlocked" 两条
 *     (ro.boot.verifiedbootstate / sys.oem_unlock_allowed) 等。
 *
 * 局限:
 *   - 包名 / Binder / 硬件 attestation 检测不在本类职责。
 */
public final class SpoofProps {
    private SpoofProps() {}

    private static final String TAG = "SpoofProps";

    /** snapshot 路径由 RootHidePaths 提供 (默认 <filesDir>/roothide/spoof_props.snapshot,
     *  未 init 时退化 /data/local/tmp/spoof_props.snapshot)。 */
    private static String snapshotPath() { return RootHidePaths.spoofSnapshot(); }

    /**
     * 需要伪装的属性列表; null 值表示需要 --delete (锁定设备本不该有的属性).
     *
     * ⚠ 这是经过删减的"安全子集": 只保留 reveny / Magisk Detector 等实际查询的
     *   verified-boot / dm-verity / OEM unlock 链。删掉了运行时高危项, 因为
     *   2026-05-17 实测在已启动的系统上改 ro.debuggable / ro.secure /
     *   ro.build.type 等会触发 system_server / zygote 卡死。这些项启动时
     *   就已经被各服务读完并缓存了, 运行时再改是有害无益。
     *
     *   被删掉的项 (供 review): ro.debuggable, ro.secure, ro.build.type,
     *   ro.build.tags, ro.boot.warranty_bit, ro.warranty_bit, ro.boot.selinux。
     *   如果以后碰到需要这些的检测器, 优先用 APatch 模块在 post-fs-data
     *   阶段 (init 起来之前) 改它们, 而不是在 App 运行时改。
     */
    private static final String[][] SPOOF = new String[][] {
        // verified boot / dm-verity / flash lock — 启动后改这些不影响已加载的服务
        {"ro.boot.verifiedbootstate",       "green"},
        {"ro.boot.vbmeta.device_state",     "locked"},
        {"ro.boot.flash.locked",            "1"},
        {"ro.boot.veritymode",              "enforcing"},
        {"vendor.boot.verifiedbootstate",   "green"},
        {"vendor.boot.vbmeta.device_state", "locked"},
        {"ro.boot.realmebootstate",         "green"},   // Realme
        {"ro.boot.mibootstate",             "green"},   // 小米
        // OEM unlock - 锁定设备应不存在, value=null => --delete
        {"sys.oem_unlock_allowed",          null},
        {"ro.oem_unlock_supported",         null},
    };

    /** 启用属性伪装。snapshot 由 SecureFile 加密落盘到 App 私有目录。 */
    public static boolean apply() {
        // 1) 先用 Java 侧 getprop 读出每个属性的真值, 写入加密 snapshot;
        //    仅当 snapshot 不存在时写, 避免重复 apply 把已伪装值当真值留底。
        if (!com.example.dobbyproject.io.SecureFile.exists(snapshotPath())) {
            StringBuilder probe = new StringBuilder("set +e\n");
            for (String[] kv : SPOOF) {
                probe.append("printf '%s=' \"").append(kv[0]).append("\"; getprop ")
                     .append(kv[0]).append("; echo\n");
            }
            final StringBuilder snap = new StringBuilder();
            SuShell.runWithLines(probe.toString(), line -> {
                if (line == null) return;
                int eq = line.indexOf('=');
                if (eq <= 0) return;
                snap.append(line).append('\n');
            });
            com.example.dobbyproject.io.SecureFile.writeText(snapshotPath(), snap.toString());
        }

        // 2) 跑 resetprop 应用伪装
        StringBuilder sh = new StringBuilder();
        sh.append("set -u\n");
        sh.append("RP=\"\"; for p in /data/adb/ap/bin/resetprop ")
          .append("/data/adb/magisk/resetprop /data/adb/ksu/bin/resetprop; do ")
          .append("[ -x \"$p\" ] && RP=\"$p\" && break; done\n");
        sh.append("if [ -z \"$RP\" ]; then ")
          .append("echo \"[").append(TAG).append("] resetprop not found\" 1>&2; ")
          .append("exit 2; fi\n");
        for (String[] kv : SPOOF) {
            String key = kv[0];
            String val = kv[1];
            if (val == null) {
                sh.append("\"$RP\" --delete ").append(key).append(" 2>/dev/null\n");
            } else {
                sh.append("\"$RP\" -n ").append(key).append(" ").append(shellQuote(val)).append("\n");
            }
        }
        sh.append("echo \"[").append(TAG).append("] apply done\"\n");
        int rc = SuShell.run(sh.toString());
        return rc == 0;
    }

    /** 关闭属性伪装。snapshot 走 SecureFile 解密读取后跑 resetprop 还原。 */
    public static boolean restore() {
        String snapText = com.example.dobbyproject.io.SecureFile.readText(snapshotPath());

        StringBuilder sh = new StringBuilder();
        sh.append("set -u\n");
        sh.append("RP=\"\"; for p in /data/adb/ap/bin/resetprop ")
          .append("/data/adb/magisk/resetprop /data/adb/ksu/bin/resetprop; do ")
          .append("[ -x \"$p\" ] && RP=\"$p\" && break; done\n");
        sh.append("if [ -z \"$RP\" ]; then ")
          .append("echo \"[").append(TAG).append("] resetprop not found\" 1>&2; ")
          .append("exit 2; fi\n");

        if (snapText == null || snapText.isEmpty()) {
            // 无 snapshot: 一律 --delete (让 init 重新填或回到不存在)
            for (String[] kv : SPOOF) {
                sh.append("\"$RP\" --delete ").append(kv[0]).append(" 2>/dev/null\n");
            }
            sh.append("echo \"[").append(TAG).append("] restore (no snapshot) done\"\n");
            int rc = SuShell.run(sh.toString());
            return rc == 0;
        }

        // 解密后按 key=val 逐行恢复
        for (String line : snapText.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String k = line.substring(0, eq).trim();
            String v = line.substring(eq + 1).trim();
            if (k.isEmpty()) continue;
            if (v.isEmpty()) {
                sh.append("\"$RP\" --delete ").append(k).append(" 2>/dev/null\n");
            } else {
                sh.append("\"$RP\" -n ").append(k).append(" ").append(shellQuote(v)).append("\n");
            }
        }
        sh.append("echo \"[").append(TAG).append("] restore done\"\n");
        int rc = SuShell.run(sh.toString());
        // 还原完成后 snapshot 文件不再需要, 删除
        com.example.dobbyproject.io.SecureFile.delete(snapshotPath());
        return rc == 0;
    }

    /**
     * 简易 shell 单引号转义: 把 ' 替换成 '\'' 然后整体外套单引号。
     * 仅用于本类已知的短字符串 (green / locked / 0 / 1 等), 足够安全。
     */
    private static String shellQuote(String s) {
        if (s == null) return "''";
        return "'" + s.replace("'", "'\\''") + "'";
    }
}
