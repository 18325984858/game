:: ============================================================
::  Dobby Project - ���� & ��װ & ���� �ű�
:: ============================================================
::
::  �÷�: build_and_run.bat [����] [llvm]
::
::  ����:
::    (�޲���)   ���� + ��װ + ���� (Ĭ��)
::    build      ������ Debug APK
::    native     ������ Native .so
::    install    ����װ���豸
::    run        ������Ӧ��
::    release    ���� Release APK
::    logcat     ���� logcat ��Ӧ�ý���
::    clean      ������������
::    help       ��ʾ�˰���
::
::  ѡ��:
::    llvm       ���� LLVM �������� (���������������)
::
::  ʾ��:
::    build_and_run.bat              ��ͨ����+��װ+����
::    build_and_run.bat llvm         LLVM��������+��װ+����
::    build_and_run.bat build llvm   ��LLVM��������
::    build_and_run.bat llvm build   ͬ�� (˳���޹�)
::    build_and_run.bat logcat       ����logcat�鿴��־
::    build_and_run.bat native llvm  ��LLVM����native .so
::
:: ============================================================
@echo off
setlocal EnableDelayedExpansion

:: --- ������ ---
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

:: --- LLVM �������� ---
set "USE_LLVM=0"
set "LLVM_ARGS="
set "BUILD_LABEL=Normal"

:: --- ����������֧������λ�õ� llvm ��־�� ---
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
    echo [����] device/-s ������ȱ���豸���к�
    exit /b 1
)

:: --- ������� LLVM��У�鹤���������ò��� ---
if "%USE_LLVM%"=="1" (
    set "BUILD_LABEL=LLVM Obfuscated"
    if not exist "%LLVM_ROOT%\bin\clang.exe" (
        echo [����] Windows-llvm clang δ�ҵ�: "%LLVM_ROOT%\bin\clang.exe"
        echo [��ʾ] ��ȷ�� Windows-llvm Ŀ¼���ڣ����޸Ľű��� LLVM_ROOT ·��
        exit /b 1
    )
    if not exist "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe" (
        echo [����] Launcher host δ�ҵ�: "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe"
        echo [��ʾ] ����: dotnet publish .\tools\windows_llvm_launcher_host\WindowsLlvmLauncher.csproj -c Release -o .\tools\windows_llvm_launcher_host
        exit /b 1
    )
    set "LLVM_ARGS=-PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot=%LLVM_ROOT% -PenableWindowsLlvmObfuscation=true"
    echo [����] LLVM ��������������
    echo [����] LLVM ·��: %LLVM_ROOT%
) else (
    echo [����] ��ͨ����ģʽ
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
echo [����] δ֪����: %CMD%
goto :help

:: ============================================================
:help
echo.
echo �÷�: build_and_run.bat [����] [llvm]
echo.
echo ����:
echo   (�޲���)   ���� + ��װ + ���� (Ĭ��)
echo   build      ������ Debug APK
echo   native     ������ Native .so
echo   install    ����װ���豸
echo   run        ������Ӧ��
echo   release    ���� Release APK
echo   logcat     ���� logcat ��Ӧ�ý���
echo   clean      ������������
echo   help       ��ʾ�˰���
echo.
echo ѡ��:
echo   llvm       ���� LLVM �������� (���������������)
echo   device XXX ָ�� ADB �豸���к�
echo   -s XXX     ָ�� ADB �豸���к�
echo.
echo ʾ��:
echo   build_and_run.bat              ��ͨ����+��װ+����
echo   build_and_run.bat llvm          LLVM��������+��װ+����
echo   build_and_run.bat build llvm    ��LLVM��������
echo   build_and_run.bat llvm build    ͬ�� (˳���޹�)
echo   build_and_run.bat device 37171FDJH001TH
echo   build_and_run.bat install device 37171FDJH001TH
echo   build_and_run.bat logcat        ����logcat�鿴��־
echo   build_and_run.bat native llvm   ��LLVM����native .so
echo.
goto :eof

:: ============================================================
:resolve_adb_device
if not exist "%ADB%" (
    echo [����] δ�ҵ� ADB: %ADB%
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
echo [1/1] ������������...
call gradlew.bat clean
if %errorlevel% neq 0 (echo [����] ����ʧ�� & exit /b 1)
echo [���] �����ɹ�
goto :eof

:: ============================================================
:build_only
echo [1/1] ���� Debug APK (%BUILD_LABEL%)...
call gradlew.bat assembleDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [����] ����ʧ�� & exit /b 1)
echo [���] APK λ��: %APK_PATH%
goto :eof

:: ============================================================
:build_native
echo [1/1] ���� Native .so (%BUILD_LABEL%)...
call gradlew.bat :app:externalNativeBuildDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [����] Native ����ʧ�� & exit /b 1)
echo [���] Native Debug �������
goto :eof

:: ============================================================
:build_release
echo [1/1] ���� Release APK (%BUILD_LABEL%)...
call gradlew.bat assembleRelease %LLVM_ARGS%
if %errorlevel% neq 0 (echo [����] ����ʧ�� & exit /b 1)
echo [���] Release APK λ��: app\build\outputs\apk\release\
goto :eof

:: ============================================================
:install_only
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo [1/1] ��װ���豸...
"%ADB%" %ADB_DEVICE% install -r "%APK_PATH%"
if errorlevel 1 (
    echo [��ʾ] ǩ����ƥ��, ����ж�غ����°�װ...
    "%ADB%" %ADB_DEVICE% uninstall %PACKAGE%
    "%ADB%" %ADB_DEVICE% install "%APK_PATH%"
    if !errorlevel! neq 0 (echo [����] ��װʧ�� & exit /b 1)
)
echo [���] ��װ�ɹ�
goto :eof

:: ============================================================
:run_only
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo [1/1] ����Ӧ��...
"%ADB%" %ADB_DEVICE% shell am start -n %ACTIVITY%
if %errorlevel% neq 0 (echo [����] ����ʧ�� & exit /b 1)
echo [���] Ӧ��������
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
    echo [����] δ�ҵ�Ӧ�ý���: %PACKAGE%
    echo [��ʾ] ��������Ӧ��: %~nx0 run
    exit /b 1
)
echo [logcat] �� PID: !APP_PID! (%PACKAGE%)
echo [logcat] Ctrl+C ֹͣ
echo ������������������������������������������������������������������
"%ADB%" %ADB_DEVICE% logcat -c
"%ADB%" %ADB_DEVICE% logcat --pid=!APP_PID!
goto :eof

:: ============================================================
:build_install_run
call :resolve_adb_device
if errorlevel 1 exit /b 1
echo ====== %BUILD_LABEL% ���� + ��װ + ���� ======
echo.

echo [1/3] ���� Debug APK (%BUILD_LABEL%)...
call gradlew.bat assembleDebug %LLVM_ARGS%
if %errorlevel% neq 0 (echo [����] ����ʧ�� & exit /b 1)
echo [1/3] ����ɹ�
echo.

echo [2/3] ��װ���豸...
"%ADB%" %ADB_DEVICE% install -r "%APK_PATH%"
if errorlevel 1 (
    echo [��ʾ] ǩ����ƥ��, ����ж�غ����°�װ...
    "%ADB%" %ADB_DEVICE% uninstall %PACKAGE%
    "%ADB%" %ADB_DEVICE% install "%APK_PATH%"
    if !errorlevel! neq 0 (echo [����] ��װʧ�� & exit /b 1)
)
echo [2/3] ��װ�ɹ�
echo.

echo [3/3] ����Ӧ��...
"%ADB%" %ADB_DEVICE% shell am start -n %ACTIVITY%
if %errorlevel% neq 0 (echo [����] ����ʧ�� & exit /b 1)
echo [3/3] Ӧ��������
echo.

echo ====== ȫ����� ======
goto :eof
