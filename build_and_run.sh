#!/usr/bin/env bash
# ============================================================
#  Dobby Project - 编译 & 安装 & 运行 脚本 (Linux 版本)
# ============================================================
#
#  用法: ./build_and_run.sh [命令] [llvm] [device SERIAL]
#
#  命令:
#    (无参数)   编译 + 安装 + 运行 (默认)
#    build      仅编译 Debug APK
#    native     仅编译 Native .so
#    install    仅安装到设备
#    run        仅启动应用
#    release    编译 Release APK
#    logcat     启动 logcat 跟应用进程
#    clean      清理构建缓存
#    help       显示此帮助
#
#  选项:
#    llvm           启用 LLVM 混淆编译 (Linux 上需自行准备 LLVM 工具链)
#    device SERIAL  指定 ADB 设备序列号
#    -s SERIAL      同上
#
#  示例:
#    ./build_and_run.sh
#    ./build_and_run.sh build
#    ./build_and_run.sh install device 37171FDJH001TH
#    ./build_and_run.sh logcat
# ============================================================

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# --- 基础变量 ---
PACKAGE="com.example.dobbyproject"
ACTIVITY="${PACKAGE}/.MainActivity"
APK_PATH="app/build/outputs/apk/debug/app-debug.apk"
RELEASE_APK_DIR="app/build/outputs/apk/release"

# JAVA_HOME: 优先使用环境变量, 否则尝试常见位置
if [[ -z "${JAVA_HOME:-}" ]]; then
    for candidate in \
        "/opt/android-studio/jbr" \
        "/usr/lib/jvm/java-17-openjdk-amd64" \
        "/usr/lib/jvm/default-java" \
        "$HOME/android-studio/jbr"; do
        if [[ -d "$candidate" ]]; then
            export JAVA_HOME="$candidate"
            break
        fi
    done
fi
if [[ -n "${JAVA_HOME:-}" ]]; then
    export PATH="$JAVA_HOME/bin:$PATH"
fi

# ADB: 优先 PATH 中的 adb, 否则尝试 Android SDK 默认位置
ADB="${ADB:-}"
if [[ -z "$ADB" ]]; then
    if command -v adb >/dev/null 2>&1; then
        ADB="$(command -v adb)"
    else
        for candidate in \
            "${ANDROID_HOME:-}/platform-tools/adb" \
            "${ANDROID_SDK_ROOT:-}/platform-tools/adb" \
            "$HOME/Android/Sdk/platform-tools/adb"; do
            if [[ -x "$candidate" ]]; then
                ADB="$candidate"
                break
            fi
        done
    fi
fi

GRADLEW="$SCRIPT_DIR/gradlew"

ADB_SERIAL=""
ADB_DEVICE=()
USE_LLVM=0
LLVM_ARGS=()
BUILD_LABEL="Normal"
LLVM_ROOT="${LLVM_ROOT:-$SCRIPT_DIR/Linux-llvm}"

# --- 颜色输出 ---
if [[ -t 1 ]]; then
    C_INFO=$'\033[36m'; C_OK=$'\033[32m'; C_ERR=$'\033[31m'; C_WARN=$'\033[33m'; C_END=$'\033[0m'
else
    C_INFO=""; C_OK=""; C_ERR=""; C_WARN=""; C_END=""
fi
info() { echo -e "${C_INFO}[信息]${C_END} $*"; }
ok()   { echo -e "${C_OK}[完成]${C_END} $*"; }
err()  { echo -e "${C_ERR}[错误]${C_END} $*" >&2; }
warn() { echo -e "${C_WARN}[提示]${C_END} $*"; }

show_help() {
    sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

# --- 解析参数 ---
CMD=""
expect_serial=0
for arg in "$@"; do
    if (( expect_serial )); then
        ADB_SERIAL="$arg"
        expect_serial=0
        continue
    fi
    case "$arg" in
        device|-s)        expect_serial=1 ;;
        device=*|serial=*) ADB_SERIAL="${arg#*=}" ;;
        llvm|LLVM)         USE_LLVM=1 ;;
        *)                 [[ -z "$CMD" ]] && CMD="$arg" ;;
    esac
done

if (( expect_serial )); then
    err "device/-s 参数后缺少设备序列号"
    exit 1
fi

if (( USE_LLVM )); then
    BUILD_LABEL="LLVM Obfuscated"
    if [[ ! -x "$LLVM_ROOT/bin/clang" ]]; then
        err "LLVM clang 未找到: $LLVM_ROOT/bin/clang"
        warn "请设置 LLVM_ROOT 环境变量, 或将 LLVM 工具链放置于 $LLVM_ROOT"
        exit 1
    fi
    LLVM_ARGS=(
        "-PuseWindowsLlvmFrontend=false"
        "-PlinuxLlvmRoot=$LLVM_ROOT"
        "-PenableWindowsLlvmObfuscation=true"
    )
    info "LLVM 混淆编译已启用"
    info "LLVM 路径: $LLVM_ROOT"
else
    info "普通编译模式"
fi

# --- ADB 设备解析 ---
resolve_adb_device() {
    if [[ -z "$ADB" || ! -x "$ADB" ]]; then
        err "未找到 adb (请安装 platform-tools 或设置 ANDROID_HOME)"
        return 1
    fi

    local devices=()
    while IFS=$'\t' read -r serial state; do
        [[ "$state" == "device" ]] && devices+=("$serial")
    done < <("$ADB" devices | tail -n +2 | awk 'NF>=2 {print $1"\t"$2}')

    if [[ -n "$ADB_SERIAL" ]]; then
        local found=0
        for s in "${devices[@]}"; do
            [[ "$s" == "$ADB_SERIAL" ]] && { found=1; break; }
        done
        if (( ! found )); then
            err "指定的设备未连接: $ADB_SERIAL"
            [[ ${#devices[@]} -gt 0 ]] && info "在线设备: ${devices[*]}"
            return 1
        fi
        ADB_DEVICE=(-s "$ADB_SERIAL")
        info "使用设备: $ADB_SERIAL"
        return 0
    fi

    case ${#devices[@]} in
        0) err "未检测到在线设备"; return 1 ;;
        1) ADB_SERIAL="${devices[0]}"
           ADB_DEVICE=(-s "$ADB_SERIAL")
           info "自动选择设备: $ADB_SERIAL" ;;
        *) err "检测到多个设备, 请指定序列号"
           info "在线设备: ${devices[*]}"
           warn "用法: $0 install device SERIAL"
           return 1 ;;
    esac
}

ensure_gradlew() {
    if [[ ! -x "$GRADLEW" ]]; then
        chmod +x "$GRADLEW" 2>/dev/null || true
    fi
    if [[ ! -x "$GRADLEW" ]]; then
        err "gradlew 不可执行: $GRADLEW"
        exit 1
    fi
}

run_install() {
    info "安装到设备..."
    if ! "$ADB" "${ADB_DEVICE[@]}" install -r "$APK_PATH"; then
        warn "签名不匹配, 尝试卸载后重新安装..."
        "$ADB" "${ADB_DEVICE[@]}" uninstall "$PACKAGE" >/dev/null 2>&1 || true
        if ! "$ADB" "${ADB_DEVICE[@]}" install "$APK_PATH"; then
            err "安装失败"
            exit 1
        fi
    fi
    ok "安装成功"
}

run_launch() {
    info "启动应用..."
    if ! "$ADB" "${ADB_DEVICE[@]}" shell am start -n "$ACTIVITY"; then
        err "启动失败"
        exit 1
    fi
    ok "应用已启动"
}

cmd_clean() {
    ensure_gradlew
    info "清理构建缓存..."
    "$GRADLEW" clean || { err "清理失败"; exit 1; }
    ok "清理成功"
}

cmd_build() {
    ensure_gradlew
    info "编译 Debug APK ($BUILD_LABEL)..."
    "$GRADLEW" assembleDebug "${LLVM_ARGS[@]}" || { err "编译失败"; exit 1; }
    ok "APK 位置: $APK_PATH"
}

cmd_native() {
    ensure_gradlew
    info "编译 Native .so ($BUILD_LABEL)..."
    "$GRADLEW" :app:externalNativeBuildDebug "${LLVM_ARGS[@]}" || { err "Native 编译失败"; exit 1; }
    ok "Native Debug 构建完成"
}

cmd_release() {
    ensure_gradlew
    info "编译 Release APK ($BUILD_LABEL)..."
    "$GRADLEW" assembleRelease "${LLVM_ARGS[@]}" || { err "编译失败"; exit 1; }
    ok "Release APK 位置: $RELEASE_APK_DIR"
}

cmd_install() {
    resolve_adb_device || exit 1
    run_install
}

cmd_run() {
    resolve_adb_device || exit 1
    run_launch
}

cmd_logcat() {
    resolve_adb_device || exit 1
    local pid
    pid="$("$ADB" "${ADB_DEVICE[@]}" shell pidof "$PACKAGE" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
    if [[ -z "$pid" ]]; then
        err "未找到应用进程: $PACKAGE"
        warn "请先启动应用: $0 run"
        exit 1
    fi
    info "跟 PID: $pid ($PACKAGE)"
    info "Ctrl+C 停止"
    echo "--------------------------------------------------------------"
    "$ADB" "${ADB_DEVICE[@]}" logcat -c
    "$ADB" "${ADB_DEVICE[@]}" logcat --pid="$pid"
}

cmd_default() {
    resolve_adb_device || exit 1
    ensure_gradlew
    echo "====== $BUILD_LABEL 编译 + 安装 + 运行 ======"
    echo

    info "[1/3] 编译 Debug APK ($BUILD_LABEL)..."
    "$GRADLEW" assembleDebug "${LLVM_ARGS[@]}" || { err "编译失败"; exit 1; }
    ok "[1/3] 编译成功"
    echo

    info "[2/3] 安装到设备..."
    run_install
    echo

    info "[3/3] 启动应用..."
    run_launch
    echo

    echo "====== 全部完成 ======"
}

case "${CMD,,}" in
    "")        cmd_default ;;
    build)     cmd_build ;;
    native)    cmd_native ;;
    install)   cmd_install ;;
    run)       cmd_run ;;
    release)   cmd_release ;;
    logcat)    cmd_logcat ;;
    clean)     cmd_clean ;;
    help|-h|--help) show_help ;;
    *)
        err "未知命令: $CMD"
        show_help
        exit 1
        ;;
esac
