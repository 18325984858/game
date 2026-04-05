# il2cppDumper

## Windows-llvm + Android NDK

This project can drive the repo-local Windows-llvm frontend while still using the Android NDK toolchain for `--target`, `--sysroot`, and platform libraries.

### Runtime DLLs required by `Windows-llvm/bin/clang(.exe)`

Copy these three DLLs into `Windows-llvm/bin` after a fresh environment setup:

- `libgcc_s_seh-1.dll`
- `libstdc++-6.dll`
- `libwinpthread-1.dll`

Verified source on this machine:

- `C:\Program Files\Git\mingw64\bin\libgcc_s_seh-1.dll`
- `C:\Program Files\Git\mingw64\bin\libstdc++-6.dll`
- `C:\Program Files\Git\mingw64\bin\libwinpthread-1.dll`

If these DLLs are missing, `Windows-llvm/bin/clang.exe --version` will usually fail with `0xC0000135`.

### How the frontend is wired in

The Android NDK toolchain will overwrite `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER`, so this project does not replace the compiler directly.

Instead, the wiring is:

1. `app/build.gradle.kts` passes `CMAKE_C_COMPILER_LAUNCHER` and `CMAKE_CXX_COMPILER_LAUNCHER`.
2. `tools/windows_llvm_launcher.cmd` is the launcher entry used by CMake/Ninja.
3. `tools/windows_llvm_launcher_host/WindowsLlvmLauncher.exe` forwards every original clang argument with `ProcessStartInfo.ArgumentList`.
4. The launcher swaps the frontend to `Windows-llvm/bin/clang.exe` or `clang++.exe` and injects the correct `-resource-dir` for `Windows-llvm/clang/16`.

### Build the launcher host

If `tools/windows_llvm_launcher_host/WindowsLlvmLauncher.exe` is missing, rebuild it with:

```powershell
dotnet publish .\tools\windows_llvm_launcher_host\WindowsLlvmLauncher.csproj -c Release -o .\tools\windows_llvm_launcher_host
```

### Enable the frontend in Gradle

Use these properties when building native code through AGP:

```powershell
.\gradlew.bat :app:externalNativeBuildDebug -PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot="c:/Users/Song/Desktop/file/lol/il2cppDumper/Windows-llvm"
```

Full obfuscated Debug APK build:

```powershell
.\gradlew.bat :app:assembleDebug -PuseWindowsLlvmFrontend=true -PwindowsLlvmRoot="c:/Users/Song/Desktop/file/lol/il2cppDumper/Windows-llvm" -PenableWindowsLlvmObfuscation=true
```

Equivalent environment variables are also supported:

- `USE_WINDOWS_LLVM_FRONTEND=true`
- `WINDOWS_LLVM_ROOT=c:/Users/Song/Desktop/file/lol/il2cppDumper/Windows-llvm`

### Obfuscation flags

When the Windows-llvm frontend is enabled, obfuscation is enabled by default unless you explicitly disable it with:

- Gradle property: `-PenableWindowsLlvmObfuscation=false`
- Environment variable: `ENABLE_WINDOWS_LLVM_OBFUSCATION=false`

Current pass layout:

- Base pass for `dobbyproject` and `injector`: `sub`
- Medium pass set for `Draw/Draw.cpp`, `Symbol/Symbol.cpp`, `li2cppDumper/li2cppdumper.cpp`: `fla,sub`
- Heavy pass set for `start.cpp`, `lol/lolm.cpp`, `interface/interface.cpp`: `fla,sub`

## Verified commands

These commands were re-verified on this machine after lowering the heavy pass set to `fla,sub` and forcing the CMake cache variables:

```powershell
.\build_obfuscated_and_run.bat native
.\build_obfuscated_and_run.bat
```

What they currently do:

- `build_obfuscated_and_run.bat native`
	- Builds obfuscated native Debug outputs only.
	- Verified result: `libdobbyproject.so` and `libinjector.so` were generated under `app/build/intermediates/cxx/Debug/.../obj/<abi>/`.
- `build_obfuscated_and_run.bat`
	- Builds the obfuscated Debug APK, installs it, and launches `com.example.dobbyproject/.MainActivity`.
	- Verified result: build succeeded, the APK was produced at `app/build/outputs/apk/debug/app-debug.apk`, and the app was started on the attached device.

## Notes from the successful run

- The current stable obfuscation setup is:
	- Base: `sub`
	- Medium: `fla,sub`
	- Heavy: `fla,sub`
- The old heavier set `fla,bcf,sub,indcall,gvenc` crashed this Windows-llvm frontend on `start.cpp`, `lol/lolm.cpp`, and `interface/interface.cpp`.
- The pass variables in `app/src/main/cpp/CMakeLists.txt` are cached, so changing them may not take effect unless the cache is forced or the native CMake build directory is cleaned.
- During install, `adb install -r` may fail with `INSTALL_FAILED_UPDATE_INCOMPATIBLE` if an older APK with a different signing key is already on the device. The script already handles this by uninstalling the old package and retrying.
- The current build still emits several C/C++ warnings, but the verified obfuscated build completed successfully with those warnings present.

## Convenience script

Use the wrapper script for the common flows:

```powershell
.\build_obfuscated_and_run.bat help
```

Useful commands:

- Default: build obfuscated Debug APK, install, and launch.
- `native`: build obfuscated native outputs only.
- `release`: build obfuscated Release APK only.
- `logcat`: attach to the running app process logcat stream after launch.