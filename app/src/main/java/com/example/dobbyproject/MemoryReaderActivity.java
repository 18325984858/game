package com.example.dobbyproject;

import android.os.Bundle;
import android.view.View;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AppCompatActivity;

import android.text.Editable;
import android.text.TextWatcher;

import java.math.BigInteger;
import java.nio.charset.Charset;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * 任意进程内存读取器
 * - 使用 root 权限通过 so_dumper 相同的 su+dd 管道读取 /proc/PID/mem
 * - 支持显示: Hex Dump / 1-2-4-8 字节整数(LE/BE, 有无符号) / 自定义字宽 / 自动识别字符串
 */
public class MemoryReaderActivity extends AppCompatActivity {

    // ── Native ──
    private native String[] nativeListRunningApps(String filter);
    private native String[] nativeListModules(int pid);
    private native String[] nativeListAllRegions(int pid);
    private native byte[]   nativeReadMemory(int pid, long address, int size);
    private native int      nativeWriteMemory(int pid, long address, byte[] data);
    private native String   nativeFindRegion(int pid, long address);
    private native long[]   nativeSearchPattern(int pid, long rangeStart, long rangeEnd,
                                                 byte[] pattern, byte[] mask, int maxHits);
    private native String[] nativeGlobalSearch(int pid, byte[] pattern, byte[] mask,
                                                int maxHits, boolean onlyWritable, boolean skipBigRo);
    /** true = 持久 root shell 已被判定为"未授权" (su 无响应). */
    private native boolean  nativeIsRootAuthDenied();

    static { System.loadLibrary("dobbyproject"); }

    // ── 数据结构 ──
    private static class AppItem {
        int pid;
        String packageName;
        AppItem(int p, String n) { pid = p; packageName = n; }
        @Override public String toString() { return packageName + " (PID:" + pid + ")"; }
    }

    private static class ModuleItem {
        long baseAddr;
        long endAddr;
        long size;
        String name;
        String path;
        String perms;  // 权限, 仅 allRegions 模式下填充
        @Override public String toString() {
            String p = (perms != null && !perms.isEmpty()) ? (" " + perms) : "";
            return name + p + "  (" + String.format("0x%X", baseAddr) + ", " + formatSize(size) + ")";
        }
        static String formatSize(long s) {
            if (s < 1024) return s + "B";
            if (s < 1024 * 1024) return String.format("%.1fK", s / 1024.0);
            return String.format("%.2fM", s / 1024.0 / 1024.0);
        }
    }

    // ── View 模式 ──
    private static final String[] VIEW_MODES = {
            "Hex Dump (16B/行)",
            "1 字节整数",
            "2 字节整数",
            "4 字节整数",
            "8 字节整数",
            "float (32-bit 单精度)",
            "double (64-bit 双精度)",
            "自定义字宽",
            "字符串 (自动识别 GBK/UTF-8/UTF-16)",
            "C 字符串 (ANSI, 遇 \\0 停止)",
            "宽字符串 (UTF-16LE, 遇 \\0\\0 停止)",
            "反汇编 (ARM64)",
    };

    private static final String[] ENDIANS = { "小端 LE", "大端 BE" };

    // ── 控件 ──
    private EditText etSearch, etModuleFilter, etAddress, etLength, etCustomWidth;
    private Spinner  spApps, spModules, spViewMode, spEndian;
    private CheckBox cbSigned, cbAllRegions;
    private Button   btnRefresh, btnRead, btnUseModuleBase;
    private Button   btnDeref, btnPrev, btnNext, btnSearch, btnWrite;
    private TextView tvOutput, tvStatus;
    private ProgressBar pbLoading;

    private final List<AppItem>    appList = new ArrayList<>();
    private final List<ModuleItem> allModules = new ArrayList<>();
    private final List<ModuleItem> filteredModules = new ArrayList<>();
    private ArrayAdapter<String> appAdapter, moduleAdapter;

    private int selectedPid = -1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // 本页面使用 process_vm_readv (普通 root 即可), 不依赖 KPM/superkey.
        // 在装了 APatch 的设备上仍走原有 superkey 闸门 (供 InjectHide 复用);
        // 没装 APatch 的设备跳过该门, 避免被 "请输入 Super Key" Toast 拦住.
        if (MainActivity.nativeIsApatchAvailable()
                && !KpKeyStore.requireOrRedirect(this)) return;
        setContentView(R.layout.activity_memory_reader);
        setTitle("内存读取器");

        etSearch         = findViewById(R.id.mem_et_search);
        etModuleFilter   = findViewById(R.id.mem_et_module_filter);
        etAddress        = findViewById(R.id.mem_et_address);
        etLength         = findViewById(R.id.mem_et_length);
        etCustomWidth    = findViewById(R.id.mem_et_custom_width);
        spApps           = findViewById(R.id.mem_spinner_apps);
        spModules        = findViewById(R.id.mem_spinner_modules);
        spViewMode       = findViewById(R.id.mem_spinner_viewmode);
        spEndian         = findViewById(R.id.mem_spinner_endian);
        cbSigned         = findViewById(R.id.mem_cb_signed);
        cbAllRegions     = findViewById(R.id.mem_cb_all_regions);
        btnRefresh       = findViewById(R.id.mem_btn_refresh);
        btnRead          = findViewById(R.id.mem_btn_read);
        btnUseModuleBase = findViewById(R.id.mem_btn_use_module_base);
        btnDeref         = findViewById(R.id.mem_btn_deref);
        btnPrev          = findViewById(R.id.mem_btn_prev);
        btnNext          = findViewById(R.id.mem_btn_next);
        btnSearch        = findViewById(R.id.mem_btn_search);
        btnWrite         = findViewById(R.id.mem_btn_write);
        tvOutput         = findViewById(R.id.mem_tv_output);
        tvStatus         = findViewById(R.id.mem_tv_status);
        pbLoading        = findViewById(R.id.mem_progress);

        appAdapter    = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        appAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spApps.setAdapter(appAdapter);

        moduleAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, new ArrayList<>());
        moduleAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spModules.setAdapter(moduleAdapter);

        ArrayAdapter<String> viewAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, VIEW_MODES);
        viewAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spViewMode.setAdapter(viewAdapter);

        ArrayAdapter<String> endianAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, ENDIANS);
        endianAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spEndian.setAdapter(endianAdapter);

        spApps.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                if (pos >= 0 && pos < appList.size()) {
                    selectedPid = appList.get(pos).pid;
                    loadModules(selectedPid);
                }
            }
            @Override public void onNothingSelected(AdapterView<?> p) { }
        });

        etModuleFilter.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int st, int c, int a) {}
            @Override public void onTextChanged(CharSequence s, int st, int b, int c) {}
            @Override public void afterTextChanged(Editable s) { applyModuleFilter(); }
        });

        cbAllRegions.setOnCheckedChangeListener((v, checked) -> {
            if (selectedPid > 0) loadModules(selectedPid);
        });

        btnRefresh.setOnClickListener(v -> refreshApps());

        btnUseModuleBase.setOnClickListener(v -> {
            int idx = spModules.getSelectedItemPosition();
            if (idx < 0 || idx >= filteredModules.size()) {
                Toast.makeText(this, "请先选择模块", Toast.LENGTH_SHORT).show();
                return;
            }
            ModuleItem selected = filteredModules.get(idx);
            long realBase = resolveModuleStartBase(selected);
            etAddress.setText(String.format("0x%X", realBase));
            tvStatus.setText(String.format(Locale.ROOT,
                    "已使用 %s 的起始 BASE: 0x%X", selected.name, realBase));
        });

        btnRead.setOnClickListener(v -> doRead());

        btnDeref.setOnClickListener(v -> doDereference());
        btnPrev.setOnClickListener(v -> shiftAddress(-1));
        btnNext.setOnClickListener(v -> shiftAddress(+1));
        btnSearch.setOnClickListener(v -> showSearchDialog());
        btnWrite.setOnClickListener(v -> showWriteDialog());

        refreshApps();
    }

    // ─── 进程 / 模块枚举 ─────────────────────────────────────────────

    private void refreshApps() {
        String filter = etSearch.getText().toString().trim();
        btnRefresh.setEnabled(false);
        tvStatus.setText("正在枚举进程...");
        new Thread(() -> {
            String[] arr;
            try { arr = nativeListRunningApps(filter); }
            catch (Throwable t) { arr = new String[0]; }
            final String[] items = arr;

            final List<AppItem> collected = new ArrayList<>();
            final List<String> labels = new ArrayList<>();
            if (items != null) {
                for (String s : items) {
                    int colon = s.indexOf(':');
                    if (colon <= 0) continue;
                    try {
                        int pid = Integer.parseInt(s.substring(0, colon));
                        String name = s.substring(colon + 1);
                        AppItem it = new AppItem(pid, name);
                        collected.add(it);
                        labels.add(it.toString());
                    } catch (NumberFormatException ignored) {}
                }
            }
            runOnUiThread(() -> {
                appList.clear();
                appList.addAll(collected);
                appAdapter.clear();
                appAdapter.addAll(labels);
                appAdapter.notifyDataSetChanged();
                btnRefresh.setEnabled(true);
                if (appList.isEmpty()) {
                    boolean denied = false;
                    try { denied = nativeIsRootAuthDenied(); } catch (Throwable ignored) {}
                    if (denied) {
                        tvStatus.setText("✘ 枚举失败: 本 app 未获 root 授权\n" +
                                "请打开 APatch / KernelSU 管理器 → 超级用户 → 为本 app 授权 → 重启 app");
                    } else {
                        tvStatus.setText("共 0 个进程 (未找到匹配, 或 root shell 无响应)");
                    }
                } else {
                    tvStatus.setText("共 " + appList.size() + " 个进程");
                    spApps.setSelection(0);
                    selectedPid = appList.get(0).pid;
                    loadModules(selectedPid);
                }
            });
        }).start();
    }

    private void loadModules(int pid) {
        final boolean allRegions = cbAllRegions != null && cbAllRegions.isChecked();
        tvStatus.setText(allRegions
                ? ("正在枚举所有映射 (PID " + pid + ")...")
                : ("正在枚举 SO 模块 (PID " + pid + ")..."));
        new Thread(() -> {
            String[] arr;
            try {
                arr = allRegions ? nativeListAllRegions(pid) : nativeListModules(pid);
            } catch (Throwable t) { arr = new String[0]; }
            final String[] items = arr;

            final List<ModuleItem> collected = new ArrayList<>();
            if (items != null) {
                for (String s : items) {
                    String[] parts = s.split(":", allRegions ? 6 : 5);
                    if (parts.length < (allRegions ? 6 : 5)) continue;
                    try {
                        ModuleItem m = new ModuleItem();
                        m.baseAddr = parseHexLong(parts[0]);
                        m.endAddr  = parseHexLong(parts[1]);
                        m.size     = Long.parseLong(parts[2]);
                        m.name     = parts[3];
                        m.path     = parts[4];
                        m.perms    = allRegions ? parts[5] : "";
                        collected.add(m);
                    } catch (Exception ignored) {}
                }
            }
            runOnUiThread(() -> {
                allModules.clear();
                allModules.addAll(collected);
                applyModuleFilter();
                tvStatus.setText("共 " + allModules.size() + (allRegions ? " 个映射" : " 个模块"));
            });
        }).start();
    }

    private void applyModuleFilter() {
        String f = etModuleFilter.getText().toString().trim().toLowerCase();
        filteredModules.clear();
        for (ModuleItem m : allModules) {
            if (f.isEmpty() || m.name.toLowerCase().contains(f)) {
                filteredModules.add(m);
            }
        }
        List<String> labels = new ArrayList<>();
        for (ModuleItem m : filteredModules) labels.add(m.toString());
        moduleAdapter.clear();
        moduleAdapter.addAll(labels);
        moduleAdapter.notifyDataSetChanged();
    }

    private static String normalizedModuleKey(ModuleItem item) {
        if (item == null) return "";
        String path = item.path == null ? "" : item.path.trim().toLowerCase(Locale.ROOT);
        if (!path.isEmpty() && path.startsWith("/")) {
            return path;
        }
        return item.name == null ? "" : item.name.trim().toLowerCase(Locale.ROOT);
    }

    private long resolveModuleStartBase(ModuleItem selected) {
        if (selected == null) return 0L;
        long minBase = selected.baseAddr;
        String selectedKey = normalizedModuleKey(selected);
        String selectedName = selected.name == null ? "" : selected.name.trim().toLowerCase(Locale.ROOT);

        for (ModuleItem m : allModules) {
            String key = normalizedModuleKey(m);
            String name = m.name == null ? "" : m.name.trim().toLowerCase(Locale.ROOT);
            boolean sameModule = (!selectedKey.isEmpty() && selectedKey.equals(key))
                    || (!selectedName.isEmpty() && selectedName.equals(name));
            if (sameModule && m.baseAddr < minBase) {
                minBase = m.baseAddr;
            }
        }
        return minBase;
    }

    // ─── 读内存 + 显示 ───────────────────────────────────────────────

    private void doRead() {
        if (selectedPid <= 0) {
            Toast.makeText(this, "请先选择目标进程", Toast.LENGTH_SHORT).show();
            return;
        }
        long addr;
        try { addr = parseAddressExpr(etAddress.getText().toString().trim()); }
        catch (Exception e) {
            Toast.makeText(this, "地址解析失败: " + e.getMessage(), Toast.LENGTH_LONG).show();
            return;
        }

        int length;
        try { length = Math.max(1, Integer.parseInt(etLength.getText().toString().trim())); }
        catch (Exception e) { length = 256; }

        // native 最大支持 64MB/次, UI 再做一个上限保护
        if (length > 64 * 1024 * 1024) length = 64 * 1024 * 1024;

        final long readAddr = addr;
        final int  readLen  = length;

        btnRead.setEnabled(false);
        pbLoading.setVisibility(View.VISIBLE);
        tvStatus.setText("正在读取 0x" + Long.toHexString(readAddr) + ", " + readLen + " 字节 ...");

        new Thread(() -> {
            byte[] data;
            try { data = nativeReadMemory(selectedPid, readAddr, readLen); }
            catch (Throwable t) { data = new byte[0]; }

            if (data == null) data = new byte[0];

            // 查询地址所在段
            String regionStr = "";
            try {
                String r = nativeFindRegion(selectedPid, readAddr);
                if (r != null && !r.isEmpty()) {
                    String[] p = r.split(":", 5);
                    if (p.length >= 5) {
                        long base = parseHexLong(p[0]);
                        regionStr = String.format("  [%s %s  base=0x%X  +0x%X]",
                                p[2], p[4], base, (readAddr & 0x00FFFFFFFFFFFFFFL) - base);
                    }
                }
            } catch (Throwable ignored) {}
            final String region = regionStr;
            final byte[] bytes = data;
            runOnUiThread(() -> {
                pbLoading.setVisibility(View.GONE);
                btnRead.setEnabled(true);
                if (bytes.length == 0) {
                    tvStatus.setText("读取失败 (无数据 / 地址不可读 / root 被拒)" + region);
                    tvOutput.setText("");
                    return;
                }
                tvStatus.setText(String.format("读取 %d 字节 @ 0x%X (请求 %d)%s",
                        bytes.length, readAddr, readLen, region));
                String formatted = formatBytes(bytes, readAddr,
                        spViewMode.getSelectedItemPosition(),
                        spEndian.getSelectedItemPosition() == 1,
                        cbSigned.isChecked(),
                        parseCustomWidth());
                tvOutput.setText(formatted);
            });
        }).start();
    }

    // ─── 指针跟随: 读当前地址的 8 字节 (LE) 视作指针, 填回地址框 ───
    private void doDereference() {
        if (selectedPid <= 0) { Toast.makeText(this, "请先选择进程", Toast.LENGTH_SHORT).show(); return; }
        long addr;
        try { addr = parseAddressExpr(etAddress.getText().toString().trim()); }
        catch (Exception e) { Toast.makeText(this, "地址解析失败", Toast.LENGTH_SHORT).show(); return; }

        btnDeref.setEnabled(false);
        final long ptrAddr = addr;
        new Thread(() -> {
            byte[] data;
            try { data = nativeReadMemory(selectedPid, ptrAddr, 8); }
            catch (Throwable t) { data = new byte[0]; }
            final byte[] bytes = (data == null ? new byte[0] : data);
            runOnUiThread(() -> {
                btnDeref.setEnabled(true);
                if (bytes.length < 8) {
                    Toast.makeText(this, "读取指针失败", Toast.LENGTH_SHORT).show();
                    return;
                }
                long p = 0;
                for (int i = 7; i >= 0; i--) p = (p << 8) | (bytes[i] & 0xFFL);
                etAddress.setText(String.format("0x%X", p));
                tvStatus.setText(String.format("指针跟随: *0x%X = 0x%X", ptrAddr, p));
                doRead();
            });
        }).start();
    }

    // ─── 前 / 后 N 字节翻页 ───
    private void shiftAddress(int direction) {
        try {
            long addr = parseAddressExpr(etAddress.getText().toString().trim());
            int len;
            try { len = Math.max(1, Integer.parseInt(etLength.getText().toString().trim())); }
            catch (Exception e) { len = 256; }
            long next = addr + (long) direction * len;
            etAddress.setText(String.format("0x%X", next));
            doRead();
        } catch (Exception e) {
            Toast.makeText(this, "地址解析失败", Toast.LENGTH_SHORT).show();
        }
    }

    // ─── 搜索字节 / 字符串对话框 ───
    private void showSearchDialog() {
        if (selectedPid <= 0) { Toast.makeText(this, "请先选择进程", Toast.LENGTH_SHORT).show(); return; }

        android.widget.LinearLayout root = new android.widget.LinearLayout(this);
        root.setOrientation(android.widget.LinearLayout.VERTICAL);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        final android.widget.RadioGroup rg = new android.widget.RadioGroup(this);
        rg.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        android.widget.RadioButton rbHex = new android.widget.RadioButton(this); rbHex.setText("十六进制 (?? 通配)");
        android.widget.RadioButton rbStr = new android.widget.RadioButton(this); rbStr.setText("字符串");
        rg.addView(rbHex); rg.addView(rbStr);
        rbHex.setChecked(true);
        root.addView(rg);

        final EditText etQuery = new EditText(this);
        etQuery.setHint("例: 48 8B ?? ?? 89 C2  或  hello");
        etQuery.setSingleLine(true);
        root.addView(etQuery);

        android.widget.LinearLayout rangeRow = new android.widget.LinearLayout(this);
        rangeRow.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        final EditText etStart = new EditText(this); etStart.setHint("起始 (空=当前段)"); etStart.setSingleLine(true);
        final EditText etEnd   = new EditText(this); etEnd.setHint("结束");               etEnd.setSingleLine(true);
        android.widget.LinearLayout.LayoutParams lp =
                new android.widget.LinearLayout.LayoutParams(0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        etStart.setLayoutParams(lp); etEnd.setLayoutParams(lp);
        rangeRow.addView(etStart); rangeRow.addView(etEnd);
        root.addView(rangeRow);

        final EditText etMax = new EditText(this); etMax.setHint("最多命中数 (默认 64)"); etMax.setText("64"); etMax.setSingleLine(true);
        etMax.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        root.addView(etMax);

        // 全局搜索: 枚举目标进程所有可读映射
        final CheckBox cbGlobal   = new CheckBox(this); cbGlobal.setText("全局搜索 (忽略起止范围)");
        final CheckBox cbOnlyWr   = new CheckBox(this); cbOnlyWr.setText("仅 rw-p (堆/数据段, 更快)");
        final CheckBox cbSkipBig  = new CheckBox(this); cbSkipBig.setText("跳过 >64MB 只读文件映射");
        cbOnlyWr.setChecked(false);
        cbSkipBig.setChecked(true);
        root.addView(cbGlobal);
        root.addView(cbOnlyWr);
        root.addView(cbSkipBig);

        // 全局搜索勾选时禁用起止地址
        cbGlobal.setOnCheckedChangeListener((v, checked) -> {
            etStart.setEnabled(!checked);
            etEnd.setEnabled(!checked);
        });

        // 默认用选中映射作为范围
        int mi = spModules.getSelectedItemPosition();
        if (mi >= 0 && mi < filteredModules.size()) {
            ModuleItem m = filteredModules.get(mi);
            etStart.setText(String.format("0x%X", m.baseAddr));
            etEnd.setText(String.format("0x%X", m.endAddr));
        }

        new androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("搜索 (PID " + selectedPid + ")")
            .setView(root)
            .setPositiveButton("搜索", (d, w) -> {
                String q = etQuery.getText().toString().trim();
                if (q.isEmpty()) { Toast.makeText(this, "请输入关键词", Toast.LENGTH_SHORT).show(); return; }

                byte[] pattern, mask;
                if (rbHex.isChecked()) {
                    try { byte[][] pm = parseHexPattern(q); pattern = pm[0]; mask = pm[1]; }
                    catch (Exception ex) { Toast.makeText(this, "pattern 解析失败: " + ex.getMessage(), Toast.LENGTH_LONG).show(); return; }
                } else {
                    pattern = q.getBytes(StandardCharsets.UTF_8);
                    mask = new byte[pattern.length];
                    for (int i = 0; i < mask.length; i++) mask[i] = (byte) 0xFF;
                }

                int maxHits;
                try { maxHits = Math.max(1, Integer.parseInt(etMax.getText().toString().trim())); }
                catch (Exception ex) { maxHits = 64; }

                if (cbGlobal.isChecked()) {
                    runGlobalSearch(pattern, mask, maxHits, cbOnlyWr.isChecked(), cbSkipBig.isChecked());
                    return;
                }

                long s, e;
                try { s = parseAddressExpr(etStart.getText().toString().trim()); }
                catch (Exception ex) { Toast.makeText(this, "起始地址错误", Toast.LENGTH_SHORT).show(); return; }
                try { e = parseAddressExpr(etEnd.getText().toString().trim()); }
                catch (Exception ex) { Toast.makeText(this, "结束地址错误", Toast.LENGTH_SHORT).show(); return; }
                if (e <= s) { Toast.makeText(this, "结束需 > 起始", Toast.LENGTH_SHORT).show(); return; }

                runSearch(s, e, pattern, mask, maxHits);
            })
            .setNegativeButton("取消", null)
            .show();
    }

    private void runSearch(long start, long end, byte[] pattern, byte[] mask, int maxHits) {
        tvStatus.setText(String.format("搜索中: [0x%X, 0x%X) patLen=%d ...", start, end, pattern.length));
        pbLoading.setVisibility(View.VISIBLE);
        final long s = start, e = end;
        final byte[] p = pattern, m = mask;
        final int mh = maxHits;
        new Thread(() -> {
            long[] hits;
            try { hits = nativeSearchPattern(selectedPid, s, e, p, m, mh); }
            catch (Throwable t) { hits = new long[0]; }
            final long[] result = (hits == null ? new long[0] : hits);
            final int previewLen = Math.max(p.length, Math.min(p.length + 8, 48));
            final byte[][] previews = new byte[result.length][];
            for (int i = 0; i < result.length; i++) {
                try { previews[i] = nativeReadMemory(selectedPid, result[i], previewLen); }
                catch (Throwable t) { previews[i] = new byte[0]; }
            }
            runOnUiThread(() -> {
                pbLoading.setVisibility(View.GONE);
                StringBuilder sb = new StringBuilder();
                sb.append(String.format("[搜索结果]  范围=[0x%X, 0x%X)  命中=%d\n", s, e, result.length));
                sb.append("─────────────────────────────────\n");
                for (int i = 0; i < result.length; i++) {
                    sb.append(String.format("  #%d  0x%X\n", i + 1, result[i]));
                    sb.append("       ").append(formatPreview(previews[i], p.length)).append('\n');
                }
                tvOutput.setText(sb.toString());
                tvStatus.setText("搜索完成, 共 " + result.length + " 条");
                if (result.length > 0) {
                    etAddress.setText(String.format("0x%X", result[0]));
                }
            });
        }).start();
    }

    /**
     * 全局搜索: 枚举目标进程所有可读 maps, 返回的每条格式:
     *   "addrHex|name|offsetHex|path|perms"
     *   - 文件映射: offset = addr - moduleBase (同 path 的最小 start)  → 显示 "name+0xOFF"
     *   - 匿名/[heap]/[stack]: offset = "+0xN" (region 内偏移) → 显示 "[heap]+0xOFF" 及原始地址
     */
    private void runGlobalSearch(byte[] pattern, byte[] mask, int maxHits,
                                 boolean onlyWritable, boolean skipBigRo) {
        tvStatus.setText(String.format("全局搜索中: patLen=%d onlyW=%s skipBigRo=%s ...",
                pattern.length, onlyWritable, skipBigRo));
        pbLoading.setVisibility(View.VISIBLE);
        final byte[] p = pattern, m = mask;
        final int mh = maxHits;
        final boolean ow = onlyWritable, sb = skipBigRo;
        new Thread(() -> {
            String[] rows;
            try { rows = nativeGlobalSearch(selectedPid, p, m, mh, ow, sb); }
            catch (Throwable t) { rows = new String[0]; }
            final String[] result = (rows == null ? new String[0] : rows);
            // 读取每条命中处的实际字节, 显示在结果后
            final int previewLen = Math.max(p.length, Math.min(p.length + 8, 48));
            final byte[][] previews = new byte[result.length][];
            for (int i = 0; i < result.length; i++) {
                try {
                    String row = result[i];
                    int bar = row.indexOf('|');
                    String addrS = (bar > 0) ? row.substring(0, bar) : row;
                    long addr = new BigInteger(addrS.startsWith("0x") ? addrS.substring(2) : addrS, 16).longValue();
                    previews[i] = nativeReadMemory(selectedPid, addr, previewLen);
                } catch (Throwable t) { previews[i] = new byte[0]; }
            }
            runOnUiThread(() -> {
                pbLoading.setVisibility(View.GONE);
                StringBuilder out = new StringBuilder();
                out.append(String.format("[全局搜索]  patLen=%d  onlyWritable=%s  skipBigRo=%s  命中=%d\n",
                        p.length, ow, sb, result.length));
                out.append("─────────────────────────────────\n");
                long firstAddr = 0;
                for (int i = 0; i < result.length; i++) {
                    String row = result[i];
                    String[] parts = row.split("\\|", -1);
                    // parts: addr | name | offset | path | perms
                    String addrS  = parts.length > 0 ? parts[0] : "";
                    String name   = parts.length > 1 ? parts[1] : "";
                    String offS   = parts.length > 2 ? parts[2] : "";
                    String path   = parts.length > 3 ? parts[3] : "";
                    String perms  = parts.length > 4 ? parts[4] : "";

                    if (i == 0) {
                        try { firstAddr = new BigInteger(addrS.startsWith("0x") ? addrS.substring(2) : addrS, 16).longValue(); }
                        catch (Exception ex) { firstAddr = 0; }
                    }

                    boolean fileBacked = path.startsWith("/");
                    if (fileBacked) {
                        out.append(String.format("  #%-3d %s+%s   (%s)  [%s]\n",
                                i + 1, name, offS, addrS, perms));
                    } else {
                        out.append(String.format("  #%-3d %s%s   (%s)  [%s]\n",
                                i + 1, name, offS, addrS, perms));
                    }
                    out.append("       ").append(formatPreview(previews[i], p.length)).append('\n');
                }
                tvOutput.setText(out.toString());
                tvStatus.setText("全局搜索完成, 共 " + result.length + " 条");
                if (result.length > 0 && firstAddr != 0) {
                    etAddress.setText(String.format("0x%X", firstAddr));
                }
            });
        }).start();
    }

    /**
     * 把命中处读取到的字节格式化成 "HEX | ASCII" 预览.
     *   hilightLen 表示 pattern 原长, 之后的字节用 '.' 分隔以区分
     */
    private static String formatPreview(byte[] data, int hilightLen) {
        if (data == null || data.length == 0) return "(读取失败)";
        StringBuilder hex = new StringBuilder();
        StringBuilder asc = new StringBuilder();
        for (int i = 0; i < data.length; i++) {
            if (i == hilightLen && i > 0) { hex.append("| "); }
            hex.append(String.format("%02X ", data[i] & 0xFF));
            int b = data[i] & 0xFF;
            asc.append((b >= 0x20 && b < 0x7F) ? (char) b : '.');
        }
        return hex.toString().trim() + "   " + asc.toString();
    }

    // ─── 写入内存 ─────────────────────────────────────────────
    private static final String[] WRITE_TYPES = {
            "字节 (十六进制 48 8B ...)",
            "字符串 (UTF-8)",
            "宽字符串 (UTF-16LE)",
            "整数 1 字节",
            "整数 2 字节",
            "整数 4 字节",
            "整数 8 字节",
            "float 32-bit",
            "double 64-bit",
    };

    private void showWriteDialog() {
        if (selectedPid <= 0) { Toast.makeText(this, "请先选择进程", Toast.LENGTH_SHORT).show(); return; }

        android.widget.LinearLayout root = new android.widget.LinearLayout(this);
        root.setOrientation(android.widget.LinearLayout.VERTICAL);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView tvAddr = new TextView(this);
        tvAddr.setText("目标地址: " + etAddress.getText().toString().trim() + "   PID: " + selectedPid);
        root.addView(tvAddr);

        final Spinner spType = new Spinner(this);
        ArrayAdapter<String> typeAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, WRITE_TYPES);
        typeAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spType.setAdapter(typeAdapter);
        root.addView(spType);

        final Spinner spEnd = new Spinner(this);
        ArrayAdapter<String> endAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, ENDIANS);
        endAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spEnd.setAdapter(endAdapter);
        root.addView(spEnd);

        final EditText etValue = new EditText(this);
        etValue.setHint("待写入的值");
        etValue.setSingleLine(false);
        etValue.setMinLines(2);
        root.addView(etValue);

        final CheckBox cbConfirm = new CheckBox(this);
        cbConfirm.setText("我已确认要修改目标进程内存 (不可撤销)");
        root.addView(cbConfirm);

        new androidx.appcompat.app.AlertDialog.Builder(this)
            .setTitle("写入内存 (PID " + selectedPid + ")")
            .setView(root)
            .setPositiveButton("写入", (d, w) -> {
                if (!cbConfirm.isChecked()) {
                    Toast.makeText(this, "请先勾选确认", Toast.LENGTH_SHORT).show();
                    return;
                }
                long addr;
                try { addr = parseAddressExpr(etAddress.getText().toString().trim()); }
                catch (Exception ex) { Toast.makeText(this, "地址错误", Toast.LENGTH_SHORT).show(); return; }

                byte[] payload;
                try {
                    payload = buildWritePayload(
                            spType.getSelectedItemPosition(),
                            etValue.getText().toString(),
                            spEnd.getSelectedItemPosition() == 1);
                } catch (Exception ex) {
                    Toast.makeText(this, "值解析失败: " + ex.getMessage(), Toast.LENGTH_LONG).show();
                    return;
                }
                if (payload == null || payload.length == 0) {
                    Toast.makeText(this, "空 payload", Toast.LENGTH_SHORT).show(); return;
                }

                runWrite(addr, payload);
            })
            .setNegativeButton("取消", null)
            .show();
    }

    /**
     * 根据类型构造要写入的字节序列
     * @param bigEndian 是否大端 (字符串/字节类型忽略)
     */
    private static byte[] buildWritePayload(int typeIdx, String input, boolean bigEndian) {
        String s = input == null ? "" : input.trim();
        switch (typeIdx) {
            case 0: { // hex 字节
                String[] toks = s.split("\\s+");
                byte[] b = new byte[toks.length];
                for (int i = 0; i < toks.length; i++) {
                    if (toks[i].length() != 2) throw new IllegalArgumentException("token: " + toks[i]);
                    b[i] = (byte) Integer.parseInt(toks[i], 16);
                }
                return b;
            }
            case 1: return input.getBytes(StandardCharsets.UTF_8);
            case 2: return input.getBytes(StandardCharsets.UTF_16LE);
            case 3: case 4: case 5: case 6: {
                int width = (typeIdx == 3) ? 1 : (typeIdx == 4) ? 2 : (typeIdx == 5) ? 4 : 8;
                // 支持 0x 前缀和负数
                long v;
                if (s.startsWith("0x") || s.startsWith("0X")) {
                    v = new BigInteger(s.substring(2), 16).longValue();
                } else if (s.startsWith("-0x") || s.startsWith("-0X")) {
                    v = -new BigInteger(s.substring(3), 16).longValue();
                } else {
                    v = new BigInteger(s).longValue();
                }
                byte[] b = new byte[width];
                for (int i = 0; i < width; i++) {
                    int shift = bigEndian ? (width - 1 - i) * 8 : i * 8;
                    b[i] = (byte) ((v >> shift) & 0xFF);
                }
                return b;
            }
            case 7: { // float
                int bits = Float.floatToRawIntBits(Float.parseFloat(s));
                byte[] b = new byte[4];
                for (int i = 0; i < 4; i++) {
                    int shift = bigEndian ? (3 - i) * 8 : i * 8;
                    b[i] = (byte) ((bits >> shift) & 0xFF);
                }
                return b;
            }
            case 8: { // double
                long bits = Double.doubleToRawLongBits(Double.parseDouble(s));
                byte[] b = new byte[8];
                for (int i = 0; i < 8; i++) {
                    int shift = bigEndian ? (7 - i) * 8 : i * 8;
                    b[i] = (byte) ((bits >> shift) & 0xFF);
                }
                return b;
            }
            default: throw new IllegalArgumentException("未知类型 " + typeIdx);
        }
    }

    private void runWrite(long addr, byte[] payload) {
        tvStatus.setText(String.format("写入中: 0x%X 共 %d 字节 ...", addr, payload.length));
        pbLoading.setVisibility(View.VISIBLE);
        final long a = addr;
        final byte[] p = payload;
        new Thread(() -> {
            int result;
            try { result = nativeWriteMemory(selectedPid, a, p); }
            catch (Throwable t) { result = -99; }
            // 写后立即回读校验
            byte[] verify;
            try { verify = nativeReadMemory(selectedPid, a, p.length); }
            catch (Throwable t) { verify = new byte[0]; }
            final int r = result;
            final byte[] vf = verify;
            runOnUiThread(() -> {
                pbLoading.setVisibility(View.GONE);
                StringBuilder sb = new StringBuilder();
                sb.append(String.format("[写入内存]  地址=0x%X  长度=%d  返回=%d\n", a, p.length, r));
                sb.append("─────────────────────────────────\n");
                sb.append("待写入: ").append(formatPreview(p, p.length)).append('\n');
                sb.append("回  读: ").append(formatPreview(vf, p.length)).append('\n');
                boolean match = vf != null && vf.length >= p.length;
                if (match) {
                    for (int i = 0; i < p.length; i++) {
                        if (p[i] != vf[i]) { match = false; break; }
                    }
                }
                sb.append("校  验: ").append(match ? "✓ 一致" : "✗ 不一致").append('\n');
                tvOutput.setText(sb.toString());
                if (r > 0 && match) {
                    tvStatus.setText("写入成功 (" + r + " 字节, 回读一致)");
                } else if (r > 0) {
                    tvStatus.setText("写入 " + r + " 字节但回读不一致, 可能被目标进程覆盖或只读页");
                } else {
                    tvStatus.setText("写入失败 code=" + r);
                }
            });
        }).start();
    }

    /** 解析形如 "48 8B ?? ?? 89 C2" 的 pattern, 返回 {pattern, mask} */
    private static byte[][] parseHexPattern(String s) {
        String[] toks = s.trim().split("\\s+");
        byte[] pat = new byte[toks.length];
        byte[] msk = new byte[toks.length];
        for (int i = 0; i < toks.length; i++) {
            String t = toks[i];
            if (t.equals("?") || t.equals("??")) {
                pat[i] = 0; msk[i] = 0;
            } else {
                if (t.length() != 2) throw new IllegalArgumentException("token: " + t);
                char c0 = t.charAt(0), c1 = t.charAt(1);
                int hi = (c0 == '?') ? 0 : Character.digit(c0, 16);
                int lo = (c1 == '?') ? 0 : Character.digit(c1, 16);
                if (hi < 0 || lo < 0) throw new IllegalArgumentException("token: " + t);
                pat[i] = (byte) ((hi << 4) | lo);
                int mh = (c0 == '?') ? 0 : 0xF;
                int ml = (c1 == '?') ? 0 : 0xF;
                msk[i] = (byte) ((mh << 4) | ml);
            }
        }
        return new byte[][] { pat, msk };
    }

    private int parseCustomWidth() {
        try {
            int w = Integer.parseInt(etCustomWidth.getText().toString().trim());
            if (w < 1) w = 1;
            if (w > 16) w = 16;
            return w;
        } catch (Exception e) {
            return 16;
        }
    }

    // ─── 地址表达式解析: "0x1234", "1234", "0x1234+0x100", "0xBASE + 512" ─────
    private static long parseAddressExpr(String expr) {
        if (expr.isEmpty()) throw new IllegalArgumentException("地址为空");
        // 允许 "+" 分段相加
        String[] parts = expr.split("\\+");
        BigInteger sum = BigInteger.ZERO;
        for (String p : parts) {
            p = p.trim();
            if (p.isEmpty()) continue;
            int radix = 10;
            String s = p;
            if (s.startsWith("0x") || s.startsWith("0X")) { s = s.substring(2); radix = 16; }
            else if (s.matches(".*[a-fA-F].*")) { radix = 16; }
            sum = sum.add(new BigInteger(s, radix));
        }
        return sum.longValue();
    }

    private static long parseHexLong(String hex) {
        String c = hex.startsWith("0x") || hex.startsWith("0X") ? hex.substring(2) : hex;
        return Long.parseUnsignedLong(c, 16);
    }

    // ─── 字节格式化 ───────────────────────────────────────────────────

    private static String formatBytes(byte[] data, long baseAddr,
                                      int mode, boolean bigEndian, boolean signed, int customWidth) {
        switch (mode) {
            case 0:  return dumpHex(data, baseAddr);
            case 1:  return dumpInts(data, baseAddr, 1, bigEndian, signed);
            case 2:  return dumpInts(data, baseAddr, 2, bigEndian, signed);
            case 3:  return dumpInts(data, baseAddr, 4, bigEndian, signed);
            case 4:  return dumpInts(data, baseAddr, 8, bigEndian, signed);
            case 5:  return dumpFloats(data, baseAddr, false, bigEndian);
            case 6:  return dumpFloats(data, baseAddr, true,  bigEndian);
            case 7:  return dumpCustom(data, baseAddr, customWidth, bigEndian, signed);
            case 8:  return dumpStringAuto(data);
            case 9:  return dumpCString(data);
            case 10: return dumpWideString(data);
            case 11: return Arm64Disassembler.disassemble(data, baseAddr);
            default: return dumpHex(data, baseAddr);
        }
    }

    /** 经典 hex dump: ADDR  HH HH HH HH ... | ASCII */
    private static String dumpHex(byte[] data, long baseAddr) {
        StringBuilder sb = new StringBuilder(data.length * 5);
        for (int off = 0; off < data.length; off += 16) {
            sb.append(String.format("%016X  ", baseAddr + off));
            StringBuilder ascii = new StringBuilder(16);
            for (int i = 0; i < 16; i++) {
                if (off + i < data.length) {
                    int b = data[off + i] & 0xFF;
                    sb.append(String.format("%02X ", b));
                    ascii.append((b >= 0x20 && b < 0x7F) ? (char) b : '.');
                } else {
                    sb.append("   ");
                    ascii.append(' ');
                }
                if (i == 7) sb.append(' ');
            }
            sb.append(" |").append(ascii).append("|\n");
        }
        return sb.toString();
    }

    /** 定宽整数列表 */
    private static String dumpInts(byte[] data, long baseAddr, int width,
                                   boolean bigEndian, boolean signed) {
        StringBuilder sb = new StringBuilder();
        int perLine = 16 / width;
        if (perLine < 1) perLine = 1;
        int hexDigits = width * 2;
        for (int i = 0; i + width <= data.length; i += width) {
            if ((i % (perLine * width)) == 0) {
                if (i > 0) sb.append('\n');
                sb.append(String.format("%016X:", baseAddr + i));
            }
            long v = readInt(data, i, width, bigEndian, signed);
            sb.append(' ');
            if (signed) {
                sb.append(String.format("%" + (hexDigits + 2) + "d", v));
            } else {
                sb.append(String.format("0x%0" + hexDigits + "X", v & maskFor(width)));
            }
        }
        sb.append('\n');
        return sb.toString();
    }

    /** 浮点数 (32-bit 单精度 / 64-bit 双精度), 每行显示 2 个, 附原始十六进制 */
    private static String dumpFloats(byte[] data, long baseAddr,
                                     boolean doublePrecision, boolean bigEndian) {
        int width = doublePrecision ? 8 : 4;
        int perLine = 2;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i + width <= data.length; i += width) {
            if ((i % (perLine * width)) == 0) {
                if (i > 0) sb.append('\n');
                sb.append(String.format("%016X:", baseAddr + i));
            }
            long bits = readInt(data, i, width, bigEndian, /*signed*/ false);
            String rawHex;
            String valStr;
            if (doublePrecision) {
                double d = Double.longBitsToDouble(bits);
                rawHex = String.format("0x%016X", bits);
                valStr = formatDouble(d);
            } else {
                float f = Float.intBitsToFloat((int) (bits & 0xFFFFFFFFL));
                rawHex = String.format("0x%08X", (int) (bits & 0xFFFFFFFFL));
                valStr = formatFloat(f);
            }
            sb.append("  ").append(rawHex).append(" = ").append(valStr);
        }
        sb.append('\n');
        return sb.toString();
    }

    private static String formatFloat(float f) {
        if (Float.isNaN(f))      return "NaN";
        if (f == Float.POSITIVE_INFINITY) return "+Inf";
        if (f == Float.NEGATIVE_INFINITY) return "-Inf";
        if (f == 0.0f) return ((Float.floatToRawIntBits(f) & 0x80000000) != 0) ? "-0" : "0";
        float af = Math.abs(f);
        // 绝对值过小或过大改用科学计数法
        if (af != 0 && (af < 1e-4f || af >= 1e10f)) return String.format("%.7e", f);
        return String.format("%.7g", f);
    }

    private static String formatDouble(double d) {
        if (Double.isNaN(d))      return "NaN";
        if (d == Double.POSITIVE_INFINITY) return "+Inf";
        if (d == Double.NEGATIVE_INFINITY) return "-Inf";
        if (d == 0.0) return ((Double.doubleToRawLongBits(d) & 0x8000000000000000L) != 0) ? "-0" : "0";
        double ad = Math.abs(d);
        if (ad != 0 && (ad < 1e-4 || ad >= 1e16)) return String.format("%.15e", d);
        return String.format("%.15g", d);
    }

    /** 自定义字宽 (1..16) */
    private static String dumpCustom(byte[] data, long baseAddr, int width,
                                     boolean bigEndian, boolean signed) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i + width <= data.length; i += width) {
            sb.append(String.format("%016X:", baseAddr + i));
            sb.append(' ');
            // 按字节显示 (自定义字宽可能超过 long), 直接 hex
            StringBuilder hex = new StringBuilder(width * 3);
            for (int j = 0; j < width; j++) {
                int idx = bigEndian ? (i + j) : (i + (width - 1 - j));
                hex.append(String.format("%02X", data[idx] & 0xFF));
            }
            sb.append("0x").append(hex);
            if (width <= 8) {
                long v = readInt(data, i, width, bigEndian, signed);
                sb.append("  (");
                if (signed) sb.append(v); else sb.append(Long.toUnsignedString(v & maskFor(width)));
                sb.append(')');
            }
            sb.append('\n');
        }
        return sb.toString();
    }

    private static long maskFor(int width) {
        if (width >= 8) return -1L;
        return (1L << (width * 8)) - 1L;
    }

    private static long readInt(byte[] data, int off, int width, boolean bigEndian, boolean signed) {
        long v = 0;
        if (bigEndian) {
            for (int j = 0; j < width; j++) v = (v << 8) | (data[off + j] & 0xFFL);
        } else {
            for (int j = width - 1; j >= 0; j--) v = (v << 8) | (data[off + j] & 0xFFL);
        }
        if (signed && width < 8) {
            long signBit = 1L << (width * 8 - 1);
            if ((v & signBit) != 0) v |= ~(maskFor(width));
        }
        return v;
    }

    // ─── 字符串解析 ──────────────────────────────────────────────────

    /** ANSI C 字符串: 遇 0x00 结束; 先按 UTF-8 尝试, 失败回退 GBK; 非法字符转 "." */
    private static String dumpCString(byte[] data) {
        int end = data.length;
        for (int i = 0; i < data.length; i++) {
            if (data[i] == 0) { end = i; break; }
        }
        byte[] slice = new byte[end];
        System.arraycopy(data, 0, slice, 0, end);
        String s = decodeBestEffort(slice, /*wide*/ false);
        return "[ANSI/UTF-8/GBK] len=" + end + "\n" + s;
    }

    /** 宽字符串: UTF-16LE, 遇 0x0000 结束 */
    private static String dumpWideString(byte[] data) {
        int end = data.length & ~1; // 两字节对齐
        for (int i = 0; i + 1 < data.length; i += 2) {
            if (data[i] == 0 && data[i + 1] == 0) { end = i; break; }
        }
        byte[] slice = new byte[end];
        System.arraycopy(data, 0, slice, 0, end);
        String s = new String(slice, StandardCharsets.UTF_16LE);
        return "[UTF-16LE] len(bytes)=" + end + " len(chars)=" + s.length() + "\n" + s;
    }

    /**
     * 自动识别字符串编码: 返回所有候选解码结果, 选评分最高的, 同时显示其它候选
     *
     * 关键改进 (针对 UTF-8 中文被误判成 GBK 的问题):
     *  1. 先用 CharsetDecoder 严格模式 (REPORT) 尝试解码, 不合法序列直接判 0 分
     *  2. 识别原始 bytes 中的 UTF-8 CJK 三字节序列 (E4-E9 80-BF 80-BF) 密度, 高则 UTF-8 加成
     *  3. UTF-8 合法 且 有 CJK 时, 对 GBK 做压制 (避免"碰巧也能解码"的假阳性)
     *  4. 中文范围进一步细分, 真实汉字 (U+4E00..U+9FFF) 比罕用 CJK Ext 加分更高
     */
    private static String dumpStringAuto(byte[] data) {
        // 切到第一个 0 结束 (对 wide 再做一次)
        int nulPos = data.length;
        for (int i = 0; i < data.length; i++) if (data[i] == 0) { nulPos = i; break; }
        byte[] ansi = new byte[nulPos];
        System.arraycopy(data, 0, ansi, 0, nulPos);

        int wideNul = data.length & ~1;
        for (int i = 0; i + 1 < data.length; i += 2)
            if (data[i] == 0 && data[i + 1] == 0) { wideNul = i; break; }
        byte[] wide = new byte[wideNul];
        System.arraycopy(data, 0, wide, 0, wideNul);

        // ── 1. 严格解码, 不合法直接判 0 分 ──
        boolean utf8Ok = isValidEncoding(ansi, StandardCharsets.UTF_8);
        boolean gbkOk  = isValidEncoding(ansi, charsetOrDefault("GBK"));
        boolean u16lOk = isValidEncoding(wide, StandardCharsets.UTF_16LE);
        boolean u16bOk = isValidEncoding(wide, StandardCharsets.UTF_16BE);
        // ASCII: 只要字节 < 0x80 且非控制字符
        boolean asciiOk = true;
        for (byte b : ansi) { int x = b & 0xFF; if (x >= 0x80) { asciiOk = false; break; } }

        // ── 2. 原始 bytes 中的 UTF-8 CJK 特征 ──
        int utf8CjkBytes = countUtf8CjkBytes(ansi);
        double utf8CjkRatio = ansi.length > 0 ? (utf8CjkBytes / (double) ansi.length) : 0.0;

        String asUtf8  = decode(ansi, StandardCharsets.UTF_8);
        String asGbk   = decode(ansi, charsetOrDefault("GBK"));
        String asAscii = decode(ansi, StandardCharsets.US_ASCII);
        String asU16le = decode(wide, StandardCharsets.UTF_16LE);
        String asU16be = decode(wide, StandardCharsets.UTF_16BE);

        double sU8  = utf8Ok ? scoreText(asUtf8) : -10;
        double sGbk = gbkOk  ? scoreText(asGbk)  : -10;
        double sAsc = asciiOk ? scoreText(asAscii) : -10;
        double s16l = u16lOk ? scoreText(asU16le) : -10;
        double s16b = u16bOk ? scoreText(asU16be) : -10;

        // ── 3. UTF-8 CJK 特征加成 + GBK 压制 ──
        if (utf8Ok && utf8CjkRatio > 0.15) {
            sU8  += 3.0 * utf8CjkRatio;
            // UTF-8 合法且明显含中文时, 即便 GBK 也能解码(必然能), 也要压下来
            sGbk -= 2.0 * utf8CjkRatio;
        }

        String winner;
        double best = sU8; String bestName = "UTF-8"; int bestLen = nulPos;
        if (sGbk  > best) { best = sGbk;  bestName = "GBK";       bestLen = nulPos; }
        if (sAsc  > best) { best = sAsc;  bestName = "ASCII";     bestLen = nulPos; }
        if (s16l  > best) { best = s16l;  bestName = "UTF-16LE";  bestLen = wideNul; }
        if (s16b  > best) { best = s16b;  bestName = "UTF-16BE";  bestLen = wideNul; }

        switch (bestName) {
            case "GBK":      winner = asGbk;   break;
            case "ASCII":    winner = asAscii; break;
            case "UTF-16LE": winner = asU16le; break;
            case "UTF-16BE": winner = asU16be; break;
            default:         winner = asUtf8;  break;
        }

        StringBuilder sb = new StringBuilder();
        sb.append("[自动识别]  最佳: ").append(bestName)
          .append("  评分=").append(String.format("%.2f", best))
          .append("  长度=").append(bestLen);
        if (utf8CjkRatio > 0) {
            sb.append(String.format("  UTF-8 中文占比=%.1f%%", utf8CjkRatio * 100));
        }
        sb.append('\n');
        sb.append("─────────────────────────────────\n");
        sb.append(winner).append("\n");
        sb.append("─────────────────────────────────\n");
        sb.append("其它候选:\n");
        sb.append(String.format("  UTF-8    %s (%.2f): %s\n", utf8Ok  ? "✓" : "✗", sU8,  preview(asUtf8)));
        sb.append(String.format("  GBK      %s (%.2f): %s\n", gbkOk   ? "✓" : "✗", sGbk, preview(asGbk)));
        sb.append(String.format("  ASCII    %s (%.2f): %s\n", asciiOk ? "✓" : "✗", sAsc, preview(asAscii)));
        sb.append(String.format("  UTF-16LE %s (%.2f): %s\n", u16lOk  ? "✓" : "✗", s16l, preview(asU16le)));
        sb.append(String.format("  UTF-16BE %s (%.2f): %s\n", u16bOk  ? "✓" : "✗", s16b, preview(asU16be)));
        return sb.toString();
    }

    /** 用 CharsetDecoder 严格模式判断 bytes 是否是该编码的合法序列 */
    private static boolean isValidEncoding(byte[] data, Charset cs) {
        if (data.length == 0) return false;
        try {
            java.nio.charset.CharsetDecoder dec = cs.newDecoder()
                    .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
                    .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT);
            dec.decode(java.nio.ByteBuffer.wrap(data));
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /**
     * 统计原始字节中 UTF-8 CJK 三字节序列占用的字节数.
     * CJK 统一汉字 U+4E00..U+9FFF 在 UTF-8 中是:
     *   lead  = 0xE4..0xE9
     *   trail = 0x80..0xBF (两个)
     * 只计合法的 3 字节 CJK 序列.
     */
    private static int countUtf8CjkBytes(byte[] data) {
        int n = 0;
        int i = 0;
        while (i + 2 < data.length) {
            int b0 = data[i] & 0xFF;
            int b1 = data[i + 1] & 0xFF;
            int b2 = data[i + 2] & 0xFF;
            if (b0 >= 0xE4 && b0 <= 0xE9
                    && b1 >= 0x80 && b1 <= 0xBF
                    && b2 >= 0x80 && b2 <= 0xBF) {
                n += 3;
                i += 3;
            } else {
                i++;
            }
        }
        return n;
    }

    private static String preview(String s) {
        if (s == null) return "";
        String t = s.replace('\n', ' ').replace('\r', ' ').replace('\t', ' ');
        if (t.length() > 120) t = t.substring(0, 120) + "…";
        return t;
    }

    private static String decode(byte[] data, Charset cs) {
        try { return new String(data, cs); } catch (Exception e) { return ""; }
    }

    private static Charset charsetOrDefault(String name) {
        try { return Charset.forName(name); } catch (Exception e) { return StandardCharsets.ISO_8859_1; }
    }

    private static String decodeBestEffort(byte[] data, boolean wide) {
        if (wide) return new String(data, StandardCharsets.UTF_16LE);
        String u = decode(data, StandardCharsets.UTF_8);
        if (scoreText(u) > scoreText(decode(data, charsetOrDefault("GBK")))) return u;
        return decode(data, charsetOrDefault("GBK"));
    }

    /**
     * 文本可读性打分:
     *  - 可打印 ASCII +1
     *  - CJK 范围 (U+4E00..U+9FFF) +2 (中文)
     *  - 其它常用区 (U+0020..U+007E 已计) / 中日韩符号 +1
     *  - 控制字符 (除 \t\n\r) 和 unpaired surrogate -3
     *  - 替换符 U+FFFD -2
     *  - 空串 -> 0
     */
    private static double scoreText(String s) {
        if (s == null || s.isEmpty()) return 0;
        double score = 0;
        int counted = 0;
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            counted++;
            if (c == '\t' || c == '\n' || c == '\r') { score += 0.5; continue; }
            if (c < 0x20) { score -= 3; continue; }
            if (c == 0xFFFD) { score -= 2; continue; }
            if (c >= 0x20 && c < 0x7F) { score += 1; continue; }
            if (c >= 0x4E00 && c <= 0x9FFF) { score += 2; continue; }   // CJK 统一汉字
            if (c >= 0x3000 && c <= 0x303F) { score += 1; continue; }   // CJK 符号
            if (c >= 0x3040 && c <= 0x30FF) { score += 1; continue; }   // 日文假名
            if (c >= 0xAC00 && c <= 0xD7AF) { score += 1; continue; }   // 韩文
            if (c >= 0xFF00 && c <= 0xFFEF) { score += 1; continue; }   // 全角
            if (Character.isSurrogate(c))    { score -= 1; continue; }
            // 其余视为未知字节, 不加分
        }
        // 归一化到"每字符平均得分" * log(长度) 以避免短串过高
        double avg = score / counted;
        return avg * (1.0 + Math.log10(Math.max(1, counted)));
    }
}
