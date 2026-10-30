#!/usr/bin/env bash
# Build for Windows with MSYS2 MinGW64.
#
# Usage:
#   tools/build-windows.sh          # OpenGL renderer (default, faster)
#   tools/build-windows.sh false    # software renderer (fallback)
#
# Run from the MSYS2 MinGW64 shell, or with:
#   C:\msys64\usr\bin\bash.exe -lc 'bash /c/path/to/tools/build-windows.sh'

set -euo pipefail

cd "$(dirname "$0")/.."

export PATH="/mingw64/bin:$PATH"

opengl="${1:-true}"

if [ -d build ]; then
  meson setup --reconfigure build -Dsdl2=true -Dopengl="$opengl" --buildtype=release
else
  meson setup build -Dsdl2=true -Dopengl="$opengl" --buildtype=release
fi

meson compile -C build

echo
echo "Build ready: build/src/platform/smbvanilla.exe"
echo "Copy SDL2.dll and the MinGW runtime DLLs next to the exe if needed."
