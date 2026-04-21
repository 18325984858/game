package com.example.dobbyproject;

import androidx.appcompat.app.AppCompatActivity;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.AutoCompleteTextView;
import android.widget.Button;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import android.widget.EditText;
import java.io.BufferedReader;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.text.DecimalFormat;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * SO Dumper 界面 —— 从运行中的应用进程 dump 指定 SO 文件并修复 ELF 头
 *
 * 功能流程:
 *   1. 通过 root 权限 (su + ps -A) 枚举所有运行中的应用进程
 *   2. 支持按包名模糊搜索快速定位目标应用
 *   3. 选中应用后, 通过 root 读取 /proc/PID/maps 列出已加载的 .so 模块
 *   4. 支持按 SO 名称 / 路径模糊搜索快速定位模块
 *   5. 通过 root + dd 从 /proc/PID/mem 读取内存并修复 ELF 头
 *      - 清零 Section Header (运行时已失效)
 *      - 对齐 PT_LOAD 段的 p_offset / p_filesz
 *   6. 输出到 /data/local/tmp/so_dump/<包名>_<so名>
 *
 * 依赖: 设备已 Root, 本应用已获得 Root 权限
 */
public class SoDumperActivity extends AppCompatActivity {

    // ─── Native 方法 (JNI → C++ soDumper/) ────────────────────────────
    /** 枚举所有运行中的应用进程, 支持按包名模糊过滤 (通过 su + ps -A) */
    private native String[] nativeListRunningApps(String filter);
    /** 枚举指定进程加载的所有 .so 模块 (通过 su + cat /proc/PID/maps) */
    private native String[] nativeListModules(int pid);
    /** Dump 指定 SO 到文件并修复 ELF (通过 su + dd /proc/PID/mem)
     *  @return 成功返回实际落盘路径 (反检测可能重写为随机 hex.bin); 失败返回 "ERR:&lt;code&gt;" */
    private native String nativeDumpSo(int pid, long baseAddr, long endAddr,
                                       String moduleName, String outPath);

    // ─── UI 元素 ─────────────────────────────────────────────────────
    private AutoCompleteTextView etSearch;       // 应用包名搜索框
    private EditText etModuleSearch;             // SO 模块名搜索框
    private Spinner spinnerApps;                 // 目标应用下拉列表
    private Spinner spinnerModules;              // SO 模块下拉列表
    private Button btnRefresh;                   // 刷新进程列表按钮
    private Button btnDump;                      // 执行 Dump 按钮
    private TextView tvStatus;                   // 底部状态文本
    private ProgressBar progressBar;             // Dump 进度条

    // ─── 数据 ────────────────────────────────────────────────────────
    private final List<AppItem> appList = new ArrayList<>();               // 当前显示的应用列表
    private final List<ModuleItem> allModuleList = new ArrayList<>();      // 全量 SO 模块列表 (未过滤)
    private final List<ModuleItem> filteredModuleList = new ArrayList<>();  // 经模糊搜索过滤后的模块列表
    private ArrayAdapter<String> appAdapter;
    private ArrayAdapter<String> moduleAdapter;

    private int selectedPid = -1;          // 当前选中的目标进程 PID
    private String selectedPackage = "";    // 当前选中的目标包名

    /** Dump 输出目录 (设备上) - 运行时初始化为应用私有外部目录, 应用进程可直接读写 */
    private String DUMP_DIR = "/data/local/tmp/so_dump";

    static {
        System.loadLibrary("dobbyproject");
    }

    // ─── 数据结构 ────────────────────────────────────────────────────

    /** 应用进程信息 */
    private static class AppItem {
        int pid;
        String packageName;

        AppItem(int pid, String packageName) {
            this.pid = pid;
            this.packageName = packageName;
        }

        @Override
        public String toString() {
            return packageName + " (PID: " + pid + ")";
        }
    }

    /** 内存映射信息 (来自 /proc/pid/maps) */
    private static class ModuleItem {
        long baseAddr;
        long endAddr;
        long size;
        long fileOffset;
        String perms;
        String name;
        String path;

        ModuleItem(long baseAddr, long endAddr, long size, long fileOffset, String perms, String name, String path) {
            this.baseAddr = baseAddr;
            this.endAddr = endAddr;
            this.size = size;
            this.fileOffset = fileOffset;
            this.perms = perms;
            this.name = name;
            this.path = path;
        }

        @Override
        public String toString() {
            String extra = "";
            if (path != null && !path.isEmpty() && !path.equals(name)) {
                extra = " - " + path;
            }
            return String.format(Locale.ROOT, "%s [%s off=0x%x] 0x%x-0x%x%s (%s)",
                    name,
                    perms == null ? "----" : perms,
                    fileOffset,
                    baseAddr,
                    endAddr,
                    extra,
                    formatSize(size));
        }

        /** 格式化字节大小为人类可读字符串 (B / KB / MB) */
        static String formatSize(long bytes) {
            if (bytes < 1024) return bytes + " B";
            DecimalFormat df = new DecimalFormat("#.##");
            if (bytes < 1024 * 1024) return df.format(bytes / 1024.0) + " KB";
            return df.format(bytes / (1024.0 * 1024.0)) + " MB";
        }
    }

    // ═════════════════════════════════════════════════════════════════

    @Override
    public boolean onOptionsItemSelected(android.view.MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_so_dumper);

        etSearch = findViewById(R.id.et_search);
        etModuleSearch = findViewById(R.id.et_module_search);
        spinnerApps = findViewById(R.id.spinner_apps);
        spinnerModules = findViewById(R.id.spinner_modules);
        btnRefresh = findViewById(R.id.btn_refresh);
        btnDump = findViewById(R.id.btn_dump);
        tvStatus = findViewById(R.id.tv_dump_status);
        progressBar = findViewById(R.id.progress_dump);

        // ActionBar 返回箭头
        if (getSupportActionBar() != null) {
            getSupportActionBar().setDisplayHomeAsUpEnabled(true);
        }

        // 初始化 adapter
        appAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        appAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerApps.setAdapter(appAdapter);

        moduleAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        moduleAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinnerModules.setAdapter(moduleAdapter);

        // 搜索框: 输入变化后自动刷新应用列表
        etSearch.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                refreshAppList(s.toString().trim());
            }
        });

        // 刷新按钮
        btnRefresh.setOnClickListener(v -> {
            String filter = etSearch.getText().toString().trim();
            refreshAppList(filter);
        });

        // 选择应用后加载 SO 列表
        spinnerApps.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (position >= 0 && position < appList.size()) {
                    AppItem item = appList.get(position);
                    selectedPid = item.pid;
                    selectedPackage = item.packageName;
                    loadModules(item.pid);
                }
            }
            @Override
            public void onNothingSelected(AdapterView<?> parent) {
                selectedPid = -1;
                selectedPackage = "";
                allModuleList.clear();
                filteredModuleList.clear();
                updateModuleSpinner();
            }
        });

        // SO 模块搜索框
        etModuleSearch.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                filterModules(s.toString().trim());
            }
        });

        // Dump 按钮
        btnDump.setOnClickListener(v -> performDump());

        // 初始加载
        refreshAppList("");
    }

    // ─── 刷新应用列表 ───────────────────────────────────────────────

    /**
     * 通过 JNI 调用 C++ 层, 以 root 身份枚举所有运行中的应用进程
     * 并刷新应用下拉列表; 自动加载第一个应用的 SO 模块
     */
    private void refreshAppList(String filter) {
        btnRefresh.setEnabled(false);
        tvStatus.setText("正在加载进程列表...");

        new Thread(() -> {
            try {
                String[] rawList = nativeListRunningApps(filter);
                List<AppItem> newList = new ArrayList<>();
                for (String raw : rawList) {
                    // 格式: "pid:packageName"
                    int colonIdx = raw.indexOf(':');
                    if (colonIdx > 0) {
                        int pid = Integer.parseInt(raw.substring(0, colonIdx));
                        String pkg = raw.substring(colonIdx + 1);
                        newList.add(new AppItem(pid, pkg));
                    }
                }

                runOnUiThread(() -> {
                    appList.clear();
                    appList.addAll(newList);
                    updateAppSpinner();
                    btnRefresh.setEnabled(true);
                    tvStatus.setText("找到 " + appList.size() + " 个应用进程");

                    // 如果列表不为空, 自动加载第一个的 SO
                    if (!appList.isEmpty()) {
                        selectedPid = appList.get(0).pid;
                        selectedPackage = appList.get(0).packageName;
                        loadModules(selectedPid);
                    } else {
                        allModuleList.clear();
                        filteredModuleList.clear();
                        updateModuleSpinner();
                    }
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    btnRefresh.setEnabled(true);
                    tvStatus.setText("加载进程列表失败: " + e.getMessage());
                });
            }
        }).start();
    }

    /** 更新应用下拉列表 Spinner 的显示内容 */
    private void updateAppSpinner() {
        List<String> labels = new ArrayList<>();
        for (AppItem item : appList) {
            labels.add(item.toString());
        }
        appAdapter.clear();
        appAdapter.addAll(labels);
        appAdapter.notifyDataSetChanged();
    }

    // ─── 加载 SO 模块列表 ───────────────────────────────────────────

    /**
     * 通过 JNI 调用 C++ 层, 以 root 身份读取 /proc/PID/maps
     * 解析出所有已加载的 .so 模块并更新 SO 下拉列表
     */
    private void loadModules(int pid) {
        tvStatus.setText("正在加载模块列表 (PID: " + pid + ")...");

        new Thread(() -> {
            try {
                String[] rawList = nativeListModules(pid);
                List<ModuleItem> newList = new ArrayList<>();

                for (String raw : rawList) {
                    // 格式: "0xBase:0xEnd:size:0xOffset:perms:name:path"
                    String[] parts = raw.split(":", 7);
                    if (parts.length >= 7) {
                        long base = parseHexLong(parts[0]);
                        long end = parseHexLong(parts[1]);
                        long size = Long.parseLong(parts[2]);
                        long fileOffset = parseHexLong(parts[3]);
                        String perms = parts[4];
                        String name = parts[5];
                        String path = parts[6];
                        newList.add(new ModuleItem(base, end, size, fileOffset, perms, name, path));
                    }
                }

                final String exportPath = dumpModuleListToFile(newList);
                runOnUiThread(() -> {
                    allModuleList.clear();
                    allModuleList.addAll(newList);
                    // 应用当前搜索过滤
                    filterModules(etModuleSearch.getText().toString().trim());
                    String suffix = (exportPath == null || exportPath.isEmpty()) ? "" : "，已导出模块清单";
                    tvStatus.setText(selectedPackage + " 加载了 " + allModuleList.size() + " 个模块候选" + suffix);
                });
            } catch (Exception e) {
                runOnUiThread(() -> {
                    tvStatus.setText("加载模块列表失败: " + e.getMessage());
                });
            }
        }).start();
    }

    /** 根据关键词模糊过滤 SO 模块列表 (匹配文件名或完整路径, 忽略大小写) */
    private void filterModules(String keyword) {
        filteredModuleList.clear();
        String lower = keyword.toLowerCase(Locale.ROOT);
        for (ModuleItem item : allModuleList) {
            String haystack = ((item.name == null ? "" : item.name) + " " +
                    (item.path == null ? "" : item.path)).toLowerCase(Locale.ROOT);
            boolean matched = keyword.isEmpty() || haystack.contains(lower);
            if (!matched && lower.contains("ue4")) {
                matched = haystack.contains("unreal");
            }
            if (!matched && lower.contains("unreal")) {
                matched = haystack.contains("ue4");
            }
            if (matched) {
                filteredModuleList.add(item);
            }
        }

        // 搜索无结果时, 回退显示全部候选模块, 方便用户手动翻找。
        if (!keyword.isEmpty() && filteredModuleList.isEmpty()) {
            filteredModuleList.addAll(allModuleList);
        }
        updateModuleSpinner();
    }

    /** 更新 SO 模块下拉列表 Spinner 的显示内容 */
    private void updateModuleSpinner() {
        List<String> labels = new ArrayList<>();
        for (ModuleItem item : filteredModuleList) {
            labels.add(item.toString());
        }
        moduleAdapter.clear();
        moduleAdapter.addAll(labels);
        moduleAdapter.notifyDataSetChanged();
        btnDump.setEnabled(!filteredModuleList.isEmpty());
    }

    /** 把当前进程的模块候选完整导出到文本文件, 便于人工检索。 */
    private String dumpModuleListToFile(List<ModuleItem> modules) {
        try {
            File dir = new File(getFilesDir(), "module_lists");
            if (!dir.exists() && !dir.mkdirs()) {
                return null;
            }
            File out = new File(dir, selectedPackage.replace(':', '_') + "_" + selectedPid + "_modules.txt");
            StringBuilder sb = new StringBuilder();
            sb.append("PID=").append(selectedPid).append('\n');
            sb.append("PACKAGE=").append(selectedPackage).append('\n');
            sb.append("COUNT=").append(modules.size()).append("\n\n");
            for (ModuleItem item : modules) {
                sb.append(String.format(Locale.ROOT, "0x%x-0x%x %d %s | %s\n",
                        item.baseAddr, item.endAddr, item.size,
                        item.name == null ? "" : item.name,
                        item.path == null ? "" : item.path));
            }
            try (FileOutputStream fos = new FileOutputStream(out, false)) {
                fos.write(sb.toString().getBytes(StandardCharsets.UTF_8));
            }
            return out.getAbsolutePath();
        } catch (Throwable ignored) {
            return null;
        }
    }

    // ─── 执行 Dump ─────────────────────────────────────────────────

    /**
     * 执行 Dump 操作:
     *  1. 在设备上创建输出目录 /data/local/tmp/so_dump/
     *  2. 调用 JNI → C++ 层, 通过 su + dd 读取 /proc/PID/mem
     *  3. C++ 层自动修复 ELF 头 (清零 SHT, 对齐 PT_LOAD)
     *  4. 输出文件名格式: <包名>_<so名>
     */
    private void performDump() {
        int moduleIdx = spinnerModules.getSelectedItemPosition();
        if (selectedPid <= 0 || moduleIdx < 0 || moduleIdx >= filteredModuleList.size()) {
            Toast.makeText(this, "请先选择应用和模块", Toast.LENGTH_SHORT).show();
            return;
        }

        ModuleItem mod = filteredModuleList.get(moduleIdx);
        btnDump.setEnabled(false);
        progressBar.setVisibility(View.VISIBLE);
        tvStatus.setText("正在 dump: " + mod.name + " ...");

        new Thread(() -> {
            try {
                // 通过 su 创建输出目录
                ensureDumpDir();

                String outFile = DUMP_DIR + "/" + selectedPackage + "_" + mod.name;

                String result = nativeDumpSo(selectedPid, mod.baseAddr, mod.endAddr, mod.name, outFile);
                boolean ok = result != null && !result.startsWith("ERR:");

                // 修复输出文件权限 (使用实际落盘路径)
                if (ok) {
                    execSuCommand("chmod 644 '" + result + "'");
                }

                final String resultMsg;
                if (ok) {
                    resultMsg = "Dump 成功!\n" + result + "\n大小: " + ModuleItem.formatSize(mod.size);
                } else {
                    String code = (result == null) ? "null" : result;
                    resultMsg = "Dump 失败 (" + code + ")\n模块: " + mod.name +
                            "\n映射: " + String.format(Locale.ROOT, "0x%x-0x%x %s off=0x%x",
                            mod.baseAddr, mod.endAddr, mod.perms, mod.fileOffset);
                }

                runOnUiThread(() -> {
                    progressBar.setVisibility(View.GONE);
                    btnDump.setEnabled(true);
                    tvStatus.setText(resultMsg);
                    Toast.makeText(SoDumperActivity.this,
                            ok ? "Dump 完成!" : "Dump 失败",
                            Toast.LENGTH_SHORT).show();
                });

            } catch (Exception e) {
                runOnUiThread(() -> {
                    progressBar.setVisibility(View.GONE);
                    btnDump.setEnabled(true);
                    tvStatus.setText("Dump 异常: " + e.getMessage());
                });
            }
        }).start();
    }

    // ─── 工具方法 ───────────────────────────────────────────────────

    /** 确保设备上的 dump 输出目录存在
     *  使用应用自己的外部私有目录 (getExternalFilesDir), 应用进程有写权限, 无需 root, 且空间在 /sdcard 上.
     *  /data/local/tmp 仅 shell 用户可写, 普通应用 fopen 会 EACCES 返回 -5. */
    private void ensureDumpDir() {
        java.io.File base = getExternalFilesDir(null);
        if (base == null) base = getFilesDir();
        java.io.File dir = new java.io.File(base, "so_dump");
        if (!dir.exists()) dir.mkdirs();
        DUMP_DIR = dir.getAbsolutePath();
    }

    /** 通过 su 执行一条 shell 命令 */
    private void execSuCommand(String cmd) {
        try {
            Process p = Runtime.getRuntime().exec("su");
            DataOutputStream os = new DataOutputStream(p.getOutputStream());
            os.writeBytes(cmd + "\n");
            os.writeBytes("exit\n");
            os.flush();
            p.waitFor();
        } catch (Exception ignored) {}
    }

    /** 解析十六进制地址字符串 ("0x1234..." → long) */
    private static long parseHexLong(String hex) {
        String clean = hex.startsWith("0x") ? hex.substring(2) : hex;
        return Long.parseUnsignedLong(clean, 16);
    }
}
