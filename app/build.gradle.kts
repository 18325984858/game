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

val enableAntiDebug = providers.gradleProperty("enableAntiDebug")
    .orElse(providers.environmentVariable("ENABLE_ANTI_DEBUG"))
    .map { it.equals("true", ignoreCase = true) || it == "1" }
    .orElse(true)
    .get()

val enableVisibilityHidden = providers.gradleProperty("enableVisibilityHidden")
    .orElse(providers.environmentVariable("ENABLE_VISIBILITY_HIDDEN"))
    .map { it.equals("true", ignoreCase = true) || it == "1" }
    .orElse(false)
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
                arguments += listOf(
                    "-DENABLE_ANTI_DEBUG=${if (enableAntiDebug) "ON" else "OFF"}",
                    "-DENABLE_VISIBILITY_HIDDEN=${if (enableVisibilityHidden) "ON" else "OFF"}"
                )
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

// ─────────────────────────────────────────────────────────────────
//  KPM 自动同步：assembleDebug 之前先 make 重编 KPM 并拷到 assets
//
//  目标产物:
//    app/src/main/assets/game-kpm.kpm  (反作弊)
//    app/src/main/assets/svc.kpm       (inject-hide 通用隐藏)
//
//  跳过条件:
//   * 源目录不存在 → 仅警告
//   * skipKpmBuild=true 通过 -PskipKpmBuild=true 命令行禁用
//
//  用法:
//   ./gradlew assembleDebug                              # 自动重编 + 同步两个 KPM
//   ./gradlew installDebug                               # 一条命令: 重编 → 同步 → APK → 安装到设备
//   ./gradlew assembleDebug -PskipKpmBuild=true          # 跳过编译
//   ./gradlew syncAllKpm                                 # 仅同步不打 APK
// ─────────────────────────────────────────────────────────────────
val gameKpmSrcDir   = rootProject.file("../FrideHide-kpm/kpms/GameKpm")
val injectHideSrcDir = rootProject.file("../FrideHide-kpm/kpms/inject-hide")
val gameKpmAssetTarget    = file("src/main/assets/game-kpm.kpm")
val injectHideAssetTarget = file("src/main/assets/svc.kpm")

// ── GameKpm ──
tasks.register<Exec>("buildGameKpm") {
    group = "GameKpm"
    description = "make 重编 GameKpm → game-kpm.kpm"
    workingDir = gameKpmSrcDir
    commandLine = listOf("make")

    onlyIf {
        val skip = (project.findProperty("skipKpmBuild") as? String) == "true"
        if (skip) { logger.lifecycle("[GameKpm] skipKpmBuild=true → 跳过 make"); return@onlyIf false }
        if (!gameKpmSrcDir.exists()) { logger.warn("[GameKpm] 源目录不存在: $gameKpmSrcDir"); return@onlyIf false }
        if (!file("${gameKpmSrcDir}/Makefile").exists()) { logger.warn("[GameKpm] Makefile 缺失"); return@onlyIf false }
        true
    }
}

tasks.register<Copy>("syncGameKpm") {
    group = "GameKpm"
    description = "把 GameKpm 编译产物拷到 assets/game-kpm.kpm"
    dependsOn("buildGameKpm")
    from(file("${gameKpmSrcDir}/game-kpm.kpm"))
    into(gameKpmAssetTarget.parentFile)
    rename { "game-kpm.kpm" }

    onlyIf {
        val src = file("${gameKpmSrcDir}/game-kpm.kpm")
        if (!src.exists()) { logger.warn("[GameKpm] 编译产物不存在 → 跳过同步"); return@onlyIf false }
        if (!gameKpmAssetTarget.exists() || src.lastModified() > gameKpmAssetTarget.lastModified()) {
            logger.lifecycle("[GameKpm] 同步: $src → $gameKpmAssetTarget (${src.length() / 1024} KB)"); true
        } else { logger.lifecycle("[GameKpm] assets 已是最新，跳过"); false }
    }
}

// ── inject-hide (svc.kpm) ──
tasks.register<Exec>("buildInjectHide") {
    group = "GameKpm"
    description = "make 重编 inject-hide → svc.kpm"
    workingDir = injectHideSrcDir
    commandLine = listOf("make")

    onlyIf {
        val skip = (project.findProperty("skipKpmBuild") as? String) == "true"
        if (skip) { logger.lifecycle("[InjectHide] skipKpmBuild=true → 跳过 make"); return@onlyIf false }
        if (!injectHideSrcDir.exists()) { logger.warn("[InjectHide] 源目录不存在: $injectHideSrcDir"); return@onlyIf false }
        if (!file("${injectHideSrcDir}/Makefile").exists()) { logger.warn("[InjectHide] Makefile 缺失"); return@onlyIf false }
        true
    }
}

tasks.register<Copy>("syncInjectHide") {
    group = "GameKpm"
    description = "把 inject-hide 编译产物拷到 assets/svc.kpm"
    dependsOn("buildInjectHide")
    from(file("${injectHideSrcDir}/svc.kpm"))
    into(injectHideAssetTarget.parentFile)
    rename { "svc.kpm" }

    onlyIf {
        val src = file("${injectHideSrcDir}/svc.kpm")
        if (!src.exists()) { logger.warn("[InjectHide] 编译产物不存在 → 跳过同步"); return@onlyIf false }
        if (!injectHideAssetTarget.exists() || src.lastModified() > injectHideAssetTarget.lastModified()) {
            logger.lifecycle("[InjectHide] 同步: $src → $injectHideAssetTarget (${src.length() / 1024} KB)"); true
        } else { logger.lifecycle("[InjectHide] assets 已是最新，跳过"); false }
    }
}

// 聚合任务：一次同步两个 KPM
tasks.register("syncAllKpm") {
    group = "GameKpm"
    description = "同步 GameKpm + inject-hide 两个 KPM 到 assets"
    dependsOn("syncGameKpm", "syncInjectHide")
}

afterEvaluate {
    listOf("preBuild", "mergeDebugAssets", "mergeReleaseAssets").forEach { name ->
        tasks.findByName(name)?.dependsOn("syncAllKpm")
    }
}
