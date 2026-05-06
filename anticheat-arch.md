# 三角洲行动手游 (`com.tencent.tmgp.dfm`) 反作弊分析

> 分析对象：`libtersafe.so`（TerSafe / TSS / TP2 SDK） + `libtprt.so`（TPRT 底层运行时）
> 工具：IDA Pro 9.3 + ida-pro-mcp（双实例分别加载两个 dump）
> 设备：Pixel 8（KernelPatch root），ADB `37171FDJH001TH`，frida-server 17.5.2-android-arm64
> 目标 PID：DFM 13333、PUBGMHD 13985（同一 TerSafe/TPRT 栈）

---

## 1. 双层架构

| 二进制          | 角色                                        | 体量                    | 暴露符号                                                                             |
| --------------- | ------------------------------------------- | ----------------------- | ------------------------------------------------------------------------------------ |
| `libtersafe.so` | 上层 SDK（**TerSafe / TSS / TP2**）         | ~1900 函数，1904 字符串 | `Tss*`、`g_AllTssExportFunc`、`tss_jni_cmd`、`TssJavaMethod_SendCmd` 等 70+ 命名导出 |
| `libtprt.so`    | 底层运行时（**TPRT — TenProtect Runtime**） | ~590 函数，435 字符串   | 几乎只有 `JNI_OnLoad` 命名，注册 7 个 native 方法                                    |

**两层协作接口**

- TPRT 给 TerSafe 暴露 `g_tprt_pfn_array`（替换函数指针）和 `g_tprt_ori_array`（保留的原始指针）。TerSafe 通过它们既能"装上 hook"又能"调原始函数比对"。
- Java 层通过 `TssJavaMethod_SendCmd` → `tss_jni_cmd` 与 native 双向通信。
- TPRT `JNI_OnLoad` 注册 7 个加密名称的 native 方法（类名通过 `sub_10B060(idx)` 解密得到）。

---

## 2. 加固 / 混淆

- **OLLVM 控制流平坦化**：`v18 / v19 / v20 / v22` 状态机派发（`tss_sdk_init`、`JNI_OnLoad`、`sub_BD1F8`、`sub_C891C` 等典型）。
- **不透明谓词**：到处出现 `(v6|~v5)+(v5&v6)+(v5&~v6)+1 != (v5&v6)` 这种恒真/恒假表达式做无意义跳转。
- **字符串全部加密**：从 1900 个可读字符串中只有几条明文（`/dev/pts/%zu`、`/proc/self/maps`、`/sdcard/Android/data` 等）。其余通过 `sub_10B060(idx)` / `sub_4ED274(idx)` 索引解密表获取，索引范围约 10000–12000。
- **运行时 Shellcode Trampoline**：`libtprt!sub_11A97C` 把 56 字节 `byte_16A381` 用 `XOR 0x1D` 解出 → `mmap` RWX → checksum (`sub_116934 == 0xC4F1A1B8`) 校验失败立刻 `kill(getpid(), 9)` → `mprotect` 改成 RX 返回。绕过静态反编译。
- **加密模块加载器**：`libtprt!sub_12BE14` 替代 `mmap`，识别自家魔数 `0x12706594` → 解密 → 重定位。配套缓存路径 `/data/user/0/com.tencent.tmgp.dfm/files/ano_tmp/74105212.xx.dat`，是远端下发的检测规则。

---

## 3. 检测项清单（按调用证据归类）

### 3.1 反 Hook / 反 Frida

| 函数 / 地址                        | 行为                                                                                                                                                                                                                                                                                                                                               |
| ---------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `libtersafe!sub_25B144`            | 一次性构造 **32 项 libc 直调指针表**：`open / fopen / access / stat / fstat / opendir / readdir / readlink / connect / sendto / recvfrom / ptrace / kill / fork / popen / execl / dlopen / dlsym / inotify_init / inotify_add_watch / mincore / mprotect / __system_property_get / dl_iterate_phdr` 等。绕过用户层 hook + 同时校验函数指针是否被改 |
| `libtersafe!sub_37041C`            | `open("/proc/self/maps") + memmem` 黑名单串扫描（典型 frida/gum/dobby/xposed needle）                                                                                                                                                                                                                                                              |
| `libtersafe!sub_3681BC`            | 动态构造 `{dlopen, dlsym, handle}` 三元组隐藏依赖                                                                                                                                                                                                                                                                                                  |
| `libtprt!sub_121C3C`、`sub_12C728` | 解密 lib 名 → `dlopen` → 解密符号名 → `dlsym`，并能把单个目标符号重定向到 TPRT 内部实现（**符号欺骗**）                                                                                                                                                                                                                                            |
| `dl_iterate_phdr` + `dladdr`       | 模块枚举 + 调用回返址校验                                                                                                                                                                                                                                                                                                                          |

> **实测**：Frida 17.5.2 attach DFM (PID 13333) 与 PUBGMHD 都被阻断（spawn-time 反 attach），需走 early-spawn 或 KPM 替代路径。

### 3.2 反调试

| 函数 / 地址             | 行为                                                                                                    |
| ----------------------- | ------------------------------------------------------------------------------------------------------- |
| `libtprt!sub_138A3C`    | 8 参数 `ptrace` 派发桥（`PTRACE_TRACEME` 占位、自附加阻断他人）                                         |
| `libtprt!sub_140428`    | `prctl(PR_SET_DUMPABLE, 1)` 探测 + 触发上报 (`sub_130F10`)                                              |
| `libtersafe!sub_323780` | 枚举 `/dev/pts/%zu`，判 jdb / scrcpy / Termux                                                           |
| 通用                    | `/proc/self/status` 读 `TracerPid`；`sigaction` 安装 `SIGTRAP/SIGSEGV/SIGILL`，配合 `BRK#0` trap 反调试 |

### 3.3 反 Root

- `popen / execl` 子进程跑 `su / id / mount / getenforce`。
- `access / stat` 黑名单：`/system/xbin/su`、`/sbin/su`、`/system/app/Magisk*`。
- `__system_property_get / find_nth / read` 读取：
  - `ro.boot.verifiedbootstate`、`ro.boot.vbmeta.device_state`、`ro.boot.flash.locked`、`ro.boot.veritymode`
  - `ro.debuggable`、`ro.secure`、`ro.build.tags`
- `libtprt!sub_130C20`：扫一个目录 + 多次 `fork() + execv()` 调外部二进制，按 Android API 等级（≥24 走分支）传不同 argv，含 `"arm64"` — **应是调 `/system/bin/linker64 --list <so>` 查 TPRT/TerSafe 自身依赖中是否被注入了陌生模块**。

### 3.4 内存 / 完整性

- `mincore`：页是否驻留（反 Stalker / 反全量内存读）。
- `mprotect` 改 `.text` 属性 — hook 安装 + 自修复。
- `pread` 在固定偏移读 fd — 二进制段完整性自检。
- `libtprt!sub_12BE14`：自定义模块加载（pread+ELF 0x464C457F 校验 → 1KB 分块复制 → `sub_12F87C` 解密 → `sub_12BA40` 校验 → `sub_12BB88` 重定位）。
- `libtprt!sub_BD1F8`：`settimeofday` 包了一层算术身份校验，函数被 hook 即破坏恒等式 → 走异常分支。

### 3.5 环境 / 模拟器

- `uname / getauxval / statfs / sysconf` 收集硬件特征。
- 系统属性扫 `qemu / goldfish / ranchu / x86 / vbox`。

### 3.6 进程 / 双开 / 分身

- `getuid / getppid / getpid` UID & 父进程校验。
- `opendir("/proc") + readdir + readlink("/proc/<pid>/exe")` 枚举同包名进程（`com.tencent.tmgp.dfm:xxx` 也会一起列）。

### 3.7 文件监控

- `inotify_init / inotify_add_watch` 监控 SDK 关键文件（含 `ano_tmp/*.dat`）被替换。

### 3.8 网络 & 检测上报

- `socket / connect / sendto / recvfrom` 实时上报。
- `tss_get_report_data{,2,3,4}` + `tss_del_report_data*` 是检测结果环形队列。
- `tss_sdk_ischeatpacket` 是网络包层的"包内容审计"。
- `tss_sdk_encryptpacket / decryptpacket` 嵌入应用层数据通路加解密。

---

## 4. Frida 注入受阻 — 替代路径

| 方案                        | 说明                                                                                                                                                                                                                                 |
| --------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **early-spawn**             | `frida -U -f com.tencent.tmgp.dfm --no-pause -l probe.js`，抢在 TerSafe 反 Frida 完成前注入；本机已建好 [\_anti_cheat_probe/probe.js](_anti_cheat_probe/probe.js)、[\_anti_cheat_probe/run_probe.py](_anti_cheat_probe/run_probe.py) |
| **KPM + libpre.so**（推荐） | 工作区已有 [FrideHide-kpm/](../FrideHide-kpm/) 内核态模块 + `libpre.so` 注入链；`stealth_hooks.cpp` 已 hook `dl_iterate_phdr / dladdr / __system_property_*` — 在该 SO 内追加对 TerSafe/TPRT 入口的探针即可，不走 frida 通道         |
| **改名 frida-server**       | hluda-server / corellium 风格；改 thread name `gum-js-loop` 与 `re.frida.server` socket 路径                                                                                                                                         |

---

## 5. 待解 / 后续

- 字符串解密表 `sub_10B060 / sub_4ED274` 的算法尚未还原 — 写完 KPM 探针后建议 dump 第一次解密返回结果（trap 在解密后的栈帧）。
- `ano_tmp/74105212.xx.dat` 文件未导出 — 它是 TPRT 远程下发的实时规则集；建议直接 `adb pull` + 分析其 0x12706594 头格式。

---

## 6. 关键地址速查

### libtersafe.so

| 地址       | 名称 / 作用                                                                                                                                                                  |
| ---------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `0x25B144` | `sub_25B144` libc 直调指针表注册                                                                                                                                             |
| `0x37041C` | `sub_37041C` `/proc/self/maps` memmem 扫描                                                                                                                                   |
| `0x3681BC` | `sub_3681BC` dlopen/dlsym 三元组隐藏依赖                                                                                                                                     |
| `0x323780` | `sub_323780` `/dev/pts/%zu` 枚举                                                                                                                                             |
| `0x1BF3A0` | `tss_sdk_init` 入口（OLLVM 平坦化）                                                                                                                                          |
| `0x25D1C`  | `JNI_OnLoad` 等                                                                                                                                                              |
| 导出符号   | `g_AllTssExportFunc` / `tss_jni_cmd` / `TssJavaMethod_SendCmd` / `tss_sdk_encryptpacket` / `tss_sdk_decryptpacket` / `tss_sdk_ischeatpacket` / `tss_get_report_data{,2,3,4}` |

### libtprt.so

| 地址                    | 名称 / 作用                                                   |
| ----------------------- | ------------------------------------------------------------- |
| `0x11A97C`              | `sub_11A97C` XOR-0x1D RWX shellcode trampoline（fail → kill） |
| `0x11AA74` / `0x11AE50` | mprotect 兜底 / 自修复                                        |
| `0x12BE14`              | `sub_12BE14` 自定义 mmap（魔数 `0x12706594`）                 |
| `0x12F87C`              | `sub_12F87C` 加密模块解密                                     |
| `0x12BA40` / `0x12BB88` | 加密模块校验 / 重定位                                         |
| `0x121C3C`              | `sub_121C3C` 加密 dlopen+dlsym 包装                           |
| `0x12C728`              | `sub_12C728` dlsym 拦截 + 符号欺骗                            |
| `0x138A3C`              | `sub_138A3C` ptrace 8 参派发桥                                |
| `0x140428`              | `sub_140428` prctl(PR_SET_DUMPABLE)                           |
| `0x130C20`              | `sub_130C20` fork+execv 链                                    |
| `0xBD1F8`               | `sub_BD1F8` settimeofday + 算术身份反 hook                    |
| `0xC891C`               | `sub_C891C` 字符串解密辅助                                    |
| `0x10B060` / `0x4ED274` | 字符串解密表入口                                              |
| 全局表                  | `g_tprt_pfn_array` / `g_tprt_ori_array`                       |
