package com.example.dobbyproject.io;

import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;

import javax.crypto.Cipher;
import javax.crypto.spec.GCMParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * 项目里"持久化敏感小状态"用的统一加密读写工具。
 *
 * 用途: Root 隐藏 / KPM 配置 等 cfg/snapshot/marker 等敏感文件统一通过本类
 *       写入。文件内容用 AES-256/GCM 加密 + 文件头 magic, 读出来如果 magic
 *       不对 (旧明文文件 / 损坏 / 别的程序写的) 会自动 fallback 当明文 UTF-8
 *       解析, 保证一次切换不丢数据。
 *
 * 文件结构 (二进制):
 *   [MAGIC 4B "SF01"][IV 12B][ciphertext + 16B GCM tag]
 *
 * 密钥派生: SHA-256(salt + Build.MANUFACTURER + Build.MODEL + uid).
 *   - 不在源代码里硬编码常量, 反编译还原难度提高;
 *   - 同一台手机同一个 App uid 内稳定; 换 Build 信息或换 uid 解不出来。
 *
 * 线程安全: 所有公共方法是无状态的, 锁限定在 key() 派生的 double-check。
 *
 * 使用示例:
 *   SecureFile.writeText("/data/data/<pkg>/files/state.cfg", "ROOT_HIDE=ON\n");
 *   String s = SecureFile.readText("/data/data/<pkg>/files/state.cfg");
 */
public final class SecureFile {
    private SecureFile() {}

    private static final String TAG = "SecureFile";

    private static final int GCM_IV_LEN  = 12;
    private static final int GCM_TAG_LEN = 128;
    private static final byte[] MAGIC    = new byte[]{'S','F','0','1'};

    // ─────────────────────────── 公共 API ───────────────────────────

    /** 加密写入 UTF-8 文本; 覆盖式 (truncate)。失败返回 false。 */
    public static boolean writeText(String path, String text) {
        return writeBytes(path, text == null ? new byte[0]
                                              : text.getBytes(StandardCharsets.UTF_8));
    }

    /** 加密写入字节。 */
    public static boolean writeBytes(String path, byte[] data) {
        if (path == null) return false;
        try {
            byte[] enc = encrypt(data == null ? new byte[0] : data);
            File f = new File(path);
            File parent = f.getParentFile();
            if (parent != null && !parent.exists()) parent.mkdirs();
            try (FileOutputStream out = new FileOutputStream(f, false)) {
                out.write(enc);
            }
            return true;
        } catch (Throwable t) {
            Log.w(TAG, "writeBytes failed " + path, t);
            return false;
        }
    }

    /** 读取并解密为 UTF-8 文本。文件不存在返回 null; 解密失败时 fallback 当明文。 */
    public static String readText(String path) {
        byte[] b = readBytes(path);
        return b == null ? null : new String(b, StandardCharsets.UTF_8);
    }

    /** 读取并解密为字节。 */
    public static byte[] readBytes(String path) {
        if (path == null) return null;
        File f = new File(path);
        if (!f.exists()) return null;
        try {
            byte[] raw;
            try (FileInputStream in = new FileInputStream(f)) {
                ByteArrayOutputStream baos = new ByteArrayOutputStream();
                byte[] buf = new byte[2048];
                int n;
                while ((n = in.read(buf)) > 0) baos.write(buf, 0, n);
                raw = baos.toByteArray();
            }
            byte[] plain = decrypt(raw);
            // 兼容: magic 不对就当明文返回 (旧版本明文文件 / 外部写入)
            return plain != null ? plain : raw;
        } catch (Throwable t) {
            Log.w(TAG, "readBytes failed " + path, t);
            return null;
        }
    }

    /** 文件存在 + 可读检查 (不解密)。 */
    public static boolean exists(String path) {
        return path != null && new File(path).exists();
    }

    /** 删除文件 (失败返回 false; 不存在视为成功)。 */
    public static boolean delete(String path) {
        if (path == null) return false;
        File f = new File(path);
        return !f.exists() || f.delete();
    }

    /** 判断给定 raw 字节流是否是 SecureFile 加密格式 (用 magic 头判断)。 */
    public static boolean isEncrypted(byte[] raw) {
        if (raw == null || raw.length < MAGIC.length + GCM_IV_LEN + 16) return false;
        for (int i = 0; i < MAGIC.length; i++) if (raw[i] != MAGIC[i]) return false;
        return true;
    }

    // ─────────────────────────── 加解密内部 ─────────────────────────

    private static byte[] encrypt(byte[] plain) throws Exception {
        byte[] iv = new byte[GCM_IV_LEN];
        new SecureRandom().nextBytes(iv);
        Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
        c.init(Cipher.ENCRYPT_MODE, key(), new GCMParameterSpec(GCM_TAG_LEN, iv));
        byte[] ct = c.doFinal(plain);

        byte[] out = new byte[MAGIC.length + iv.length + ct.length];
        System.arraycopy(MAGIC, 0, out, 0, MAGIC.length);
        System.arraycopy(iv, 0, out, MAGIC.length, iv.length);
        System.arraycopy(ct, 0, out, MAGIC.length + iv.length, ct.length);
        return out;
    }

    private static byte[] decrypt(byte[] enc) {
        if (!isEncrypted(enc)) return null;
        byte[] iv = new byte[GCM_IV_LEN];
        System.arraycopy(enc, MAGIC.length, iv, 0, GCM_IV_LEN);
        int ctLen = enc.length - MAGIC.length - GCM_IV_LEN;
        byte[] ct = new byte[ctLen];
        System.arraycopy(enc, MAGIC.length + GCM_IV_LEN, ct, 0, ctLen);
        try {
            Cipher c = Cipher.getInstance("AES/GCM/NoPadding");
            c.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(GCM_TAG_LEN, iv));
            return c.doFinal(ct);
        } catch (Exception e) {
            Log.w(TAG, "GCM decrypt fail (key mismatch / tampered)", e);
            return null;
        }
    }

    private static volatile SecretKeySpec CACHED_KEY;
    private static SecretKeySpec key() {
        SecretKeySpec k = CACHED_KEY;
        if (k != null) return k;
        synchronized (SecureFile.class) {
            if (CACHED_KEY != null) return CACHED_KEY;
            try {
                MessageDigest md = MessageDigest.getInstance("SHA-256");
                md.update("dobbyproject:securefile:v1".getBytes(StandardCharsets.UTF_8));
                md.update(safe(android.os.Build.MANUFACTURER).getBytes(StandardCharsets.UTF_8));
                md.update(safe(android.os.Build.MODEL).getBytes(StandardCharsets.UTF_8));
                md.update(("uid=" + android.os.Process.myUid()).getBytes(StandardCharsets.UTF_8));
                CACHED_KEY = new SecretKeySpec(md.digest(), "AES");
                return CACHED_KEY;
            } catch (Exception e) {
                throw new RuntimeException("key derive fail", e);
            }
        }
    }
    private static String safe(String s) { return s == null ? "" : s; }
}
