param(
    [string]$Command  = "",
    [string]$Device   = "",
    [switch]$NoLogcat
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---- Config ----
$JAVA_HOME   = "C:\Program Files\Android\Android Studio\jbr"
$ADB         = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
$PACKAGE     = "com.example.dobbyproject"
$ACTIVITY    = "com.example.dobbyproject/.MainActivity"
$APK_DEBUG   = "app\build\outputs\apk\debug\app-debug.apk"
$APK_RELEASE = "app\build\outputs\apk\release\app-release.apk"
$LLVM_BIN    = Join-Path $PSScriptRoot "Windows-llvm\bin\clang.exe"
$LAUNCHER    = Join-Path $PSScriptRoot "tools\windows_llvm_launcher_host\WindowsLlvmLauncher.exe"
$LOG_FILTER  = "WallScan|WallCheck|Movement|MiniMap|Radar|SFK|Dobby|dobby"
$GRADLEW     = Join-Path $PSScriptRoot "gradlew.bat"

$env:JAVA_HOME = $JAVA_HOME
$env:PATH = "$JAVA_HOME\bin;$env:PATH"

$script:Step = 0
$script:TotalSteps = 1

function Write-Step([string]$msg) {
    $script:Step++
    Write-Host ("[{0}/{1}] {2}" -f $script:Step, $script:TotalSteps, $msg) -ForegroundColor Cyan
}
function Write-Ok([string]$msg)   { Write-Host "    OK  $msg" -ForegroundColor Green }
function Write-Warn([string]$msg) { Write-Host "    >>  $msg" -ForegroundColor Yellow }
function Write-Err([string]$msg)  { Write-Host "    ERR $msg" -ForegroundColor Red }
function Write-Info([string]$msg) { Write-Host "    ... $msg" -ForegroundColor DarkGray }

function Write-Banner([string]$msg) {
    $line = "=" * ($msg.Length + 4)
    Write-Host ""
    Write-Host $line -ForegroundColor DarkCyan
    Write-Host ("  " + $msg) -ForegroundColor White
    Write-Host $line -ForegroundColor DarkCyan
    Write-Host ""
}

function Get-AdbArgs {
    if ($Device) { return [string[]]@("-s", $Device) }
    return [string[]]@()
}

function Invoke-Adb([string[]]$adbArgs) {
    $all = (Get-AdbArgs) + $adbArgs
    & $ADB @all | Out-Host
    return $LASTEXITCODE
}

function Assert-Prerequisites {
    if (-not (Test-Path $ADB)) {
        Write-Err "ADB not found: $ADB"
        Write-Warn "Install Android SDK Platform-Tools first."
        exit 1
    }
    $oldEAP = $ErrorActionPreference; $ErrorActionPreference = "SilentlyContinue"
    $devices = & $ADB devices 2>$null | Select-Object -Skip 1 | Where-Object { $_ -match "\bdevice\b" }
    $ErrorActionPreference = $oldEAP
    if (-not $devices) {
        Write-Err "No authorized Android device detected."
        Write-Warn "Connect device and accept USB debug prompt on phone."
        exit 1
    }
    if ($Device -and -not ($devices -match $Device)) {
        Write-Err "Device $Device not found."
        Write-Warn "Connected: $devices"
        exit 1
    }
    if (-not (Test-Path $LLVM_BIN)) {
        Write-Err "LLVM clang.exe not found: $LLVM_BIN"
        exit 1
    }
    if (-not (Test-Path $LAUNCHER)) {
        Write-Err "Launcher not found: $LAUNCHER"
        exit 1
    }
    Write-Ok "ADB ready, device connected, LLVM verified."
}

function Invoke-Build([string]$variant = "Debug") {
    $task = if ($variant -eq "Release") { ":app:assembleRelease" } else { ":app:assembleDebug" }
    Write-Info "Gradle task: $task  (LLVM from gradle.properties)"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    Push-Location $PSScriptRoot
    try {
        & $GRADLEW $task
        if ($LASTEXITCODE -ne 0) { Write-Err "Gradle build failed (exit $LASTEXITCODE)"; exit $LASTEXITCODE }
    } finally { Pop-Location }
    $sw.Stop()
    $elapsed = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    $apk = if ($variant -eq "Release") { $APK_RELEASE } else { $APK_DEBUG }
    $size = if (Test-Path $apk) { "{0:N1} MB" -f ((Get-Item $apk).Length / 1MB) } else { "?" }
    Write-Ok "Build done  ${elapsed}s  APK: $size"
    Write-Info "APK: $apk"
}

function Invoke-BuildNative {
    Write-Info "Gradle task: :app:externalNativeBuildDebug"
    Push-Location $PSScriptRoot
    try {
        & $GRADLEW ":app:externalNativeBuildDebug"
        if ($LASTEXITCODE -ne 0) { Write-Err "Native build failed (exit $LASTEXITCODE)"; exit $LASTEXITCODE }
    } finally { Pop-Location }
    Write-Ok "Native build done."
}

function Invoke-Install([string]$apk = $APK_DEBUG) {
    if (-not (Test-Path $apk)) { Write-Err "APK not found: $apk  (build first)"; exit 1 }
    Write-Info "Installing: $apk"
    $rc = Invoke-Adb @("install", "-r", $apk)
    if ($rc -ne 0) {
        Write-Warn "Signature mismatch - uninstalling and retrying..."
        Invoke-Adb @("uninstall", $PACKAGE) | Out-Null
        $rc = Invoke-Adb @("install", $apk)
        if ($rc -ne 0) { Write-Err "Install failed (exit $rc)"; exit 1 }
    }
    Write-Ok "Installed successfully."
}

function Invoke-Launch {
    Write-Info "Starting: $ACTIVITY"
    $rc = Invoke-Adb @("shell", "am", "start", "-n", $ACTIVITY)
    if ($rc -ne 0) { Write-Err "Launch failed (exit $rc)"; exit 1 }
    Write-Ok "App launched."
}

function Invoke-Logcat {
    $appPid = $null
    $waited = 0
    while ($waited -lt 8) {
        $pidStr = (& $ADB (Get-AdbArgs) shell pidof $PACKAGE 2>$null)
        if ($pidStr -and $pidStr.Trim() -ne "") { $appPid = $pidStr.Trim().Split(" ")[0]; break }
        Start-Sleep -Milliseconds 500; $waited += 0.5
    }
    & $ADB (Get-AdbArgs) logcat -c 2>$null
    if (-not $appPid) {
        Write-Warn "Process not found - showing filtered full logcat."
        Write-Info "Filter: $LOG_FILTER  (Ctrl+C to stop)"
        Write-Host ""
        & $ADB (Get-AdbArgs) logcat | Select-String -Pattern $LOG_FILTER
    } else {
        Write-Ok "Bound to PID: $appPid  ($PACKAGE)"
        Write-Info "Filter: $LOG_FILTER  (Ctrl+C to stop)"
        Write-Host ""
        & $ADB (Get-AdbArgs) logcat "--pid=$appPid" | Select-String -Pattern $LOG_FILTER
    }
}

function Invoke-Clean {
    Push-Location $PSScriptRoot
    try {
        & $GRADLEW clean
        if ($LASTEXITCODE -ne 0) { Write-Err "Clean failed"; exit 1 }
    } finally { Pop-Location }
    Write-Ok "Build outputs cleaned."
}

function Show-Help {
    Write-Host ""
    Write-Host "Usage:" -ForegroundColor White
    Write-Host "  .\build_llvm_push.bat [command] [-Device <serial>] [-NoLogcat]"
    Write-Host ""
    Write-Host "Commands:" -ForegroundColor White
    Write-Host "  (none)    Build -> Install -> Launch -> Logcat (default)"
    Write-Host "  build     Build Debug APK only"
    Write-Host "  native    Build native .so only"
    Write-Host "  install   Install existing Debug APK"
    Write-Host "  run       Launch app only"
    Write-Host "  logcat    Attach logcat (bind app PID)"
    Write-Host "  release   Build Release APK"
    Write-Host "  clean     Clean build outputs"
    Write-Host "  help      Show this help"
    Write-Host ""
    Write-Host "Options:" -ForegroundColor White
    Write-Host "  -Device   ADB device serial (multi-device)"
    Write-Host "  -NoLogcat Skip logcat after launch"
    Write-Host ""
    Write-Host "Note: LLVM controlled by gradle.properties (useWindowsLlvmFrontend=true)" -ForegroundColor DarkGray
    Write-Host ""
}

switch ($Command.ToLower()) {
    "help" { Show-Help; exit 0 }
    "clean" {
        Write-Banner "Clean Build Outputs"
        $script:TotalSteps = 1
        Write-Step "Gradle clean"
        Invoke-Clean; Write-Host ""; exit 0
    }
    "build" {
        Write-Banner "LLVM Build Debug APK"
        $script:TotalSteps = 2
        Write-Step "Prerequisites"
        Assert-Prerequisites
        Write-Step "Gradle assembleDebug (LLVM)"
        Invoke-Build "Debug"; Write-Host ""; exit 0
    }
    "native" {
        Write-Banner "LLVM Build Native .so"
        $script:TotalSteps = 2
        Write-Step "Prerequisites"
        Assert-Prerequisites
        Write-Step "Gradle externalNativeBuildDebug (LLVM)"
        Invoke-BuildNative; Write-Host ""; exit 0
    }
    "release" {
        Write-Banner "LLVM Build Release APK"
        $script:TotalSteps = 2
        Write-Step "Prerequisites"
        Assert-Prerequisites
        Write-Step "Gradle assembleRelease (LLVM)"
        Invoke-Build "Release"; Write-Host ""; exit 0
    }
    "install" {
        Write-Banner "Install APK"
        $script:TotalSteps = 2
        Write-Step "Prerequisites"
        Assert-Prerequisites
        Write-Step "ADB Install"
        Invoke-Install $APK_DEBUG; Write-Host ""; exit 0
    }
    "run" {
        Write-Banner "Launch App"
        $script:TotalSteps = 2
        Write-Step "Prerequisites"
        Assert-Prerequisites
        Write-Step "ADB am start"
        Invoke-Launch; Write-Host ""; exit 0
    }
    "logcat" {
        Write-Banner "Realtime Logcat"
        $script:TotalSteps = 2
        Write-Step "Prerequisites"
        Assert-Prerequisites
        Write-Step "ADB logcat (bind PID)"
        Invoke-Logcat; exit 0
    }
    default {
        Write-Banner "Dobby Project - LLVM Build + Push + Run"
        $script:TotalSteps = if ($NoLogcat) { 4 } else { 5 }
        Write-Step "Prerequisites (ADB / device / LLVM)"
        Assert-Prerequisites
        Write-Step "LLVM Build Debug APK"
        Invoke-Build "Debug"
        Write-Step "Install APK"
        Invoke-Install $APK_DEBUG
        Write-Step "Launch App"
        Invoke-Launch
        if (-not $NoLogcat) {
            Write-Step "Realtime Logcat (Ctrl+C to stop)"
            Invoke-Logcat
        }
        Write-Host ""
        Write-Host "=== All done ===" -ForegroundColor Green
        Write-Host ""
        exit 0
    }
}