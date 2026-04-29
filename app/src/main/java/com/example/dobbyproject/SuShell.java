package com.example.dobbyproject;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.InputStreamReader;
import java.util.function.Consumer;
import java.util.function.Predicate;

/**
 * 统一的 root shell 执行工具。
 *
 * 收敛 MainActivity / GameLauncher 里 25+ 处重复的
 * {@code Runtime.getRuntime().exec("su") + DataOutputStream.writeBytes(...) +
 *  BufferedReader.readLine() + waitFor()} 模板。
 *
 * 所有方法:
 *  - 异常吞掉, 返回安全默认值 (匹配旧代码 try { ... } catch (Exception ignored) {} 风格)
 *  - 自动给脚本追加 "\nexit\n" 保证 su 终止 (重复 exit 无害)
 *  - 不捕获 stderr (旧代码绝大多数也只读 stdout)
 */
public final class SuShell {
    private SuShell() {}

    private static final String SU = "su";

    /** 执行 root shell, 不读 stdout, 返回 exit code; 异常返回 -1. */
    public static int run(String script) {
        try {
            Process p = Runtime.getRuntime().exec(SU);
            DataOutputStream os = new DataOutputStream(p.getOutputStream());
            os.writeBytes(script);
            os.writeBytes("\nexit\n");
            os.flush();
            return p.waitFor();
        } catch (Exception e) {
            return -1;
        }
    }

    /** 执行 root shell, 每行 stdout 调一次回调, 返回 exit code; 异常返回 -1. */
    public static int runWithLines(String script, Consumer<String> onLine) {
        try {
            Process p = Runtime.getRuntime().exec(SU);
            DataOutputStream os = new DataOutputStream(p.getOutputStream());
            os.writeBytes(script);
            os.writeBytes("\nexit\n");
            os.flush();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            while ((line = r.readLine()) != null) {
                onLine.accept(line);
            }
            return p.waitFor();
        } catch (Exception e) {
            return -1;
        }
    }

    /** 执行 root shell, 取首行 (trim 后非空); 无输出/异常返回 null. */
    public static String firstLine(String script) {
        try {
            Process p = Runtime.getRuntime().exec(SU);
            DataOutputStream os = new DataOutputStream(p.getOutputStream());
            os.writeBytes(script);
            os.writeBytes("\nexit\n");
            os.flush();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = r.readLine();
            p.waitFor();
            if (line == null) return null;
            line = line.trim();
            return line.isEmpty() ? null : line;
        } catch (Exception e) {
            return null;
        }
    }

    /** 执行 root shell, 任一行 contains(marker) 即返回 true. */
    public static boolean containsLine(String script, String marker) {
        return anyLineMatches(script, l -> l.contains(marker));
    }

    /** 执行 root shell, 任一行匹配 pred 即返回 true. */
    public static boolean anyLineMatches(String script, Predicate<String> pred) {
        try {
            Process p = Runtime.getRuntime().exec(SU);
            DataOutputStream os = new DataOutputStream(p.getOutputStream());
            os.writeBytes(script);
            os.writeBytes("\nexit\n");
            os.flush();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line;
            boolean hit = false;
            while ((line = r.readLine()) != null) {
                if (!hit && pred.test(line)) hit = true;
            }
            p.waitFor();
            return hit;
        } catch (Exception e) {
            return false;
        }
    }
}
