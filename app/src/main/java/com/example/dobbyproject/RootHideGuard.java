package com.example.dobbyproject;

import android.util.Log;

/**
 * Root 隐藏守护：保证"启用 root hide 但应用没正常退出"时, 下次启动能自动还原。
 *
 * 三道防线:
 *   ① 启动期补救  ─ {@link #recoverIfDirty()} 在 Application.onCreate 调用
 *                   读 /data/local/tmp/dobby_roothide.dirty, 若其中记录的 PID
 *                   与"当前进程 PID"不同 (上一次 Java 进程已退出却没清),
 *                   则一次性把 KPM / SpoofProps / ApatchHide / hide_pkg
 *                   全部还原, 然后删 flag。
 *   ② 崩溃兜底    ─ {@link #installCrashGuard()} 链式装一层
 *                   Thread.setDefaultUncaughtExceptionHandler, 任意 Java 线程
 *                   抛未捕获异常时尽力跑一次 doRestore() 再委托原处理器。
 *   ③ JVM 关闭钩子 ─ {@link #installShutdownHook()} Runtime.addShutdownHook,
 *                   System.exit / finishAffinity 等正常退出路径会触发。
 *
 * 调用约定:
 *   - 启用 root hide 成功后 → {@link #armDirtyFlag()} 写脏标记;
 *   - 关闭 root hide 成功后 → {@link #disarmDirtyFlag()} 删脏标记;
 *   - "正常退出"不应自动 restore (用户可能想保持隐藏跨重启);
 *     restore 仅在"上一次 hide 是 armed 状态但当前进程 PID 已经换了" 才触发。
 *
 * 注意:
 *   - SIGKILL (force stop / OOM kill) 任何 Java 钩子都跑不到, 只能靠 ①;
 *   - SuShell.run() 在崩溃路径里仍可能因 fork 失败拉不起 su, 失败就忍受,
 *     用户下次启动 game 还会再补一次。
 */
public final class RootHideGuard {
    private RootHideGuard() {}

    static {
        // native 入口 nativeRawCtl 与 InjectHideActivity 共享同一 .so;
        // 重复 loadLibrary 是 no-op, 这里保证即使先调 RootHideGuard 也能链接到符号。
        try { System.loadLibrary("dobbyproject"); } catch (Throwable ignored) {}
    }

    private static final String TAG = "RootHideGuard";

    /** snapshot 文件路径, 用 /data/local/tmp 避开 SELinux. */
    private static final String DIRTY_FLAG = "/data/local/tmp/dobby_roothide.dirty";
    private static final String SPOOF_SNAPSHOT = "/data/local/tmp/spoof_props.snapshot";

    private static volatile boolean installed = false;
    private static final java.util.concurrent.atomic.AtomicBoolean bootSyncDone =
            new java.util.concurrent.atomic.AtomicBoolean(false);

    /** 标记 root hide 已启用; 任意一处异常退出后, 下次启动会自动 restore。 */
    public static void armDirtyFlag() {
        int pid = android.os.Process.myPid();
        // 用 su 写, 避免 SELinux 拒绝普通 App 写 /data/local/tmp
        SuShell.run("echo " + pid + " > " + DIRTY_FLAG + " && chmod 644 " + DIRTY_FLAG);
    }

    /** root hide 已干净关闭, 抹掉脏标记。 */
    public static void disarmDirtyFlag() {
        SuShell.run("rm -f " + DIRTY_FLAG);
    }

    /**
     * 应用启动早期调用。若发现脏标记 PID 不属于本进程 → 上轮没干净退出 → 还原。
     */
    public static void recoverIfDirty() {
        StringBuilder sb = new StringBuilder();
        SuShell.runWithLines("cat " + DIRTY_FLAG + " 2>/dev/null", line -> {
            if (line != null) sb.append(line.trim());
        });
        String text = sb.toString().trim();
        Log.i(TAG, "recoverIfDirty read flag='" + text + "' pid=" + android.os.Process.myPid());
        if (text.isEmpty()) return;
        int oldPid;
        try { oldPid = Integer.parseInt(text); } catch (NumberFormatException e) {
            Log.w(TAG, "recoverIfDirty bad flag content: " + text);
            return;
        }

        int curPid = android.os.Process.myPid();
        if (oldPid == curPid) {
            // 同一进程残留 flag (例如 Activity 重建): 不还原, 让现有逻辑继续走
            return;
        }
        // 检查老 PID 是否还活着 (理论上不应该, 但 PID 回绕保险一下)
        final boolean[] alive = {false};
        SuShell.runWithLines("[ -d /proc/" + oldPid + " ] && echo yes",
                line -> { if (line != null && line.trim().equals("yes")) alive[0] = true; });
        if (alive[0]) {
            // 旧进程还在跑 → 不是我们想 recover 的场景
            return;
        }

        Log.w(TAG, "detected dirty flag from pid=" + oldPid + ", running cleanup");
        doRestore("startup-recover");
        disarmDirtyFlag();
    }

    /** 注册全部异常钩子。可重复调用, 只生效一次。 */
    public static synchronized void install() {
        if (installed) return;
        installed = true;
        installCrashGuard();
        installShutdownHook();
        Log.i(TAG, "hooks installed (uncaught + shutdown)");
    }

    private static void installCrashGuard() {
        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((t, e) -> {
            try {
                Log.e(TAG, "uncaught in thread " + t.getName(), e);
                doRestore("uncaught");
                disarmDirtyFlag();
            } catch (Throwable cleanup) {
                Log.e(TAG, "cleanup failed", cleanup);
            } finally {
                if (prev != null) prev.uncaughtException(t, e);
                else {
                    // 最后兜底: 直接退出, 别让进程僵死
                    android.os.Process.killProcess(android.os.Process.myPid());
                    System.exit(10);
                }
            }
        });
    }

    private static void installShutdownHook() {
        try {
            Runtime.getRuntime().addShutdownHook(new Thread(() -> {
                try {
                    doRestore("shutdown-hook");
                    disarmDirtyFlag();
                } catch (Throwable ignored) {}
            }, "RootHideGuard-Shutdown"));
        } catch (IllegalStateException ignored) {
            // 已在 shutdown 阶段, 忽略
        }
    }

    /**
     * 内核 ↔ 用户态对齐 (Boot-Sync): 每个进程仅跑一次。
     *
     * 场景: 手机重启后, KPM 自身重新加载 → root_hide_enabled 默认 1
     *       (root_kw_count=161). 但用户态那两层 (SpoofProps 改 verified-boot 属性、
     *       ApatchHide pm hide root 管理器) 都是非持久化的, 重启就丢。
     *       结果用户进 KPM 管理页, 看到 "Root 痕迹隐藏: 已开启" 但 verifiedbootstate
     *       还是 orange, APatch 图标也还在, 跟 UI 显示完全不一致。
     *
     * 本方法在 refreshAll 第一次拿到内核状态后调用:
     *   - kernelRootHideOn == true 且 dirty flag / snapshot 都不存在
     *     → 重启后首次进入, 自动 apply 用户态两层 + add_hide_pkg + arm dirty flag;
     *   - 其它情况 (用户已手动关过 / 本轮已 armed) 不动, 避免污染 snapshot。
     *
     * @return 是否真的执行了 apply 动作; UI 可据此 toast。
     */
    public static boolean syncOnceFromKernel(boolean kernelRootHideOn) {
        if (!bootSyncDone.compareAndSet(false, true)) return false;
        if (!kernelRootHideOn) return false;

        // 已经 armed 过 (snapshot 已存在或 dirty flag 存在) → 这一轮 boot 已经同步过,
        // 不要重复 apply (会用已伪装值覆盖 snapshot)。
        if (fileExistsAsRoot(DIRTY_FLAG) || fileExistsAsRoot(SPOOF_SNAPSHOT)) {
            Log.i(TAG, "syncOnceFromKernel: already armed (dirty/snapshot present), skip");
            return false;
        }

        Log.i(TAG, "syncOnceFromKernel: kernel root_hide=on but userspace empty → applying");
        try { SpoofProps.apply(); } catch (Throwable t) { Log.w(TAG, "SpoofProps.apply", t); }
        try { ApatchHide.apply(); } catch (Throwable t) { Log.w(TAG, "ApatchHide.apply", t); }
        try {
            for (String p : InjectHideActivity.ROOT_MGR_PKGS) {
                try { nativeRawCtl("add_hide_pkg:" + p); } catch (Throwable ignored) {}
            }
        } catch (Throwable t) { Log.w(TAG, "add_hide_pkg", t); }
        try { armDirtyFlag(); } catch (Throwable t) { Log.w(TAG, "armDirtyFlag", t); }
        return true;
    }

    private static boolean fileExistsAsRoot(String path) {
        final boolean[] exists = {false};
        SuShell.runWithLines("[ -e " + path + " ] && echo yes",
                line -> { if (line != null && line.trim().equals("yes")) exists[0] = true; });
        return exists[0];
    }

    /** 真正的清理动作。每一步独立 try/catch, 互不影响。 */
    private static void doRestore(String reason) {
        Log.i(TAG, "doRestore reason=" + reason);

        // ① 关掉 KPM root_hide (撤销 root_kw 注入 + file_hide)
        try { Log.i(TAG, "rawCtl disable_root_hide -> " + nativeRawCtl("disable_root_hide")); }
        catch (Throwable t) { Log.w(TAG, "disable root_hide failed", t); }

        // ② 撤销 hide_pkg 里的 root 管理器
        try {
            for (String p : InjectHideActivity.ROOT_MGR_PKGS) {
                try { nativeRawCtl("remove_hide_pkg:" + p); } catch (Throwable ignored2) {}
            }
        } catch (Throwable t) { Log.w(TAG, "remove hide_pkg failed", t); }

        // ③ 还原 verified-boot / OEM unlock 属性
        try { SpoofProps.restore(); } catch (Throwable t) {
            Log.w(TAG, "SpoofProps.restore failed", t);
        }

        // ④ pm unhide root 管理器, 让 launcher 图标回来
        try { ApatchHide.restore(); } catch (Throwable t) {
            Log.w(TAG, "ApatchHide.restore failed", t);
        }
    }

    // 通过 KPM control0 文本命令分发器走原始命令。native 实现见
    // app/src/main/cpp/kpatch/kpm_inject_hide_jni.cpp 中
    // Java_com_example_dobbyproject_RootHideGuard_nativeRawCtl。
    static native String nativeRawCtl(@androidx.annotation.NonNull String cmd);
}
