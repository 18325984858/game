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

    /** 路径都由 RootHidePaths 提供。默认 App 私有目录, 未 init() 时
     *  退化到 /data/local/tmp 以保证旧部署能恢复。 */
    private static String dirtyFlag()     { return RootHidePaths.dirtyFlag(); }
    private static String spoofSnapshot() { return RootHidePaths.spoofSnapshot(); }

    private static volatile boolean installed = false;
    private static final java.util.concurrent.atomic.AtomicBoolean bootSyncDone =
            new java.util.concurrent.atomic.AtomicBoolean(false);

    /** 标记 root hide 已启用; 任意一处异常退出后, 下次启动会自动 restore。 */
    public static void armDirtyFlag() {
        int pid = android.os.Process.myPid();
        com.example.dobbyproject.io.SecureFile.writeText(dirtyFlag(), String.valueOf(pid));
    }

    /** root hide 已干净关闭, 抹掉脏标记。 */
    public static void disarmDirtyFlag() {
        com.example.dobbyproject.io.SecureFile.delete(dirtyFlag());
    }

    /**
     * 应用启动早期调用。若发现脏标记 PID 不属于本进程 → 上轮没干净退出 → 还原。
     */
    public static void recoverIfDirty() {
        String text = com.example.dobbyproject.io.SecureFile.readText(dirtyFlag());
        if (text != null) text = text.trim();
        Log.i(TAG, "recoverIfDirty read flag='" + text + "' pid=" + android.os.Process.myPid());
        if (text == null || text.isEmpty()) return;
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
        // 注意 token 顺序: 先做 "条件不满足就返回" 的廉价检查, 最后才占用 token。
        //   早期版本把 compareAndSet 放在最前面, 结果导致 KPM 还没装好的第一次
        //   refreshAll 已经把 token 消耗掉 (rootHide=0, no-op 但占了名额),
        //   等用户随后点 "安装 KPM" KPM 加载后再 refreshAll, sync 直接跳过,
        //   现象就是 "默认装 KPM 时 Root 隐藏 UI 已开, 但 SpoofProps 不生效"。
        if (!kernelRootHideOn) return false;

        // 已经 armed 过 → 跳过, 避免重复 apply 把已伪装值当真值覆盖 snapshot。
        //
        // 关键: 这里只看 dirty flag, 不再看 snapshot。
        //   dirty flag 是 "applyAll 跑完 + 没干净 restore" 的真正凭证, restoreAll
        //   会在结尾删除它; 而 snapshot 仅在 SpoofProps.apply 写入、SpoofProps.restore
        //   删除。如果 snapshot 因为某些救援操作残留 (比如用户手动 pm unhide /
        //   resetprop 恢复了真实状态) 而 dirty flag 已经没了, 之前的逻辑会误以为
        //   "已 armed" 而跳过 apply, 导致用户态隐藏永远启动不了, 进而 reveny 等
        //   检测器仍能看到 verifiedbootstate / apatch 等。
        if (com.example.dobbyproject.io.SecureFile.exists(dirtyFlag())) {
            Log.i(TAG, "syncOnceFromKernel: already armed (dirty flag present), skip");
            bootSyncDone.set(true);
            return false;
        }

        if (!bootSyncDone.compareAndSet(false, true)) return false;

        Log.i(TAG, "syncOnceFromKernel: kernel root_hide=on but userspace empty → applying");
        // 已确认 dirty flag 不存在 → 我们当前 NOT applied; 残留的 snapshot 都是过期数据
        // (来自上一次进程的 apply, 重启后 props 已回到真值)。先删掉, 让 SpoofProps.apply
        // 重新基于真值采样, 否则 restore 会写回过期值, 造成 "verifiedbootstate 还原为空"
        // 之类的诡异结果。
        try {
            com.example.dobbyproject.io.SecureFile.delete(spoofSnapshot());
            Log.i(TAG, "syncOnceFromKernel: wiped stale snapshot before apply");
        } catch (Throwable ignored) {}
        try {
            boolean ok = RootHideOrchestrator.applyAll();
            Log.i(TAG, "syncOnceFromKernel: RootHideOrchestrator.applyAll -> " + ok);
        } catch (Throwable t) {
            Log.w(TAG, "syncOnceFromKernel: applyAll failed", t);
        }
        return true;
    }

    private static boolean fileExistsAsRoot(String path) {
        final boolean[] exists = {false};
        SuShell.runWithLines("[ -e " + path + " ] && echo yes",
                line -> { if (line != null && line.trim().equals("yes")) exists[0] = true; });
        return exists[0];
    }

    /** 真正的清理动作。委托给 RootHideOrchestrator.restoreAll, 保证跟用户
     *  手动关闭 ROOT 隐藏走同一份代码。每一子步骤独立 try/catch。 */
    private static void doRestore(String reason) {
        Log.i(TAG, "doRestore reason=" + reason);
        try {
            boolean ok = RootHideOrchestrator.restoreAll();
            Log.i(TAG, "doRestore RootHideOrchestrator.restoreAll -> " + ok);
        } catch (Throwable t) {
            Log.w(TAG, "doRestore RootHideOrchestrator.restoreAll failed", t);
        }
    }

    // 通过 KPM control0 文本命令分发器走原始命令。native 实现见
    // app/src/main/cpp/kpatch/kpm_inject_hide_jni.cpp 中
    // Java_com_example_dobbyproject_RootHideGuard_nativeRawCtl。
    static native String nativeRawCtl(@androidx.annotation.NonNull String cmd);
}
