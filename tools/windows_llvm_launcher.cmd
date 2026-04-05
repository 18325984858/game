@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
set "LAUNCHER_EXE=%SCRIPT_DIR%windows_llvm_launcher_host\WindowsLlvmLauncher.exe"
if not exist "%LAUNCHER_EXE%" (
    echo [windows_llvm_launcher] launcher host not found: %LAUNCHER_EXE% 1>&2
    exit /b 1
)

"%LAUNCHER_EXE%" %*
exit /b %ERRORLEVEL%