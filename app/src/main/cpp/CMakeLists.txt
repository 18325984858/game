# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html.
# For more examples on how to use CMake, see https://github.com/android/ndk-samples.

# Sets the minimum CMake version required for this project.
cmake_minimum_required(VERSION 3.22.1)

option(USE_WINDOWS_LLVM_FRONTEND "Use custom Windows LLVM clang frontend binaries" OFF)
option(ENABLE_WINDOWS_LLVM_OBFUSCATION "Enable Polaris/OLLVM compile passes when using the Windows LLVM frontend" OFF)
set(WINDOWS_LLVM_BASE_PASSES "sub,strcry" CACHE STRING "Base obfuscation passes for all frontend-enabled native targets" FORCE)
set(WINDOWS_LLVM_MEDIUM_PASSES "bcf,sub,strcry" CACHE STRING "Extra obfuscation passes for medium-sensitivity sources" FORCE)
set(WINDOWS_LLVM_HEAVY_PASSES "bcf,sub,strcry" CACHE STRING "Extra obfuscation passes for high-sensitivity sources" FORCE)

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
        # List C/C++ source files with relative paths to this CMakeLists.txt.
        publicOverlay/PublicOverlayRenderer.cpp
        unityGame/native-lib.cpp
        unityGame/unitystart.cpp
        "unityGame/Draw/Draw.cpp"
        "Log/log.cpp"
        "unityGame/Symbol/Symbol.cpp"
        "File/file.cpp"
        "unityGame/UnityApi/unityapi.cpp"
        "unityGame/li2cppDumper/li2cppdumper.cpp"
        "unityGame/lol/lolm.cpp"
        "unityGame/interface/interface.cpp"
        "unityGame/il2cppHeader/il2cppHeader.cpp"
        "Injector/Injector.cpp"
        "ueGamepubgmhd/libUE4Struct/ilbUE4Struct.cpp"
        "ueGamepubgmhd/libUE4Dumper/UE4Dumper.cpp"
        "ueGamepubgmhd/libUE4Header/UE4Header.cpp"
        "ueGamepubgmhd/interface/interface.cpp"
        "ueGamepubgmhd/Draw/UE4Draw.cpp"
        "ueGamepubgmhd/pubgmhd/pubgmhd.cpp"
        "ueGamepubgmhd/uestart.cpp"
        "ueGamedfm/uestart.cpp"
        "ueGamedfm/ilbUE5Struct/UE5DfmStruct.cpp"
        "ueGamedfm/ilbUE5Dumper/UE5DfmDumper.cpp"
        "ueGamedfm/libUE5Header/UE5Header.cpp"
        "ueGamedfm/interface/interface.cpp"
        "ueGamedfm/dfm/dfm.cpp"
        "ueGamedfm/Draw/DfmDraw.cpp"
        "soDumper/so_dumper.cpp"
        "soDumper/so_dumper_jni.cpp"
        "ReadProcessMemory/mem_reader.cpp"
        "ReadProcessMemory/mem_reader_jni.cpp"
        "ReadProcessMemory/kp_ctl.cpp"
        "KpmInterface/kpm_inject_hide_jni.cpp"
)

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
        ueGamedfm
        ueGamedfm/ilbUE5Struct
        ueGamedfm/ilbUE5Dumper
        ueGamedfm/libUE5Header
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
        "-include" "${CMAKE_CURRENT_SOURCE_DIR}/unityGame/obfu_attrs.h"
)

if(USE_WINDOWS_LLVM_FRONTEND AND ENABLE_WINDOWS_LLVM_OBFUSCATION)
        message(STATUS "Windows LLVM obfuscation enabled")
        message(STATUS "WINDOWS_LLVM_BASE_PASSES=${WINDOWS_LLVM_BASE_PASSES}")
        message(STATUS "WINDOWS_LLVM_MEDIUM_PASSES=${WINDOWS_LLVM_MEDIUM_PASSES}")
        message(STATUS "WINDOWS_LLVM_HEAVY_PASSES=${WINDOWS_LLVM_HEAVY_PASSES}")

        # 强制包含注解头，�?Polaris pass 对所有函数生�?
        target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE OBFU_ENABLE_ANNOTATIONS)
        target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
                "-include" "${CMAKE_CURRENT_SOURCE_DIR}/unityGame/obfu_attrs.h"
                "-mllvm"
                "-passes=${WINDOWS_LLVM_BASE_PASSES}"
        )

        set_source_files_properties(
                "unityGame/Draw/Draw.cpp"
                "unityGame/Symbol/Symbol.cpp"
                "unityGame/li2cppDumper/li2cppdumper.cpp"
                "ueGamepubgmhd/libUE4Struct/ilbUE4Struct.cpp"
                "ueGamepubgmhd/libUE4Dumper/UE4Dumper.cpp"
                PROPERTIES COMPILE_OPTIONS "-mllvm;-passes=${WINDOWS_LLVM_MEDIUM_PASSES}"
        )

        set_source_files_properties(
                unityGame/unitystart.cpp
                "unityGame/lol/lolm.cpp"
                "unityGame/interface/interface.cpp"
                PROPERTIES COMPILE_OPTIONS "-mllvm;-passes=${WINDOWS_LLVM_HEAVY_PASSES}"
        )
endif()

######################################################################################################
# 独立注入器可执行文件 (需要通过 su �?root 运行)
# 伪装�?.so 文件, �?Gradle 自动打包�?APK
######################################################################################################
add_executable(injector
        "Injector/injector_main.cpp"
        "Injector/Injector.cpp"
        "Log/log.cpp"
)

target_link_libraries(injector
        log
        ${dl-lib}
)

if(USE_WINDOWS_LLVM_FRONTEND AND ENABLE_WINDOWS_LLVM_OBFUSCATION)
        target_compile_definitions(injector PRIVATE OBFU_ENABLE_ANNOTATIONS)
        target_compile_options(injector PRIVATE
                "-include" "${CMAKE_CURRENT_SOURCE_DIR}/unityGame/obfu_attrs.h"
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

