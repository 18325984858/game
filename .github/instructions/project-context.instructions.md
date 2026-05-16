---
applyTo: "**"
description: game 项目记忆 — Android 游戏外挂/工具
---

# 项目: game（Android Native + Gradle）

## 技术栈

- **构建**: Gradle Kotlin DSL (`build.gradle.kts`)，AGP + Kotlin + JNI (C++)
- **Native**: NDK + LLVM/OLLVM 混淆构建 (`build_llvm_push.bat`/`.ps1`, `build_obfuscated.bat`)
- **目标**: 通过 KernelPatch (FrideHide-kpm) 注入到目标 app（DFM / PUBGMHD / 三角洲 / UE 系列）

## 目录约定

| 路径                             | 内容                                         |
| -------------------------------- | -------------------------------------------- |
| `app/src/main/`                  | Android 主代码（Kotlin/Java + JNI cpp）      |
| `tools/`                         | 注入器辅助 / Windows LLVM launcher 等        |
| `lolm_orig.cpp` / `my_patch.cpp` | 单文件 native 实验代码（非 Gradle 编译路径） |
| `logcat_*.txt`                   | 现场抓取的日志，**不要 commit 到上游**       |

## 反作弊上下文（DFM = com.tencent.tmgp.dfm）

工作区中目标 app 使用 Tencent ACE：

- `libtersafe.so` 上层 + `libtprt.so` 底层
- 反 Frida：早期 attach 即被阻断；需要 early-spawn 或经 KPM 在 SO 层 stealth
- 反 Root：检查 `/system/xbin/su`、Magisk 路径、属性 `ro.boot.verifiedbootstate` 等
- 反 Debug：ptrace 派发桥、`/dev/pts/*` 扫描、TracerPid 校验

完整架构见 `anticheat-arch.md`。改动 stealth 相关代码前先读它。

## UE 引擎注入

- 必须同时支持 UE4 与 UE5 模块命名（UE5 用 `libUnreal*`，UE4 用 `libUE4*`）
- 枚举模块仅基于 `/proc/<pid>/maps` 路径，匿名映射的引擎库会漏

## 构建命令

```bash
# 普通调试构建
./gradlew :app:assembleDebug

# 混淆 native 构建 + push
./build_and_run.sh        # Linux/macOS
./build_llvm_push.bat     # Windows

# 抓日志
./logcat.bat              # 或 adb logcat -s <tag>
```

## 风格

- Kotlin: 官方 style，4 空格
- C++: 4 空格，RAII，不要在 hook 入口分配堆内存
- JNI: 函数名严格匹配 Java 包路径

## 任务约束（Hook）

- 改完 `app/src/main/cpp/**` → 跑 `gradle: assembleDebug`，失败必须先修
- 改动 stealth 相关代码 → 必须在 PR/会话中说明影响哪类检测项
- 切勿在源码中留下真实 superkey / 设备序列号 / 包名硬编码（除 DFM/PUBG 这种公开包名）
