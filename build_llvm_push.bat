@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_llvm_push.ps1" %*
