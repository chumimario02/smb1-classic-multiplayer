#!/usr/bin/env bash
# Build for Windows with MSYS2 MinGW64 (SDL2 + software renderer).
# Run from the MSYS2 MinGW64 shell, or with:
#   C:\msys64\usr\bin\bash.exe -lc 'bash /c/path/to/tools/build-windows.sh'

set -euo pipefail

cd "$(dirname "$0")/.."

export PATH="/mingw64/bin:$PATH"

meson setup build -Dsdl2=true -Dopengl=false --buildtype=release
meson compile -C build

echo
echo "Build ready: build/src/platform/smbvanilla.exe"
echo "Copy SDL2.dll and the MinGW runtime DLLs next to the exe if needed."
