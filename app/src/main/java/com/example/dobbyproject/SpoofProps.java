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

    /** snapshot 文件路径, 用 /data/local/tmp 避开 SELinux. */
    private static final String SNAPSHOT = "/data/local/tmp/spoof_props.snapshot";

    /**
     * 需要伪装的属性列表; null 值表示需要 --delete (锁定设备本不该有的属性).
     */
    private static final String[][] SPOOF = new String[][] {
        // verified boot / dm-verity / flash lock
        {"ro.boot.verifiedbootstate",       "green"},
        {"ro.boot.vbmeta.device_state",     "locked"},
        {"ro.boot.flash.locked",            "1"},
        {"ro.boot.veritymode",              "enforcing"},
        {"vendor.boot.verifiedbootstate",   "green"},
        {"vendor.boot.vbmeta.device_state", "locked"},
        {"ro.boot.realmebootstate",         "green"},
        {"ro.boot.mibootstate",             "green"},
        // warranty / debuggable / secure
        {"ro.boot.warranty_bit",            "0"},
        {"ro.warranty_bit",                 "0"},
        {"ro.debuggable",                   "0"},
        {"ro.secure",                       "1"},
        {"ro.build.type",                   "user"},
        {"ro.build.tags",                   "release-keys"},
        {"ro.boot.selinux",                 "enforcing"},
        // OEM unlock - 锁定设备应不存在, value=null => --delete
        {"sys.oem_unlock_allowed",          null},
        {"ro.oem_unlock_supported",         null},
    };

    /** 启用属性伪装。返回 true 表示 su 退出码为 0。 */
    public static boolean apply() {
        StringBuilder sh = new StringBuilder();
        sh.append("set -u\n");
        sh.append("RP=\"\"; for p in /data/adb/ap/bin/resetprop ")
          .append("/data/adb/magisk/resetprop /data/adb/ksu/bin/resetprop; do ")
          .append("[ -x \"$p\" ] && RP=\"$p\" && break; done\n");
        sh.append("if [ -z \"$RP\" ]; then ")
          .append("echo \"[").append(TAG).append("] resetprop not found\" 1>&2; ")
          .append("exit 2; fi\n");

        // 1) 写 snapshot (仅当不存在时), 以避免重复 apply 把已伪装的值当真值保存
        sh.append("SNAP=").append(SNAPSHOT).append("\n");
        sh.append("if [ ! -f \"$SNAP\" ]; then\n");
        sh.append("  : > \"$SNAP.tmp\"\n");
        for (String[] kv : SPOOF) {
            String key = kv[0];
            // base64 编码当前值, 防止特殊字符 / 多行 break shell
            sh.append("  V=$(getprop ").append(key).append(" 2>/dev/null)\n");
            sh.append("  B=$(printf %s \"$V\" | base64 -w0 2>/dev/null || printf %s \"$V\" | base64)\n");
            sh.append("  echo \"").append(key).append("=$B\" >> \"$SNAP.tmp\"\n");
        }
        sh.append("  mv \"$SNAP.tmp\" \"$SNAP\"\n");
        sh.append("  chmod 600 \"$SNAP\"\n");
        sh.append("fi\n");

        // 2) 应用伪装
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

    /** 关闭属性伪装。按 snapshot 还原; 没有 snapshot 时退化为 --delete 全部伪装项。 */
    public static boolean restore() {
        StringBuilder sh = new StringBuilder();
        sh.append("set -u\n");
        sh.append("RP=\"\"; for p in /data/adb/ap/bin/resetprop ")
          .append("/data/adb/magisk/resetprop /data/adb/ksu/bin/resetprop; do ")
          .append("[ -x \"$p\" ] && RP=\"$p\" && break; done\n");
        sh.append("if [ -z \"$RP\" ]; then ")
          .append("echo \"[").append(TAG).append("] resetprop not found\" 1>&2; ")
          .append("exit 2; fi\n");

        sh.append("SNAP=").append(SNAPSHOT).append("\n");
        // 无 snapshot: 直接 --delete 所有伪装项; 让属性回到 "不存在/由 init 重新填"
        sh.append("if [ ! -f \"$SNAP\" ]; then\n");
        for (String[] kv : SPOOF) {
            sh.append("  \"$RP\" --delete ").append(kv[0]).append(" 2>/dev/null\n");
        }
        sh.append("  echo \"[").append(TAG).append("] restore (no snapshot) done\"\n");
        sh.append("  exit 0\n");
        sh.append("fi\n");

        // 有 snapshot: 逐行恢复
        sh.append("while IFS='=' read -r K B; do\n");
        sh.append("  [ -z \"$K\" ] && continue\n");
        sh.append("  if [ -z \"$B\" ]; then\n");
        sh.append("    \"$RP\" --delete \"$K\" 2>/dev/null\n");
        sh.append("  else\n");
        sh.append("    V=$(printf %s \"$B\" | base64 -d 2>/dev/null)\n");
        sh.append("    if [ -z \"$V\" ]; then\n");
        sh.append("      \"$RP\" --delete \"$K\" 2>/dev/null\n");
        sh.append("    else\n");
        sh.append("      \"$RP\" -n \"$K\" \"$V\"\n");
        sh.append("    fi\n");
        sh.append("  fi\n");
        sh.append("done < \"$SNAP\"\n");
        sh.append("rm -f \"$SNAP\"\n");
        sh.append("echo \"[").append(TAG).append("] restore done\"\n");

        int rc = SuShell.run(sh.toString());
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
