package com.example.dobbyproject;

import com.example.dobbyproject.io.SecureFile;

import java.util.ArrayList;
import java.util.List;

/**
 * Root 痕迹隐藏的"做了什么"账本 (state file)。
 *
 * 持久化层全部委托 {@link SecureFile}: AES-256/GCM 加密 + 文件头 magic。
 * 本类只负责 KEY=VALUE 文本的序列化与读取。
 *
 * 文件路径: {@link RootHidePaths#cfgPath()} (= <filesDir>/roothide/dobby_roothide.cfg).
 *
 * 解密后明文 (每行 KEY=VALUE):
 *   PID=<pid>                  applyAll 时进程 PID
 *   ROOT_HIDE=ON               KPM root_hide 已开
 *   HIDE_PKG=<pkg>             此包加入 KPM hide_pkg
 *   PM_HIDE=<pkg>              此包被 pm hide
 *   SPOOF_SNAPSHOT=<path>      SpoofProps snapshot 路径
 *   APPLIED_AT=<unix秒>        apply 完成时刻
 */
public final class RootHideConfig {
    private RootHideConfig() {}

    /** 内存镜像。begin/record/clear 同步写盘; read() 先 loadFromDisk 再返回。 */
    private static final List<String[]> mem = new ArrayList<>();

    public static String cfgPath() { return RootHidePaths.cfgPath(); }

    public static synchronized void begin() {
        mem.clear();
        mem.add(new String[]{"PID", String.valueOf(android.os.Process.myPid())});
        mem.add(new String[]{"APPLIED_AT", String.valueOf(System.currentTimeMillis() / 1000)});
        writeBack();
    }

    public static synchronized void record(String key, String value) {
        if (key == null || key.isEmpty()) return;
        if (mem.isEmpty()) {
            mem.add(new String[]{"PID", String.valueOf(android.os.Process.myPid())});
        }
        String safeV = value == null ? "" :
                value.replace('\n', ' ').replace('\r', ' ');
        mem.add(new String[]{key, safeV});
        writeBack();
    }

    public static synchronized List<String[]> read() {
        loadFromDisk();
        return new ArrayList<>(mem);
    }

    public static synchronized void clear() {
        mem.clear();
        SecureFile.delete(cfgPath());
    }

    public static boolean exists() { return SecureFile.exists(cfgPath()); }

    public static int readPid() {
        for (String[] kv : read()) {
            if ("PID".equals(kv[0])) {
                try { return Integer.parseInt(kv[1]); } catch (NumberFormatException ignored) {}
            }
        }
        return -1;
    }

    public static List<String> pmHidePkgs() {
        List<String> out = new ArrayList<>();
        for (String[] kv : read()) if ("PM_HIDE".equals(kv[0])) out.add(kv[1]);
        return out;
    }

    public static List<String> hidePkgs() {
        List<String> out = new ArrayList<>();
        for (String[] kv : read()) if ("HIDE_PKG".equals(kv[0])) out.add(kv[1]);
        return out;
    }

    public static String dump() {
        StringBuilder sb = new StringBuilder();
        for (String[] kv : read()) sb.append(kv[0]).append('=').append(kv[1]).append('\n');
        return sb.toString();
    }

    // ─────────────────────────── 序列化 ───────────────────────────

    private static void writeBack() {
        StringBuilder plain = new StringBuilder();
        for (String[] kv : mem) plain.append(kv[0]).append('=').append(kv[1]).append('\n');
        SecureFile.writeText(cfgPath(), plain.toString());
    }

    private static void loadFromDisk() {
        mem.clear();
        String text = SecureFile.readText(cfgPath());
        if (text == null) return;
        for (String line : text.split("\n")) {
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String k = line.substring(0, eq).trim();
            String v = line.substring(eq + 1).trim();
            if (!k.isEmpty()) mem.add(new String[]{k, v});
        }
    }
}
