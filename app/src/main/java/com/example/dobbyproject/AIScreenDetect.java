package com.example.dobbyproject;

import android.content.Context;
import android.content.res.AssetManager;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * NCNN NanoDet 屏幕检测控制 (旁路截屏 + 推理, 无 Hook).
 *
 * 使用方式:
 *   AIScreenDetect.init(context);          // 一次性: 解压 assets 模型
 *   AIScreenDetect.start();                // 启动后台线程
 *   AIScreenDetect.setEnabled(true);       // 通过 ImGui 菜单也可控制
 */
public final class AIScreenDetect {

    private static final String TAG = "AIScreenDetect";
    private static volatile boolean sInited = false;

    static { System.loadLibrary("dobbyproject"); }

    private AIScreenDetect() {}

    /** 解压 assets/nanodet/* 到 filesDir 并设置路径. */
    public static synchronized void init(Context ctx) {
        if (sInited) return;
        try {
            File modelDir = new File(ctx.getFilesDir(), "nanodet");
            if (!modelDir.exists()) modelDir.mkdirs();
            File paramFile = new File(modelDir, "nanodet-m.param");
            File binFile   = new File(modelDir, "nanodet-m.bin");
            extractAsset(ctx, "nanodet/nanodet-m.param", paramFile);
            extractAsset(ctx, "nanodet/nanodet-m.bin",   binFile);
            nativeSetModelPaths(paramFile.getAbsolutePath(), binFile.getAbsolutePath());
            sInited = true;
            Log.i(TAG, "init ok param=" + paramFile + " bin=" + binFile);
        } catch (IOException e) {
            Log.e(TAG, "init failed", e);
        }
    }

    private static void extractAsset(Context ctx, String name, File dest) throws IOException {
        if (dest.exists() && dest.length() > 0) return;
        AssetManager am = ctx.getAssets();
        try (InputStream in = am.open(name);
             OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    public static native void nativeSetModelPaths(String paramPath, String binPath);
    public static native void nativeSetUseGpu(boolean useGpu);
    public static native void nativeSetUseSu(boolean useSu);
    public static native void nativeSetIntervalMs(int ms);
    public static native void nativeStart();
    public static native void nativeStop();
    public static native void nativeSetEnabled(boolean enabled);
    public static native boolean nativeIsRunning();
    public static native boolean nativeIsReady();
    public static native String  nativeGetLastError();
    public static native void nativeSetScoreThreshold(float thr);
    public static native void nativeSetClassFilter(int cls);   // -1=all, 0=person

    // ── AI 辅助瞄准 ──
    public static native void    nativeAimSetEnabled(boolean v);
    public static native void    nativeAimSetVisualOnly(boolean v);
    public static native void    nativeAimSetTrigger(boolean v);
    public static native boolean nativeAimInjectorReady();
    public static native String  nativeAimInjectorError();

    /**
     * 触屏注入需要 /dev/uinput 可写权限. 通过 magisk su 提权 chmod.
     * 仅在用户启用 "AI 辅助瞄准 (注入触屏)" 时调用一次.
     */
    public static void prepareTouchInjection() {
        try {
            Process p = Runtime.getRuntime().exec(new String[] {
                    "su", "-c", "chmod 666 /dev/uinput"
            });
            int rc = p.waitFor();
            Log.i(TAG, "chmod /dev/uinput rc=" + rc);
        } catch (Throwable t) {
            Log.w(TAG, "chmod /dev/uinput failed (non-fatal)", t);
        }
    }

    /**
     * 跨进程检测共享内存: /data/local/tmp/ai_dets.bin
     * AIPipeline 在 dobbyproject 进程写, UE4Draw 在 PUBG 进程读.
     * 必须 chmod 666 + chcon 让 PUBG 的 untrusted_app 能 mmap.
     */
    public static void prepareSharedDets() {
        try {
            Process p = Runtime.getRuntime().exec(new String[] {
                    "su", "-c",
                    // 1) 创建文件并预分配 16KB (大于 Header 实际大小, 满足 mmap)
                    // 2) chmod 666 让任何 uid 可读写
                    // 3) chcon 设为 magisk_file: untrusted_app 允许 read/write/map
                    //    (若该 context 不存在则降级为默认 device, 仍优于 shell_data_file)
                    "F=/data/local/tmp/ai_dets.bin; " +
                    "rm -f $F; dd if=/dev/zero of=$F bs=1 count=16384 2>/dev/null; " +
                    "chmod 666 $F; " +
                    "chcon u:object_r:magisk_file:s0 $F 2>/dev/null || " +
                    "chcon u:object_r:system_data_file:s0 $F 2>/dev/null || " +
                    "true"
            });
            int rc = p.waitFor();
            Log.i(TAG, "prepareSharedDets rc=" + rc);
        } catch (Throwable t) {
            Log.w(TAG, "prepareSharedDets failed (non-fatal)", t);
        }
    }

    // 友好封装
    public static void start() { nativeStart(); }
    public static void stop()  { nativeStop(); }
    public static void setEnabled(boolean v) { nativeSetEnabled(v); }
    public static void setUseGpu(boolean v) { nativeSetUseGpu(v); }
    public static void setUseSu(boolean v)  { nativeSetUseSu(v); }
    public static void setIntervalMs(int ms) { nativeSetIntervalMs(ms); }
    public static void setScoreThreshold(float v) { nativeSetScoreThreshold(v); }
}
