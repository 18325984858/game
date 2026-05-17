package com.example.dobbyproject;

import android.content.Context;

import java.io.File;

/**
 * Root 隐藏相关的 state 文件统一定位。
 *
 * 之前散落在 /data/local/tmp 的三个文件:
 *   - dobby_roothide.dirty       脏标记 (RootHideGuard)
 *   - spoof_props.snapshot       属性原值快照 (SpoofProps)
 *   - dobby_roothide.cfg         apply 账本 (RootHideConfig)
 * 全部收敛到 App 私有目录 <filesDir>/roothide/:
 *   - 卸载 app 时自动清理, 不留残留
 *   - 路径不再跟其它 root 工具共享 /data/local/tmp 命名空间, 不会被误删
 *   - SELinux 上下文是 app_data_file, su 仍可访问
 *
 * 调用约定: MainActivity / InjectHideActivity 的 onCreate 早期调一次
 *   {@link #init(Context)}。在此之前调 {@link #dirtyFlag()} 等会
 *   退化到 /data/local/tmp/ 路径 (保持向后兼容)。
 */
public final class RootHidePaths {
    private RootHidePaths() {}

    /** App 私有 root-hide 目录, init() 后才有值。 */
    private static volatile File ROOT_DIR;

    /** 回退路径 (init 之前/失败时用), 跟旧版兼容。 */
    private static final String FALLBACK_DIR = "/data/local/tmp";

    /** 在 Activity onCreate 早期调一次, 创建并 chmod app 私有 roothide/ 目录。 */
    public static synchronized void init(Context ctx) {
        if (ROOT_DIR != null) return;
        if (ctx == null) return;
        try {
            File dir = new File(ctx.getFilesDir(), "roothide");
            if (!dir.exists()) dir.mkdirs();
            // 让 su 进程也能 readdir + 写 (700 默认会让 root 也能进, 但保险起见 750)
            try {
                dir.setReadable(true, false);
                dir.setExecutable(true, false);
            } catch (Throwable ignored) {}
            ROOT_DIR = dir;
        } catch (Throwable ignored) {}
    }

    /** 当前已初始化的 root-hide 目录路径; 没初始化返回 FALLBACK_DIR。 */
    public static String dir() {
        File d = ROOT_DIR;
        return d != null ? d.getAbsolutePath() : FALLBACK_DIR;
    }

    public static String dirtyFlag()     { return dir() + "/dobby_roothide.dirty"; }
    public static String spoofSnapshot() { return dir() + "/spoof_props.snapshot"; }
    public static String cfgPath()       { return dir() + "/dobby_roothide.cfg"; }

    /**
     * 旧版本路径迁移: 把 /data/local/tmp 里残留的三个文件移到新目录。
     * 跑一次即可, 失败不要紧 (旧文件会被独立的 restore 路径覆盖掉)。
     */
    public static void migrateLegacy() {
        if (ROOT_DIR == null) return;
        SuShell.run(
                "for f in dobby_roothide.dirty spoof_props.snapshot dobby_roothide.cfg; do\n"
              + "  if [ -f " + FALLBACK_DIR + "/$f ] && [ ! -f " + dir() + "/$f ]; then\n"
              + "    mv " + FALLBACK_DIR + "/$f " + dir() + "/ 2>/dev/null;\n"
              + "  fi;\n"
              + "done");
    }
}
