#!/bin/sh
# Cross-compile for Raspberry Pi (ARM64) from a Debian/Ubuntu PC.
#
#   ./build-pi.sh
#
# Requirements:
#   sudo dpkg --add-architecture arm64 && sudo apt update
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
#                    libsdl2-dev:arm64 libwebsockets-dev:arm64 pkg-config meson ninja-build
set -eu
cd "$(dirname "$0")"

CROSS_FILE="${CROSS_FILE:-meson-aarch64.ini}"

if [ ! -f "$CROSS_FILE" ]; then
  cat > "$CROSS_FILE" <<'EOF'
[binaries]
c = 'aarch64-linux-gnu-gcc'
cpp = 'aarch64-linux-gnu-g++'
ar = 'aarch64-linux-gnu-ar'
strip = 'aarch64-linux-gnu-strip'
pkgconfig = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF
fi

export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig
meson setup build-arm64 --cross-file "$CROSS_FILE" -Dsdl2=true --buildtype=release
meson compile -C build-arm64

echo
echo "OK: build-arm64/src/platform/smbvanilla"
echo "Recuerda: en la Pi necesitas libsdl2-2.0-0, libwebsockets19 y tu smb.nes"
