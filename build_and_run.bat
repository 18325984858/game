@echo off
chcp 65001 >nul
setlocal EnableDelayedExpansion
cd /d "%~dp0"

:: ============================================================
::  Dobby Project - 编译 & 安装 & 运行 脚本
:: ============================================================
::
::  用法: build_and_run.bat [命令] [llvm]
::
::  命令:
::    (无参数)   编译 + 安装 + 运行 (默认)
::    build      仅编译 Debug APK
::    native     仅编译 Native .so
::    install    仅安装到设备
::    run        仅启动应用
::    release    编译 Release APK
::    logcat     启动 logcat 跟应用进程
::    clean      清理构建缓存
::    help       显示此帮助
::
::  选项:
::    llvm       启用 LLVM 混淆编译 (仅编译相关命令生效)
::
::  示例:
::    build_and_run.bat              普通编译+安装+运行
::    build_and_run.bat llvm         LLVM混淆编译+安装+运行
::    build_and_run.bat build llvm   仅LLVM混淆编译
::    build_and_run.bat llvm build   同上 (顺序无关)
::    build_and_run.bat logcat       启动logcat查看日志
::    build_and_run.bat native llvm  仅LLVM编译native .so
::
:: ============================================================

:: --- 基础变量 ---
set "JAVA_HOME=C:\Program Files\Android\Android Studio\jbr"
set "PATH=%JAVA_HOME%\bin;%PATH%"
set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
set "ADB_SERIAL="
set "ADB_DEVICE="
set "PACKAGE=com.example.dobbyproject"
set "ACTIVITY=%PACKAGE%/.MainActivity"
set "APK_PATH=app\build\outputs\apk\debug\app-debug.apk"
set "SCRIPT_DIR=%~dp0"
set "DEFAULT_LLVM_ROOT=%SCRIPT_DIR%Windows-llvm"
if "%DEFAULT_LLVM_ROOT:~-1%"=="\" set "DEFAULT_LLVM_ROOT=%DEFAULT_LLVM_ROOT:~0,-1%"
set "LLVM_ROOT=%DEFAULT_LLVM_ROOT%"

:: --- LLVM 编译开关 ---
set "USE_LLVM=0"
set "LLVM_ARGS="
set "BUILD_LABEL=Normal"

:: --- 参数解析：支持任意位置的 llvm 标志词 ---
set "CMD="
set "EXPECT_DEVICE_SERIAL=0"
for %%A in (%*) do (
    set "ARG=%%~A"
    if "!EXPECT_DEVICE_SERIAL!"=="1" (
        set "ADB_SERIAL=!ARG!"
        set "EXPECT_DEVICE_SERIAL=0"
    ) else if /i "!ARG!"=="device" (
        set "EXPECT_DEVICE_SERIAL=1"
    ) else if /i "!ARG!"=="-s" (
        set "EXPECT_DEVICE_SERIAL=1"
    ) else if /i "!ARG:~0,7!"=="device=" (
        set "ADB_SERIAL=!ARG:~7!"
    ) else if /i "!ARG:~0,7!"=="serial=" (
        set "ADB_SERIAL=!ARG:~7!"
    ) else if /i "!ARG!"=="llvm" (
        set "USE_LLVM=1"
    ) else if not defined CMD (
        set "CMD=!ARG!"
    )
)

if "%EXPECT_DEVICE_SERIAL%"=="1" (
    echo [错误] device/-s 参数后缺少设备序列号
    exit /b 1
)

:: --- 如果启用 LLVM，校验工具链并设置参数 ---
if "%USE_LLVM%"=="1" (
    set "BUILD_LABEL=LLVM Obfuscated"
    if not exist "%LLVM_ROOT%\bin\clang.exe" (
        echo [错误] Windows-llvm clang 未找到: "%LLVM_ROOT%\bin\clang.exe"
        echo [提示] 请确保 Windows-llvm 目录存在，或修改脚本中 LLVM_ROOT 路径
        exit /b 1
    )
    if not exist "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe" (
        echo [错误] Launcher host 未找到: "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe"
        echo [提示] 构建: dotnet publish .\tools\windows_llvm_launcher_host\WindowsLlvmLauncher.csproj -c Release -o .\tools\windows_llvm_launcher_host
        exit /b 1
    )
    set "LLVM_ARGS=-PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot=%LLVM_ROOT% -PenableWindowsLlvmObfuscation=true"
    echo [信息] LLVM 混淆编译已启用
    echo [信息] LLVM 路径: %LLVM_ROOT%
) else (
    echo [信息] 普通编译模式
)

if not defined CMD goto :build_install_run
if /i "%CMD%"=="build"   goto :build_only
if /i "%CMD%"=="native"  goto :build_native
if /i "%CMD%"=="install" goto :install_only
if /i "%CMD%"=="run"     goto :run_only
if /i "%CMD%"=="logcat"  goto :logcat
if /i "%CMD%"=="clean"   goto :clean
if /i "%CMD%"=="release" goto :build_release
if /i "%CMD%"=="help"    goto :help
echo [错误] 未知命令: %CMD%
goto :help

:: ============================================================
:help
echo.
echo 用法: build_and_run.bat [命令] [llvm]
echo.
echo 命令:
echo   (无参数)   编译 + 安装 + 运行 (默认)
echo   build      仅编译 Debug APK
echo   native     仅编译 Native .so
echo   install    仅安装到设备
echo   run        仅启动应用
echo   release    编译 Release APK
echo   logcat     启动 logcat 跟应用进程
echo   clean      清理构建缓存
echo   help       显示此帮助
echo.
echo 选项:
echo   llvm       启用 LLVM 混淆编译 (仅编译相关命令生效)
echo   device XXX 指定 ADB 设备序列号
echo   -s XXX     指定 ADB 设备序列号
echo.
echo 示例:
echo   build_and_run.bat              普通编译+安装+运行
echo   build_and_run.bat llvm          LLVM混淆编译+安装+运行
echo   build_and_run.bat build llvm    仅LLVM混淆编译
echo   build_and_run.bat llvm build    同上 (顺序无关)
echo   build_and_run.bat device 37171FDJH001TH
echo   build_and_run.bat install device 37171FDJH001TH
echo   build_and_run.bat logcat        启动logcat查看日志
echo   build_and_run.bat native llvm   仅LLVM编译native .so
echo.
goto :eof

:: ============================================================
:resolve_adb_device
if not exist "%ADB%" (
    echo [错误] 未找到 ADB: %ADB%
    exit /b 1
)

set "ADB_DEVICE="
set "ADB_DEVICE_COUNT=0"
set "ADB_FIRST_SERIAL="
set "ADB_DEVICE_LIST="
set "ADB_DEVICE_FOUND=0"

for /f "skip=1 tokens=1,2" %%A in ('"%ADB%" devices') do (
    if "%%B"=="device" (
        set /a ADB_DEVICE_COUNT+=1
        if not defined ADB_FIRST_SERIAL set "ADB_FIRST_SERIAL=%%A"
        if defined ADB_DEVICE_LIST (
            set "ADB_DEVICE_LIST=!ADB_DEVICE_LIST!, %%A"
        ) else (
            set "ADB_DEVICE_LIST=%%A"
        )
        if defined ADB_SERIAL (
            if /i "%%A"=="!ADB_SERIAL!" set "ADB_DEVICE_FOUND=1"
        )
    )
)

if defined ADB_SERIAL (
    if not "!ADB_DEVICE_FOUND!"=="1" (
        echo [错误] 指定的设备未连接: !ADB_SERIAL!
        if defined ADB_DEVICE_LIST echo [信息] 在线设备: !ADB_DEVICE_LIST!
        exit /b 1
    )
    set "ADB_DEVICE=-s !ADB_SERIAL!"
    echo [信息] 使用设备: !ADB_SERIAL!
    exit /b 0
)

if "!ADB_DEVICE_COUNT!"=="0" (
    echo [错误] 未检测到在线设备
    exit /b 1
)

if "!ADB_DEVICE_COUNT!"=="1" (
    set "ADB_SERIAL=!ADB_FIRST_SERIAL!"
    set "ADB_DEVICE=-s !ADB_SERIAL!"
    echo [信息] 自动选择设备: !ADB_SERIAL!
    exit /b 0
)

echo [错误] 检测到多个设备，请指定序列号
echo [信息] 在线设备: !ADB_DEVICE_LIST!
echo [用法] %~nx0 install device SERIAL
exit /b 1

:: ============================================================
:clean
echo [1/1] 清理构建缓存...
call gradlew.bat clean
if %errorlevel% neq 0 (echo [错误] 清理失败 & exit /b 1)
echo [完成] 清理成功
goto :eof

:: ============================================================
:build_only
echo [1/1] 编译 Debug APK (%BUILD_LABEL%)...
call gradlew.bat assembleDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [错误] 编译失败 & exit /b 1)
echo [完成] APK 位置: %APK_PATH%
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
echo [完成] Release APK 位置: app\build\outputs\apk\release\
goto :eof

:: ============================================================
:install_only
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo [1/1] 安装到设备...
"%ADB%" %ADB_DEVICE% install -r "%APK_PATH%"
if errorlevel 1 (
    echo [提示] 签名不匹配，尝试卸载后重新安装...
    "%ADB%" %ADB_DEVICE% uninstall %PACKAGE%
    "%ADB%" %ADB_DEVICE% install "%APK_PATH%"
    if !errorlevel! neq 0 (echo [错误] 安装失败 & exit /b 1)
)
echo [完成] 安装成功
goto :eof

:: ============================================================
:run_only
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo [1/1] 启动应用...
"%ADB%" %ADB_DEVICE% shell am start -n %ACTIVITY%
if %errorlevel% neq 0 (echo [错误] 启动失败 & exit /b 1)
echo [完成] 应用已启动
goto :eof

:: ============================================================
:logcat
call :resolve_adb_device
if errorlevel 1 exit /b 1
set "APP_PID="
for /f "usebackq delims=" %%P in (`"%ADB%" %ADB_DEVICE% shell pidof %PACKAGE% 2^>nul`) do (
    set "APP_PID=%%P"
)
if not defined APP_PID (
    echo [错误] 未找到应用进程: %PACKAGE%
    echo [提示] 请先启动应用: %~nx0 run
    exit /b 1
)
echo [logcat] 跟 PID: !APP_PID! (%PACKAGE%)
echo [logcat] Ctrl+C 停止
echo --------------------------------------------------------------
"%ADB%" %ADB_DEVICE% logcat -c
"%ADB%" %ADB_DEVICE% logcat --pid=!APP_PID!
goto :eof

:: ============================================================
:build_install_run
call :resolve_adb_device
if errorlevel 1 exit /b 1
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
    echo [提示] 签名不匹配，尝试卸载后重新安装...
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
:: ============================================================
::  Dobby Project - ???? & ??? & ???? ???
:: ============================================================
::
::  ?÷?: build_and_run.bat [????] [llvm]
::
::  ????:
::    (?????)   ???? + ??? + ???? (???)
::    build      ?????? Debug APK
::    native     ?????? Native .so
::    install    ????????豸
::    run        ?????????
::    release    ???? Release APK
::    logcat     ???? logcat ????y???
::    clean      ????????????
::    help       ????????
::
::  ???:
::    llvm       ???? LLVM ???????? (???????????????)
::
::  ???:
::    build_and_run.bat              ???????+???+????
::    build_and_run.bat llvm         LLVM????????+???+????
::    build_and_run.bat build llvm   ??LLVM????????
::    build_and_run.bat llvm build   ??? (??????)
::    build_and_run.bat logcat       ????logcat?????
::    build_and_run.bat native llvm  ??LLVM????native .so
::
:: ============================================================
@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

:: --- ?????? ---
set "JAVA_HOME=C:\Program Files\Android\Android Studio\jbr"
set "PATH=%JAVA_HOME%\bin;%PATH%"
set "ADB=%LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe"
set "ADB_SERIAL="
set "ADB_DEVICE="
set "PACKAGE=com.example.dobbyproject"
set "ACTIVITY=%PACKAGE%/.MainActivity"
set "APK_PATH=app\build\outputs\apk\debug\app-debug.apk"
set "SCRIPT_DIR=%~dp0"
set "DEFAULT_LLVM_ROOT=%SCRIPT_DIR%Windows-llvm"
if "%DEFAULT_LLVM_ROOT:~-1%"=="\" set "DEFAULT_LLVM_ROOT=%DEFAULT_LLVM_ROOT:~0,-1%"
set "LLVM_ROOT=%DEFAULT_LLVM_ROOT%"

:: --- LLVM ???????? ---
set "USE_LLVM=0"
set "LLVM_ARGS="
set "BUILD_LABEL=Normal"

:: --- ?????????????????λ??? llvm ????? ---
set "CMD="
set "EXPECT_DEVICE_SERIAL=0"
for %%A in (%*) do (
    set "ARG=%%~A"
    if "!EXPECT_DEVICE_SERIAL!"=="1" (
        set "ADB_SERIAL=!ARG!"
        set "EXPECT_DEVICE_SERIAL=0"
    ) else if /i "!ARG!"=="device" (
        set "EXPECT_DEVICE_SERIAL=1"
    ) else if /i "!ARG!"=="-s" (
        set "EXPECT_DEVICE_SERIAL=1"
    ) else if /i "!ARG:~0,7!"=="device=" (
        set "ADB_SERIAL=!ARG:~7!"
    ) else if /i "!ARG:~0,7!"=="serial=" (
        set "ADB_SERIAL=!ARG:~7!"
    ) else if /i "!ARG!"=="llvm" (
        set "USE_LLVM=1"
    ) else if not defined CMD (
        set "CMD=!ARG!"
    )
)

if "%EXPECT_DEVICE_SERIAL%"=="1" (
    echo [????] device/-s ??????????豸???к?
    exit /b 1
)

:: --- ??????? LLVM??У?鹤?????????ò??? ---
if "%USE_LLVM%"=="1" (
    set "BUILD_LABEL=LLVM Obfuscated"
    if not exist "%LLVM_ROOT%\bin\clang.exe" (
        echo [????] Windows-llvm clang δ???: "%LLVM_ROOT%\bin\clang.exe"
        echo [???] ????? Windows-llvm ???????????????? LLVM_ROOT ·??
        exit /b 1
    )
    if not exist "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe" (
        echo [????] Launcher host δ???: "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe"
        echo [???] ????: dotnet publish .\tools\windows_llvm_launcher_host\WindowsLlvmLauncher.csproj -c Release -o .\tools\windows_llvm_launcher_host
        exit /b 1
    )
    set "LLVM_ARGS=-PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot=%LLVM_ROOT% -PenableWindowsLlvmObfuscation=true"
    echo [????] LLVM ??????????????
    echo [????] LLVM ·??: %LLVM_ROOT%
) else (
    echo [????] ?????????
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
echo [????] δ?????: %CMD%
goto :help

:: ============================================================
:help
echo.
echo ?÷?: build_and_run.bat [????] [llvm]
echo.
echo ????:
echo   (?????)   ???? + ??? + ???? (???)
echo   build      ?????? Debug APK
echo   native     ?????? Native .so
echo   install    ????????豸
echo   run        ?????????
echo   release    ???? Release APK
echo   logcat     ???? logcat ????y???
echo   clean      ????????????
echo   help       ????????
echo.
echo ???:
echo   llvm       ???? LLVM ???????? (???????????????)
echo   device XXX ??? ADB ?豸???к?
echo   -s XXX     ??? ADB ?豸???к?
echo.
echo ???:
echo   build_and_run.bat              ???????+???+????
echo   build_and_run.bat llvm          LLVM????????+???+????
echo   build_and_run.bat build llvm    ??LLVM????????
echo   build_and_run.bat llvm build    ??? (??????)
echo   build_and_run.bat device 37171FDJH001TH
echo   build_and_run.bat install device 37171FDJH001TH
echo   build_and_run.bat logcat        ????logcat?????
echo   build_and_run.bat native llvm   ??LLVM????native .so
echo.
goto :eof

:: ============================================================
:resolve_adb_device
if not exist "%ADB%" (
    echo [????] δ??? ADB: %ADB%
    exit /b 1
)

set "ADB_DEVICE="
set "ADB_DEVICE_COUNT=0"
set "ADB_FIRST_SERIAL="
set "ADB_DEVICE_LIST="
set "ADB_DEVICE_FOUND=0"

for /f "skip=1 tokens=1,2" %%A in ('"%ADB%" devices') do (
    if "%%B"=="device" (
        set /a ADB_DEVICE_COUNT+=1
        if not defined ADB_FIRST_SERIAL set "ADB_FIRST_SERIAL=%%A"
        if defined ADB_DEVICE_LIST (
            set "ADB_DEVICE_LIST=!ADB_DEVICE_LIST!, %%A"
        ) else (
            set "ADB_DEVICE_LIST=%%A"
        )
        if defined ADB_SERIAL (
            if /i "%%A"=="!ADB_SERIAL!" set "ADB_DEVICE_FOUND=1"
        )
    )
)

if defined ADB_SERIAL (
    if not "!ADB_DEVICE_FOUND!"=="1" (
        echo [ERROR] Specified device not connected: !ADB_SERIAL!
        if defined ADB_DEVICE_LIST echo [INFO] Online devices: !ADB_DEVICE_LIST!
        exit /b 1
    )
    set "ADB_DEVICE=-s !ADB_SERIAL!"
    echo [INFO] Using device: !ADB_SERIAL!
    exit /b 0
)

if "!ADB_DEVICE_COUNT!"=="0" (
    echo [ERROR] No online device detected.
    exit /b 1
)

if "!ADB_DEVICE_COUNT!"=="1" (
    set "ADB_SERIAL=!ADB_FIRST_SERIAL!"
    set "ADB_DEVICE=-s !ADB_SERIAL!"
    echo [INFO] Auto-selected device: !ADB_SERIAL!
    exit /b 0
)

echo [ERROR] Multiple devices detected. Please specify a serial.
echo [INFO] Online devices: !ADB_DEVICE_LIST!
echo [INFO] Usage: %~nx0 install device SERIAL
exit /b 1

:: ============================================================
:clean
echo [1/1] ????????????...
call gradlew.bat clean
if %errorlevel% neq 0 (echo [????] ??????? & exit /b 1)
echo [???] ???????
goto :eof

:: ============================================================
:build_only
echo [1/1] ???? Debug APK (%BUILD_LABEL%)...
call gradlew.bat assembleDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [????] ??????? & exit /b 1)
echo [???] APK λ??: %APK_PATH%
goto :eof

:: ============================================================
:build_native
echo [1/1] ???? Native .so (%BUILD_LABEL%)...
call gradlew.bat :app:externalNativeBuildDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [????] Native ??????? & exit /b 1)
echo [???] Native Debug ???????
goto :eof

:: ============================================================
:build_release
echo [1/1] ???? Release APK (%BUILD_LABEL%)...
call gradlew.bat assembleRelease %LLVM_ARGS%
if %errorlevel% neq 0 (echo [????] ??????? & exit /b 1)
echo [???] Release APK λ??: app\build\outputs\apk\release\
goto :eof

:: ============================================================
:install_only
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo [1/1] ??????豸...
"%ADB%" %ADB_DEVICE% install -r "%APK_PATH%"
if errorlevel 1 (
    echo [???] ????????, ????ж??????°??...
    "%ADB%" %ADB_DEVICE% uninstall %PACKAGE%
    "%ADB%" %ADB_DEVICE% install "%APK_PATH%"
    if !errorlevel! neq 0 (echo [????] ?????? & exit /b 1)
)
echo [???] ??????
goto :eof

:: ============================================================
:run_only
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo [1/1] ???????...
"%ADB%" %ADB_DEVICE% shell am start -n %ACTIVITY%
if %errorlevel% neq 0 (echo [????] ??????? & exit /b 1)
echo [???] ?????????
goto :eof

:: ============================================================
:logcat
call :resolve_adb_device
if errorlevel 1 exit /b 1
set "APP_PID="
for /f "usebackq delims=" %%P in (`"%ADB%" %ADB_DEVICE% shell pidof %PACKAGE% 2^>nul`) do (
    set "APP_PID=%%P"
)
if not defined APP_PID (
    echo [????] δ?????y???: %PACKAGE%
    echo [???] ???????????: %~nx0 run
    exit /b 1
)
echo [logcat] ?? PID: !APP_PID! (%PACKAGE%)
echo [logcat] Ctrl+C ??
echo ??????????????????????????????????????????????????????????????????
"%ADB%" %ADB_DEVICE% logcat -c
"%ADB%" %ADB_DEVICE% logcat --pid=!APP_PID!
goto :eof

:: ============================================================
:build_install_run
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo ====== %BUILD_LABEL% ???? + ??? + ???? ======
echo.

echo [1/3] ???? Debug APK (%BUILD_LABEL%)...
call gradlew.bat assembleDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [????] ??????? & exit /b 1)
echo [1/3] ??????
echo.

echo [2/3] ??????豸...
"%ADB%" %ADB_DEVICE% install -r "%APK_PATH%"
if errorlevel 1 (
    echo [???] ????????, ????ж??????°??...
    "%ADB%" %ADB_DEVICE% uninstall %PACKAGE%
    "%ADB%" %ADB_DEVICE% install "%APK_PATH%"
    if !errorlevel! neq 0 (echo [????] ?????? & exit /b 1)
)
echo [2/3] ??????
echo.

echo [3/3] ???????...
"%ADB%" %ADB_DEVICE% shell am start -n %ACTIVITY%
if %errorlevel% neq 0 (echo [????] ??????? & exit /b 1)
echo [3/3] ?????????
echo.

echo ====== ?????? ======
goto :eof
