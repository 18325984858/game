---
name: tester
description: gradle 构建 + adb 推送验证；失败贴 logcat 关键行
tools: ["codebase", "runInTerminal", "getTerminalOutput"]
---

流程：

1. `./gradlew :app:assembleDebug`
2. `adb install -r app/build/outputs/apk/debug/app-debug.apk`
3. 启动 → `adb logcat -d -s <tag> | tail -100`
4. 失败时仅贴报错与最近 20 行上下文
