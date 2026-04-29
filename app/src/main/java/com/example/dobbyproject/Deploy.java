package com.example.dobbyproject;

import java.io.FileOutputStream;
import java.io.InputStream;

/**
 * 把 InputStream 落到 tmp 文件再 root cp + chmod 到目标路径。
 * 收敛 extractLibFromApk / deployChineseFont 共用的 "Stream -> tmp -> su cp" 模板。
 */
public final class Deploy {
    private Deploy() {}

    /**
     * 把 is 内容写到 tmpPath, 再用 root 拷贝到 dstPath 并 chmod。
     * 调用方负责 is/zip 的关闭? 本方法读完后 close is。
     *
     * @param chmod 例如 "755" / "644"
     * @return true 全流程成功
     */
    public static boolean fromInputStream(InputStream is, String tmpPath, String dstPath, String chmod) {
        try {
            FileOutputStream fos = new FileOutputStream(tmpPath);
            byte[] buf = new byte[8192];
            int len;
            while ((len = is.read(buf)) > 0) fos.write(buf, 0, len);
            fos.close();
            is.close();
        } catch (Exception e) {
            MainActivity.LogUtil.e("[Deploy] tmp 写入失败 " + tmpPath + ": " + e.getMessage(), e);
            return false;
        }
        int exit = SuShell.run("cp -f " + tmpPath + " " + dstPath + "\nchmod " + chmod + " " + dstPath);
        if (exit != 0) {
            MainActivity.LogUtil.e("[Deploy] su cp 失败 exit=" + exit + " dst=" + dstPath, null);
            return false;
        }
        return true;
    }
}
