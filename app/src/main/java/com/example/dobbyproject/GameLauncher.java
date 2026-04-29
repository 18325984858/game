package com.example.dobbyproject;

import android.app.Activity;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.InputStreamReader;
import java.util.Locale;

/**
 * 统一的游戏启动 + 注入入口。所有游戏共用同一段 shell 流程,
 * 仅靠 {@link GameSpec} 描述的差异点 (包名 / SO 改名 / chcon /
 * 等待时长 / 是否带 trace 轮询的重试 / KPM hide_so 名) 来区分.
 *
 * 流程 (同 PUBG 现状):
 *   1. cp libdobbyproject.so → spec.dstDir/spec.soFileName  (可选 chcon)
 *   2. cp libinjector.so → /data/local/tmp/injector
 *   3. monkey 启动 spec.pkg
 *   4. sleep spec.waitMs
 *   5. 写 /data/local/tmp/dobby_config.txt + 清空 trace 文件
 *   6. 执行 injector:
 *      - retryWithTrace=true : 4 次重试, 每次后轮询
 *        /data/data/<pkg>/cache/ue4_gui_trace.txt 判成功 (PUBG 模式)
 *      - retryWithTrace=false: 单次, 用 exit code 判成败 (DFM/NRC 现状)
 *   7. 成功且配了 hideSoName, 调 KPM add_hide_so
 *
 * 添加新游戏: 直接 new GameSpec(...) 并调 launch() 即可, 无需复制粘贴
 * writeFile/launchAndInject 的两份方法.
 */
public final class GameLauncher {
    private GameLauncher() {}

    /** 描述一款游戏的注入参数。所有 PUBG/DFM/NRC 之间的差异都收敛到这里。 */
    public static final class GameSpec {
        /** 短标签, 用于日志前缀 / trace 文件名 (大写: PUBG/DFM/NRC). */
        public final String tag;
        /** 中文显示名, 用于 UI 状态栏. */
        public final String displayName;
        /** 目标游戏包名. */
        public final String pkg;
        /** 传给 injector argv[3] 的模式 (pubg/dfm/nrc/lol). */
        public final String mode;
        /** 部署到目标的 SO 文件名 (PUBG=libpre.so, 其它=libdobbyproject.so). */
        public final String soFileName;
        /** true=部署到 APK 自带的 nativeLibraryDir (PUBG); false=/data/data/<pkg>/files. */
        public final boolean useApkLibDir;
        /** true=拷贝后打 chcon u:object_r:apk_data_file:s0 标签 (PUBG 反扫描伪装). */
        public final boolean chconApkData;
        /** monkey 启动后等待游戏初始化的毫秒数. */
        public final int waitMs;
        /** true=4 次重试 + 轮询 ue4_gui_trace.txt; false=单次 exit code. */
        public final boolean retryWithTrace;
        /** 注入成功后调 add_hide_so 的目标文件名; null=不调. */
        public final String hideSoName;

        public GameSpec(String tag, String displayName, String pkg, String mode,
                        String soFileName, boolean useApkLibDir, boolean chconApkData,
                        int waitMs, boolean retryWithTrace, String hideSoName) {
            this.tag = tag;
            this.displayName = displayName;
            this.pkg = pkg;
            this.mode = mode;
            this.soFileName = soFileName;
            this.useApkLibDir = useApkLibDir;
            this.chconApkData = chconApkData;
            this.waitMs = waitMs;
            this.retryWithTrace = retryWithTrace;
            this.hideSoName = hideSoName;
        }
    }

    /**
     * 由调用方 (MainActivity) 提供的环境/回调集合, 让 GameLauncher 不直接依赖
     * Activity 实例, 也不直接持有 native 方法引用.
     */
    public interface Env {
        /** root binary 名称, 一般是 "su". */
        String rootBinary();
        /** 本 app 的 nativeLibraryDir (libdobbyproject.so / libinjector.so 源). */
        String appNativeLibDir();
        /** 解析目标游戏的安装 nativeLibraryDir; 返回 null 时回退到 /data/data/<pkg>/files. */
        String resolveTargetLibDir(String pkg);
        /** 安全调 KPM rawCtl, 内部捕获异常; 不可用时实现里 no-op 即可. */
        void kpmCtl(String cmd);
        /** UI 线程上回调注入结果. */
        void onResult(boolean ok, int exitCode);
    }

    /**
     * 异步执行整套部署+启动+注入流程。worker 线程上跑 shell, 结果回调到 UI 线程
     * 由 {@link Env#onResult} 自行 post.
     */
    public static void launch(GameSpec spec,
                              boolean enableUeDumper, boolean enableUeHeader, boolean enableLog,
                              Env env) {
        new Thread(() -> launchSync(spec, enableUeDumper, enableUeHeader, enableLog, env),
                "GameLauncher-" + spec.tag).start();
    }

    private static void launchSync(GameSpec spec,
                                   boolean enableUeDumper, boolean enableUeHeader, boolean enableLog,
                                   Env env) {
        final String injectorDst   = "/data/local/tmp/injector";
        final String injectorTrace = "/data/local/tmp/" + spec.tag.toLowerCase(Locale.ROOT) + "_injector_trace.txt";
        final String ue4Trace      = "/data/data/" + spec.pkg + "/cache/ue4_gui_trace.txt";

        // 1. 解析目标 SO 路径
        String dstDir;
        if (spec.useApkLibDir) {
            String r = env.resolveTargetLibDir(spec.pkg);
            dstDir = (r != null && !r.isEmpty()) ? r : ("/data/data/" + spec.pkg + "/files");
        } else {
            dstDir = "/data/data/" + spec.pkg + "/files";
        }
        final String soPath = dstDir + "/" + spec.soFileName;
        final String soSrc  = env.appNativeLibDir() + "/libdobbyproject.so";
        final String injSrc = env.appNativeLibDir() + "/libinjector.so";
        final String suBin  = env.rootBinary();
        final String tag    = spec.tag;

        // 注入前先撤销 hide_so, 否则改名/重写可能被内核拦
        if (spec.hideSoName != null) {
            env.kpmCtl("remove_hide_so:" + spec.hideSoName);
        }

        int exitCode;
        try {
            // 2. 部署 SO + injector + (可选)chcon
            StringBuilder deploy = new StringBuilder();
            deploy.append("mkdir -p ").append(dstDir).append('\n');
            deploy.append("cp -f ").append(soSrc).append(' ').append(soPath).append('\n');
            deploy.append("chmod 755 ").append(soPath).append('\n');
            if (spec.chconApkData) {
                deploy.append("chcon u:object_r:apk_data_file:s0 ").append(soPath)
                      .append(" 2>/dev/null || true\n");
            }
            deploy.append("cp -f ").append(injSrc).append(' ').append(injectorDst).append('\n');
            deploy.append("chmod 755 ").append(injectorDst).append('\n');
            deploy.append("sync\nexit\n");
            log(tag, "部署 SO+injector → " + soPath);
            runRoot(suBin, deploy.toString(), tag);

            // 3. monkey 启动
            log(tag, "启动 " + spec.displayName);
            runRoot(suBin,
                    "monkey -p " + spec.pkg + " -c android.intent.category.LAUNCHER 1 2>/dev/null\nexit\n",
                    tag);

            // 4. 等待初始化
            log(tag, "等待 " + (spec.waitMs / 1000) + " 秒游戏初始化...");
            Thread.sleep(spec.waitMs);

            // 5. 写配置 + 清 trace
            String cfgContent = "ue_dumper=" + (enableUeDumper ? "1" : "0") + "\n"
                              + "ue_header=" + (enableUeHeader ? "1" : "0") + "\n"
                              + "log="       + (enableLog ? "1" : "0") + "\n";
            String cfgScript = "echo '" + cfgContent + "' > /data/local/tmp/dobby_config.txt\n"
                             + "chmod 644 /data/local/tmp/dobby_config.txt\n"
                             + "rm -f " + injectorTrace + " " + ue4Trace + "\nexit\n";
            runRoot(suBin, cfgScript, tag);

            // 6. 执行 injector
            log(tag, "执行注入: " + injectorDst + " " + spec.pkg + " " + soPath + " " + spec.mode);
            String injectScript = spec.retryWithTrace
                    ? buildRetryWithTraceScript(spec, injectorDst, soPath, injectorTrace, ue4Trace, enableLog)
                    : injectorDst + " " + spec.pkg + " " + soPath + " " + spec.mode + " 2>&1\nexit $?\n";
            exitCode = runRootCapture(suBin, injectScript, tag);
            log(tag, "注入完成, exitCode=" + exitCode);

            // 7. 成功后重新 hide_so
            if (exitCode == 0 && spec.hideSoName != null) {
                env.kpmCtl("add_hide_so:" + spec.hideSoName);
            }
        } catch (Exception e) {
            log(tag, "注入异常: " + e.getMessage());
            exitCode = -1;
        }

        final boolean ok = exitCode == 0;
        final int finalCode = exitCode;
        env.onResult(ok, finalCode);
    }

    /** 4 次重试, 每次后轮询 ue4_gui_trace.txt 判成功 (PUBG 现状). */
    private static String buildRetryWithTraceScript(GameSpec spec, String injectorDst, String soPath,
                                                    String injectorTrace, String ue4Trace,
                                                    boolean enableLog) {
        StringBuilder s = new StringBuilder();
        s.append("success=0\nlast_ret=2\n");
        s.append("for attempt in 1 2 3 4; do\n");
        s.append("  pid_before=$(pidof ").append(spec.pkg).append(" 2>/dev/null)\n");
        s.append("  echo [").append(spec.tag).append("_TRACE] attempt=${attempt} pid_before=${pid_before}\n");
        s.append("  ").append(injectorDst).append(' ').append(spec.pkg).append(' ').append(soPath)
         .append(' ').append(spec.mode).append(" > ").append(injectorTrace).append(" 2>&1\n");
        s.append("  last_ret=$?\n");
        s.append("  echo [").append(spec.tag).append("_TRACE] injector_ret=${last_ret}\n");
        s.append("  echo [").append(spec.tag).append("_TRACE] injector_output_begin\n");
        s.append("  cat ").append(injectorTrace).append(" 2>/dev/null\n");
        s.append("  echo [").append(spec.tag).append("_TRACE] injector_output_end\n");
        s.append("  sleep 8\n");
        s.append("  if [ -s ").append(ue4Trace).append(" ]; then\n");
        s.append("    success=1\n");
        s.append("    echo [").append(spec.tag).append("_TRACE] ue4_gui_trace_detected attempt=${attempt}\n");
        s.append("    break\n");
        s.append("  fi\n");
        s.append("  pid_after=$(pidof ").append(spec.pkg).append(" 2>/dev/null)\n");
        s.append("  echo [").append(spec.tag).append("_TRACE] no_ue4_trace attempt=${attempt} pid_after=${pid_after}\n");
        s.append("  sleep 5\n");
        s.append("done\n");
        if (enableLog) {
            s.append("echo [").append(spec.tag).append("_TRACE] ue4_gui_output_begin\n");
            s.append("cat ").append(ue4Trace).append(" 2>/dev/null\n");
            s.append("echo [").append(spec.tag).append("_TRACE] ue4_gui_output_end\n");
        }
        s.append("if [ \"$success\" = \"1\" ]; then exit 0; else exit 2; fi\nexit\n");
        return s.toString();
    }

    /** 同步执行 root shell, 不读 stdout. */
    private static void runRoot(String suBin, String script, String tag) throws Exception {
        Process p = Runtime.getRuntime().exec(suBin);
        DataOutputStream os = new DataOutputStream(p.getOutputStream());
        os.writeBytes(script);
        os.flush();
        p.waitFor();
    }

    /** 同步执行 root shell, 把 stdout 逐行打印到 logcat, 返回 exit code. */
    private static int runRootCapture(String suBin, String script, String tag) throws Exception {
        Process p = Runtime.getRuntime().exec(suBin);
        DataOutputStream os = new DataOutputStream(p.getOutputStream());
        os.writeBytes(script);
        os.flush();
        BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
        String line;
        while ((line = r.readLine()) != null) {
            log(tag, line);
        }
        return p.waitFor();
    }

    private static void log(String tag, String msg) {
        android.util.Log.i("[SFK]", "[" + tag + "] " + msg);
    }
}
