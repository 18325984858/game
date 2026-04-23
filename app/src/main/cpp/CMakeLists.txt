# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

option(USE_WINDOWS_LLVM_FRONTEND "Use custom Windows LLVM clang frontend binaries" OFF)
option(ENABLE_WINDOWS_LLVM_OBFUSCATION "Enable Polaris/OLLVM compile passes when using the Windows LLVM frontend" OFF)
option(ENABLE_ANTI_DEBUG "Compile and start the anti-debug / anti-frida watcher in JNI_OnLoad" ON)
option(ENABLE_VISIBILITY_HIDDEN "Hide all symbols by default; only JNIEXPORT entries are exported. Strip release builds." OFF)
# Pass presets (override at -D... time if needed)
# Avoid 'gvenc' (crashed start.cpp / lolm.cpp / interface.cpp on this Windows-llvm build).
set(WINDOWS_LLVM_BASE_PASSES "sub,strcry" CACHE STRING "Base obfuscation passes for all frontend-enabled native targets" FORCE)
set(WINDOWS_LLVM_MEDIUM_PASSES "bcf,sub,strcry,indbr" CACHE STRING "Extra obfuscation passes for medium-sensitivity sources" FORCE)
set(WINDOWS_LLVM_HEAVY_PASSES "fla,bcf,sub,strcry,indbr,indcall" CACHE STRING "Extra obfuscation passes for high-sensitivity sources" FORCE)

# Declares the project name. The project name can be accessed via ${ PROJECT_NAME},
# Since this is the top level CMakeLists.txt, the project name is also accessible
# with ${CMAKE_PROJECT_NAME} (both CMake variables are in-sync within the top level
# build script scope).
project("dobbyproject")

if(USE_WINDOWS_LLVM_FRONTEND)
        message(STATUS "Windows LLVM frontend enabled")
        message(STATUS "WINDOWS_LLVM_ROOT=${WINDOWS_LLVM_ROOT}")
        message(STATUS "CMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
        message(STATUS "CMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
        message(STATUS "CMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
        message(STATUS "CMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}")
        message(STATUS "ENABLE_WINDOWS_LLVM_OBFUSCATION=${ENABLE_WINDOWS_LLVM_OBFUSCATION}")
endif()

######################################################################################################
# 设置自定义输出文件名

option(DOBBY_GENERATE_SHARED "Build shared library" OFF)

option(DOBBY_DEBUG "Enable debug logging" ON)

option(NearBranch "Enable near branch trampoline" ON)

option(FullFloatingPointRegisterPack "Save and pack all floating-point registers" OFF)

option(Plugin.SymbolResolver "Enable symbol resolver" ON)

option(Plugin.ImportTableReplace "Enable import table replace " OFF)

option(Plugin.Android.BionicLinkerUtil "Enable android bionic linker util" OFF)

option(DOBBY_BUILD_EXAMPLE "Build example" OFF)

option(DOBBY_BUILD_TEST "Build test" OFF)

#set(DobbyHome Dobby)
#SET_OPTION(DOBBY_DEBUG ON)
#SET_OPTION(DOBBY_GENERATE_SHARED OFF)

#SET_OPTION(Plugin.LinkerLoadCallback OFF)

add_subdirectory(Dobby)
add_subdirectory(AndroidSurfaceImgui)


######################################################################################################


# 添加编译定义
add_definitions(-D_GNU_SOURCE)

# 检测目标架�?
if(${ANDROID_ABI} STREQUAL "arm64-v8a")
    add_definitions(-D__aarch64__)
    message("构建 ARM64 架构")
elseif(${ANDROID_ABI} STREQUAL "armeabi-v7a")
    add_definitions(-D__arm__)
    message("构建 ARM 架构")
elseif(${ANDROID_ABI} STREQUAL "x86")
    add_definitions(-D__i386__)
    message("构建 x86 架构")
elseif(${ANDROID_ABI} STREQUAL "x86_64")
    add_definitions(-D__x86_64__)
    message("构建 x86_64 架构")
endif()

# 设置C++标准
set(CMAKE_CXX_STANDARD 20)


find_library(
        dl-lib
        dl
)
######################################################################################################

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.
#
# In this top level CMakeLists.txt, ${CMAKE_PROJECT_NAME} is used to define
# the target library name; in the sub-module's CMakeLists.txt, ${PROJECT_NAME}
# is preferred for the same purpose.
#
# In order to load a library into your app from Java/Kotlin, you must call
# System.loadLibrary() and pass the name of the library defined here;
# for GameActivity/NativeActivity derived applications, the same library name must be
# used in the AndroidManifest.xml file.
add_library(${CMAKE_PROJECT_NAME} SHARED
        # ── core (跨游戏基础设施) ──
        "core/log/log.cpp"
        "core/file/file.cpp"
        "core/overlay/PublicOverlayRenderer.cpp"

        # ── memory (跨进程内存读取 + JNI) ──
        "memory/mem_reader.cpp"
        "memory/mem_reader_jni.cpp"

        # ── so_dumper (运行时 SO dump + JNI) ──
        "so_dumper/so_dumper.cpp"
        "so_dumper/so_dumper_jni.cpp"

        # ── kpatch (KernelPatch / KPM 控制 + JNI) ──
        "kpatch/kp_ctl.cpp"
        "kpatch/kpm_inject_hide_jni.cpp"

        # ── injector (注入器实现, 同时被 libinjector.so 复用) ──
        "injector/Injector.cpp"

        # ── games/unity_lol (Unity / LoL) ──
        "games/unity_lol/native-lib.cpp"
        "games/unity_lol/unitystart.cpp"
        "games/unity_lol/draw/Draw.cpp"
        "games/unity_lol/symbol/Symbol.cpp"
        "games/unity_lol/unity_api/unityapi.cpp"
        "games/unity_lol/il2cpp_dumper/il2cpp_dumper.cpp"
        "games/unity_lol/il2cpp_header/il2cppHeader.cpp"
        "games/unity_lol/lol/lolm.cpp"
        "games/unity_lol/interface/interface.cpp"

        # ── games/ue_pubgmhd (UE4 / PUBGMHD) ──
        "games/ue_pubgmhd/uestart.cpp"
        "games/ue_pubgmhd/engine/UE4Struct.cpp"
        "games/ue_pubgmhd/engine/UE4Dumper.cpp"
        "games/ue_pubgmhd/engine/UE4Header.cpp"
        "games/ue_pubgmhd/draw/UE4Draw.cpp"
        "games/ue_pubgmhd/interface/interface.cpp"
        "games/ue_pubgmhd/pubgmhd/pubgmhd.cpp"

        # ── games/ue_dfm (UE5 / DFM) ──
        "games/ue_dfm/uestart.cpp"
        "games/ue_dfm/engine/UE5DfmStruct.cpp"
        "games/ue_dfm/engine/UE5DfmDumper.cpp"
        "games/ue_dfm/engine/UE5Header.cpp"
        "games/ue_dfm/draw/DfmDraw.cpp"
        "games/ue_dfm/interface/interface.cpp"
        "games/ue_dfm/dfm/dfm.cpp"
        "games/ue_dfm/dfm/dfm_item_registry.cpp"

        # ── games/ue_nrc (UE 4.26 / 洛克王国手游 NRC) ──
        "games/ue_nrc/uestart.cpp"
        "games/ue_nrc/engine/UE4NrcStruct.cpp"
        "games/ue_nrc/engine/UE4NrcDumper.cpp"
        "games/ue_nrc/nrc/nrc.cpp"

        # ── core/anti_debug (反调试 / 反 Frida 自检线程) ──
        $<$<BOOL:${ENABLE_ANTI_DEBUG}>:core/anti_debug/anti_debug.cpp>
)

if(ENABLE_ANTI_DEBUG)
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE ENABLE_ANTI_DEBUG)
endif()

# Specifies libraries CMake should link to your target library. You
# can link libraries from various origins, such as libraries defined in this
# build script, prebuilt third-party libraries, or Android system libraries.
target_link_libraries(${CMAKE_PROJECT_NAME}
        # List libraries link to the target library
        dobby_static
        AImGui
        android
        EGL
        GLESv3
        log
        ${log-lib}
        ${dl-lib}
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        AndroidSurfaceImgui/includes
        AndroidSurfaceImgui/modules
        AndroidSurfaceImgui/third_party
        AndroidSurfaceImgui/third_party/imgui
        games/ue_dfm
        games/ue_dfm/engine
        games/ue_nrc
        games/ue_nrc/engine
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        "-include" "${CMAKE_CURRENT_SOURCE_DIR}/games/unity_lol/obfu_attrs.h"
)

if(USE_WINDOWS_LLVM_FRONTEND AND ENABLE_WINDOWS_LLVM_OBFUSCATION)
        message(STATUS "Windows LLVM obfuscation enabled")
        message(STATUS "WINDOWS_LLVM_BASE_PASSES=${WINDOWS_LLVM_BASE_PASSES}")
        message(STATUS "WINDOWS_LLVM_MEDIUM_PASSES=${WINDOWS_LLVM_MEDIUM_PASSES}")
        message(STATUS "WINDOWS_LLVM_HEAVY_PASSES=${WINDOWS_LLVM_HEAVY_PASSES}")

        # 强制包含注解头，�?Polaris pass 对所有函数生�?
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE OBFU_ENABLE_ANNOTATIONS)
        target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
                "-include" "${CMAKE_CURRENT_SOURCE_DIR}/games/unity_lol/obfu_attrs.h"
                "-mllvm"
                "-passes=${WINDOWS_LLVM_BASE_PASSES}"
        )

        set_source_files_properties(
                "games/unity_lol/draw/Draw.cpp"
                "games/unity_lol/symbol/Symbol.cpp"
                "games/unity_lol/il2cpp_dumper/il2cpp_dumper.cpp"
                "games/unity_lol/il2cpp_header/il2cppHeader.cpp"
                "games/unity_lol/unity_api/unityapi.cpp"
                "games/ue_pubgmhd/engine/UE4Struct.cpp"
                "games/ue_pubgmhd/engine/UE4Dumper.cpp"
                "games/ue_pubgmhd/engine/UE4Header.cpp"
                "games/ue_pubgmhd/draw/UE4Draw.cpp"
                "games/ue_dfm/engine/UE5DfmStruct.cpp"
                "games/ue_dfm/engine/UE5DfmDumper.cpp"
                "games/ue_dfm/engine/UE5Header.cpp"
                "games/ue_dfm/draw/DfmDraw.cpp"
                "games/ue_nrc/engine/UE4NrcStruct.cpp"
                "games/ue_nrc/engine/UE4NrcDumper.cpp"
                "core/overlay/PublicOverlayRenderer.cpp"
                "memory/mem_reader.cpp"
                "memory/mem_reader_jni.cpp"
                "so_dumper/so_dumper.cpp"
                "so_dumper/so_dumper_jni.cpp"
                PROPERTIES COMPILE_OPTIONS "-mllvm;-passes=${WINDOWS_LLVM_MEDIUM_PASSES}"
        )

        set_source_files_properties(
                "games/unity_lol/unitystart.cpp"
                "games/unity_lol/lol/lolm.cpp"
                "games/unity_lol/interface/interface.cpp"
                "games/ue_pubgmhd/uestart.cpp"
                "games/ue_pubgmhd/interface/interface.cpp"
                "games/ue_pubgmhd/pubgmhd/pubgmhd.cpp"
                "games/ue_dfm/uestart.cpp"
                "games/ue_dfm/interface/interface.cpp"
                "games/ue_dfm/dfm/dfm.cpp"
                "games/ue_dfm/dfm/dfm_item_registry.cpp"
                "games/ue_nrc/uestart.cpp"
                "games/ue_nrc/nrc/nrc.cpp"
                "kpatch/kp_ctl.cpp"
                "kpatch/kpm_inject_hide_jni.cpp"
                "injector/Injector.cpp"
                PROPERTIES COMPILE_OPTIONS "-mllvm;-passes=${WINDOWS_LLVM_HEAVY_PASSES}"
        )
endif()

######################################################################################################
# 可选: 符号可见性收紧 + release strip
# 默认 OFF (开启前请确保所有从 Java 调用的本地方法都带 JNIEXPORT,
# 且没有用 dlsym 在自身 .so 内查找非 JNI 符号).
######################################################################################################
if(ENABLE_VISIBILITY_HIDDEN)
        message(STATUS "Visibility hardening enabled (hidden + gc-sections + exclude-libs ALL)")
        target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
                -fvisibility=hidden -fvisibility-inlines-hidden
                -ffunction-sections -fdata-sections)
        target_link_options(${CMAKE_PROJECT_NAME} PRIVATE
                "-Wl,--gc-sections"
                "-Wl,--exclude-libs,ALL")
        if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
                target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-Wl,-s")
        endif()
endif()

######################################################################################################
# 独立注入器可执行文件 (需要通过 su �?root 运行)
# 伪装�?.so 文件, �?Gradle 自动打包�?APK
######################################################################################################
add_executable(injector
        "injector/injector_main.cpp"
        "injector/Injector.cpp"
        "core/log/log.cpp"
)

target_link_libraries(injector
        log
        ${dl-lib}
)

if(USE_WINDOWS_LLVM_FRONTEND AND ENABLE_WINDOWS_LLVM_OBFUSCATION)
        target_compile_definitions(injector PRIVATE OBFU_ENABLE_ANNOTATIONS)
        target_compile_options(injector PRIVATE
                "-include" "${CMAKE_CURRENT_SOURCE_DIR}/games/unity_lol/obfu_attrs.h"
                "-mllvm"
                "-passes=${WINDOWS_LLVM_BASE_PASSES}"
        )
endif()

# 重命名为 libinjector.so, Gradle 只打�?lib*.so �?APK
set_target_properties(injector PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
        OUTPUT_NAME "injector"
        SUFFIX ".so"
        PREFIX "lib"
)

######################################################################################################
# pvr_helper: 跨进程内存读取 helper, 由 root shell exec 后用 process_vm_readv
# 替代 toybox dd /proc/PID/mem (后者 skip*bs > 2GB 时报 Permission denied 的 bug).
# 同样伪装为 lib*.so 让 Gradle 自动打包到 APK 的 jniLibs 中.
######################################################################################################
add_executable(pvrhelper
        "pvr_helper/pvr_helper.c"
)

# 不连 log: helper 内部不打印任何字符串, 减少 ELF symbol/strings 痕迹
# strip + GC sections, 让 `strings libpvrhelper.so` 看不到 process_vm_readv 上下文
target_compile_options(pvrhelper PRIVATE
        -ffunction-sections -fdata-sections -Os -fvisibility=hidden)
target_link_options(pvrhelper PRIVATE
        "-Wl,--gc-sections" "-Wl,-s")

set_target_properties(pvrhelper PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
        OUTPUT_NAME "pvrhelper"
        SUFFIX ".so"
        PREFIX "lib"
)

