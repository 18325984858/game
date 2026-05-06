/*
 * ═══════════════════════════════════════════════════════════════════════
 *  DobbyProject - 注入流程说明
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  onCreate() 启动后依次执行:
 *
 *  1. setSelinuxPermissive()
 *     └─ su → setenforce 0  (设置 SELinux 为宽容模式)
 *
 *  2. fixInputPermission()
 *     └─ su → chmod 666 /dev/input/event*  (开放触摸输入设备权限)
 *
 *  3. writeFiletoTargetProgram()
 *     └─ su → cp libdobbyproject.so → /data/data/com.tencent.lolm/files/
 *     └─ chmod 777  (拷贝 SO 到目标应用目录)
 *
 *  4. launchAndInject()  [子线程]
 *     ├─ ls -la 检查 SO 文件是否就位
 *     ├─ su → cp libinjector.so → /data/local/tmp/injector + chmod 755
 *     ├─ su → am start 启动 LoL 进程 (备用: monkey)
 *     ├─ sleep 15s  (等待目标进程初始化)
 *     └─ su → /data/local/tmp/injector com.tencent.lolm <soPath>
 *            ├─ findPidByName() → pidof 查找目标 PID
 *            ├─ ptrace(ATTACH) → 附加到目标进程 (root)
 *            ├─ 远程 mmap → 在目标进程分配内存
 *            ├─ 远程 dlopen → 加载 libdobbyproject.so
 *            ├─ 远程 dlsym → 查找 _Z12MyStartPointPvS_S_S_S_
 *            ├─ 读取 libil2cpp.so 偏移处的 IL2CPP 元数据指针:
 *            │   +0xF45D838 → pCodeRegistration
 *            │   +0xF45D840 → pMetadataRegistration
 *            │   +0xF45D858 → pGlobalMetadataHeader
 *            │   +0x1D21140 → pMetadataImagesTable
 *            ├─ 远程调用 MyStartPoint(base, codeReg, metaReg, globalMeta, metaImages)
 *            ├─ 远程 munmap → 释放临时内存
 *            └─ ptrace(DETACH) → 恢复寄存器并分离
 *
 * ═══════════════════════════════════════════════════════════════════════
 */
package com.example.dobbyproject;

import androidx.appcompat.app.AppCompatActivity;
import android.os.Bundle;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ImageButton;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import android.view.View;
import android.view.ViewGroup;
import android.text.method.HideReturnsTransformationMethod;
import android.text.method.PasswordTransformationMethod;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.provider.Settings;

import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.InputStreamReader;
import java.io.InputStream;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.io.File;
import java.util.zip.ZipFile;
import java.util.zip.ZipEntry;

import android.app.AlertDialog;
public class MainActivity extends AppCompatActivity {

    // --- 1. LogUtil 保持不变 ---
    public static class LogUtil {
        private static final boolean DEBUG = true;
        private static final String TAG = "[SFK]";

        public static void d(String msg) {
            if (DEBUG) android.util.Log.d(TAG, msg);
        }

        public static void e(String msg, Throwable tr) {
            if (DEBUG) android.util.Log.e(TAG, msg, tr);
        }

        public static void i(String msg) {
            if (DEBUG) {
                // 这里 [3] 是正确的，对应调用 i() 的位置
                StackTraceElement element = Thread.currentThread().getStackTrace()[3];
                String info = "[" + element.getFileName() + ":" + element.getLineNumber() + "] ";
                android.util.Log.i(TAG, info + msg);
            }
        }
    }

    // --- 2. 静态块加载 SO ---
    static {
        System.loadLibrary("dobbyproject");
    }

    String g_packFileName = "com.tencent.lolm";

    String g_nativeLibPath = "";

    // 状态跟踪
    private boolean selinuxDone = false;
    private boolean inputPermDone = false;
    private boolean fontDone = false;
    private boolean launchDone = false;
    private TextView tvStatus;
    private CheckBox cbPubgDumper;
    private CheckBox cbPubgHeader;
    private CheckBox cbDfmDumper;
    private CheckBox cbDfmHeader;
    private CheckBox cbNrcDumper;
    private CheckBox cbNrcHeader;
    private static final String UE4_OVERLAY_STATUS = "UE4 公开 Overlay 已启动";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        // 立即隐藏主功能区，等异步 superkey 校验通过再显示。避免在校验完成
        // 前用户能操作 inject-hide / GameKpm 面板（那些面板会调 nativeKpmRawCtl
        // 触发 sc_hello 失败 → 反复弹"未检测到 Superkey"Toast）。
        setMainContentVisible(false);

        g_nativeLibPath = getApplicationContext().getApplicationInfo().nativeLibraryDir;

        // ── AI 屏幕检测: 解压模型并启动后台线程 (默认 disabled, 通过 ImGui 菜单开启) ──
        try {
            AIScreenDetect.init(getApplicationContext());
            // 必须 FIRST: 创建跨进程共享文件并设置 SELinux context.
            // 之后任何调用 AISharedData::getInstance() 的 native 方法 (setScoreThreshold/
            // setEnabled 等) 才能成功 mmap. 否则单例首次构造会因文件不存在而 m_hdr=null,
            // 永远不再 retry.
            AIScreenDetect.prepareSharedDets();
            AIScreenDetect.setUseGpu(true);
            AIScreenDetect.setUseSu(true);           // screencap 在 app uid 下被 SELinux 拒绝, 必须走 su
            AIScreenDetect.setIntervalMs(120);
            AIScreenDetect.setScoreThreshold(0.25f);
            AIScreenDetect.start();
            // 提前 chmod /dev/uinput, 启用 "AI 辅助瞄准(注入)" 时不再需要等待
            AIScreenDetect.prepareTouchInjection();
            // 临时: 启动即开启 AI 检测, 便于验证 screencap+infer 链路
            AIScreenDetect.setEnabled(true);
            LogUtil.i("[AI] AIScreenDetect 已启动 (默认开启, 用于诊断)");
        } catch (Throwable t) {
            LogUtil.e("AIScreenDetect 启动失败", t);
        }

        TextView tv = findViewById(R.id.sample_text);
        tv.setText(stringFromJNI());
        tvStatus = findViewById(R.id.tv_status);

        CheckBox cbDumper = findViewById(R.id.cb_dumper);
        CheckBox cbHeader = findViewById(R.id.cb_header);
        CheckBox cbLog = findViewById(R.id.cb_log);
        cbPubgDumper = findViewById(R.id.cb_pubg_dumper);
        cbPubgHeader = findViewById(R.id.cb_pubg_header);
        cbDfmDumper = findViewById(R.id.cb_dfm_dumper);
        cbDfmHeader = findViewById(R.id.cb_dfm_header);
        cbNrcDumper = findViewById(R.id.cb_nrc_dumper);
        cbNrcHeader = findViewById(R.id.cb_nrc_header);

        // ── 日志开关: 启动时从 C++ 端读取默认值, 改动时主动同步到 C++ ──
        try {
            cbLog.setChecked(nativeIsLogEnabled());
        } catch (Throwable t) {
            LogUtil.e("nativeIsLogEnabled 调用失败", t);
        }
        cbLog.setOnCheckedChangeListener((buttonView, isChecked) -> {
            try {
                nativeSetLogEnabled(isChecked);
            } catch (Throwable t) {
                LogUtil.e("nativeSetLogEnabled 调用失败", t);
            }
            LogUtil.i("[LogSwitch] UI -> C++ g_runtimeLogEnabled=" + isChecked);
        });

        // ── 折叠区域: ⚙ 环境准备 ──
        TextView tvEnvHeader = findViewById(R.id.tv_section_env_header);
        LinearLayout layoutEnvContent = findViewById(R.id.layout_env_content);
        if (tvEnvHeader != null && layoutEnvContent != null) {
            tvEnvHeader.setOnClickListener(v -> {
                if (layoutEnvContent.getVisibility() == View.VISIBLE) {
                    layoutEnvContent.setVisibility(View.GONE);
                    tvEnvHeader.setText("▶ ⚙ 环境准备 (宽容模式 / 输入权限 / 字体)");
                } else {
                    layoutEnvContent.setVisibility(View.VISIBLE);
                    tvEnvHeader.setText("▼ ⚙ 环境准备 (宽容模式 / 输入权限 / 字体)");
                }
            });
        }

        // ── 折叠区域: lol手游 ──
        TextView tvSectionHeader = findViewById(R.id.tv_section_lol_header);
        LinearLayout layoutLolContent = findViewById(R.id.layout_lol_content);
        tvSectionHeader.setOnClickListener(v -> {
            if (layoutLolContent.getVisibility() == View.VISIBLE) {
                layoutLolContent.setVisibility(View.GONE);
                tvSectionHeader.setText("▶ lol手游");
            } else {
                layoutLolContent.setVisibility(View.VISIBLE);
                tvSectionHeader.setText("▼ lol手游");
            }
        });

        // ── 折叠区域: 和平精英 ──
        TextView tvPubgHeader = findViewById(R.id.tv_section_pubg_header);
        LinearLayout layoutPubgContent = findViewById(R.id.layout_pubg_content);
        tvPubgHeader.setOnClickListener(v -> {
            if (layoutPubgContent.getVisibility() == View.VISIBLE) {
                layoutPubgContent.setVisibility(View.GONE);
                tvPubgHeader.setText("▶ 和平精英");
            } else {
                layoutPubgContent.setVisibility(View.VISIBLE);
                tvPubgHeader.setText("▼ 和平精英");
            }
        });

        // ── 折叠区域: 三角洲 ──
        TextView tvDfmHeader = findViewById(R.id.tv_section_dfm_header);
        LinearLayout layoutDfmContent = findViewById(R.id.layout_dfm_content);
        tvDfmHeader.setOnClickListener(v -> {
            if (layoutDfmContent.getVisibility() == View.VISIBLE) {
                layoutDfmContent.setVisibility(View.GONE);
                tvDfmHeader.setText("▶ 三角洲");
            } else {
                layoutDfmContent.setVisibility(View.VISIBLE);
                tvDfmHeader.setText("▼ 三角洲");
            }
        });

        // ── 折叠区域: 洛克王国手游 (NRC) ──
        TextView tvNrcHeader = findViewById(R.id.tv_section_nrc_header);
        LinearLayout layoutNrcContent = findViewById(R.id.layout_nrc_content);
        tvNrcHeader.setOnClickListener(v -> {
            if (layoutNrcContent.getVisibility() == View.VISIBLE) {
                layoutNrcContent.setVisibility(View.GONE);
                tvNrcHeader.setText("▶ 洛克王国手游");
            } else {
                layoutNrcContent.setVisibility(View.VISIBLE);
                tvNrcHeader.setText("▼ 洛克王国手游");
            }
        });

        Button btnSelinux = findViewById(R.id.btn_selinux);
        Button btnInputPerm = findViewById(R.id.btn_input_perm);
        Button btnFont = findViewById(R.id.btn_deploy_font);
        Button btnLaunch = findViewById(R.id.btn_launch);
        Button btnPubgLaunch = findViewById(R.id.btn_pubg_launch);
        Button btnDfmLaunch = findViewById(R.id.btn_dfm_launch);
        Button btnNrcLaunch = findViewById(R.id.btn_nrc_launch);
        Button btnSoDumper = findViewById(R.id.btn_so_dumper);

        // ── SO Dumper 入口 ──
        btnSoDumper.setOnClickListener(v -> {
            Intent soDumperIntent = new Intent(this, SoDumperActivity.class);
            startActivity(soDumperIntent);
        });

        // ── Memory Reader 入口 ──
        Button btnMemReader = findViewById(R.id.btn_mem_reader);
        btnMemReader.setOnClickListener(v -> {
            Intent memIntent = new Intent(this, MemoryReaderActivity.class);
            startActivity(memIntent);
        });

        // ── Inject-Hide KPM 管理入口 ──
        Button btnInjectHide = findViewById(R.id.btn_inject_hide);
        btnInjectHide.setOnClickListener(v -> {
            Intent ihIntent = new Intent(this, InjectHideActivity.class);
            startActivity(ihIntent);
        });

        // ── ⚙ GameKpm 高级设置面板 ──
        wireGameKpmAdvancedPanel();

        // ── 和平精英启动按钮 ──
        CheckBox cbPubgAntiCheat = findViewById(R.id.cb_pubg_anticheat);
        wireGameButton(btnPubgLaunch, GAME_PUBG, "和平精英",
                cbPubgDumper, cbPubgHeader, cbLog, "[UE4 Dumper]", "[UE4 Header]",
                cbPubgAntiCheat, "pubgmhd");

        // ── 三角洲启动按钮 ──
        CheckBox cbDfmAntiCheat = findViewById(R.id.cb_dfm_anticheat);
        wireGameButton(btnDfmLaunch, GAME_DFM, "三角洲",
                cbDfmDumper, cbDfmHeader, cbLog, "[UE5 Dumper]", "[UE5 Header]",
                cbDfmAntiCheat, "dfm");

        // ── 洛克王国手游 (NRC) 启动按钮 ── (无反检测需求)
        wireGameButton(btnNrcLaunch, GAME_NRC, "洛克王国手游",
                cbNrcDumper, cbNrcHeader, cbLog, "[UE4 Dumper]", null);

        // ── 启动时初始化检测 ──
        updateStatus("正在检测环境...");
        new Thread(() -> {
            boolean rootOk = checkRootAccess();
            if (!rootOk) {
                runOnUiThread(() -> {
                    findViewById(android.R.id.content).setVisibility(View.INVISIBLE);
                    new AlertDialog.Builder(this)
                            .setTitle("Root 权限不可用")
                            .setMessage("本应用需要 Root 权限才能正常运行。\n请确保设备已 Root 并授予本应用 Root 权限后重新打开。")
                            .setPositiveButton("退出", (dialog, which) -> finish())
                            .setCancelable(false)
                            .show();
                });
                return;
            }

            // ── Root 校验通过后：验证 KernelPatch superkey ──
            // 唯一真源: /data/data/<pkg>/files/.kp_key (app 私有, 不需 root)。
            // 缺失或验证失败 → 删除 + 弹输入框。
            String savedKey = readKpKeyFromFiles();
            boolean keyOk = false;
            String promptMsg = "未检测到 Superkey，请输入 APatch Super Key：";
            if (savedKey != null) {
                try { keyOk = nativeValidateKpKey(savedKey); }
                catch (Throwable t) { LogUtil.e("nativeValidateKpKey 调用异常", t); }
                if (!keyOk) {
                    deleteKpKeyFile();
                    promptMsg = "❌ 保存的 Superkey 已失效，请重新输入 APatch Super Key：";
                }
            }
            final boolean keyOkFinal = keyOk;
            final String promptMsgFinal = promptMsg;
            runOnUiThread(() -> {
                if (keyOkFinal) {
                    // key 有效：彻底隐藏这块 UI
                    View layoutKp = findViewById(R.id.layout_kpkey);
                    if (layoutKp != null) layoutKp.setVisibility(View.GONE);
                    setMainContentVisible(true);
                } else {
                    setMainContentVisible(false);
                    showKpKeyPrompt(promptMsgFinal);
                }
            });

            boolean selinuxOk = checkSelinuxPermissive();
            boolean inputOk = checkInputPermission();
            boolean fontOk = checkFileExists("/data/local/tmp/chinese.ttf");
            boolean gameInstalled = checkGameInstalled();
            boolean pubgInstalled = checkPackageInstalled(PUBG_PACKAGE);
            boolean dfmInstalled = checkPackageInstalled(DFM_PACKAGE);
            boolean nrcInstalled = checkPackageInstalled(NRC_PACKAGE);

            runOnUiThread(() -> {
                StringBuilder sb = new StringBuilder();
                if (selinuxOk) {
                    selinuxDone = true;
                    btnSelinux.setText("✅ 宽容模式已设置");
                    btnSelinux.setEnabled(false);
                    sb.append("宽容模式 ✓  ");
                }
                if (inputOk) {
                    inputPermDone = true;
                    btnInputPerm.setText("✅ 输入权限已修改");
                    btnInputPerm.setEnabled(false);
                    sb.append("输入权限 ✓  ");
                }
                if (fontOk) {
                    fontDone = true;
                    btnFont.setText("✅ 字体已部署");
                    btnFont.setEnabled(false);
                    sb.append("字体 ✓  ");
                }
                if (!gameInstalled) {
                    btnLaunch.setEnabled(false);
                    btnLaunch.setText("⚠ 游戏未安装");
                    sb.append("LOL未安装 ✗  ");
                } else {
                    sb.append("LOL已安装 ✓  ");
                }
                if (!pubgInstalled) {
                    btnPubgLaunch.setEnabled(false);
                    btnPubgLaunch.setText("⚠ 游戏未安装");
                    sb.append("和平精英未安装 ✗  ");
                } else {
                    sb.append("和平精英已安装 ✓  ");
                }
                if (!dfmInstalled) {
                    btnDfmLaunch.setEnabled(false);
                    btnDfmLaunch.setText("⚠ 游戏未安装");
                    sb.append("三角洲未安装 ✗  ");
                } else {
                    sb.append("三角洲已安装 ✓  ");
                }
                if (!nrcInstalled) {
                    btnNrcLaunch.setEnabled(false);
                    btnNrcLaunch.setText("⚠ 游戏未安装");
                    sb.append("洛克王国手游未安装 ✗");
                } else {
                    sb.append("洛克王国手游已安装 ✓");
                }
                updateStatus(sb.length() > 0 ? sb.toString().trim() : "就绪");
            });
        }).start();

        // ① 设置宽容模式
        btnSelinux.setOnClickListener(v -> {
            if (selinuxDone) {
                Toast.makeText(this, "已设置过宽容模式，无需重复操作", Toast.LENGTH_SHORT).show();
                return;
            }
            btnSelinux.setEnabled(false);
            setSelinuxPermissive();
            selinuxDone = true;
            btnSelinux.setText("✅ 宽容模式已设置");
            updateStatus("宽容模式 ✓");
            Toast.makeText(this, "宽容模式已设置", Toast.LENGTH_SHORT).show();
        });

        // ② 修改输入设备权限
        btnInputPerm.setOnClickListener(v -> {
            if (inputPermDone) {
                Toast.makeText(this, "输入权限已修改过，无需重复操作", Toast.LENGTH_SHORT).show();
                return;
            }
            btnInputPerm.setEnabled(false);
            fixInputPermission();
            inputPermDone = true;
            btnInputPerm.setText("✅ 输入权限已修改");
            updateStatus("输入权限 ✓");
            Toast.makeText(this, "输入设备权限已修改", Toast.LENGTH_SHORT).show();
        });

        // ③ 部署中文字体
        btnFont.setOnClickListener(v -> {
            if (fontDone) {
                Toast.makeText(this, "字体已部署过，无需重复操作", Toast.LENGTH_SHORT).show();
                return;
            }
            btnFont.setEnabled(false);
            deployChineseFont();
            fontDone = true;
            btnFont.setText("✅ 字体已部署");
            updateStatus("字体部署 ✓");
            Toast.makeText(this, "字体部署完成", Toast.LENGTH_SHORT).show();
        });

        // ④ 启动游戏
        btnLaunch.setOnClickListener(v -> {
            if (launchDone) {
                Toast.makeText(this, "游戏已启动过，请勿重复注入", Toast.LENGTH_SHORT).show();
                return;
            }
            // 初始化检测: 确保前置步骤已完成
            if (!selinuxDone) {
                Toast.makeText(this, "请先设置宽容模式", Toast.LENGTH_SHORT).show();
                return;
            }

            btnLaunch.setEnabled(false);
            launchDone = true;

            boolean enableDumper = cbDumper.isChecked();
            boolean enableHeader = cbHeader.isChecked();
            boolean enableLog = cbLog.isChecked();

            // 清除旧的完成标记
            clearDumpMarkers();

            writeFiletoTargetProgram();
            launchAndInject(enableDumper, enableHeader, enableLog);

            // 如果勾选了 dump，启动后台轮询等待完成
            if (enableDumper || enableHeader) {
                pollDumpCompletion(enableDumper, enableHeader);
            }

            String options = "";
            if (enableDumper) options += " [Dumper]";
            if (enableHeader) options += " [Header]";
            btnLaunch.setText("✅ 游戏已启动" + options);
            updateStatus("游戏启动中..." + options);
            Toast.makeText(this, "正在启动游戏并注入..." + options, Toast.LENGTH_SHORT).show();
        });
    }

    private void updateStatus(String msg) {
        if (tvStatus != null) tvStatus.setText(msg);
    }

    private boolean ensureOverlayPermission() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || Settings.canDrawOverlays(this)) {
            return true;
        }

        Intent intent = new Intent(
                Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                Uri.parse("package:" + getPackageName())
        );
        startActivity(intent);
        Toast.makeText(this, "请先授予悬浮窗权限", Toast.LENGTH_LONG).show();
        return false;
    }

    private void startUe4OverlayService() {
        startUe4OverlayService(null);
    }

    private void startUe4OverlayService(String gamePkg) {
        Intent intent = new Intent(this, Ue4OverlayService.class);
        intent.setAction(Ue4OverlayService.ACTION_START);
        if (gamePkg != null) {
            intent.putExtra(Ue4OverlayService.EXTRA_PACKAGE, gamePkg);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForegroundService(intent);
        } else {
            startService(intent);
        }
    }

    private void stopUe4OverlayService() {
        Intent intent = new Intent(this, Ue4OverlayService.class);
        stopService(intent);
    }

    /**
     * 清除旧的 dump 完成标记文件
     */
    private void clearDumpMarkers() {
        SuShell.run("rm -f /data/local/tmp/dobby_dumper_done /data/local/tmp/dobby_header_done");
    }

    /**
     * 后台轮询 dump 完成标记文件，完成后在 UI 线程弹 Toast
     */
    private void pollDumpCompletion(boolean waitDumper, boolean waitHeader) {
        new Thread(() -> {
            boolean dumperDone = !waitDumper;
            boolean headerDone = !waitHeader;
            int maxWait = 300; // 最多等 5 分钟 (300 × 1s)

            for (int i = 0; i < maxWait && (!dumperDone || !headerDone); i++) {
                try { Thread.sleep(1000); } catch (InterruptedException ignored) { return; }

                if (!dumperDone) {
                    dumperDone = checkFileExists("/data/local/tmp/dobby_dumper_done");
                    if (dumperDone) {
                        runOnUiThread(() -> {
                            Toast.makeText(this, "✓ il2cppDumper 导出完成！", Toast.LENGTH_LONG).show();
                            updateStatus("Dumper 导出完成 ✓");
                        });
                    }
                }
                if (!headerDone) {
                    headerDone = checkFileExists("/data/local/tmp/dobby_header_done");
                    if (headerDone) {
                        runOnUiThread(() -> {
                            Toast.makeText(this, "✓ il2cppHeader 导出完成！", Toast.LENGTH_LONG).show();
                            updateStatus("Header 导出完成 ✓");
                        });
                    }
                }
            }

            if (dumperDone && headerDone) {
                runOnUiThread(() -> {
                    String msg = "全部导出完成 ✓";
                    updateStatus(msg);
                    Toast.makeText(this, msg, Toast.LENGTH_LONG).show();
                });
            }
        }).start();
    }

    private boolean checkFileExists(String path) {
        return SuShell.containsLine("test -f " + path + " && echo YES", "YES");
    }

    /**
     * 检测设备是否已授予 Root 权限。
     *
     * 不同 root 方案 / 反检测配置下, su 二进制可能被改名:
     *   - 标准: /system/bin/su
     *   - APatch 防检测: /system/bin/kp (或用户自定义)
     *   - KernelSU: /system/bin/ksu (少见)
     *
     * 因此遍历候选列表, 任意一个返回 uid=0 即认为 root 可用。
     * 命中后把名字缓存到 sRootBinary 供后续 exec 复用。
     */
    private static volatile String sRootBinary = null;
    static String getRootBinary() { return sRootBinary != null ? sRootBinary : "su"; }
    private boolean checkRootAccess() {
        // 先尝试缓存的, 再尝试常见候选
        String[] candidates = sRootBinary != null
                ? new String[]{ sRootBinary }
                : new String[]{ "su", "kp", "/system/bin/su", "/system/bin/kp",
                                "/system/xbin/su", "ksu", "magisk" };
        for (String bin : candidates) {
            try {
                Process p = Runtime.getRuntime().exec(new String[]{ bin, "-c", "id" });
                BufferedReader reader = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line = reader.readLine();
                int exitCode = p.waitFor();
                if (exitCode == 0 && line != null && line.contains("uid=0")) {
                    sRootBinary = bin;
                    android.util.Log.i("RootCheck", "root via: " + bin + " -> " + line);
                    return true;
                }
            } catch (Exception ignored) {}
        }
        android.util.Log.w("RootCheck", "no working su binary among candidates");
        return false;
    }

    /**
     * 检测 SELinux 是否已经是 Permissive 模式
     */
    private boolean checkSelinuxPermissive() {
        return SuShell.anyLineMatches("getenforce", l -> l.trim().equalsIgnoreCase("Permissive"));
    }

    /**
     * 检测 /dev/input/event* 是否已有 666 权限 (other 可读写)
     */
    private boolean checkInputPermission() {
        return SuShell.containsLine("ls -l /dev/input/event0 | grep -q 'crw-rw-rw' && echo OK", "OK");
    }

    /**
     * 检测目标游戏是否已安装（通过 root 权限绕过 Android 11+ 包可见性限制）
     */
    private boolean checkGameInstalled() {
        return checkPackageInstalled(g_packFileName);
    }

    private boolean checkPackageInstalled(String packageName) {
        return SuShell.containsLine(
                "pm list packages " + packageName + " | grep -q " + packageName + " && echo INSTALLED",
                "INSTALLED");
    }

    private String runSuFirstLine(String cmd) {
        return SuShell.firstLine(cmd);
    }

    private String resolvePackageNativeLibDir(String packageName) {
        String dir = runSuFirstLine("dumpsys package " + packageName + " 2>/dev/null | sed -n 's/.*nativeLibraryDir=//p' | head -n 1");
        if (dir != null && dir.startsWith("/")) return dir;

        dir = runSuFirstLine("apk=$(pm path " + packageName + " 2>/dev/null | sed -n 's/package://p' | head -n 1); " +
                "if [ -n \"$apk\" ]; then d=$(dirname \"$apk\")/lib/arm64; [ -d \"$d\" ] && echo \"$d\"; fi");
        return (dir != null && dir.startsWith("/")) ? dir : null;
    }

    public native String stringFromJNI();
    public native int injectSoToTarget(String packageName, String soPath);
    public native String nativeKpmRawCtl(String cmd);

    /** 向 game-kpm 发送 control0 命令（GameKpm 反作弊 KPM）。返回 "OK: ..." / "FAIL: ..."。 */
    public native String nativeGameKpmCtl(String cmd);

    /** 加载 KPM (root supercall)。path = 设备绝对路径，args 可空。 */
    public native boolean nativeKpmLoad(String path, String args);

    /** 卸载 KPM (root supercall)。name = KPM_NAME，例如 "game-kpm"。 */
    public native boolean nativeKpmUnload(String name);

    /** 查询 game-kpm 是否已加载。 */
    public native boolean nativeGameKpmIsLoaded();

    /** 查询 kpm-svc (inject-hide) 是否已加载。 */
    public native boolean nativeKpmSvcIsLoaded();

    /**
     * 校验 KernelPatch superkey：空串表示用 /data/local/tmp/.kp_key 等文件里的 key。
     * 成功时 native 层会把 key 缓存为当前 superkey，后续 InjectHideCtl 直接可用。
     * @return true = sc_hello 成功（key 正确）
     */
    public native boolean nativeValidateKpKey(String key);

    // ─── 日志运行时开关 (Java <-> C++ 双向同步) ──────────────────────
    /** 读取 C++ 端 g_runtimeLogEnabled 当前值 (用于初始化 UI) */
    public static native boolean nativeIsLogEnabled();
    /** 把 UI 状态同步到 C++ 端 g_runtimeLogEnabled */
    public static native void nativeSetLogEnabled(boolean enabled);

    // ─── KernelPatch Superkey UI / 持久化 ────────────────────────────
    /**
     * 隐藏/显示除 layout_kpkey 之外的主界面控件，实现 Key 未验证时单独一页展示输入框。
     */
    private void setMainContentVisible(boolean visible) {
        View layoutKp = findViewById(R.id.layout_kpkey);
        if (layoutKp == null) return;
        ViewGroup parent = (ViewGroup) layoutKp.getParent();
        if (parent == null) return;
        int vis = visible ? View.VISIBLE : View.GONE;
        for (int i = 0; i < parent.getChildCount(); i++) {
            View child = parent.getChildAt(i);
            if (child.getId() != R.id.layout_kpkey) {
                child.setVisibility(vis);
            }
        }
    }

    /**
     * 展开 KPM key 输入控件，prompt 作为提示文案。首次以及验证失败都会调用。
     */
    private void showKpKeyPrompt(String prompt) {
        LinearLayout layoutKp = findViewById(R.id.layout_kpkey);
        TextView tvHint = findViewById(R.id.tv_kpkey_hint);
        EditText etKey = findViewById(R.id.et_kpkey);
        Button btnSubmit = findViewById(R.id.btn_kpkey_submit);
        ImageButton btnShow = findViewById(R.id.btn_kpkey_show);
        if (layoutKp == null || etKey == null || btnSubmit == null) return;

        layoutKp.setVisibility(View.VISIBLE);
        if (tvHint != null && prompt != null) tvHint.setText(prompt);
        etKey.setText("");

        // 眼睛图标切换显示/隐藏密码
        if (btnShow != null) {
            final boolean[] shown = {false};
            etKey.setTransformationMethod(PasswordTransformationMethod.getInstance());
            btnShow.setImageResource(R.drawable.ic_eye_off);
            btnShow.setOnClickListener(v -> {
                shown[0] = !shown[0];
                etKey.setTransformationMethod(shown[0]
                        ? HideReturnsTransformationMethod.getInstance()
                        : PasswordTransformationMethod.getInstance());
                btnShow.setImageResource(shown[0] ? R.drawable.ic_eye : R.drawable.ic_eye_off);
                etKey.setSelection(etKey.getText().length());
            });
        }

        btnSubmit.setOnClickListener(v -> {
            String key = etKey.getText().toString().trim();
            if (key.isEmpty()) {
                Toast.makeText(this, "Super Key 不能为空", Toast.LENGTH_SHORT).show();
                return;
            }
            btnSubmit.setEnabled(false);
            new Thread(() -> {
                boolean ok = false;
                try { ok = nativeValidateKpKey(key); }
                catch (Throwable t) { LogUtil.e("nativeValidateKpKey", t); }
                final boolean fok = ok;
                if (fok) saveKpKeyToFile(key);   // 写入 files/.kp_key
                runOnUiThread(() -> {
                    btnSubmit.setEnabled(true);
                    if (fok) {
                        layoutKp.setVisibility(View.GONE);
                        setMainContentVisible(true);
                        Toast.makeText(this, "Super Key 验证通过，已保存", Toast.LENGTH_SHORT).show();
                    } else {
                        if (tvHint != null) {
                            tvHint.setText("❌ Key 不正确，请重新输入 APatch Super Key：");
                        }
                        etKey.setText("");
                        Toast.makeText(this, "Super Key 不正确，请重试", Toast.LENGTH_SHORT).show();
                    }
                });
            }).start();
        });
    }

    /**
     * App 私有目录下的 superkey 文件路径：
     *   /data/data/<package>/files/.kp_key
     * 只有本 app (同 uid) 能读写；不需要 root 权限；clear data 才会被清。
     */
    private File getKpKeyFile() {
        return new File(getFilesDir(), ".kp_key");
    }

    /** 从 files/.kp_key 读出 key；不存在或读失败返回 null。 */
    private String readKpKeyFromFiles() {
        // 走 KpKeyStore （AES-256-GCM 加密 + Android Keystore 主密钥）
        // 旧明文文件会在 read 返回后被静默迫到密文重写。
        return KpKeyStore.read(this);
    }

    /**
     * 把验证成功的 key 保存到 App 自己的缓存目录。
     * 不需要 root；该路径只有本 app 可访问（uid 隔离）。
     */
    private void saveKpKeyToFile(String key) {
        // 走 KpKeyStore 加密写入 (AES-256-GCM + Android Keystore)
        boolean ok = KpKeyStore.write(this, key);
        LogUtil.i("saveKpKeyToFile encrypted -> " + KpKeyStore.file(this).getAbsolutePath()
                + " plain_len=" + key.length() + " ok=" + ok);
    }

    /** 删除 files/.kp_key (验证失败时调用)。 */
    private void deleteKpKeyFile() {
        KpKeyStore.clear(this);
    }

    /**
     * 获取真实内核架构 (uname -m), 不受 ARM 翻译层影响
     */
    private String getKernelArch() {
        try {
            Process p = Runtime.getRuntime().exec("uname -m");
            BufferedReader reader = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String line = reader.readLine();
            p.waitFor();
            if (line != null) {
                String arch = line.trim();
                LogUtil.i("[架构] 内核架构 (uname -m): " + arch);
                return arch;
            }
        } catch (Exception ignored) {}
        return "unknown";
    }

    /**
     * 将内核架构名映射为 Android ABI 名
     */
    private String kernelArchToAbi(String kernelArch, boolean is32bit) {
        if (kernelArch.contains("x86_64") || kernelArch.contains("amd64")) {
            return is32bit ? "x86" : "x86_64";
        }
        if (kernelArch.contains("x86") || kernelArch.contains("i686") || kernelArch.contains("i386")) {
            return "x86";
        }
        if (kernelArch.contains("aarch64") || kernelArch.contains("armv8")) {
            return is32bit ? "armeabi-v7a" : "arm64-v8a";
        }
        if (kernelArch.contains("arm")) {
            return "armeabi-v7a";
        }
        return "unknown";
    }

    /**
     * 检测目标进程的实际运行架构 (从 /proc/pid/maps 中加载的 libc 路径判断)
     * lib/ = 32-bit, lib64/ = 64-bit
     */
    private boolean isTargetProcess32Bit(String packageName) {
        // 主探测: 看加载的 libc.so 路径在 lib 还是 lib64
        boolean[] result = { false };
        boolean[] decided = { false };
        SuShell.runWithLines("cat /proc/$(pidof " + packageName + ")/maps | grep libc.so | head -1", line -> {
            LogUtil.i("[架构] 目标 libc 映射: " + line);
            if (decided[0]) return;
            if (line.contains("/lib64/")) { result[0] = false; decided[0] = true; }
            else if (line.contains("/lib/"))  { result[0] = true;  decided[0] = true; }
        });
        if (decided[0]) return result[0];

        // 回退: 检查地址范围
        SuShell.runWithLines("head -3 /proc/$(pidof " + packageName + ")/maps", line -> {
            if (decided[0]) return;
            String addr = line.split("-")[0].trim();
            if (addr.length() > 8) { result[0] = false; decided[0] = true; }
        });
        if (decided[0]) return result[0];
        return true;
    }

    /**
     * 从 APK 中提取指定架构的 native 库到指定路径
     */
    private boolean extractLibFromApk(String abi, String libName, String destPath) {
        String apkPath = getApplicationInfo().sourceDir;
        String entryName = "lib/" + abi + "/" + libName;
        LogUtil.i("[提取] 从 APK 提取: " + entryName);
        try {
            ZipFile zip = new ZipFile(apkPath);
            ZipEntry entry = zip.getEntry(entryName);
            if (entry == null) {
                LogUtil.e("[提取] APK 中不存在: " + entryName, null);
                zip.close();
                return false;
            }
            String tmpPath = getCacheDir() + "/" + abi + "_" + libName;
            boolean ok = Deploy.fromInputStream(zip.getInputStream(entry), tmpPath, destPath, "755");
            zip.close();
            if (ok) LogUtil.i("[提取] ✓ 已部署: " + destPath);
            return ok;
        } catch (Exception e) {
            LogUtil.e("[提取] 失败: " + e.getMessage(), e);
            return false;
        }
    }

    /**
     * 检测目标进程是否正在运行
     */
    private boolean isProcessRunning(String packageName) {
        return SuShell.containsLine("pidof " + packageName + " && echo RUNNING", "RUNNING");
    }

    public void setSelinuxPermissive() {
        int exitCode = SuShell.run("setenforce 0");
        if (exitCode == 0) {
            LogUtil.i("SELinux 已设置为宽容模式 (Permissive)");
        } else {
            LogUtil.e("设置 SELinux 宽容模式失败，错误码: " + exitCode, null);
        }
    }

    public void fixInputPermission() {
        int exitCode = SuShell.run("chmod 666 /dev/input/event*");
        if (exitCode == 0) {
            LogUtil.i("输入设备权限修改成功");
        } else {
            LogUtil.e("输入设备权限修改失败，错误码: " + exitCode, null);
        }
    }

    public void launchAndInject(boolean enableDumper, boolean enableHeader, boolean enableLog) {
        String soPath = "/data/data/" + g_packFileName + "/files/libdobbyproject.so";
        String injectorSrc = g_nativeLibPath + (g_nativeLibPath.endsWith("/") ? "" : "/") + "libinjector.so";
        String injectorDst = "/data/local/tmp/injector";

        new Thread(() -> {
            // 检查 SO 文件是否存在
            LogUtil.i("[注入流程] 检查 SO 文件: " + soPath);
            SuShell.runWithLines("ls -la " + soPath,
                    l -> LogUtil.i("[注入流程] SO 文件信息: " + l));

            // 部署 injector 可执行文件
            LogUtil.i("[注入流程] 部署 injector: " + injectorSrc + " -> " + injectorDst);
            SuShell.run("cp -f " + injectorSrc + " " + injectorDst + "\nchmod 755 " + injectorDst);
            LogUtil.i("[注入流程] injector 部署完成");

            // 启动目标应用
            LogUtil.i("[注入流程] 正在启动目标应用: " + g_packFileName);
            boolean appLaunched = false;

            // 方式1: monkey 启动 (最通用, 无需知道 Activity 名称)
            if (!appLaunched) {
                try {
                    LogUtil.i("[注入流程] 尝试 monkey 启动...");
                    SuShell.runWithLines(
                            "monkey -p " + g_packFileName + " -c android.intent.category.LAUNCHER 1 2>/dev/null",
                            l -> LogUtil.i("[注入流程] monkey: " + l));
                    Thread.sleep(2000);
                    if (isProcessRunning(g_packFileName)) {
                        appLaunched = true;
                        LogUtil.i("[注入流程] ✓ monkey 启动成功");
                    } else {
                        LogUtil.i("[注入流程] monkey 后进程未出现, 尝试其他方式...");
                    }
                } catch (Exception e) {
                    LogUtil.e("[注入流程] monkey 启动异常: " + e.getMessage(), e);
                }
            }

            // 方式2: am start 指定 Activity (适用于已知 Activity 的场景)
            if (!appLaunched) {
                try {
                    LogUtil.i("[注入流程] 尝试 am start 启动...");
                    boolean[] hasError = { false };
                    SuShell.runWithLines(
                            "am start -n " + g_packFileName + "/com.riotgames.league.RiotNativeActivity 2>&1",
                            l -> {
                                LogUtil.i("[注入流程] am start: " + l);
                                if (l.contains("Error") || l.contains("error")) hasError[0] = true;
                            });
                    if (!hasError[0]) {
                        Thread.sleep(2000);
                        if (isProcessRunning(g_packFileName)) {
                            appLaunched = true;
                            LogUtil.i("[注入流程] ✓ am start 启动成功");
                        }
                    }
                } catch (Exception e) {
                    LogUtil.e("[注入流程] am start 异常: " + e.getMessage(), e);
                }
            }

            // 方式3: am start 不指定 Activity, 用 launcher intent
            if (!appLaunched) {
                try {
                    LogUtil.i("[注入流程] 尝试 am start launcher intent...");
                    SuShell.runWithLines(
                            "am start -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n $(cmd package resolve-activity --brief " + g_packFileName + " 2>/dev/null | tail -1) 2>&1 || am start -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -p " + g_packFileName + " 2>&1",
                            l -> LogUtil.i("[注入流程] launch: " + l));
                    Thread.sleep(2000);
                    if (isProcessRunning(g_packFileName)) {
                        appLaunched = true;
                        LogUtil.i("[注入流程] ✓ launcher intent 启动成功");
                    }
                } catch (Exception e) {
                    LogUtil.e("[注入流程] launcher intent 异常: " + e.getMessage(), e);
                }
            }

            if (!appLaunched) {
                LogUtil.e("[注入流程] ✗ 所有启动方式均失败!", null);
            }

            // 等待 15 秒让目标进程完成初始化
            LogUtil.i("[注入流程] 等待 15 秒让目标进程初始化...");
            try { Thread.sleep(15000); } catch (InterruptedException ignored) {}

            // ── 检测目标进程架构, 部署匹配的 SO 和 injector ──
            // 用内核架构(uname -m)而非 nativeLibPath 来判断, 避免 ARM 翻译层误导
            String kernelArch = getKernelArch();
            boolean targetIs32 = false;
            try {
                targetIs32 = isTargetProcess32Bit(g_packFileName);
            } catch (Exception e) {
                LogUtil.e("[注入流程] 检测目标架构异常: " + e.getMessage(), e);
            }
            String targetAbi = kernelArchToAbi(kernelArch, targetIs32);
            // injector 必须匹配目标进程架构 (ptrace 要求同架构)
            String injectorAbi = targetAbi;
            LogUtil.i("[注入流程] 内核=" + kernelArch + ", 目标32位=" + targetIs32 + ", 目标ABI=" + targetAbi);

            // 判断当前已部署的 injector/SO 架构是否匹配
            // nativeLibPath 反映的是 App 自身使用的 ABI
            String appAbi = "unknown";
            if (g_nativeLibPath.contains("x86_64")) appAbi = "x86_64";
            else if (g_nativeLibPath.contains("x86")) appAbi = "x86";
            else if (g_nativeLibPath.contains("arm64")) appAbi = "arm64-v8a";
            else if (g_nativeLibPath.contains("armeabi")) appAbi = "armeabi-v7a";
            LogUtil.i("[注入流程] App ABI=" + appAbi + ", 需要ABI=" + targetAbi);

            if (!targetAbi.equals(appAbi) && !targetAbi.equals("unknown")) {
                LogUtil.i("[注入流程] 架构不匹配, 从 APK 提取 " + targetAbi + " 版本...");
                // 提取并部署匹配架构的 injector
                extractLibFromApk(targetAbi, "libinjector.so", injectorDst);
                // 提取并部署匹配架构的 libdobbyproject.so
                extractLibFromApk(targetAbi, "libdobbyproject.so", soPath);
            }

            // 通过 su 执行 injector (root 权限, 可以 ptrace)
            LogUtil.i("[注入流程] 以 root 身份执行 injector...");

            // 写入配置文件，告知 C++ 层是否启用 Dumper/Header
            String cfgContent = "dumper=" + (enableDumper ? "1" : "0") + "\n"
                              + "header=" + (enableHeader ? "1" : "0") + "\n"
                              + "log=" + (enableLog ? "1" : "0") + "\n"
                              + "ue_dumper=" + (cbPubgDumper.isChecked() ? "1" : "0") + "\n";
            SuShell.run("echo '" + cfgContent + "' > /data/local/tmp/dobby_config.txt\n"
                    + "chmod 644 /data/local/tmp/dobby_config.txt");
            LogUtil.i("[注入流程] 配置文件已写入: dumper=" + enableDumper + " header=" + enableHeader);

            long startTime = System.currentTimeMillis();
            int exitCode = SuShell.runWithLines(
                    injectorDst + " " + g_packFileName + " " + soPath + " 2>&1",
                    l -> LogUtil.i("[注入流程] " + l));
            long elapsed = System.currentTimeMillis() - startTime;

            if (exitCode == 0) {
                LogUtil.i("[注入流程] ✓ 注入成功! 耗时: " + elapsed + "ms");
            } else {
                LogUtil.e("[注入流程] ✗ 注入失败, 退出码: " + exitCode + " 耗时: " + elapsed + "ms", null);
            }
        }).start();
    }

    /**
     * 从 assets 提取 chinese.ttf 到 /data/local/tmp/chinese.ttf
     */
    public void deployChineseFont() {
        String tmpFile = getCacheDir() + "/chinese.ttf";
        String dstFile = "/data/local/tmp/chinese.ttf";

        // 检查目标是否已存在（避免重复复制）
        if (SuShell.containsLine("test -f " + dstFile + " && echo EXISTS", "EXISTS")) {
            LogUtil.i("[Font] chinese.ttf 已存在于 " + dstFile + "，跳过部署");
            return;
        }

        // 从 assets 提取 + su 部署 (一步完成)
        try {
            if (Deploy.fromInputStream(getAssets().open("chinese.ttf"), tmpFile, dstFile, "644")) {
                LogUtil.i("[Font] ✓ chinese.ttf 已部署到 " + dstFile);
            }
        } catch (Exception e) {
            LogUtil.e("[Font] 提取 chinese.ttf 失败: " + e.getMessage(), e);
        }
    }

    public void writeFiletoTargetProgram() {
        String srcFile = g_nativeLibPath + (g_nativeLibPath.endsWith("/") ? "" : "/") + "libdobbyproject.so";
        String dstDir = "/data/data/" + g_packFileName + "/files";
        String dstFile = dstDir + "/libdobbyproject.so";

        LogUtil.i("源文件: " + srcFile);
        LogUtil.i("拷贝 SO -> " + dstFile);

        // 构建拷贝命令
        String cmd = "mkdir -p " + dstDir + "\n" +
                "cp -f " + srcFile + " " + dstFile + "\n" +
                "chmod 777 " + dstFile + "\n" +
                "sync\n" +
                "exit\n";

        // 依次尝试: su -M (KernelSU) → su -mm (Magisk) → su (通用)
        String[] suVariants = {"su -M", "su -mm", "su"};
        for (String suCmd : suVariants) {
            try {
                LogUtil.i("尝试 " + suCmd + " 执行拷贝...");
                Process p = Runtime.getRuntime().exec(suCmd);
                DataOutputStream os = new DataOutputStream(p.getOutputStream());
                os.writeBytes(cmd);
                os.flush();

                BufferedReader reader = new BufferedReader(new InputStreamReader(p.getInputStream()));
                String line;
                while ((line = reader.readLine()) != null) {
                    LogUtil.i("Output: " + line);
                }

                int exitCode = p.waitFor();
                if (exitCode == 0) {
                    LogUtil.i("✓ 使用 " + suCmd + " 拷贝成功");
                    return;
                } else {
                    LogUtil.i(suCmd + " 失败 (exitCode=" + exitCode + "), 尝试下一个...");
                }
            } catch (Exception e) {
                LogUtil.i(suCmd + " 不可用: " + e.getMessage() + ", 尝试下一个...");
            }
        }
        LogUtil.e("所有 su 方式均失败，无法拷贝 SO 文件", null);
    }

    // ═══════════════════════════════════════════════════════════════════
    //  和平精英 (PUBG Mobile) 注入流程
    // ═══════════════════════════════════════════════════════════════════

    private static final String PUBG_PACKAGE = "com.tencent.tmgp.pubgmhd";

    /** 部署到目标进程时统一使用的 SO 文件名 (伪装为通用预加载库). */
    private static final String INJECT_SO_NAME = "libpre.so";

    // ═══════════════════════════════════════════════════════════════════
    //  三角洲 (Delta Force Mobile)
    // ═══════════════════════════════════════════════════════════════════

    private static final String DFM_PACKAGE = "com.tencent.tmgp.dfm";

    // ═══════════════════════════════════════════════════════════════════
    //  洛克王国手游 (NRC)
    // ═══════════════════════════════════════════════════════════════════

    private static final String NRC_PACKAGE = "com.tencent.nrc";

    // ═══════════════════════════════════════════════════════════════════
    //  统一注入入口的 GameSpec 配置
    //  添加新游戏: 只需 new GameLauncher.GameSpec(...) + 一个按钮 onClick
    //  → GameLauncher.launch(SPEC, ..., makeGameLauncherEnv(...))
    // ═══════════════════════════════════════════════════════════════════

    /**
     * 和平精英 (PUBG Mobile) 的注入参数。
     *
     * 字段对应 {@link GameLauncher.GameSpec} 构造参数:
     *   tag            = "PUBG"           日志前缀 + trace 文件名: pubg_injector_trace.txt
     *   displayName    = "和平精英"        UI 状态栏显示名
     *   pkg            = PUBG_PACKAGE     com.tencent.tmgp.pubgmhd
     *   mode           = "pubg"           injector argv[3], native 端按此分支选偏移
     *   soFileName     = INJECT_SO_NAME   部署到目标的 SO 文件名 (libpre.so, 伪装通用预加载库)
     *   useApkLibDir   = true             部署到 APK 自带 nativeLibraryDir
     *                                      (/data/app/.../lib/arm64/), 而非 /data/data/<pkg>/files
     *   chconApkData   = true             部署后打 SELinux 标签 u:object_r:apk_data_file:s0,
     *                                      让 ACE 反作弊扫描时与游戏自身 .so 同标签, 不易被识破
     *   waitMs         = 15000            monkey 启动后等 15s 让游戏初始化 (libUE4.so 加载完)
     *   retryWithTrace = true             4 次重试, 每次后轮询
     *                                      /data/data/<pkg>/cache/ue4_gui_trace.txt 判成功;
     *                                      PUBG 反作弊偶尔 ptrace 失败需要重试
     *   hideSoName     = INJECT_SO_NAME   注入成功后调 KPM add_hide_so:libpre.so,
     *                                      让 maps/dlopen 看不到该 SO; 注入前 launcher 会
     *                                      自动 remove_hide_so 撤销, 否则 dlopen 会报
     *                                      "not accessible for the namespace"
     *
     * 调用入口 (见上方 btnPubgLaunch.setOnClickListener):
     *   GameLauncher.launch(GAME_PUBG, enableUeDumper, enableUeHeader, enableLog,
     *                       makeGameLauncherEnv(GAME_PUBG, btnPubgLaunch, "和平精英"));
     */
    private static final GameLauncher.GameSpec GAME_PUBG = new GameLauncher.GameSpec(
            "PUBG", "和平精英", PUBG_PACKAGE, "pubg",
            INJECT_SO_NAME, /*useApkLibDir=*/true, /*chconApkData=*/true,
            /*waitMs=*/15000, /*retryWithTrace=*/true, /*hideSoName=*/INJECT_SO_NAME);

    private static final GameLauncher.GameSpec GAME_DFM = new GameLauncher.GameSpec(
            "DFM", "三角洲", DFM_PACKAGE, "dfm",
            INJECT_SO_NAME, /*useApkLibDir=*/true, /*chconApkData=*/true,
            /*waitMs=*/15000, /*retryWithTrace=*/true, /*hideSoName=*/INJECT_SO_NAME);

    private static final GameLauncher.GameSpec GAME_NRC = new GameLauncher.GameSpec(
            "NRC", "洛克王国手游", NRC_PACKAGE, "nrc",
            INJECT_SO_NAME, /*useApkLibDir=*/false, /*chconApkData=*/false,
            /*waitMs=*/15000, /*retryWithTrace=*/false, /*hideSoName=*/INJECT_SO_NAME);

    /**
     * 把 4 个游戏启动按钮共用的 onClick 流程抽出, 仅靠 GameSpec + 按钮 + 文案标签区分。
     *
     * 流程: selinuxDone 检查 → "未安装" 文案检查 → overlay 权限 → 启动 service →
     *       clearDumpMarkers → GameLauncher.launch → 更新按钮文案/状态/Toast.
     *
     * @param headerLabel 为 null 表示该游戏的 header 选项不显示在按钮文案里 (NRC 行为)
     */
    private void wireGameButton(android.widget.Button btn, GameLauncher.GameSpec spec,
                                String displayName,
                                CheckBox cbDumper, CheckBox cbHeader, CheckBox cbLog,
                                String dumperLabel, String headerLabel) {
        wireGameButton(btn, spec, displayName, cbDumper, cbHeader, cbLog,
                       dumperLabel, headerLabel, null, null);
    }

    /**
     * 增强版：增加 GameKpm 反检测开关 cbAntiCheat（可空）。
     *   - cbAntiCheat 勾上 → 启动游戏前先发 preset_<presetTag> 给 game-kpm 启用
     *     ptrace/prctl/pts/mincore/inotify/uname/status_filter 全部反检测开关
     *     + 委托 inject-hide 注册 hide_pkg/comm/so + enable_file_hide
     *   - 不勾 → 完全不接触 game-kpm，只装 dobby（用于对比/排查反作弊副作用）
     */
    private void wireGameButton(android.widget.Button btn, GameLauncher.GameSpec spec,
                                String displayName,
                                CheckBox cbDumper, CheckBox cbHeader, CheckBox cbLog,
                                String dumperLabel, String headerLabel,
                                CheckBox cbAntiCheat, String presetTag) {
        btn.setOnClickListener(v -> {
            if (!selinuxDone) {
                Toast.makeText(this, "请先设置宽容模式", Toast.LENGTH_SHORT).show();
                return;
            }
            if ("⚠ 游戏未安装".equals(btn.getText().toString())) {
                Toast.makeText(this, displayName + "未安装，请先安装游戏", Toast.LENGTH_SHORT).show();
                return;
            }
            if (!ensureOverlayPermission()) {
                updateStatus("请授予悬浮窗权限后重试");
                return;
            }

            startUe4OverlayService(spec.pkg);
            btn.setEnabled(false);

            boolean enableUeDumper = cbDumper.isChecked();
            boolean enableUeHeader = cbHeader.isChecked();
            boolean enableLog = cbLog.isChecked();
            boolean enableAntiCheat = (cbAntiCheat != null && cbAntiCheat.isChecked());

            // ── GameKpm 反检测：勾选则在启动游戏前应用 preset ──
            // 启动顺序很重要：preset 先于游戏进程出现，确保 hook 在 zygote fork
            // 出 game tgid 之前已就绪，is_target_current 命中 comm 前缀立即自动
            // 注册 tgid，所有反作弊 syscall 都能被拦截。
            if (enableAntiCheat && presetTag != null) {
                try {
                    String r1 = nativeGameKpmCtl("enable_log");
                    String r2 = nativeGameKpmCtl("preset_" + presetTag);
                    LogUtil.i("[GameKpm] enable_log -> " + r1);
                    LogUtil.i("[GameKpm] preset_" + presetTag + " -> " + r2);
                } catch (Throwable t) {
                    LogUtil.e("[GameKpm] preset failed", t);
                }
            }

            clearDumpMarkers();
            GameLauncher.launch(spec, enableUeDumper, enableUeHeader, enableLog,
                    makeGameLauncherEnv(spec, btn, displayName));

            String options = "";
            if (enableUeDumper) options += " " + dumperLabel;
            if (enableUeHeader && headerLabel != null) options += " " + headerLabel;
            if (enableAntiCheat && presetTag != null) options += " [反检测]";
            btn.setText("✅ 游戏已启动" + options);
            updateStatus(UE4_OVERLAY_STATUS + " | " + displayName + "启动中..." + options);
            Toast.makeText(this, "正在启动" + displayName + "并注入..." + options, Toast.LENGTH_SHORT).show();
        });
    }

    /** 把 Activity 上的依赖打包成 GameLauncher.Env, 让启动逻辑与 Activity 解耦. */
    private GameLauncher.Env makeGameLauncherEnv(GameLauncher.GameSpec spec,
                                                 android.widget.Button btn,
                                                 String displayName) {
        return new GameLauncher.Env() {
            @Override public String rootBinary() { return "su"; }
            @Override public String appNativeLibDir() { return g_nativeLibPath; }
            @Override public String resolveTargetLibDir(String pkg) {
                return resolvePackageNativeLibDir(pkg);
            }
            @Override public void kpmCtl(String cmd) {
                try { nativeKpmRawCtl(cmd); } catch (Throwable ignored) {}
            }
            @Override public void onResult(boolean ok, int exitCode) {
                runOnUiThread(() -> {
                    if (ok) {
                        updateStatus(displayName + "注入成功");
                    } else {
                        stopUe4OverlayService();
                        updateStatus(displayName + "注入失败 (code=" + exitCode + ")");
                    }
                });
            }
        };
    }

    // ─────────────────────────────────────────────────────────────
    //  ⚙ GameKpm 高级设置面板
    //
    //  10 个 CheckBox 即时同步到 game-kpm（control0 命令）。
    //  名字与 GameKpm.c 的 SWITCH 宏对齐：
    //    anti_ptrace / anti_prctl / pts_block / mincore_lie /
    //    exec_block / inotify_swallow / uname_spoof / status_filter /
    //    private_dir_watch / log
    //
    //  + 刷新按钮：解析 nativeGameKpmCtl("status") 把当前值回填到 UI
    //  + DFM/PUBG 预设按钮：一键 preset_dfm/pubgmhd（前 8 项全开）
    // ─────────────────────────────────────────────────────────────
    private CheckBox[] gkSwitchBoxes;
    private String[]   gkSwitchNames;

    private void wireGameKpmAdvancedPanel() {
        // 1. 折叠头
        TextView header = findViewById(R.id.tv_section_gk_adv_header);
        LinearLayout content = findViewById(R.id.layout_gk_adv_content);
        if (header == null || content == null) return;
        header.setOnClickListener(v -> {
            if (content.getVisibility() == View.VISIBLE) {
                content.setVisibility(View.GONE);
                header.setText("▶ ⚙ GameKpm 高级设置 (反检测开关)");
            } else {
                content.setVisibility(View.VISIBLE);
                header.setText("▼ ⚙ GameKpm 高级设置 (反检测开关)");
                refreshGameKpmStatus();
            }
        });

        // 2. CheckBox 注册（顺序 = 显示顺序）
        gkSwitchBoxes = new CheckBox[]{
            findViewById(R.id.cb_gk_anti_ptrace),
            findViewById(R.id.cb_gk_anti_prctl),
            findViewById(R.id.cb_gk_pts_block),
            findViewById(R.id.cb_gk_mincore_lie),
            findViewById(R.id.cb_gk_exec_block),
            findViewById(R.id.cb_gk_inotify_swallow),
            findViewById(R.id.cb_gk_uname_spoof),
            findViewById(R.id.cb_gk_status_filter),
            findViewById(R.id.cb_gk_private_dir_watch),
            findViewById(R.id.cb_gk_log),
        };
        gkSwitchNames = new String[]{
            "anti_ptrace", "anti_prctl", "pts_block", "mincore_lie",
            "exec_block", "inotify_swallow", "uname_spoof", "status_filter",
            "private_dir_watch", "log",
        };

        for (int i = 0; i < gkSwitchBoxes.length; i++) {
            final int idx = i;
            CheckBox cb = gkSwitchBoxes[i];
            if (cb == null) continue;
            cb.setOnCheckedChangeListener((buttonView, isChecked) -> {
                if (!buttonView.isPressed() && !buttonView.isFocused()) {
                    // 程序刷新触发的 setChecked 不要往 KPM 发
                    return;
                }
                String cmd = (isChecked ? "enable_" : "disable_") + gkSwitchNames[idx];
                String resp = safeGameKpmCtl(cmd);
                Toast.makeText(this, gkSwitchNames[idx] + " → " + (isChecked ? "ON" : "OFF"),
                        Toast.LENGTH_SHORT).show();
                LogUtil.i("[GameKpm] " + cmd + " -> " + resp);
            });
        }

        // 3. 刷新按钮
        Button btnRefresh = findViewById(R.id.btn_gk_refresh);
        if (btnRefresh != null) btnRefresh.setOnClickListener(v -> refreshGameKpmStatus());

        // 4. 预设按钮
        Button btnDfm = findViewById(R.id.btn_gk_preset_dfm);
        if (btnDfm != null) btnDfm.setOnClickListener(v -> {
            String r = safeGameKpmCtl("preset_dfm");
            Toast.makeText(this, "preset_dfm: " + r, Toast.LENGTH_SHORT).show();
            refreshGameKpmStatus();
        });
        Button btnPubg = findViewById(R.id.btn_gk_preset_pubg);
        if (btnPubg != null) btnPubg.setOnClickListener(v -> {
            String r = safeGameKpmCtl("preset_pubgmhd");
            Toast.makeText(this, "preset_pubgmhd: " + r, Toast.LENGTH_SHORT).show();
            refreshGameKpmStatus();
        });

        // 5. 安装 / 卸载 KPM
        Button btnInstall = findViewById(R.id.btn_gk_install);
        if (btnInstall != null) btnInstall.setOnClickListener(v -> installGameKpm());
        Button btnUninstall = findViewById(R.id.btn_gk_uninstall);
        if (btnUninstall != null) btnUninstall.setOnClickListener(v -> uninstallGameKpm());
    }

    /**
     * 安装 GameKpm：从 APK assets/game-kpm.kpm 拷到 app 私有目录
     * /data/data/com.example.dobbyproject/files/game-kpm.kpm 然后
     * supercall(SUPERCALL_KPM_LOAD)。已加载会先卸载再装（幂等）。
     *
     * 不再使用 /data/local/tmp（避免污染共享路径 + 不需 su cp）。
     * 内核态 filp_open() 用 root cred 直接读 app 私有目录文件，不存在权限问题。
     *
     * 依赖检查：game-kpm 的 preset_dfm/pubgmhd 通过用户态脚本派发命令到
     * kpm-svc（inject-hide），所以必须先加载 kpm-svc。本方法在加载 game-kpm
     * 前会先检查 kpm-svc 是否在线，未加载会弹对话框让用户选择：
     *   - 一键先装 kpm-svc 再装 game-kpm（推荐）
     *   - 仅装 game-kpm（没有通用隐藏，反作弊命中率会下降）
     *   - 取消
     */
    private void installGameKpm() {
        boolean svcLoaded = false;
        try { svcLoaded = nativeKpmSvcIsLoaded(); } catch (Throwable ignored) {}

        if (!svcLoaded) {
            new AlertDialog.Builder(this)
                .setTitle("缺少前置 KPM: kpm-svc")
                .setMessage("GameKpm 反作弊功能依赖 inject-hide (kpm-svc) 提供的通用隐藏（路径/SO/PID/线程名 等）。\n\n" +
                            "推荐先安装 kpm-svc 再装 GameKpm。")
                .setPositiveButton("一键安装 kpm-svc + GameKpm", (d, w) -> doInstallKpmSvcThenGame())
                .setNeutralButton("仅装 GameKpm（不推荐）", (d, w) -> doInstallGameKpmOnly())
                .setNegativeButton("取消", null)
                .setCancelable(true)
                .show();
            return;
        }
        // kpm-svc 已加载，直接装 game-kpm
        doInstallGameKpmOnly();
    }

    /** 顺序加载 kpm-svc → game-kpm */
    private void doInstallKpmSvcThenGame() {
        new Thread(() -> {
            // 1. 装 kpm-svc
            java.io.File svcFile = new java.io.File(getFilesDir(), "svc.kpm");
            String err = copyAsset("svc.kpm", svcFile);
            if (err != null) {
                runOnUiThread(() -> Toast.makeText(this, "kpm-svc 拷贝失败: " + err, Toast.LENGTH_LONG).show());
                return;
            }
            try { nativeKpmUnload("kpm-svc"); } catch (Throwable ignored) {}
            boolean okSvc;
            try { okSvc = nativeKpmLoad(svcFile.getAbsolutePath(), ""); }
            catch (Throwable t) { okSvc = false; }

            if (!okSvc) {
                runOnUiThread(() -> Toast.makeText(this, "❌ kpm-svc 加载失败，停止", Toast.LENGTH_LONG).show());
                return;
            }
            LogUtil.i("[KpmSvc] loaded ok (" + svcFile.length() + " bytes)");

            // 2. 装 game-kpm（同 doInstallGameKpmOnly 内部逻辑）
            doInstallGameKpmOnlyBlocking();
        }).start();
    }

    /** 仅在 kpm-svc 已加载时调；不阻塞，自己开线程 */
    private void doInstallGameKpmOnly() {
        new Thread(this::doInstallGameKpmOnlyBlocking).start();
    }

    /** game-kpm 的实际加载逻辑（已在工作线程内调用） */
    private void doInstallGameKpmOnlyBlocking() {
        java.io.File kpmFile = new java.io.File(getFilesDir(), "game-kpm.kpm");
        String err = copyAsset("game-kpm.kpm", kpmFile);
        if (err != null) {
            runOnUiThread(() -> Toast.makeText(this, "GameKpm 拷贝失败: " + err, Toast.LENGTH_LONG).show());
            return;
        }
        String absPath = kpmFile.getAbsolutePath();
        LogUtil.i("[GameKpm] kpm file at " + absPath + " size=" + kpmFile.length());

        try { nativeKpmUnload("game-kpm"); } catch (Throwable ignored) {}

        boolean ok;
        try { ok = nativeKpmLoad(absPath, ""); }
        catch (Throwable t) { ok = false; }
        final boolean okFinal = ok;

        runOnUiThread(() -> {
            Toast.makeText(this,
                okFinal ? "✅ GameKpm 已加载 (" + (kpmFile.length()/1024) + "KB)"
                        : "❌ 加载失败 — 检查 superkey",
                Toast.LENGTH_LONG).show();
            refreshGameKpmStatus();
        });
    }

    private void uninstallGameKpm() {
        new Thread(() -> {
            boolean ok;
            try { ok = nativeKpmUnload("game-kpm"); }
            catch (Throwable t) { ok = false; }
            final boolean okFinal = ok;
            runOnUiThread(() -> {
                Toast.makeText(this, okFinal ? "✅ GameKpm 已卸载" : "❌ 卸载失败 (未加载？)",
                        Toast.LENGTH_LONG).show();
                refreshGameKpmStatus();
            });
        }).start();
    }

    /** 把 assets/<name> 拷到 dstFile（app 私有目录）。返回 null 表示成功。 */
    private String copyAsset(String assetName, java.io.File dstFile) {
        try {
            java.io.InputStream in = getAssets().open(assetName);
            java.io.FileOutputStream out = new java.io.FileOutputStream(dstFile);
            byte[] buf = new byte[8192];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            out.close(); in.close();
            // app 私有目录默认 0600，root 内核仍可读
            return null;
        } catch (Throwable t) {
            return t.getMessage();
        }
    }

    /**
     * 调 nativeGameKpmCtl("status") 解析返回文本，把每个开关值回填到对应 CheckBox。
     */
    private void refreshGameKpmStatus() {
        TextView tv = findViewById(R.id.tv_gk_status);
        String resp = safeGameKpmCtl("status");

        // 在 status 文本顶部加一行 "前置依赖" 状态：kpm-svc 是否在线
        boolean svcLoaded = false;
        try { svcLoaded = nativeKpmSvcIsLoaded(); } catch (Throwable ignored) {}
        boolean gkLoaded = false;
        try { gkLoaded = nativeGameKpmIsLoaded(); } catch (Throwable ignored) {}

        StringBuilder banner = new StringBuilder();
        banner.append("[依赖] kpm-svc=").append(svcLoaded ? "✅" : "❌缺失");
        banner.append("  game-kpm=").append(gkLoaded ? "✅" : "❌");
        banner.append('\n');
        if (!svcLoaded && gkLoaded) {
            banner.append("⚠ kpm-svc 未加载，反检测命中率会下降\n");
        }
        if (tv != null) tv.setText(banner.toString() + (resp == null ? "" : resp));

        if (gkSwitchBoxes == null || gkSwitchNames == null) return;

        for (int i = 0; i < gkSwitchNames.length; i++) {
            CheckBox cb = gkSwitchBoxes[i];
            if (cb == null) continue;
            int v = parseSwitchValue(resp, gkSwitchNames[i]);
            // 不触发 listener
            cb.setOnCheckedChangeListener(null);
            cb.setChecked(v == 1);
            final int idx = i;
            cb.setOnCheckedChangeListener((buttonView, isChecked) -> {
                if (!buttonView.isPressed() && !buttonView.isFocused()) return;
                String cmd = (isChecked ? "enable_" : "disable_") + gkSwitchNames[idx];
                String r2 = safeGameKpmCtl(cmd);
                Toast.makeText(this, gkSwitchNames[idx] + " → " + (isChecked ? "ON" : "OFF"),
                        Toast.LENGTH_SHORT).show();
                LogUtil.i("[GameKpm] " + cmd + " -> " + r2);
            });
        }
    }

    /** 在 status 文本里抓 "  <key>  = <0|1>" 行；找不到返回 -1 */
    private static int parseSwitchValue(String resp, String key) {
        if (resp == null || key == null) return -1;
        String[] lines = resp.split("\n");
        for (String line : lines) {
            // 形如  "  anti_ptrace      = 1"
            int eq = line.indexOf('=');
            if (eq <= 0) continue;
            String left = line.substring(0, eq).trim();
            // 关键名带前缀如 "anti_ptrace"，要等于
            if (!left.equals(key) && !left.equalsIgnoreCase(key)) continue;
            String right = line.substring(eq + 1).trim();
            if (right.startsWith("1")) return 1;
            if (right.startsWith("0")) return 0;
        }
        return -1;
    }

    private String safeGameKpmCtl(String cmd) {
        try {
            String r = nativeGameKpmCtl(cmd);
            return (r == null) ? "" : r;
        } catch (Throwable t) {
            return "FAIL: " + t.getMessage();
        }
    }


}

