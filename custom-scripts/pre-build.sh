#!/bin/sh
set -e

PROJECT_DIR="$BASE_DIR/.."
TARGET_DIR="$BASE_DIR/target"
TOOLCHAIN_DIR="$PROJECT_DIR/output/host/bin"
MODULE_DIR="$PROJECT_DIR/modules/clook"
APP_SRC="$PROJECT_DIR/apps/clook-teste.c"
APP_DST="$TARGET_DIR/usr/bin/clook-teste"

install_executable_if_exists() {
    src="$1"
    dst="$2"

    if [ -f "$src" ]; then
        mkdir -p "$(dirname "$dst")"
        cp "$src" "$dst"
        chmod +x "$dst"
    else
        echo "pre-build: aviso: arquivo nao encontrado, ignorando: $src"
    fi
}

install_executable_if_exists "$PROJECT_DIR/custom-scripts/S41network-config" "$TARGET_DIR/etc/init.d/S41network-config"
install_executable_if_exists "$PROJECT_DIR/apps/hello" "$TARGET_DIR/usr/bin/hello"
install_executable_if_exists "$PROJECT_DIR/custom-scripts/hello" "$TARGET_DIR/etc/init.d/S50hello"
install_executable_if_exists "$PROJECT_DIR/custom-scripts/start-httpd" "$TARGET_DIR/etc/init.d/S60httpd"

make -C "$MODULE_DIR"

CC="$TOOLCHAIN_DIR/i686-buildroot-linux-gnu-gcc"
if [ ! -x "$CC" ]; then
    CC="$TOOLCHAIN_DIR/i686-linux-gcc"
fi

if [ ! -x "$CC" ]; then
    echo "pre-build: erro: compilador i686 do Buildroot nao encontrado em $TOOLCHAIN_DIR" >&2
    exit 1
fi

mkdir -p "$(dirname "$APP_DST")"
"$CC" -D_GNU_SOURCE -std=c11 -Wall -Wextra -O2 "$APP_SRC" -o "$APP_DST"
chmod +x "$APP_DST"

echo "pre-build: modulo clook compilado e clook-teste instalado em /usr/bin/clook-teste"
