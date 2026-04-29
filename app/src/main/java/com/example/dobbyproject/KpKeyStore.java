package com.example.dobbyproject;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;

/**
 * 统一的 superkey 读取入口。唯一真源：
 *   /data/data/<pkg>/files/.kp_key
 *
 * 任何 Activity 在 onCreate 里依赖 KPM/native 操作前应调用
 * {@link #requireOrRedirect(Activity)} 自检并自动喂给 native。
 */
public final class KpKeyStore {
    private KpKeyStore() {}

    public static File file(Context ctx) {
        return new File(ctx.getFilesDir(), ".kp_key");
    }

    /** 读取 key；不存在/为空/超长返回 null。 */
    public static String read(Context ctx) {
        File f = file(ctx);
        if (!f.exists() || f.length() == 0 || f.length() > 128) return null;
        try (FileInputStream fis = new FileInputStream(f)) {
            byte[] buf = new byte[(int) f.length()];
            int n = fis.read(buf);
            if (n <= 0) return null;
            String s = new String(buf, 0, n, "UTF-8").trim();
            return s.isEmpty() ? null : s;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * 没有 key 时跳回 MainActivity 让用户输入；返回 false 表示当前 Activity
     * 应立即退出。已存在 key 时通过反射调用 caller 的 nativeSetSuperkey()
     * (各 Activity 都有自己的 JNI 桥, 签名相同)。
     */
    public static boolean requireOrRedirect(Activity activity) {
        String key = read(activity);
        if (key == null) {
            Toast.makeText(activity,
                    "未检测到 Superkey，请先在主界面输入 APatch Super Key",
                    Toast.LENGTH_LONG).show();
            Intent i = new Intent(activity, MainActivity.class);
            i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP | Intent.FLAG_ACTIVITY_SINGLE_TOP);
            activity.startActivity(i);
            activity.finish();
            return false;
        }
        try {
            activity.getClass()
                    .getMethod("nativeSetSuperkey", String.class)
                    .invoke(activity, key);
        } catch (NoSuchMethodException ignored) {
            // 该 Activity 没有 nativeSetSuperkey 也没关系——native 单例 g_superkey
            // 已被 MainActivity 的 nativeValidateKpKey 设置过(同一进程同一 SO)。
        } catch (Throwable ignored) {}
        return true;
    }
}
