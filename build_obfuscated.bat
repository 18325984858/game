@echo off
chcp 65001 >nul 2>&1
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
cd /d "%SCRIPT_DIR%"

set "DEFAULT_LLVM_ROOT=%SCRIPT_DIR%Windows-llvm"
if "%DEFAULT_LLVM_ROOT:~-1%"=="\" set "DEFAULT_LLVM_ROOT=%DEFAULT_LLVM_ROOT:~0,-1%"

set "LLVM_ROOT=%DEFAULT_LLVM_ROOT%"
set "MODE=debug"

if not "%~1"=="" set "MODE=%~1"
if not "%~2"=="" set "LLVM_ROOT=%~2"

if /i "%MODE%"=="help" goto :help
if /i "%MODE%"=="/h" goto :help
if /i "%MODE%"=="-h" goto :help

if not exist "%LLVM_ROOT%\bin\clang.exe" (
    echo [错误] 未找到 Windows-llvm clang: "%LLVM_ROOT%\bin\clang.exe"
    echo [提示] 用法: build_obfuscated.bat [debug^|release^|native^|clean] [Windows-llvm路径]
    exit /b 1
)

if not exist "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe" (
    echo [错误] 未找到 launcher host: "%SCRIPT_DIR%tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe"
    echo [提示] 先执行: dotnet publish .\tools\windows_llvm_launcher_host\WindowsLlvmLauncher.csproj -c Release -o .\tools\windows_llvm_launcher_host
    exit /b 1
)

set "COMMON_ARGS=-PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot=%LLVM_ROOT% -PenableWindowsLlvmObfuscation=true"

if /i "%MODE%"=="debug" goto :build_debug
if /i "%MODE%"=="release" goto :build_release
if /i "%MODE%"=="native" goto :build_native
if /i "%MODE%"=="clean" goto :clean

echo [错误] 未知模式: %MODE%
goto :help

:help
echo.
echo 用法: build_obfuscated.bat [debug^|release^|native^|clean] [Windows-llvm路径]
echo.
echo 模式:
echo   debug    构建带混淆的 Debug APK ^(默认^)
echo   release  构建带混淆的 Release APK
echo   native   仅执行 externalNativeBuildDebug
echo   clean    清理构建产物
echo.
echo 示例:
echo   build_obfuscated.bat
echo   build_obfuscated.bat release
echo   build_obfuscated.bat native "c:\Users\Song\Desktop\file\lol\il2cppDumper\Windows-llvm"
echo.
goto :eof

:clean
echo [1/1] 清理构建产物...
call gradlew.bat clean
if errorlevel 1 (
    echo [错误] 清理失败
    exit /b 1
)
echo [完成] 清理成功
goto :eof

:build_native
echo [1/1] 编译带混淆的 Native Debug 产物...
call gradlew.bat :app:externalNativeBuildDebug %COMMON_ARGS%
if errorlevel 1 (
    echo [错误] Native 编译失败
    exit /b 1
)
echo [完成] Native Debug 构建完成
goto :eof

:build_debug
echo [1/1] 编译带混淆的 Debug APK...
call gradlew.bat :app:assembleDebug %COMMON_ARGS%
if errorlevel 1 (
    echo [错误] Debug APK 编译失败
    exit /b 1
)
echo [完成] Debug APK 位于: app\build\outputs\apk\debug\
goto :eof

:build_release
echo [1/1] 编译带混淆的 Release APK...
call gradlew.bat :app:assembleRelease %COMMON_ARGS%
if errorlevel 1 (
    echo [错误] Release APK 编译失败
    exit /b 1
)
echo [完成] Release APK 位于: app\build\outputs\apk\release\
goto :eof