/*
 * Inject-Hide KPM 管理界面（Java 端）
 * ─────────────────────────────────────────────────────────────
 * 本页面与 KernelPatch/kpms/inject-hide 模块一一对应，提供如下能力：
 *   1. 三个总开关按钮（PID 隐藏 / 文件隐藏 / 线程名隐藏），按钮文案
 *      根据 KPM status 实时反馈 已开启/已关闭；
 *   2. 隐藏 SO 关键字列表（hide_so）增删改；
 *   3. 监控应用包名列表（hide_pkg）增删改；命中的进程会被 KPM 自动
 *      把 tgid 加入 hide_pid 并开启 proc_hide；
 *   4. 隐藏线程名关键字列表（hide_comm）增删改；
 *   5. 进程枚举（root ps -A，通过 JNI 复用 SoDumper 的实现），
 *      支持模糊搜索并在选中后把包名填入 hide_pkg 输入框；
 *   6. 刷新时调用内核列表并扫描当前运行进程快照，自动清理失效的
 *      包名 (hide_pkg) 与消亡的 pid (hide_pid)；
 *   7. 原始命令通道（调试用），可直接向 KPM control0 发送任意指令。
 *
 * 与内核交互的通道由 InjectHideCtl (JNI) 统一封装，底层通过
 * SUPERCALL_KPM_CONTROL 向 KernelPatch 的 kpm-inject-hide 模块发命令。
 */
package com.example.dobbyproject;

import android.app.AlertDialog;
import android.os.Bundle;
import android.text.TextUtils;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ListView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.appcompat.app.AppCompatActivity;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Inject-Hide KPM 管理页面 (优化版)。
 *
 * UI 特性：
 *   - 三个全局开关 (proc_hide / file_hide / hideSelf) 均为 **单按钮**，
 *     进入页面后自动从 KPM 读取当前状态并反馈到按钮文案/底色。
 *   - 隐藏 PID：
 *       * 支持输入进程名关键字，扫描 /proc 模糊匹配；
 *       * 结果写入 Spinner 下拉框；
 *       * 选择后自动填到 PID 输入框；
 *       * "刷新" 按钮 → 重新扫描。
 *       * PID 列表改为 ListView，**点击条目弹框删除**。
 *   - 隐藏 SO：
 *       * 列表自动从 `list_hide_so`（即 inject-hide.kpm 中的数组）遍历得到；
 *       * ListView 展示，**点击条目弹框删除**。
 */
public class InjectHideActivity extends AppCompatActivity {

    static { System.loadLibrary("dobbyproject"); }

    private static final String SO_FILE  = ".kpm_hide_so.key";

    private TextView tvStatus, tvRawResp;
    private EditText etSo, etRaw, etProcSearch, etPkg, etComm;
    private Button   btnToggleProc, btnToggleFile, btnToggleComm;
    // Root 痕迹隐藏（RootHide 模块）总开关按钮
    private Button   btnToggleRoot;
    // 系统进程豁免开关按钮：默认开启，避免 installd/system_server
    // 等 UID<10000 的系统链路被隐藏规则误拦
    private Button   btnToggleSysExempt;
    // KPM 日志总开关按钮：控制 klog()/klog_dbg() 输出（不影响 klog_err）
    private Button   btnToggleLog;
    private Spinner  spProcMatch;
    private ListView lvSoList, lvPkgList, lvCommList;

    private ArrayAdapter<String> soAdapter;       // 隐藏 SO 列表
    private ArrayAdapter<String> pkgAdapter;      // 监控的包名列表
    private ArrayAdapter<String> commAdapter;     // 隐藏的线程名列表
    private ArrayAdapter<String> procMatchAdapter;// 搜索到的候选进程
    private final List<String> procMatchNames = new ArrayList<>(); // cmdline/comm, 与 spinner 下标对齐

    // 当前状态缓存（refreshAll 时更新）
    private int curProcHide = -1;
    private int curFileHide = -1;
    private int curCommHide = -1;
    // Root 痕迹隐藏当前状态（-1=未知, 0=关, 1=开）。由 status_root 解析
    private int curRootHide = -1;
    // 系统进程豁免状态（解析自 status 的 sys_exempt 字段）
    private int curSysExempt = -1;
    // KPM 日志总开关状态（解析自 status 的 log_enabled 字段）
    private int curLogEnabled = -1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_inject_hide);

        tvStatus       = findViewById(R.id.ih_tv_status);
        tvRawResp      = findViewById(R.id.ih_tv_raw_resp);
        etSo           = findViewById(R.id.ih_et_so);
        etRaw          = findViewById(R.id.ih_et_raw);
        etProcSearch   = findViewById(R.id.ih_et_proc_search);
        etPkg          = findViewById(R.id.ih_et_pkg);
        etComm         = findViewById(R.id.ih_et_comm);
        btnToggleProc  = findViewById(R.id.ih_btn_toggle_proc);
        btnToggleFile  = findViewById(R.id.ih_btn_toggle_file);
        btnToggleComm  = findViewById(R.id.ih_btn_toggle_comm);
        btnToggleRoot  = findViewById(R.id.ih_btn_toggle_root);
        btnToggleSysExempt = findViewById(R.id.ih_btn_toggle_sys_exempt);
        btnToggleLog       = findViewById(R.id.ih_btn_toggle_log);
        spProcMatch    = findViewById(R.id.ih_sp_proc_match);
        lvSoList       = findViewById(R.id.ih_lv_so_list);
        lvPkgList      = findViewById(R.id.ih_lv_pkg_list);
        lvCommList     = findViewById(R.id.ih_lv_comm_list);

        soAdapter  = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, new ArrayList<>());
        pkgAdapter = new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, new ArrayList<>());
        commAdapter= new ArrayAdapter<>(this, android.R.layout.simple_list_item_1, new ArrayList<>());
        procMatchAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        procMatchAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        lvSoList.setAdapter(soAdapter);
        lvPkgList.setAdapter(pkgAdapter);
        lvCommList.setAdapter(commAdapter);
        spProcMatch.setAdapter(procMatchAdapter);

        // 把缓存里的 superkey 喂给 native
        String cachedKey = readSuperkeyFromCache();
        if (cachedKey != null) nativeSetSuperkey(cachedKey);

        // ── 全局开关（单按钮） ──
        btnToggleProc.setOnClickListener(v -> toggleProcHide());
        btnToggleFile.setOnClickListener(v -> toggleFileHide());
        btnToggleComm.setOnClickListener(v -> toggleCommHide());
        // Root 痕迹隐藏：按当前 curRootHide 反向操作；启用时 KPM 内部会
        // 自动把默认关键词注入 hide_so 并打开 file_hide。
        btnToggleRoot.setOnClickListener(v -> toggleRootHide());
        // 系统进程豁免切换：开启时 UID<10000 直接 trusted，不拦截。
        btnToggleSysExempt.setOnClickListener(v -> toggleSysExempt());
        // KPM 日志开关：关闭后内核不再输出 klog/klog_dbg（错误日志保留）
        btnToggleLog.setOnClickListener(v -> toggleKpmLog());

        // ── 进程名模糊搜索 (用于选包名) ──
        findViewById(R.id.ih_btn_proc_refresh).setOnClickListener(v -> refreshProcMatches());
        etProcSearch.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {
                applyProcFilter(s == null ? "" : s.toString());
            }
            @Override public void afterTextChanged(android.text.Editable s) {}
        });
        spProcMatch.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> p, View v, int pos, long id) {
                if (pos >= 0 && pos < procMatchNames.size()) {
                    // 输入框自动填充包名（仅取 cmdline 第一段的基名，去掉可能的 ':xxx' 子进程后缀）
                    String s = procMatchNames.get(pos);
                    int col = s.indexOf(':');
                    if (col > 0) s = s.substring(0, col);
                    etPkg.setText(s);
                }
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> p) {}
        });

        // ── SO：添加 ──
        findViewById(R.id.ih_btn_add_so).setOnClickListener(v -> {
            String name = etSo.getText().toString().trim();
            if (TextUtils.isEmpty(name)) { toast("SO 名为空"); return; }
            runNativeAsync("add_hide_so:" + name, () -> {
                boolean ok = nativeAddHideSo(name);
                if (ok) persistAdd(SO_FILE, name);
                return ok;
            });
        });
        findViewById(R.id.ih_btn_list_so).setOnClickListener(v -> refreshAll());
        findViewById(R.id.ih_btn_clear_so).setOnClickListener(v ->
            confirm("清空所有 hide_so？", () ->
                runNativeAsync("clear_hide_so", () -> {
                    boolean ok = nativeClearHideSo();
                    if (ok) persistClear(SO_FILE);
                    return ok;
                })));

        // SO ListView：点击条目 → 删除
        lvSoList.setOnItemClickListener((parent, view, position, id) -> {
            final String name = soAdapter.getItem(position);
            if (TextUtils.isEmpty(name)) return;
            confirm("从 hide_so 中删除 '" + name + "' ?", () ->
                runNativeAsync("remove_hide_so:" + name, () -> {
                    boolean ok = nativeRemoveHideSo(name);
                    if (ok) persistRemove(SO_FILE, name);
                    return ok;
                }));
        });

        // ── 包名监控 (hide_pkg) ──
        findViewById(R.id.ih_btn_add_pkg).setOnClickListener(v -> {
            String name = etPkg.getText().toString().trim();
            if (TextUtils.isEmpty(name)) { toast("包名为空"); return; }
            runNativeAsync("add_hide_pkg:" + name, () -> nativeAddHidePkg(name));
        });
        findViewById(R.id.ih_btn_list_pkg).setOnClickListener(v -> refreshAll());
        findViewById(R.id.ih_btn_clear_pkg).setOnClickListener(v ->
            confirm("清空所有监控包名？", () ->
                runNativeAsync("clear_hide_pkg", () -> nativeClearHidePkg())));
        lvPkgList.setOnItemClickListener((parent, view, position, id) -> {
            final String item = pkgAdapter.getItem(position);
            if (TextUtils.isEmpty(item)) return;
            // 条目格式为 "<pkg>  → pids: …"，取空格前的 pkg 名
            int sp = item.indexOf(' ');
            final String name = sp > 0 ? item.substring(0, sp) : item;
            confirm("从监控列表中删除 '" + name + "' ?", () ->
                runNativeAsync("remove_hide_pkg:" + name, () -> nativeRemoveHidePkg(name)));
        });

        // ── 线程名隐藏 (hide_comm) ──
        findViewById(R.id.ih_btn_add_comm).setOnClickListener(v -> {
            String name = etComm.getText().toString().trim();
            if (TextUtils.isEmpty(name)) { toast("线程名为空"); return; }
            runNativeAsync("add_hide_comm:" + name, () -> nativeAddHideComm(name));
        });
        findViewById(R.id.ih_btn_list_comm).setOnClickListener(v -> refreshAll());
        findViewById(R.id.ih_btn_clear_comm).setOnClickListener(v ->
            confirm("清空所有 hide_comm？", () ->
                runNativeAsync("clear_hide_comm", () -> nativeClearHideComm())));
        lvCommList.setOnItemClickListener((parent, view, position, id) -> {
            final String name = commAdapter.getItem(position);
            if (TextUtils.isEmpty(name)) return;
            confirm("从 hide_comm 中删除 '" + name + "' ?", () ->
                runNativeAsync("remove_hide_comm:" + name, () -> nativeRemoveHideComm(name)));
        });

        // ── 原始命令 ──
        findViewById(R.id.ih_btn_raw).setOnClickListener(v -> {
            String cmd = etRaw.getText().toString().trim();
            if (TextUtils.isEmpty(cmd)) { toast("命令为空"); return; }
            new Thread(() -> {
                String resp;
                try { resp = nativeRawCtl(cmd); }
                catch (Throwable t) { resp = "EX: " + t.getMessage(); }
                final String r = resp;
                runOnUiThread(() -> {
                    tvRawResp.setText("> " + cmd + "\n" + r);
                    refreshAll();
                });
            }).start();
        });

        refreshAll();
        refreshProcMatches(); // 进入页面即列出所有进程
    }

    // ── 单按钮开关：按当前状态反向操作 ───────────────────────
    private void toggleProcHide() {
        final boolean target = !(curProcHide == 1);
        runNativeAsync((target ? "enable" : "disable") + "_proc_hide",
                () -> target ? nativeEnableProcHide() : nativeDisableProcHide());
    }
    private void toggleFileHide() {
        final boolean target = !(curFileHide == 1);
        runNativeAsync((target ? "enable" : "disable") + "_file_hide",
                () -> target ? nativeEnableFileHide() : nativeDisableFileHide());
    }
    private void toggleCommHide() {
        final boolean target = !(curCommHide == 1);
        runNativeAsync((target ? "enable" : "disable") + "_comm_hide",
                () -> target ? nativeEnableCommHide() : nativeDisableCommHide());
    }
    // Root 痕迹隐藏总开关：开启后 KPM 的 RootHide 模块会把默认 root 关键词
    //（su/magisk/kernelsu/apatch/zygisk/…）注入 hide_so，并启用 file_hide，
    // 从而让 openat/faccessat 对这些路径返回 -ENOENT。关闭时则撤销这些
    // 注入项（不影响用户自行添加的其它 hide_so 关键词）。
    private void toggleRootHide() {
        final boolean target = !(curRootHide == 1);
        runNativeAsync((target ? "enable" : "disable") + "_root_hide",
                () -> target ? nativeEnableRootHide() : nativeDisableRootHide());
    }
    // 系统进程豁免切换：豁免开启时，installd/system_server 等系统
    // UID 直接放行，避免 adb install / am start 被隐藏规则误拦。默认开启。
    private void toggleSysExempt() {
        final boolean target = !(curSysExempt == 1);
        runNativeAsync((target ? "enable" : "disable") + "_sys_exempt",
                () -> target ? nativeEnableSysExempt() : nativeDisableSysExempt());
    }
    // KPM 日志总开关切换：关闭后内核不会因 hook 拦截等频繁操作
    // 合行产生 dmesg 噪声，也可减少日志侧信道泄露。
    private void toggleKpmLog() {
        final boolean target = !(curLogEnabled == 1);
        runNativeAsync((target ? "enable" : "disable") + "_log",
                () -> target ? nativeEnableLog() : nativeDisableLog());
    }
    // ── 通用 native 执行 ─────────────────────────────────────
    private interface BoolOp { boolean run(); }

    private void runNativeAsync(String label, BoolOp op) {
        new Thread(() -> {
            boolean ok;
            try { ok = op.run(); } catch (Throwable t) { ok = false; }
            final boolean f = ok;
            runOnUiThread(() -> {
                toast(label + (f ? " 成功" : " 失败"));
                refreshAll();
            });
        }).start();
    }

    // ── 统一刷新：状态 + 按钮 + 两个 ListView ───────────────
    private void refreshAll() {
        new Thread(() -> {
            try {
            boolean loaded = false;
            try { loaded = nativeIsModuleLoaded(); } catch (Throwable ignored) {}
            String st = "";
            try { st = nativeGetStatus(); } catch (Throwable ignored) {}
            // Root 痕迹隐藏状态由独立命令 status_root 返回，格式固定为：
            //   root_hide=<0/1>\nfile_hide=<0/1>\nroot_kw_count=<N>\n
            String stRoot = "";
            try { stRoot = nativeGetStatusRoot(); } catch (Throwable ignored) {}
            String soListRaw = "";
            try { soListRaw = nativeListHideSo(); } catch (Throwable ignored) {}
            String pkgListRaw = "";
            try { pkgListRaw = nativeListHidePkg(); } catch (Throwable ignored) {}
            String pidListRaw = "";
            try { pidListRaw = nativeListHidePid(); } catch (Throwable ignored) {}
            String commListRaw = "";
            try { commListRaw = nativeListHideComm(); } catch (Throwable ignored) {}

            final int proc = parseKvInt(st, "proc_hide", -1);
            final int file = parseKvInt(st, "file_hide", -1);
            final int comm = parseKvInt(st, "comm_hide", -1);
            final int sysExempt = parseKvInt(st, "sys_exempt", -1);
            final int sysExemptUid = parseKvInt(st, "sys_exempt_uid_max", -1);
            // KPM 日志总开关：从 status 的 log_enabled 字段解析
            final int logEnabled = parseKvInt(st, "log_enabled", -1);
            final int rootHide = parseKvInt(stRoot, "root_hide", -1);
            final int rootKwCount = parseKvInt(stRoot, "root_kw_count", -1);
            final List<String> soItems  = parseListItems(soListRaw);
            final List<String> commItems= parseListItems(commListRaw);
            List<String> pkgNames = parseListItems(pkgListRaw);
            List<String> pidLines = parseListItems(pidListRaw);

            // 通过 JNI (root ps -A) 拿到全进程快照：pid -> 包基名
            // 每条形如 "<pid>:<pkgBase>"
            //
            // ⚠️ 关键：listRunningApps 走 mem_reader 的持久 root shell
            // 跑 "ps -A"。该 shell 在某些 hook 配置下可能 hang（例如
            // 子进程 fork 后 openat /proc/self/* 被新加的 stat hook 拦
            // 死返回 -ENOENT 让 ps 启动失败，或 shell 自己被 P0
            // before_execve 拦下）。一旦 hang 整个 refreshAll 后台线程
            // 就永远 blocking，UI 永远停在 layout 默认 "模块状态检测中..."。
            //
            // 因此采用 "两阶段刷新"：
            //   1) 先用 status / list_hide_* 数据立即更新 UI
            //   2) 再异步跑 ps 枚举做 "失效清理 + pkg→pids 显示"
            // 即便阶段 2 卡死，UI 至少不会一直 "未知"。
            final boolean fLoaded0 = loaded;
            final List<String> pkgNamesStage1 = new ArrayList<>(pkgNames);
            final int pidLinesSizeStage1 = pidLines.size();
            runOnUiThread(() -> {
                curProcHide = proc;
                curFileHide = file;
                curCommHide = comm;
                curRootHide = rootHide;
                curSysExempt = sysExempt;
                curLogEnabled = logEnabled;

                StringBuilder bar = new StringBuilder();
                bar.append(fLoaded0 ? "✅ KPM 已加载" : "❌ KPM 未加载");
                bar.append("  |  pkg=").append(pkgNamesStage1.size());
                bar.append("  |  pid(kernel)=").append(pidLinesSizeStage1);
                bar.append("  |  so=").append(soItems.size());
                bar.append("  |  comm=").append(commItems.size());
                if (rootKwCount >= 0) bar.append("  |  root_kw=").append(rootKwCount);
                tvStatus.setText(bar.toString());

                applyToggleUi(btnToggleProc, "PID隐藏", proc);
                applyToggleUi(btnToggleFile, "文件隐藏", file);
                applyToggleUi(btnToggleComm, "线程名隐藏", comm);
                applyToggleUi(btnToggleRoot,
                        "Root痕迹隐藏" + (rootKwCount >= 0 ? "(kw=" + rootKwCount + ")" : ""),
                        rootHide);
                applyToggleUi(btnToggleSysExempt,
                        "系统进程豁免" + (sysExemptUid > 0 ? "(<" + sysExemptUid + ")" : ""),
                        sysExempt);
                applyToggleUi(btnToggleLog, "KPM日志", logEnabled);

                soAdapter.clear();
                soAdapter.addAll(soItems);
                soAdapter.notifyDataSetChanged();

                // pkg 列表先按"无运行中进程"占位渲染，阶段 2 完成后再覆盖
                pkgAdapter.clear();
                pkgAdapter.addAll(pkgNamesStage1);
                pkgAdapter.notifyDataSetChanged();

                commAdapter.clear();
                commAdapter.addAll(commItems);
                commAdapter.notifyDataSetChanged();
            });

            // ───── 阶段 2：异步跑 ps -A 做清理 + pkg→pids 富化 ─────
            String[] runningArr = null;
            try { runningArr = nativeListRunningApps(""); } catch (Throwable ignored) {}
            final java.util.Map<Integer, String> livePidToName = new java.util.HashMap<>();
            final java.util.Map<String, List<Integer>> liveNameToPids = new java.util.HashMap<>();
            if (runningArr != null) {
                for (String s : runningArr) {
                    if (s == null) continue;
                    int col = s.indexOf(':');
                    if (col <= 0) continue;
                    int pid;
                    try { pid = Integer.parseInt(s.substring(0, col)); }
                    catch (NumberFormatException e) { continue; }
                    String name = s.substring(col + 1);
                    String base = name;
                    int c2 = base.indexOf(':');
                    if (c2 > 0) base = base.substring(0, c2);
                    livePidToName.put(pid, name);
                    liveNameToPids.computeIfAbsent(base, k -> new ArrayList<>()).add(pid);
                }
            }

            // ① 清理失效包名：hide_pkg 中不再有任何对应运行进程 → remove_hide_pkg
            int removedPkg = 0;
            if (!livePidToName.isEmpty()) { // 没取到快照时不执行清理，避免误删
                List<String> alive = new ArrayList<>();
                for (String pkg : pkgNames) {
                    List<Integer> hits = liveNameToPids.get(pkg);
                    if (hits == null || hits.isEmpty()) {
                        try {
                            if (nativeRemoveHidePkg(pkg)) removedPkg++;
                        } catch (Throwable ignored) {}
                    } else {
                        alive.add(pkg);
                    }
                }
                pkgNames = alive;
            }

            // ② 清理失效 PID：内核 hide_pid 列表里 /proc/<pid> 不存在 → remove_hide_pid
            int removedPid = 0;
            if (!livePidToName.isEmpty()) {
                List<String> alivePidLines = new ArrayList<>();
                for (String line : pidLines) {
                    int pid = extractLeadingInt(line);
                    if (pid <= 0) { alivePidLines.add(line); continue; }
                    if (livePidToName.containsKey(pid)) {
                        alivePidLines.add(line);
                    } else {
                        try {
                            if (nativeRemoveHidePid(pid)) removedPid++;
                        } catch (Throwable ignored) {}
                    }
                }
                pidLines = alivePidLines;
            }

            // 根据快照重建 pkg → 命中的 [pid (name)...]
            java.util.Map<String, List<String>> pkgHits = new java.util.LinkedHashMap<>();
            for (String pkg : pkgNames) {
                List<String> cells = new ArrayList<>();
                List<Integer> pids = liveNameToPids.get(pkg);
                if (pids != null) {
                    java.util.Collections.sort(pids);
                    for (int pid : pids) {
                        String full = livePidToName.get(pid);
                        String base = full;
                        int c2 = base == null ? -1 : base.indexOf(':');
                        if (c2 > 0) base = base.substring(0, c2);
                        cells.add(pid + (full != null && !full.equals(base) ? "(" + full + ")" : ""));
                    }
                }
                pkgHits.put(pkg, cells);
            }

            // 拼装 pkg 显示项：  "<pkg>  → pids: 1234, 2345(:push)"  或  "<pkg>  (无运行中进程)"
            final List<String> pkgItemsShow = new ArrayList<>();
            for (String pkg : pkgNames) {
                List<String> hits = pkgHits.get(pkg);
                if (hits == null || hits.isEmpty()) {
                    pkgItemsShow.add(pkg + "  (无运行中进程)");
                } else {
                    StringBuilder sb = new StringBuilder();
                    sb.append(pkg).append("  → pids: ");
                    for (int i = 0; i < hits.size(); i++) {
                        if (i > 0) sb.append(", ");
                        sb.append(hits.get(i));
                    }
                    pkgItemsShow.add(sb.toString());
                }
            }

            final int fRemovedPkg = removedPkg, fRemovedPid = removedPid;

            // 阶段 2 收尾：仅覆盖 pkg 列表（已带 →pids 富化）+ 失效清理 toast
            runOnUiThread(() -> {
                pkgAdapter.clear();
                pkgAdapter.addAll(pkgItemsShow);
                pkgAdapter.notifyDataSetChanged();

                if (fRemovedPkg > 0 || fRemovedPid > 0) {
                    toast("已清理失效  pkg=" + fRemovedPkg + "  pid=" + fRemovedPid);
                }
            });
            } catch (final Throwable t) {
                // 任何未被内层捕获的异常都会让 refreshAll 后台线程
                // 静默死掉，造成 UI 永远停在“模块状态检测中...”。
                // 这里把错误打出并包括栈顶三帧贴到状态栏，方便定位。
                android.util.Log.e("InjectHide", "refreshAll crashed", t);
                runOnUiThread(() -> {
                    StringBuilder msg = new StringBuilder();
                    msg.append("❌ refreshAll 崩溃: ");
                    msg.append(t.getClass().getSimpleName()).append(": ");
                    msg.append(String.valueOf(t.getMessage()));
                    StackTraceElement[] st = t.getStackTrace();
                    for (int i = 0; i < Math.min(3, st.length); i++) {
                        msg.append("\n  at ").append(st[i].toString());
                    }
                    if (tvStatus != null) tvStatus.setText(msg.toString());
                });
            }
        }).start();
    }

    private static void applyToggleUi(Button b, String label, int state) {
        String s = (state == 1) ? "已开启" : (state == 0 ? "已关闭" : "未知");
        b.setText(label + "：" + s + "  (点击切换)");
    }

    // 把 "total: N\nline1\nline2\n" 拆成 [line1, line2, ...]
    private static List<String> parseListItems(String raw) {
        List<String> out = new ArrayList<>();
        if (raw == null) return out;
        for (String line : raw.split("\n")) {
            String t = line.trim();
            if (t.isEmpty()) continue;
            if (t.toLowerCase(Locale.ROOT).startsWith("total:")) continue;
            out.add(t);
        }
        return out;
    }

    // 从 status 文本里抽 key=value
    private static int parseKvInt(String text, String key, int defVal) {
        if (text == null) return defVal;
        int i = text.indexOf(key + "=");
        if (i < 0) return defVal;
        int j = i + key.length() + 1;
        int end = text.indexOf('\n', j);
        String v = (end < 0) ? text.substring(j) : text.substring(j, end);
        try { return Integer.parseInt(v.trim()); } catch (Exception e) { return defVal; }
    }

    // 从 "<pid>  <name...>" 取开头的数字
    private static int extractLeadingInt(String line) {
        if (line == null) return 0;
        int n = 0, i = 0;
        while (i < line.length() && line.charAt(i) >= '0' && line.charAt(i) <= '9') {
            n = n * 10 + (line.charAt(i) - '0'); i++;
        }
        return n;
    }

    // 当前所有进程的快照（刷新时填充；搜索在本快照上过滤，不重扫 /proc）
    private final List<String>  allProcShow = new ArrayList<>(); // "<pid>  <cmdline>"
    private final List<String>  allProcName = new ArrayList<>(); // cmdline

    // ── 进程搜索：通过 JNI (SoDumper: su ps -A) 枚举所有 app 进程 ─────
    private void refreshProcMatches() {
        new Thread(() -> {
            String[] arr;
            try { arr = nativeListRunningApps(""); }
            catch (Throwable t) { arr = new String[0]; }

            final List<String> showSorted = new ArrayList<>();
            final List<String> nameSorted = new ArrayList<>();
            if (arr != null) {
                for (String s : arr) {
                    if (s == null) continue;
                    int colon = s.indexOf(':');
                    if (colon <= 0) continue;
                    int pid;
                    try { pid = Integer.parseInt(s.substring(0, colon)); }
                    catch (NumberFormatException e) { continue; }
                    String name = s.substring(colon + 1);
                    if (name.isEmpty()) continue;
                    showSorted.add(name + " (PID:" + pid + ")");
                    nameSorted.add(name);
                }
            }

            runOnUiThread(() -> {
                allProcShow.clear(); allProcShow.addAll(showSorted);
                allProcName.clear(); allProcName.addAll(nameSorted);
                toast("已抓取 " + showSorted.size() + " 个进程");
                applyProcFilter(etProcSearch.getText().toString());
            });
        }).start();
    }

    // 在 allProc* 快照上按关键字过滤，写入 Spinner
    private void applyProcFilter(String kw) {
        String k = (kw == null ? "" : kw.trim().toLowerCase(Locale.ROOT));
        List<String> fshow = new ArrayList<>();
        List<String> fname = new ArrayList<>();
        for (int i = 0; i < allProcShow.size(); i++) {
            String line = allProcShow.get(i);
            if (k.isEmpty() || line.toLowerCase(Locale.ROOT).contains(k)) {
                fshow.add(line);
                fname.add(allProcName.get(i));
            }
        }
        procMatchNames.clear();
        procMatchNames.addAll(fname);
        procMatchAdapter.clear();
        if (fshow.isEmpty()) {
            procMatchAdapter.add("(无匹配进程)");
            procMatchNames.add(""); // keep aligned
        } else {
            procMatchAdapter.addAll(fshow);
        }
        procMatchAdapter.notifyDataSetChanged();

        // 搜索有匹配时自动把第一个候选写入包名输入框
        if (!fname.isEmpty() && !TextUtils.isEmpty(k)) {
            String s = fname.get(0);
            int col = s.indexOf(':');
            if (col > 0) s = s.substring(0, col);
            etPkg.setText(s);
        }
    }

    private static String readProcName(int pid) {
        // 先 cmdline
        String s = readFirstLine("/proc/" + pid + "/cmdline");
        if (s != null) {
            // cmdline 是 \0 分隔，Java read 后会保留 \0；先截到第一个 \0
            int z = s.indexOf('\0');
            if (z >= 0) s = s.substring(0, z);
            s = s.trim();
            if (!s.isEmpty()) return s;
        }
        // 再 /proc/pid/comm
        s = readFirstLine("/proc/" + pid + "/comm");
        return s == null ? "" : s.trim();
    }

    private static String readFirstLine(String path) {
        try (FileInputStream fis = new FileInputStream(path)) {
            byte[] buf = new byte[512];
            int n = fis.read(buf);
            if (n <= 0) return "";
            return new String(buf, 0, n, "UTF-8");
        } catch (Exception e) { return null; }
    }

    // ── 确认对话框 ─────────────────────────────────────────
    private void confirm(String msg, Runnable onYes) {
        new AlertDialog.Builder(this)
                .setMessage(msg)
                .setPositiveButton("确定", (d, w) -> onYes.run())
                .setNegativeButton("取消", null)
                .show();
    }

    // ── 小工具 ───────────────────────────────────────────────
    private void toast(String msg) { Toast.makeText(this, msg, Toast.LENGTH_SHORT).show(); }

    private int parseIntOrZero(String s) {
        try { return Integer.parseInt(s); } catch (Exception e) { return 0; }
    }

    // ── 本地持久化 ──────────────────────────────────────────
    private File keyFile(String name) { return new File(getCacheDir(), name); }

    private synchronized Set<String> loadKey(String name) {
        Set<String> set = new HashSet<>();
        File f = keyFile(name);
        if (!f.exists()) return set;
        try (BufferedReader br = new BufferedReader(new FileReader(f))) {
            String line;
            while ((line = br.readLine()) != null) {
                String t = line.trim();
                if (!t.isEmpty()) set.add(t);
            }
        } catch (IOException ignored) {}
        return set;
    }

    private synchronized void saveKey(String name, Set<String> set) {
        File f = keyFile(name);
        List<String> lines = new ArrayList<>(set);
        Collections.sort(lines);
        try (FileOutputStream fos = new FileOutputStream(f, false)) {
            StringBuilder sb = new StringBuilder();
            for (String s : lines) sb.append(s).append('\n');
            fos.write(sb.toString().getBytes("UTF-8"));
        } catch (IOException ignored) {}
    }

    private void persistAdd(String name, String item) {
        Set<String> s = loadKey(name); s.add(item); saveKey(name, s);
    }
    private void persistRemove(String name, String item) {
        Set<String> s = loadKey(name); s.remove(item); saveKey(name, s);
    }
    private void persistClear(String name) {
        File f = keyFile(name); if (f.exists()) f.delete();
    }

    private String readSuperkeyFromCache() {
        File f = new File(getCacheDir(), ".kp_key");
        if (!f.exists() || f.length() == 0 || f.length() > 128) return null;
        try (BufferedReader br = new BufferedReader(new FileReader(f))) {
            StringBuilder sb = new StringBuilder();
            String line; while ((line = br.readLine()) != null) sb.append(line);
            String s = sb.toString().trim();
            return s.isEmpty() ? null : s;
        } catch (IOException e) { return null; }
    }

    // ── Native 绑定 ─────────────────────────────────────────
    public native boolean nativeIsModuleLoaded();
    public native void    nativeSetSuperkey(@NonNull String key);
    public native String  nativeRawCtl(@NonNull String cmd);
    public native String  nativeGetStatus();

    public native boolean nativeEnableProcHide();
    public native boolean nativeDisableProcHide();
    public native boolean nativeAddHidePid(int pid);
    public native boolean nativeRemoveHidePid(int pid);
    public native boolean nativeClearHidePid();
    public native String  nativeListHidePid();

    public native boolean nativeEnableFileHide();
    public native boolean nativeDisableFileHide();
    public native boolean nativeAddHideSo(@NonNull String name);
    public native boolean nativeRemoveHideSo(@NonNull String name);
    public native boolean nativeClearHideSo();
    public native String  nativeListHideSo();

    public native boolean nativeHideSelf();
    public native boolean nativeUnhideSelf();

    // 包名级监控接口
    public native String  nativeListHidePkg();
    public native boolean nativeAddHidePkg(@NonNull String name);
    public native boolean nativeRemoveHidePkg(@NonNull String name);
    public native boolean nativeClearHidePkg();

    // 线程名级隐藏接口
    public native String  nativeListHideComm();
    public native boolean nativeAddHideComm(@NonNull String name);
    public native boolean nativeRemoveHideComm(@NonNull String name);
    public native boolean nativeClearHideComm();
    public native boolean nativeEnableCommHide();
    public native boolean nativeDisableCommHide();

    // 进程枚举 (root 读 ps -A, 复用 SoDumper 的实现)
    public native String[] nativeListRunningApps(@NonNull String filter);

    // ── Root 痕迹隐藏 (RootHide 模块) ─────────────────────
    // enable/disable 切换总开关；KPM 会自动同步 file_hide 与 hide_so 注入
    public native boolean nativeEnableRootHide();
    public native boolean nativeDisableRootHide();
    // 返回 status_root 文本 (root_hide=.., file_hide=.., root_kw_count=..)
    public native String  nativeGetStatusRoot();
    // Root 关键词列表读写（与 hide_so 共享底层存储，但独立管理注入状态）
    public native String  nativeListHideRoot();
    public native boolean nativeAddHideRoot(@NonNull String name);
    public native boolean nativeRemoveHideRoot(@NonNull String name);
    public native boolean nativeClearHideRoot();
    public native boolean nativeResetHideRoot();

    // ── 系统进程豁免 (sys_exempt) ───────────────────────
    // 启用后 UID < sys_exempt_uid_max 的调用方被视为 trusted，避免
    // installd/system_server/zygote 等系统链路被 file_hide/root_hide 误拦。
    public native boolean nativeEnableSysExempt();
    public native boolean nativeDisableSysExempt();
    public native boolean nativeSetSysExemptUid(int uidMax);

    // ── KPM 日志总开关 ─────────────────────────────
    // 控制内核侧 klog/klog_dbg 的输出；klog_err/klog_always 不受影响。
    public native boolean nativeEnableLog();
    public native boolean nativeDisableLog();
    public native String  nativeGetStatusLog();
}
