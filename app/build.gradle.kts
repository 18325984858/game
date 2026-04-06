import java.io.File

plugins {
    alias(libs.plugins.android.application)
}

val windowsLlvmRoot = providers.gradleProperty("windowsLlvmRoot")
    .orElse(providers.environmentVariable("WINDOWS_LLVM_ROOT"))
    .orNull
    ?.takeIf { it.isNotBlank() }

val useWindowsLlvmFrontend = providers.gradleProperty("useWindowsLlvmFrontend")
    .orElse(providers.environmentVariable("USE_WINDOWS_LLVM_FRONTEND"))
    .map { it.equals("true", ignoreCase = true) || it == "1" }
    .orElse(false)
    .get()

val enableWindowsLlvmObfuscation = providers.gradleProperty("enableWindowsLlvmObfuscation")
    .orElse(providers.environmentVariable("ENABLE_WINDOWS_LLVM_OBFUSCATION"))
    .map { it.equals("true", ignoreCase = true) || it == "1" }
    .orElse(useWindowsLlvmFrontend)
    .get()

val windowsLlvmClang = windowsLlvmRoot?.let { File(it, "bin/clang.exe") }
val windowsLlvmClangxx = windowsLlvmRoot?.let { File(it, "bin/clang++.exe") }
val windowsLlvmLauncher = rootProject.file("tools/windows_llvm_launcher.cmd")
val enableWindowsLlvmFrontend = useWindowsLlvmFrontend
    && windowsLlvmClang?.isFile == true
    && windowsLlvmClangxx?.isFile == true
    && windowsLlvmLauncher.isFile

if (useWindowsLlvmFrontend && !enableWindowsLlvmFrontend) {
    logger.warn("Windows LLVM frontend requested, but clang.exe/clang++.exe were not found under: ${windowsLlvmRoot ?: "<unset>"}")
}

android {
    namespace = "com.example.dobbyproject"
    // 注意：建议确认 SDK 36 是否已正式发布，通常当前稳定版为 34 或 35
    compileSdk = 36

    defaultConfig {
        applicationId = "com.example.dobbyproject"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = "1.0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        // 支持 ARM + x86 模拟器架构
        externalNativeBuild {
            cmake {
                abiFilters.addAll(listOf("arm64-v8a"))//, "armeabi-v7a", "x86", "x86_64"))
                if (enableWindowsLlvmFrontend) {
                    val cmakeLlvmRoot = windowsLlvmRoot!!.replace('\\', '/')
                    arguments += listOf(
                        "-DUSE_WINDOWS_LLVM_FRONTEND=ON",
                        "-DENABLE_WINDOWS_LLVM_OBFUSCATION=${if (enableWindowsLlvmObfuscation) "ON" else "OFF"}",
                        "-DWINDOWS_LLVM_ROOT=$cmakeLlvmRoot",
                        "-DCMAKE_C_COMPILER_LAUNCHER=${windowsLlvmLauncher.absolutePath.replace('\\', '/')}",
                        "-DCMAKE_CXX_COMPILER_LAUNCHER=${windowsLlvmLauncher.absolutePath.replace('\\', '/')}"
                    )
                }
            }
        }
    }

    // --- 添加以下这段配置 ---
    packaging {
        jniLibs {
            // 强制将 SO 文件解压缩到 lib 目录，而不是留在 APK 中
            useLegacyPackaging = true
        }
    }
    // -----------------------

    buildTypes {
        release {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_11
        targetCompatibility = JavaVersion.VERSION_11
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        viewBinding = true
    }
}

dependencies {
    implementation(libs.appcompat)
    implementation(libs.material)
    implementation(libs.constraintlayout)
    testImplementation(libs.junit)
    androidTestImplementation(libs.ext.junit)
    androidTestImplementation(libs.espresso.core)
}