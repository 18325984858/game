package com.example.dobbyproject;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.security.KeyStore;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Superkey 加密读写存储。
 *
 * 文件路径：/data/data/<pkg>/files/.kp_key
 * 文件格式：magic(4) "KPK1" || iv(12) || ciphertext+tag(N+16)
 *           AES-256-GCM 加密 superkey UTF-8 字节
 *
 * 主密钥：Android Keystore 别名 "kp_superkey"
 *   存储：硬件 TEE / StrongBox（root 也无法导出原始密钥）
 *   生成：256-bit AES，仅用于 GCM 模式，自动产生
 *
 * 兼容性：读到旧明文文件（无 KPK1 magic）时自动透明迁移为密文重写
 *        （单次运行用户无感，下次启动文件已是密文）。
 */
public final class KpKeyStore {
    private static final String TAG = "[SFK-KpKey]";

    private KpKeyStore() {}

    private static final String KS_PROVIDER = "AndroidKeyStore";
    private static final String KEY_ALIAS   = "kp_superkey";
    private static final String AES_GCM     = "AES/GCM/NoPadding";
    private static final byte[] MAGIC       = { 'K', 'P', 'K', '1' };
    private static final int    IV_LEN      = 12;
    private static final int    TAG_BITS    = 128;

    public static File file(Context ctx) {
        return new File(ctx.getFilesDir(), ".kp_key");
    }

    private static SecretKey getOrCreateMasterKey() throws Exception {
        KeyStore ks = KeyStore.getInstance(KS_PROVIDER);
        ks.load(null);
        if (ks.containsAlias(KEY_ALIAS)) {
            return ((KeyStore.SecretKeyEntry) ks.getEntry(KEY_ALIAS, null)).getSecretKey();
        }
        KeyGenerator kg = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KS_PROVIDER);
        // 优先 StrongBox（独立安全芯片），失败回退普通 TEE
        try {
            KeyGenParameterSpec.Builder b = new KeyGenParameterSpec.Builder(
                    KEY_ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setRandomizedEncryptionRequired(true);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                try { b.setIsStrongBoxBacked(true); } catch (Throwable ignored) {}
            }
            kg.init(b.build());
            return kg.generateKey();
        } catch (Throwable t) {
            kg.init(new KeyGenParameterSpec.Builder(
                    KEY_ALIAS,
                    KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setRandomizedEncryptionRequired(true)
                .build());
            return kg.generateKey();
        }
    }

    private static byte[] encryptToBlob(String plaintext) throws Exception {
        Cipher c = Cipher.getInstance(AES_GCM);
        c.init(Cipher.ENCRYPT_MODE, getOrCreateMasterKey());
        byte[] iv = c.getIV();
        byte[] ct = c.doFinal(plaintext.getBytes("UTF-8"));
        byte[] out = new byte[MAGIC.length + iv.length + ct.length];
        System.arraycopy(MAGIC, 0, out, 0, MAGIC.length);
        System.arraycopy(iv,    0, out, MAGIC.length, iv.length);
        System.arraycopy(ct,    0, out, MAGIC.length + iv.length, ct.length);
        return out;
    }

    private static String decryptFromBlob(byte[] blob) throws Exception {
        if (blob == null || blob.length == 0) return null;
        // 旧明文兼容：没有 magic 则按 UTF-8 直读
        if (blob.length < MAGIC.length + IV_LEN + 1
                || blob[0] != MAGIC[0] || blob[1] != MAGIC[1]
                || blob[2] != MAGIC[2] || blob[3] != MAGIC[3]) {
            String s = new String(blob, "UTF-8").trim();
            return s.isEmpty() ? null : s;
        }
        byte[] iv = Arrays.copyOfRange(blob, MAGIC.length, MAGIC.length + IV_LEN);
        Cipher c = Cipher.getInstance(AES_GCM);
        c.init(Cipher.DECRYPT_MODE, getOrCreateMasterKey(), new GCMParameterSpec(TAG_BITS, iv));
        byte[] pt = c.doFinal(blob, MAGIC.length + IV_LEN, blob.length - MAGIC.length - IV_LEN);
        return new String(pt, "UTF-8");
    }

    /** 读取 superkey 明文。旧明文 → 自动迁移密文。失败返回 null。 */
    public static String read(Context ctx) {
        File f = file(ctx);
        if (!f.exists() || f.length() == 0 || f.length() > 4096) return null;
        try (FileInputStream fis = new FileInputStream(f)) {
            byte[] blob = new byte[(int) f.length()];
            int n = fis.read(blob);
            if (n <= 0) return null;
            if (n < blob.length) blob = Arrays.copyOf(blob, n);

            String pt = decryptFromBlob(blob);
            if (pt == null) return null;
            pt = pt.trim();
            if (pt.isEmpty()) return null;

            boolean isOldFormat = (n < MAGIC.length
                    || blob[0] != MAGIC[0] || blob[1] != MAGIC[1]
                    || blob[2] != MAGIC[2] || blob[3] != MAGIC[3]);
            if (isOldFormat) {
                Log.i(TAG, "migrate plaintext .kp_key -> encrypted (KPK1)");
                writeRaw(ctx, pt);
            }
            return pt;
        } catch (Throwable t) {
            Log.e(TAG, "read failed: " + t.getMessage(), t);
            return null;
        }
    }

    /** 写 superkey 到文件。自动加密。 */
    public static boolean write(Context ctx, String superkey) {
        if (superkey == null || superkey.isEmpty()) return false;
        return writeRaw(ctx, superkey);
    }

    private static boolean writeRaw(Context ctx, String superkey) {
        File f = file(ctx);
        try {
            byte[] blob = encryptToBlob(superkey);
            try (FileOutputStream fos = new FileOutputStream(f, false)) {
                fos.write(blob);
                fos.flush();
            }
            try { f.setReadable(false, false); f.setReadable(true, true); } catch (Throwable ignored) {}
            try { f.setWritable(false, false); f.setWritable(true, true); } catch (Throwable ignored) {}
            Log.i(TAG, "wrote encrypted .kp_key (" + blob.length + " bytes, plain len=" + superkey.length() + ")");
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "write failed: " + t.getMessage(), t);
            return false;
        }
    }

    /** 删除 .kp_key 文件（保留 Keystore 主密钥）。 */
    public static void clear(Context ctx) {
        try {
            File f = file(ctx);
            if (f.exists()) {
                f.delete();
                Log.i(TAG, "cleared .kp_key");
            }
        } catch (Throwable ignored) {}
    }

    /** 同时删除 Keystore 主密钥 + 密文文件，重置后无法再解密旧密文。 */
    public static void wipeAll(Context ctx) {
        clear(ctx);
        try {
            KeyStore ks = KeyStore.getInstance(KS_PROVIDER);
            ks.load(null);
            if (ks.containsAlias(KEY_ALIAS)) {
                ks.deleteEntry(KEY_ALIAS);
                Log.i(TAG, "deleted Keystore master key " + KEY_ALIAS);
            }
        } catch (Throwable t) {
            Log.e(TAG, "wipeAll keystore: " + t.getMessage(), t);
        }
    }

    /**
     * 没有 key 时跳回 MainActivity 让用户输入；返回 false 表示当前 Activity
     * 应立即退出。已存在 key 时通过反射调用 caller 的 nativeSetSuperkey()。
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
        } catch (Throwable ignored) {}
        return true;
    }
}
