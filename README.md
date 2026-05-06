# Dobby Project

一个基于 Android NDK + 自定义 Windows-LLVM 前端（带 OLLVM/Polaris 混淆）的多游戏运行时分析 / Overlay 框架。
项目同时支持 Unity（IL2CPP）与 Unreal Engine（UE4 / UE5）目标，集成了内存读取、SO Dump、注入器、KernelPatch 控制、反调试 / 反 Frida、AI 屏检（NCNN + NanoDet）等模块。

> 仅供学习与逆向研究使用。

---

## 目录

- [项目目录结构](#项目目录结构)
- [功能模块说明](#功能模块说明)
- [技术栈与开源项目](#技术栈与开源项目)
- [开发环境 / 编译要求](#开发环境--编译要求)
- [克隆与编译步骤](#克隆与编译步骤)
- [Windows-LLVM 前端 + Android NDK](#windows-llvm-前端--android-ndk)
- [常用命令](#常用命令)

---

## 项目目录结构

```
game/
├── app/
│   ├── build.gradle.kts                # AGP 模块构建脚本（NDK / CMake / Windows-LLVM 接线）
│   ├── proguard-rules.pro
│   └── src/main/
│       ├── AndroidManifest.xml
│       ├── assets/                     # 字体、模型等资源
│       ├── res/                        # 布局、图标
│       ├── java/com/example/dobbyproject/   # Java 层
│       │   ├── MainActivity.java
│       │   ├── GameLauncher.java
│       │   ├── InjectHideActivity.java     # KPM 注入隐藏控制
│       │   ├── MemoryReaderActivity.java   # 跨进程内存读取 UI
│       │   ├── SoDumperActivity.java       # 运行时 SO Dump UI
│       │   ├── PublicOverlayBridge.java    # Overlay 桥
│       │   ├── Ue4OverlayService.java      # UE4 Overlay 后台服务
│       │   ├── AIScreenDetect.java         # NCNN 屏检入口
│       │   ├── Arm64Disassembler.java
│       │   └── KpKeyStore.java / SuShell.java / Deploy.java
│       └── cpp/                        # 全部 Native 源码（见下）
│           ├── CMakeLists.txt
│           ├── core/                   # 跨游戏基础设施
│           │   ├── log/                # 日志
│           │   ├── file/               # 文件 IO
│           │   ├── overlay/            # PublicOverlayRenderer（ImGui 渲染层）
│           │   ├── stealth/            # stealth_hooks（隐藏自身）
│           │   └── anti_debug/         # 反调试 / 反 Frida 自检线程
│           ├── memory/                 # /proc/<pid>/mem 跨进程读取 + JNI
│           ├── so_dumper/              # 运行时 SO dump + JNI
│           ├── injector/               # 注入器实现（同时被 libinjector.so 复用）
│           ├── parasite/               # D 方案 / 寄生执行骨架
│           ├── stack_spoof/            # ARM64 sleep-mask 调用栈伪造
│           ├── kpatch/                 # KernelPatch / KPM 控制 + JNI
│           ├── ai_overlay/             # NCNN + NanoDet 屏检管线 + JNI
│           ├── pvr_helper/             # PVR 纹理辅助
│           ├── ncnn/<ABI>/             # 预编译 ncnn 静态库（按 ABI）
│           ├── Dobby/                  # Dobby Hook 框架（子模块/源码）
│           ├── AndroidSurfaceImgui/    # Android Surface + ImGui 渲染
│           ├── Polaris-Obfuscator/     # OLLVM/Polaris 混淆器源码
│           └── games/                  # 游戏适配层（一种游戏一个目录）
│               ├── unity_lol/          # Unity / IL2CPP 通用 + 英雄联盟手游
│               │   ├── il2cpp_dumper/  # IL2CPP 运行时 Dump
│               │   ├── il2cpp_header/
│               │   ├── unity_api/      # Unity API 包装
│               │   ├── draw/           # Draw.cpp（绘制）
│               │   ├── symbol/         # Symbol.cpp（符号解析）
│               │   ├── lol/lolm.cpp    # LoL 业务逻辑
│               │   └── interface/      # ImGui 菜单
│               ├── ue_pubgmhd/         # UE4 / 和平精英 (PUBGMHD)
│               │   ├── engine/         # UE4Struct/Dumper/Header
│               │   ├── draw/UE4Draw.cpp
│               │   ├── pubgmhd/pubgmhd.cpp
│               │   └── interface/
│               ├── ue_dfm/             # UE5 / 三角洲行动 (DFM)
│               │   ├── engine/         # UE5DfmStruct/Dumper + UE5Header
│               │   ├── draw/DfmDraw.cpp
│               │   ├── dfm/            # dfm.cpp + dfm_item_registry.cpp
│               │   └── interface/
│               └── ue_nrc/             # UE 4.26 / 洛克王国手游 (NRC)
│                   ├── engine/
│                   └── nrc/nrc.cpp
├── tools/
│   ├── windows_llvm_launcher.cmd       # CMake/Ninja 用 launcher
│   └── windows_llvm_launcher_host/     # C# 编写的 launcher 实现
├── gradle/libs.versions.toml           # 版本目录
├── settings.gradle.kts
├── build.gradle.kts
├── gradlew / gradlew.bat
├── build_obfuscated.bat                # 一键混淆构建
├── build_and_run.bat / .sh
├── build_llvm_push.bat / .ps1
├── logcat.bat
└── README.md
```

---

## 功能模块说明

| 模块 | 路径 | 作用 |
| --- | --- | --- |
| Core 基础设施 | `cpp/core/` | 日志、文件、Overlay 渲染、隐藏自身、反调试 |
| 跨进程内存读取 | `cpp/memory/` | 通过 `/proc/<pid>/mem` 读取目标游戏内存，并通过 JNI 暴露给 Java 层 |
| SO 运行时 Dump | `cpp/so_dumper/` | 在目标进程运行时把已加载 SO 修复并 dump 到磁盘 |
| 注入器 | `cpp/injector/` | 远程注入，独立产物 `libinjector.so` |
| KernelPatch / KPM | `cpp/kpatch/` | 控制 KPM 模块（隐藏注入痕迹等） |
| 反调试 / 反 Frida | `cpp/core/anti_debug/` | `JNI_OnLoad` 启动自检线程，可通过 `ENABLE_ANTI_DEBUG` 开关 |
| Stack Spoof | `cpp/stack_spoof/` | ARM64 sleep-mask 风格调用栈伪造 |
| Parasite | `cpp/parasite/` | "D 方案" 寄生执行骨架 |
| AI 屏检 | `cpp/ai_overlay/` | NCNN + NanoDet 旁路截屏推理（不 Hook 渲染管线），含 AimAssist |
| Overlay 渲染 | `cpp/AndroidSurfaceImgui/` + `core/overlay/` | 基于 Android Surface 的独立 ImGui Overlay |
| Unity / IL2CPP | `cpp/games/unity_lol/` | IL2CPP Dump、Unity API 包装、LoL 业务 |
| UE4 / PUBGMHD | `cpp/games/ue_pubgmhd/` | UE4 Struct/Dumper + 和平精英业务 |
| UE5 / DFM | `cpp/games/ue_dfm/` | UE5 Struct/Dumper + 三角洲行动业务 |
| UE 4.26 / NRC | `cpp/games/ue_nrc/` | 洛克王国手游业务 |

---

## 技术栈与开源项目

**语言**

- Java 11（Android 应用层 / UI / JNI 桥接）
- C++20（全部 Native 业务、Hook、Overlay、Dump、注入）
- C#（`tools/windows_llvm_launcher_host/`，.NET 发布的 launcher 转发器）
- CMake / Kotlin DSL（构建脚本）
- PowerShell / Batch / Bash（构建和推送脚本）

**核心框架与库**

- [Android Gradle Plugin 9.1.0](https://developer.android.com/build) + Gradle Wrapper
- Android NDK（CMake 3.22.1，Ninja 生成器）
- AndroidX：`appcompat 1.6.1`、`material 1.10.0`、`constraintlayout 2.1.4`
- 测试：JUnit 4.13.2、AndroidX Test、Espresso 3.5.1

**Native 第三方 / 开源项目**

| 项目 | 用途 | 位置 |
| --- | --- | --- |
| [Dobby](https://github.com/jmpews/Dobby) | Inline Hook / Trampoline | `cpp/Dobby/` |
| [ImGui](https://github.com/ocornut/imgui) | Overlay UI 渲染 | `cpp/AndroidSurfaceImgui/third_party/imgui` |
| AndroidSurfaceImgui | Android Surface + ImGui 适配层 | `cpp/AndroidSurfaceImgui/` |
| [Polaris Obfuscator](https://github.com/za233/Polaris-Obfuscator) / OLLVM | 控制流扁平化（fla）/ 字符串加密（strcry）/ 间接调用 / BCF / Substitution 等编译期混淆 | `cpp/Polaris-Obfuscator/` + 自定义 Windows-LLVM 前端 |
| [ncnn](https://github.com/Tencent/ncnn) | 移动端神经网络推理（Vulkan） | `cpp/ncnn/<ABI>/`（预编译） |
| [NanoDet](https://github.com/RangiLyu/nanodet) | 轻量目标检测模型，用于 AI 屏检 / AimAssist | `cpp/ai_overlay/` |
| KernelPatch / KPM | 内核态注入隐藏 | `cpp/kpatch/` |
| il2cppDumper 思路 | Unity IL2CPP 运行时 Dump | `cpp/games/unity_lol/il2cpp_dumper/` |

**渲染 / 系统库**

- OpenGL ES 3 (`GLESv3`) + EGL，Android 原生 `log`、`android`、`dl`

---

## 开发环境 / 编译要求

在新机器上准备以下环境：

| 组件 | 版本 / 说明 |
| --- | --- |
| 操作系统 | Windows 10/11（脚本以 PowerShell / Batch 编写）；macOS / Linux 也可用 `build_and_run.sh`，但 Windows-LLVM 前端为 Windows 专用 |
| JDK | 17+（AGP 9.x 要求） |
| Android Studio | Hedgehog 或更高（可选，便于打开工程） |
| Android SDK | `compileSdk = 36`、`targetSdk = 36`、`minSdk = 24` |
| Android NDK | 与 AGP 9.1.0 兼容的版本（推荐 NDK r26+） |
| CMake | 3.22.1（由 SDK Manager 安装） |
| Ninja | 由 NDK 自带 |
| .NET SDK | 6.0+（仅当需要重建 `WindowsLlvmLauncher.exe` 时） |
| Git | 用于克隆 + 子模块 |
| Windows-LLVM 工具链 | 仓库外，存放路径例如 `C:\...\il2cppDumper\Windows-llvm`（可选，仅启用混淆时需要） |
| MinGW DLL | `libgcc_s_seh-1.dll` / `libstdc++-6.dll` / `libwinpthread-1.dll` 复制到 `Windows-llvm/bin`（可选） |
| ADB | 若需 `build_and_run` 自动安装 / 拉起 APK |

ABI：当前 `app/build.gradle.kts` 仅启用 `arm64-v8a`，如需其它 ABI 请取消注释 `abiFilters` 中的对应行，并在 `cpp/ncnn/<ABI>/` 提供对应 ncnn 预编译。

---

## 克隆与编译步骤

### 1. 克隆仓库

```powershell
git clone <你的仓库地址> game
cd game
# 如包含 submodule（Dobby / Polaris-Obfuscator 等）
git submodule update --init --recursive
```

### 2. 配置 `local.properties`

在 `game/local.properties` 中指定你的 SDK / NDK 路径：

```properties
sdk.dir=C\:\\Users\\<You>\\AppData\\Local\\Android\\Sdk
ndk.dir=C\:\\Users\\<You>\\AppData\\Local\\Android\\Sdk\\ndk\\<version>
```

### 3. （可选）准备 Windows-LLVM 前端

如果想启用混淆 / Polaris：

1. 把 Windows-LLVM 工具链放到任意位置，例如 `D:\toolchains\Windows-llvm`，需要存在 `bin/clang.exe` 与 `bin/clang++.exe`。
2. 复制以下 DLL 到 `Windows-llvm/bin`（典型来源：`C:\Program Files\Git\mingw64\bin\`）：
   - `libgcc_s_seh-1.dll`
   - `libstdc++-6.dll`
   - `libwinpthread-1.dll`

   缺失这三个 DLL 通常会让 `Windows-llvm/bin/clang.exe --version` 报 `0xC0000135`。
3. 若 `tools/windows_llvm_launcher_host/WindowsLlvmLauncher.exe` 缺失，重新构建：

   ```powershell
   dotnet publish .\tools\windows_llvm_launcher_host\WindowsLlvmLauncher.csproj -c Release -o .\tools\windows_llvm_launcher_host
   ```

### 4. 构建

**普通 Debug（不启用 Windows-LLVM 混淆）**

```powershell
.\gradlew.bat :app:assembleDebug
```

**启用 Windows-LLVM 前端 + 混淆 Debug**

```powershell
.\gradlew.bat :app:assembleDebug `
    -PuseWindowsLlvmFrontend=true `
    -PwindowsLlvmRoot="D:/toolchains/Windows-llvm" `
    -PenableWindowsLlvmObfuscation=true
```

也可使用环境变量：

```powershell
$env:USE_WINDOWS_LLVM_FRONTEND="true"
$env:WINDOWS_LLVM_ROOT="D:/toolchains/Windows-llvm"
$env:ENABLE_WINDOWS_LLVM_OBFUSCATION="true"
.\gradlew.bat :app:assembleDebug
```

**只构建 Native**

```powershell
.\gradlew.bat :app:externalNativeBuildDebug -PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot="D:/toolchains/Windows-llvm"
```

**一键混淆 + 安装 + 启动**

```powershell
.\build_obfuscated.bat            # 安装并启动
.\build_obfuscated.bat native     # 只构建 native
.\build_obfuscated.bat release    # 构建混淆 Release APK
.\build_obfuscated.bat logcat     # 启动后挂 logcat
.\build_obfuscated.bat help
```

构建产物：

- APK：`app/build/outputs/apk/debug/app-debug.apk`
- Native：`app/build/intermediates/cxx/Debug/.../obj/<abi>/libdobbyproject.so`、`libinjector.so`

### 5. 可调开关（Gradle 属性 / 环境变量）

| Gradle 属性 | 环境变量 | 默认 | 作用 |
| --- | --- | --- | --- |
| `useWindowsLlvmFrontend` | `USE_WINDOWS_LLVM_FRONTEND` | false | 启用自定义 Windows-LLVM 前端 |
| `windowsLlvmRoot` | `WINDOWS_LLVM_ROOT` | 无 | Windows-LLVM 根目录 |
| `enableWindowsLlvmObfuscation` | `ENABLE_WINDOWS_LLVM_OBFUSCATION` | 跟随上面 | 启用 Polaris/OLLVM Pass |
| `enableAntiDebug` | `ENABLE_ANTI_DEBUG` | true | 启用反调试 / 反 Frida 自检线程 |
| `enableVisibilityHidden` | `ENABLE_VISIBILITY_HIDDEN` | false | 默认隐藏所有符号，仅 JNIEXPORT 导出 |

混淆 Pass 在 `app/src/main/cpp/CMakeLists.txt` 中按强度分组（`sub` / `fla,sub` 等），并按源文件应用。

---

## Windows-LLVM 前端 + Android NDK

由于 Android NDK 工具链会覆盖 `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER`，本项目并不直接替换编译器，而是通过 launcher 转发：

1. `app/build.gradle.kts` 注入 `CMAKE_C_COMPILER_LAUNCHER` / `CMAKE_CXX_COMPILER_LAUNCHER`。
2. `tools/windows_llvm_launcher.cmd` 是 CMake/Ninja 的 launcher 入口。
3. `tools/windows_llvm_launcher_host/WindowsLlvmLauncher.exe`（C# 编写）使用 `ProcessStartInfo.ArgumentList` 转发原始 clang 参数。
4. Launcher 把前端切换为 `Windows-llvm/bin/clang(.exe)` 或 `clang++.exe`，并注入正确的 `-resource-dir`（`Windows-llvm/clang/16`）。

这样即可在保留 NDK `--target` / `--sysroot` / 平台库的同时使用 Polaris/OLLVM Pass。

---

## 常用命令

```powershell
# Debug 构建并安装运行
.\build_and_run.bat

# 推送构建产物到设备
.\build_llvm_push.bat
.\build_llvm_push.ps1

# logcat 抓取
.\logcat.bat
```

> 安装时若遇到 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`（旧 APK 签名不同），脚本会自动卸载旧包后重试。

---

## 集成 KernelPatch / KPM 反作弊穿透

本项目已与 [FrideHide-kpm](https://github.com/18325984858/FrideHide-kpm) 深度集成。APK 内已 bundle 两个 KPM：

| KPM | 路径 | 作用 |
|---|---|---|
| **kpm-svc** (inject-hide) | `app/src/main/assets/svc.kpm` | 通用隐藏：hide_pkg / hide_so / hide_pid / hide_comm / 路径过滤 / frida 端口屏蔽 |
| **game-kpm** (GameKpm) | `app/src/main/assets/game-kpm.kpm` | 游戏反作弊穿透：ptrace / prctl / mincore / inotify / TracerPid / uname / 私有目录观测 |

### 使用流程（普通用户）

1. 设备：Pixel 8 (Android 14) + APatch / KernelPatch + 已知 superkey
2. `./gradlew installDebug` → 自动重编 KPM + 同步 assets + 打 APK + 推到设备
3. 打开 dobbyproject app → 输入 superkey
4. **必须先装 inject-hide**：💎 INJECT-HIDE 管理 (KPM) → 📥 安装 KPM
5. **再装 GameKpm**：⚙ GameKpm 高级设置 → 📥 安装 KPM（如未装 svc 会弹"一键先装 svc + GameKpm"对话框）
6. 启动 PUBG / DFM 时勾选"启用 GameKpm 反检测 (TerSafe/TPRT)"，CheckBox 默认勾选

### 一条命令开发流程

```bash
# 改 KPM 源码或 Java 后：
./gradlew installDebug

# 自动执行：
#   1. make GameKpm     →  game-kpm.kpm
#   2. make inject-hide →  svc.kpm
#   3. 同步到 assets/
#   4. 打 APK
#   5. push + install 到设备
```

跳过 KPM 编译只重打 APK：
```bash
./gradlew installDebug -PskipKpmBuild=true
```

仅同步 KPM：
```bash
./gradlew syncAllKpm
```

### Gradle Task 矩阵

| Task | 行为 |
|---|---|
| `buildGameKpm` | 在 `../FrideHide-kpm/kpms/GameKpm/` 跑 `make` |
| `syncGameKpm` | 把 `game-kpm.kpm` 拷到 `assets/`（按时间戳增量） |
| `buildInjectHide` | 在 `../FrideHide-kpm/kpms/inject-hide/` 跑 `make` |
| `syncInjectHide` | 把 `svc.kpm` 拷到 `assets/` |
| `syncAllKpm` | 聚合：syncGameKpm + syncInjectHide |
| `assembleDebug` / `installDebug` | 自动依赖 syncAllKpm（preBuild 阶段触发） |

### 主页 UI（MainActivity）

```
─── Superkey 输入区（首次启动）
─── ▼ ⚙ 环境准备 (宽容模式 / 输入权限 / 字体)
─── 📝 启用日志输出
─── ▼ lol 手游 / 和平精英(★含反检测☑) / 三角洲(★含反检测☑) / 洛克王国
─── 🟣 SO Dumper / 🔍 内存读取器 / 💎 INJECT-HIDE 管理 (KPM)
─── ▼ ⚙ GameKpm 高级设置 (10 反检测开关 + DFM/PUBG 预设 + 安装/卸载)
─── 状态栏
```

### 反检测能力矩阵

| 检测点（TerSafe / TPRT） | 拦截方式 | 状态 |
|---|---|---|
| `prctl(PR_GET_DUMPABLE)` 反调试 | game-kpm hook 返回 1 | ✅ 实测 60s 命中 3700+ 次 |
| `mincore` 页驻留探测 | game-kpm vec 全填 1 | ✅ 命中 70+ 次 |
| `/proc/self/status` TracerPid | game-kpm read 后 → "0" | ✅ 命中 3+ 次 |
| `inotify_add_watch` 文件监控 | game-kpm 返回伪 wd | ✅ 防御性挡板 |
| `/dev/pts/*` PTY 探测 | game-kpm openat → -ENOENT | ✅ 防御性挡板 |
| frida-agent 27042 端口 | inject-hide connect 拦截 | ✅ 实测命中 |
| dobby/frida SO 路径暴露 | inject-hide hide_so 列表 | ✅ /proc/maps 行隐藏 |
| dobbyproject 自家 APK 加载 | inject-hide 自家路径豁免 | ✅ /com.example.dobbyproject 全路径放行 |
| 系统属性 ro.boot.verifiedbootstate | ⚠ 需 APM resetprop 模块 | 计划外 |
| 硬件 Key Attestation | ⚠ 需 Tricky Store + keybox | 计划外 |

### 稳定性

- DFM 60 秒冷启动 + 登录界面：0 闪退 0 panic
- KPM 装卸压测 40 轮：0 失败 0 重启
- dobbyproject + kpm-svc + game-kpm 三方共存稳定

### 故障排查

| 现象 | 原因 | 解决 |
|---|---|---|
| 安装 KPM 失败 | superkey 错误或未输入 | 主页输入正确的 APatch SuperKey |
| game-kpm 安装但反检测无效果 | 没装 inject-hide / 没勾 cb_*_anticheat | UI 弹"一键先装 svc + GameKpm"按钮 |
| dobbyproject 启动闪退 "library not found" | inject-hide hide_so 误拦了 dobbyproject 自家 SO | 已修复（任何 `/com.example.dobbyproject` 路径都豁免） |
| 设备重启 | 旧 build 的 stack overflow / lazy lookup race | 升级到最新 GameKpm（已在 init 一次性解析符号） |

### 配套项目

- 内核侧 KPM 源码：[FrideHide-kpm](https://github.com/18325984858/FrideHide-kpm)
- 上游：[KernelPatch](https://github.com/bmax121/KernelPatch) / [APatch](https://github.com/bmax121/APatch)

仅供安全研究学习。**滥用本项目导致的封号 / 法律责任，作者概不承担**。
