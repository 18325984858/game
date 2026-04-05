:: ============================================================
::  Dobby Project - 编译 & 安装 & 运行 脚本
:: ============================================================
::
::  用法: build_and_run.bat [命令] [llvm]
::
::  命令:
::    (无参数)   编译 + 安装 + 启动 (默认)
::    build      仅编译 Debug APK
::    native     仅编译 Native .so
::    install    仅安装到设备
::    run        仅启动应用
::    release    编译 Release APK
::    logcat     附加 logcat 到应用进程
::    clean      清理构建产物
::    help       显示此帮助
::
::  选项:
::    llvm       启用 LLVM 混淆编译 (可与任意命令组合)
::
::  示例:
::    build_and_run.bat              普通编译+安装+运行
::    build_and_run.bat llvm         LLVM混淆编译+安装+运行
::    build_and_run.bat build llvm   仅LLVM混淆编译
::    build_and_run.bat llvm build   同上 (顺序无关)
::    build_and_run.bat logcat       附加logcat查看日志
::    build_and_run.bat native llvm  仅LLVM编译native .so
::
:: ============================================================
@echo off
setlocal EnableDelayedExpansion

:: --- 配置区 ---
set "JAVA_HOME=C:\Program Files\Android\Android Studio\jbr"
set "PATH=%JAVA_HOME%\bin;%PATH%"
set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
set "ADB_DEVICE="
set "PACKAGE=com.example.dobbyproject"
set "ACTIVITY=%PACKAGE%/.MainActivity"
set "APK_PATH=app\build\outputs\apk\debug\app-debug.apk"
set "SCRIPT_DIR=%~dp0"
set "DEFAULT_LLVM_ROOT=%SCRIPT_DIR%Windows-llvm"
if "%DEFAULT_LLVM_ROOT:~-1%"=="\" set "DEFAULT_LLVM_ROOT=%DEFAULT_LLVM_ROOT:~0,-1%"
set "LLVM_ROOT=%DEFAULT_LLVM_ROOT%"

:: --- LLVM 混淆开关 ---
set "USE_LLVM=0"
set "LLVM_ARGS="
set "BUILD_LABEL=Normal"

:: --- 解析参数（支持任意位置的 llvm 标志） ---
set "CMD="
for %%A in (%*) do (
    if /i "%%A"=="llvm" (
        set "USE_LLVM=1"
    ) else if not defined CMD (
        set "CMD=%%A"
    )
)

:: --- 如果启用 LLVM，校验工具链并设置参数 ---
if "%USE_LLVM%"=="1" (
    set "BUILD_LABEL=LLVM Obfuscated"
    if not exist "%LLVM_ROOT%\bin\clang.exe" (
        echo [错误] Windows-llvm clang 未找到: "%LLVM_ROOT%\bin\clang.exe"
        echo [提示] 请确认 Windows-llvm 目录存在，或修改脚本中 LLVM_ROOT 路径
        exit /b 1
    )
    if not exist "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe" (
        echo [错误] Launcher host 未找到: "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe"
        echo [提示] 运行: dotnet publish .\tools\windows_llvm_launcher_host\WindowsLlvmLauncher.csproj -c Release -o .\tools\windows_llvm_launcher_host
        exit /b 1
    )
    set "LLVM_ARGS=-PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot=%LLVM_ROOT% -PenableWindowsLlvmObfuscation=true"
    echo [配置] LLVM 混淆编译已启用
    echo [配置] LLVM 路径: %LLVM_ROOT%
) else (
    echo [配置] 普通编译模式
)

if not defined CMD goto :build_install_run
if /i "%CMD%"=="build"   goto :build_only
if /i "%CMD%"=="native"   goto :build_native
if /i "%CMD%"=="install"  goto :install_only
if /i "%CMD%"=="run"      goto :run_only
if /i "%CMD%"=="logcat"   goto :logcat
if /i "%CMD%"=="clean"    goto :clean
if /i "%CMD%"=="release"  goto :build_release
if /i "%CMD%"=="help"     goto :help
echo [错误] 未知参数: %CMD%
goto :help

:: ============================================================
:help
echo.
echo 用法: build_and_run.bat [命令] [llvm]
echo.
echo 命令:
echo   (无参数)   编译 + 安装 + 启动 (默认)
echo   build      仅编译 Debug APK
echo   native     仅编译 Native .so
echo   install    仅安装到设备
echo   run        仅启动应用
echo   release    编译 Release APK
echo   logcat     附加 logcat 到应用进程
echo   clean      清理构建产物
echo   help       显示此帮助
echo.
echo 选项:
echo   llvm       启用 LLVM 混淆编译 (可与任意命令组合)
echo.
echo 示例:
echo   build_and_run.bat              普通编译+安装+运行
echo   build_and_run.bat llvm          LLVM混淆编译+安装+运行
echo   build_and_run.bat build llvm    仅LLVM混淆编译
echo   build_and_run.bat llvm build    同上 (顺序无关)
echo   build_and_run.bat logcat        附加logcat查看日志
echo   build_and_run.bat native llvm   仅LLVM编译native .so
echo.
goto :eof

:: ============================================================
:clean
echo [1/1] 清理构建产物...
call gradlew.bat clean
if %errorlevel% neq 0 (echo [错误] 清理失败 & exit /b 1)
echo [完成] 清理成功
goto :eof

:: ============================================================
:build_only
echo [1/1] 编译 Debug APK (%BUILD_LABEL%)...
call gradlew.bat assembleDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [错误] 编译失败 & exit /b 1)
echo [完成] APK 位于: %APK_PATH%
goto :eof

:: ============================================================
:build_native
echo [1/1] 编译 Native .so (%BUILD_LABEL%)...
call gradlew.bat :app:externalNativeBuildDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [错误] Native 编译失败 & exit /b 1)
echo [完成] Native Debug 构建完成
goto :eof

:: ============================================================
:build_release
echo [1/1] 编译 Release APK (%BUILD_LABEL%)...
call gradlew.bat assembleRelease %LLVM_ARGS%
if %errorlevel% neq 0 (echo [错误] 编译失败 & exit /b 1)
echo [完成] Release APK 位于: app\build\outputs\apk\release\
goto :eof

:: ============================================================
:install_only
echo [1/1] 安装到设备...
"%ADB%" %ADB_DEVICE% install -r "%APK_PATH%"
if errorlevel 1 (
    echo [提示] 签名不匹配, 尝试卸载后重新安装...
    "%ADB%" %ADB_DEVICE% uninstall %PACKAGE%
    "%ADB%" %ADB_DEVICE% install "%APK_PATH%"
    if !errorlevel! neq 0 (echo [错误] 安装失败 & exit /b 1)
)
echo [完成] 安装成功
goto :eof

:: ============================================================
:run_only
echo [1/1] 启动应用...
"%ADB%" %ADB_DEVICE% shell am start -n %ACTIVITY%
if %errorlevel% neq 0 (echo [错误] 启动失败 & exit /b 1)
echo [完成] 应用已启动
goto :eof

:: ============================================================
:logcat
set "APP_PID="
for /f "usebackq delims=" %%P in (`"%ADB%" %ADB_DEVICE% shell pidof %PACKAGE% 2^>nul`) do (
    set "APP_PID=%%P"
)
if not defined APP_PID (
    echo [错误] 未找到应用进程: %PACKAGE%
    echo [提示] 请先启动应用: %~nx0 run
    exit /b 1
)
echo [logcat] 绑定 PID: !APP_PID! (%PACKAGE%)
echo [logcat] Ctrl+C 停止
echo ─────────────────────────────────
"%ADB%" %ADB_DEVICE% logcat -c
"%ADB%" %ADB_DEVICE% logcat --pid=!APP_PID!
goto :eof

:: ============================================================
:build_install_run
echo ====== %BUILD_LABEL% 编译 + 安装 + 运行 ======
echo.

echo [1/3] 编译 Debug APK (%BUILD_LABEL%)...
call gradlew.bat assembleDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [错误] 编译失败 & exit /b 1)
echo [1/3] 编译成功
echo.

echo [2/3] 安装到设备...
"%ADB%" %ADB_DEVICE% install -r "%APK_PATH%"
if errorlevel 1 (
    echo [提示] 签名不匹配, 尝试卸载后重新安装...
    "%ADB%" %ADB_DEVICE% uninstall %PACKAGE%
    "%ADB%" %ADB_DEVICE% install "%APK_PATH%"
    if !errorlevel! neq 0 (echo [错误] 安装失败 & exit /b 1)
)
echo [2/3] 安装成功
echo.

echo [3/3] 启动应用...
"%ADB%" %ADB_DEVICE% shell am start -n %ACTIVITY%
if %errorlevel% neq 0 (echo [错误] 启动失败 & exit /b 1)
echo [3/3] 应用已启动
echo.

echo ====== 全部完成 ======
goto :eof
