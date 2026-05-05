# libtersafe.so 检测点 / 特征 / 技术 报告

- **目标模块**: `libtersafe.so` (TP2 / TenProtect 2 / TSS)
- **应用包名**: `com.tencent.tmgp.dfm` (三角洲行动)
- **版本**: 7.7.49.57576
- **架构**: arm64
- **上报域名**: `down.anticheatexpert.com`,CDN: `dl.putdl.com` / `dl.timedl.com`
- **数据来源**: IDA 反编译 + Frida 运行时 dump (`tersafe_log.txt` / `plain_strings.txt`)

---

## 0. 整体架构

```
┌───────────────── Java 层 ─────────────────┐
│ com.tencent.tp.TssSdk                      │
│   native: setuserinfo / setgamestatus /    │
│           senddatatosvr / hasMatchRate ... │
│ MainThreadDispatcher2  (JNI 命令分发)      │
└──────────────────┬─────────────────────────┘
                   │ JNI
┌──────────────────▼─────────────────────────┐
│ libtersafe.so  (Native 反作弊 SDK)         │
│   ┌──────────────────────────────────┐     │
│   │ 字符串加密表 + sub_4EAC3C 解密器 │     │
│   ├──────────────────────────────────┤     │
│   │ 环境检测     调试器/Root/模拟器  │     │
│   │ 自身保护     inline-hook 自检    │     │
│   │ MVM 扫描引擎 云控特征码扫描内存  │     │
│   │ 包级反外挂   ischeatpacket       │     │
│   │ 上报通道     原生 socket 自定义协议  │
│   └──────────────────────────────────┘     │
└──────────────────┬─────────────────────────┘
                   │ raw socket
                   ▼ down.anticheatexpert.com
```

---

## 1. 字符串加密 / 解密机制

| 项 | 详情 |
|---|---|
| 密文表 | `qword_FBBB0` 指向(运行时填充) |
| 明文缓存 | `unk_568FC4`(SO 内 RW 段) |
| 解密函数 | `sub_4EAC3C(int idx) -> char*` |
| 算法 | 按字节滚动 XOR:`key = (key + i ^ 0x40) + 6`,末字节校验和 |
| 触发 | `.init_array` 构造期预解密一批,运行时按需 lazy decrypt |
| 防御目的 | 让所有敏感关键字(frida/magisk/路径/类名)在静态分析中不可见 |
| 绕过方法 | Frida 启动后扫 `unk_568FC4` 整段 → 一次拿到全部明文 |

实战:Frida 单次 dump 抓到 357 条明文(见 `plain_strings.txt`)。

---

## 2. 环境检测

### 2.1 调试器 / Tracer
| 检测点 | 特征字符串 / API | 技术 |
|---|---|---|
| TracerPid | `/proc/%u/status` + `TracerPid` | 读取自身 status,匹配非 0 的 TracerPid |
| 进程名扫描 | `/proc/%u/cmdline` | 遍历 `/proc/*/cmdline` 比对调试器名 |
| 线程枚举 | `/proc/%u/task`、`/proc/%u/task/%u` | 检测异常线程数 / Frida 工作线程 |
| 自反附加 | `ptrace(PTRACE_TRACEME, ...)` | 自附加抢占,后续附加调用失败 |
| 调试器名上报 | `debugger=%s` | 上报字段 |

### 2.2 ADB / USB 调试
| 检测点 | 特征 | 技术 |
|---|---|---|
| ADB 开关 | `Settings$Secure.ADB_ENABLED` (Java) | 反射 + JNI 调 ContentResolver |
| USB 状态 | `android.hardware.usb.action.USB_STATE` 广播 | 注册 BroadcastReceiver,读 `connected` extra |
| 上报字段 | `ADBOverUsb` | |

### 2.3 Root 检测
| 类别 | 黑名单 | 技术 |
|---|---|---|
| Su 包 | com.noshufou.android.su(.elite)、eu.chainfire.supersu、com.koushikdutta.superuser、com.thirdparty.superuser、com.yellowes.su | `access()` / `stat()` `/data/data/<pkg>` |
| 一键 Root | com.kingroot.kinguser、com.kingo.root、com.smedialink.oneclickroot、com.zhiqupk.root.global、com.alephzain.framaroot、com.zachspong.temprootremovejb、com.ramdroid.appquarantine | 同上 |
| Magisk | `/data/data/com.topjohnwu.magisk` | 路径检测 |
| KernelSU | `/data/data/me.weishu.kernelsu` | 路径检测 |
| Su 二进制 | `/system/app/Superuser.apk`、`/system/etc/init.d/99SuperSUDaemon`、`/dev/com.koushikdutta.superuser.daemon`、`/system/xbin/daemonsu` | `access()` |
| Su 路径前缀 | `/data/local`、`/data/local/bin`、`/data/local/xbin`、`/su/bin`、`/system/bin`、`/system/bin/.ext`、`/system/bin/failsafe`、`/system/sd/xbin`、`/system/usr/we-need-root`、`/system/xbin` | 拼 `su` 后 `access()` |
| 上报字段 | `gp4_no_root`、`mt2_no_root`、`mt2_ko`、`is_root=%d` | |

### 2.4 模拟器检测
| 特征 | 技术 |
|---|---|
| `emulator_name` / `NotEmulator` | 综合系统属性比对(`__system_property_get` 读 `ro.product.*`、`ro.kernel.qemu`) |
| `machine_name`、`sys_ver` | `uname()` + `/proc/cpuinfo` |
| `cert_md5`、`cert_author` | APK 签名校验,防重打包 |

### 2.5 传感器异常
| 特征 | 技术 |
|---|---|
| `SensorChangeTooMuch` | 检测加速度/陀螺仪数据帧间跳变,识别模拟器伪造数据 |

---

## 3. 自身代码完整性 / 反 Hook

| 检测点 | 特征 | 技术 |
|---|---|---|
| Inline hook 自检 | `set_inline_hook_error` (reason 0x0A) | SDK 自身在关键函数预先布桩,启动时验证桩是否被覆盖 |
| 外部 hook 检测 | `inline_hook_opcode_dismatch` / `ms_hook_opcode` (reason 0x0B) | 比对函数前导字节与原始 opcode,检测 Substrate / Frida-gum / Xposed inline hook |
| SO 模块名校验 | `_engine_module_name`、`sst_engine_module_name` | `dl_iterate_phdr` 枚举模块,自检名字未改 |
| SO 大小越界 | `module_size_in_range` | 校验 `.text` 段大小,防 patching 后膨胀 |
| Maps 扫描 | `/proc/self/maps` | 解析 `%zx-%zx %c%c%c%c %x %x:%x %u %s` 格式,枚举所有已加载映射,匹配可疑 SO |
| libc 自校验 | `system/lib/libc.so` + `libc_malloc` (`memtrap_name`) | 对 libc 关键函数 (`malloc`) 设内存陷阱,检测 GOT/PLT/inline hook |
| SO 热更 | `libtersafeupdate2.so`、`tersafe.update`、` __ENABLE_SO_UPDATE__` | 服务器可热更新自身 |
| API 名预解密 | `ptrace`/`syscall`/`readdir`/`opendir`/`access`/`statfs`/`select`/`connect`/`gettimeofday`/`clock_gettime`/`fdopen`/`getpid` | 运行时 `dlsym` 拿地址后自检前缀字节,防止用户对这些 libc 导出函数 hook 后绕过反作弊 |

---

## 4. MVM 云控扫描引擎(核心)

腾讯自研的"移动 VM",在反作弊进程内运行扫描脚本(.img 格式),由服务端按需下发。

### 4.1 引擎组件
| 文件 | 用途 |
|---|---|
| `vm_main.img` | 主调度脚本 |
| `vm_x_task.img` | 子任务执行脚本 |
| `vm_debug.img` | 调试用脚本 |
| `vm_rom.zip` | VM 字节码资源包 |
| `hot_fix.img` | 热修复脚本 |

### 4.2 规则集(`*_objvm` 命名)
| 规则名 | 推测用途 |
|---|---|
| `various_opcode` / `various_opcode_objvm` | 多形态 opcode 比对(检测变形 hook) |
| `attv_objvm` | attestation 完整性扫描 |
| `attest_scan_objvm` | 同上 |
| `struggle_task_objvm` | 抗对抗任务(检测调试器/dump) |
| `config2_ctime_objvm` | 配置文件 ctime 校验,防回滚 |
| `commdat_value_objvm` | comm.dat 值校验 |
| `r2slot_extension_objvm` | 反 root2 槽位扩展 |
| `win_fea_sync` | 窗口特征同步 |
| `mem_trap2` | 内存陷阱(MIE2) |
| `attest_scan` | 远程证明扫描 |

### 4.3 扫描类型(从错误码反推)
| reason | 名称 | 含义 |
|---|---|---|
| 0x14 | `mrpcs_single_data_not_match` | 单点特征不匹配(精准命中) |
| 0x15 | `mrpcs_common_data_not_match` | 通用特征不匹配 |
| 0x45 | `rule_exe_fail` | 规则执行失败 |
| 0x4D | `rule_op_is_change` | 规则操作码被篡改 |
| `wild scan` | | 野扫描(全堆扫) |
| `rect header check` | | 区段头校验 |
| `incorrect data check` | | 数据完整性校验 |

### 4.4 引擎线程
- `scan_loop2`(扫描线程)
- `update_loop`(规则下载线程)
- `mrpcs_scan_thread` / `mrpcs_send_data_thread` / `mrpcs_download_data_thread`
- `XTask_%s_%s`(子任务命名格式)

### 4.5 数据文件 / 缓存
- 配置: `comm.dat`、`comm.zip`、`tcjcfg.dat`、`config2.dat`、`config3.dat`
- 缓存: `cache_crc.dat`、`cache_md5.dat`、`cache_cus_fea`
- ANO 子系统: `tss_ano.dat`、`ano.ano3.dat`、`ano.rwc3.tmp`、`rcu.o.dat`、`ano_app_915c.dat`
- 标识: `g3s.dat`、`a64.dat`、`a64.sig`
- 引擎子包(15 个): `ob_builtin.zip`、`ob_x.zip`/`ob_x_64.zip`/`ob_x_ace.zip`/`ob_x_ace_64.zip`、`ob_cdn1(_64).zip`、`ob_cdn2(_64).zip`、`ob_cs1(_64).zip`、`ob_cs2(_64).zip`、`ob_gs1.zip`、`ob_gs2(_64).zip`、`ob_custom.zip`、`ob_ace_gs`

---

## 5. 包级反外挂

| 检测点 | 特征 | 技术 |
|---|---|---|
| 作弊封包 | `tss_sdk_ischeatpacket` | 通过游戏 `senddatatosdk` 上送的协议包,检查特征位/魔数,识别协议层外挂 |
| 服务器签名校验 | `tss_recv_sec_signature` / `TssSDKOnRecvSignature` | 服务端下发签名,客户端校验防中间人/重放 |
| 命中率 | `hasMatchRate` (Java native 名) / 上报字段 `name=%s\|rate=%.2f\|is_root=%d` | 单次扫描多次命中聚合后上报作弊器名+置信度 |

---

## 6. JNI / IPC 通道

### 6.1 注册的 native 方法 (`com.tencent.tp.TssSdk`)
| 方法 | 签名 | 用途 |
|---|---|---|
| `initialize` | | SDK 初始化 |
| `setuserinfo` | `(Lcom/tencent/tp/TssSdkUserInfo;)V` | 设置用户信息 |
| `setuserinfoex` | `(Lcom/tencent/tp/TssSdkUserInfoEx;)V` | 扩展用户信息 |
| `setgamestatus` | `(Lcom/tencent/tp/TssSdkGameStatusInfo;)V` | 切换游戏状态(登录/匹配/对局) |
| `getsdkantidata` | `([BI)V` | 取反作弊数据 |
| `setsenddatatosvrcb` | `(Lcom/tencent/tp/TssIOCtlResult;)I` | 注册"发数据到服务器"回调 |
| `senddatatosdk` | `([BI)V` | 游戏 → SDK 数据 |
| `senddatatosvr` | `(Ljava/lang/Object;)V` | SDK → 服务器数据 |
| `onruntimeinfo` | | 运行时信息回调 |
| `hasMatchRate` | | 命中率查询 |

### 6.2 ioctl 命令派发(`tss_sdk_ioctl(cmd, p1, p2, p3)`)
| cmd | 实测用途 |
|---|---|
| `0x10` | 推送扫描脚本到 MVM |
| `0x1c` | 加载 8 字节配置(返回扫描句柄) |
| `0x23` | 读结果(配 0x24 使用) |
| `0x24` | 查询心跳/状态 |
| `0x35` | 大数据(0x140 字节)上报触发 |
| `0x51` | 加密上报包(0xE0 字节) |
| `0x58` | 高频心跳(每秒数次) |

---

## 7. 上报通道

| 项 | 详情 |
|---|---|
| 主上报域 | `down.anticheatexpert.com` |
| 更新 CDN | `dl.putdl.com`、`dl.timedl.com` |
| URL 模板 | `%s://%s/iedsafe/Client/%s`、`%s/%d/%08X/%s` |
| 协议 | 自定义二进制,包头 `01 00 00 00 <len> ... <uid> <token> ...`,**绕过应用层 HTTP** 直接 `sendto` |
| 上报字段格式 | `seq=%u\|pid=%d\|time=%ld`、`info=%s`、`opt_%s` |
| 状态向量 | `,state:%08x,r:%d/%d/%d/%d/%d/%d/%d,p:%d/%d,%d` —— 7 维风险评分 + 进程状态 |
| 上报触发函数 | libtersafe + 0x4B675C(实测 sendto 调用源) |
| 设备指纹格式 | `H%08X%08X%02X%02X%02X%02X%01X%01X%01X%01X%04X%04X%04X%04X%04X%04X%04X%08X` |

---

## 8. 完整 reason code 检测项表(从 `sub_3FD4A0` 提取)

| reason | tag | 描述 | 严重度 |
|---|---|---|---|
| 0x00 | `ms_data_crc` | 下发数据 CRC 错误 | 中 |
| 0x01 | `ms_data_len` | 数据长度异常 | 中 |
| 0x02 | `ms_data_mod_len` | 模块名长度异常 | 中 |
| 0x03 | `ms_data_mod_info` | 模块信息异常(附 mod nm) | 中 |
| 0x06 | `ms_open_file` | 文件打开失败(附 nm/err) | 低 |
| 0x08 | `ms_mode_path` | 模块路径获取失败(附 nm) | 低 |
| 0x0A | `ms_inline_hook_err` | **设置自检 hook 失败** | 高 |
| 0x0B | `ms_hook_opcode` | **检测到外部 inline hook**(附 pointID) | 极高 |
| 0x0E | `ms_download` | 规则下载失败(附 crc) | 中 |
| 0x0F | `ms_fc_start` | fc 线程启动失败 | 中 |
| 0x10 | `ms_scan_start` | 扫描线程启动失败 | 高 |
| 0x11 | `ms_send_start` | 上报线程启动失败 | 高 |
| 0x12 | `ms_dl_start` | 下载线程启动失败 | 高 |
| 0x14 | `ms_single_match` | **单点特征命中(精准外挂指纹)** | 极高 |
| 0x15 | `ms_common_match` | **通用特征命中** | 极高 |
| 0x19 | `ms_send_one_data_size` | 发送单包超长(附 o_sz/bf_sz/pID) | 中 |
| 0x2C | `ms_mmap` | mmap 失败(附 error) | 中 |
| 0x38 | `ms_push_game` | 推送游戏失败 | 中 |
| 0x39 | `ms_game_remain` | 游戏数据剩余 | 中 |
| 0x3A | `ms_game_data_toolong` | 游戏数据过长 | 中 |
| 0x3B | `ms_not_find_mode_base` | 未找到模块基址(附 mod_nm) | 中 |
| 0x45 | `ms_rule_exe_fail` | **MVM 规则执行失败**(附 pID/rID) | 高 |
| 0x4D | `ms_rule_op_is_change` | **MVM 规则操作码被篡改**(附 pID/rID) | 极高 |

---

## 9. 关键函数地址(基址 `libtersafe.so` + 偏移)

| 偏移 | 名称 | 说明 |
|---|---|---|
| `0x1B7000` 起 | `tss_*` 系列导出 | 旧式 C API |
| `0x1CC000` 起 | `TssSDK*` 导出 | 新式 C++ API |
| `0x1D62C4` | `JNI_OnLoad` | JNI 入口,注册 native 方法 |
| `0x4EAC3C` | `sub_4EAC3C` | **字符串解密器** |
| `0x568FC4` | `unk_568FC4` | **明文字符串缓存(dump 全部明文从这里)** |
| `0x3FD054` | `sub_3FD054(buf, key, val)` | 上报字段 builder |
| `0x3FD4A0` | `sub_3FD4A0(a1, a2)` | **检测命中 reason 分发器** |
| `0x4B675C` | (调用源) | sendto 上报触发点 |
| `0x1C000C` | `tp2_sdk_ioctl` | TP2 ioctl 派发 |
| `0x1BCE54` | `tss_sdk_ioctl` | TSS ioctl 派发 |
| `0x4CC4F0` | `sub_4CC4F0` | 作弊命中记录构造(`name=%s|rate=...|is_root=%d`) |

---

## 10. 防御 / 绕过 要点

| 反作弊点 | 绕过思路 |
|---|---|
| 字符串加密 | 已破:dump `unk_568FC4`,见本报告 |
| Frida 检测 | 改 frida-server 文件名/端口 + zygisk-rezygisk;实测当前隐藏方案有效 |
| TracerPid | 内核模块改 `/proc/*/status` 输出 |
| Magisk 路径检测 | Magisk DenyList / Shamiko |
| inline hook 自检 (0x0B) | 用 stalker / 内核态 hook 替代 inline,或定位 `sub_3FD4A0` 直接 patch case 0x0B |
| MVM 规则扫描 (0x14/0x15) | 难——规则云控,需逆 vm_*.img bytecode;最稳妥是扫描期暂停作弊器内存写入 |
| 上报通道 | 拦截 sendto @ libtersafe+0x4B675C 直接 return,但 SDK 有心跳互锁,长时间无报告会被服务端封 |
| 进程被踢 | 服务器侧 `cmd=0x35/0x51` 大包是触发踢出的关键路径,可观察其内容定位被识破的特征 |

---

_报告生成时间: 2026-05-05_
_数据来源: IDA Pro (libtersafe.so.i64) + Frida 17.5.2 运行时 dump_
