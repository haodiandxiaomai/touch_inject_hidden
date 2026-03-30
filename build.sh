#!/bin/bash
# build.sh — 编译 touch_inject_hidden
#
# 用法:
#   ./build.sh kernel                    — 编译内核模块
#   ./build.sh client                    — 编译远程控制客户端
#   ./build.sh all                       — 编译全部
#   ./build.sh all KDIR=/path/to/kernel  — 指定内核源码路径编译
#
# 环境变量:
#   KDIR          — 内核源码路径
#   CROSS_COMPILE — 交叉编译前缀 (默认 aarch64-linux-gnu-)
#   ARCH          — 目标架构 (默认 arm64)
#   LLVM          — 是否使用 LLVM (默认 1)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CROSS_COMPILE="${CROSS_COMPILE:-aarch64-linux-gnu-}"
ARCH="${ARCH:-arm64}"
LLVM="${LLVM:-1}"

log_info() { echo -e "\033[32m[INFO]\033[0m $*"; }
log_warn() { echo -e "\033[33m[WARN]\033[0m $*"; }
log_err()  { echo -e "\033[31m[ERR]\033[0m  $*"; }

build_kernel() {
    log_info "编译内核模块..."

    if [ -z "$KDIR" ]; then
        log_err "KDIR 未设置，请指定内核源码路径"
        log_info "示例: ./build.sh kernel KDIR=/path/to/kernel/source"
        exit 1
    fi

    cd "$SCRIPT_DIR/kernel"

    make ARCH=$ARCH LLVM=$LLVM \
         CROSS_COMPILE=$CROSS_COMPILE \
         KDIR=$KDIR \
         modules

    log_info "内核模块编译完成: kernel/touch_inject_hidden.ko"
    ls -la touch_inject_hidden.ko
    file touch_inject_hidden.ko
}

build_client() {
    log_info "编译远程控制客户端..."

    cd "$SCRIPT_DIR"

    ${CROSS_COMPILE}gcc -static -o remote_touch remote/client.c -lm

    log_info "客户端编译完成: remote_touch"
    ls -la remote_touch
    file remote_touch
}

build_all() {
    build_kernel
    build_client
    log_info "全部编译完成"
}

case "${1:-all}" in
    kernel)
        build_kernel
        ;;
    client)
        build_client
        ;;
    all)
        build_all
        ;;
    clean)
        cd "$SCRIPT_DIR/kernel"
        make clean
        rm -f "$SCRIPT_DIR/remote_touch"
        log_info "清理完成"
        ;;
    *)
        echo "Usage: $0 {kernel|client|all|clean} [KDIR=...]"
        exit 1
        ;;
esac
