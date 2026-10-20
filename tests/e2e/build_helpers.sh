#!/usr/bin/env bash
# Builds the helper programs for the drag-and-drop end-to-end tests into $E2E_BIN
# (default /tmp/cryptvault-e2e). Needs: gcc/g++, wayland-scanner, libwayland-dev,
# wayland-protocols, and a configured+built CryptVault tree in $BUILD_DIR (for GLFW).
#   vptr        injects pointer events into a wlroots compositor (sway) via zwlr_virtual_pointer_v1
#   wldrop      a minimal Wayland window that accepts file drops and prints them
#   droptarget  a GLFW window (X11) that prints dropped files
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
E2E_BIN="${E2E_BIN:-/tmp/cryptvault-e2e}"
BUILD_DIR="${BUILD_DIR:-$HERE/../../build}"
mkdir -p "$E2E_BIN" && cd "$E2E_BIN"
# protocol code
curl -sSL -o wlr-virtual-pointer-unstable-v1.xml \
  https://raw.githubusercontent.com/swaywm/wlroots/master/protocol/wlr-virtual-pointer-unstable-v1.xml
wayland-scanner client-header wlr-virtual-pointer-unstable-v1.xml wlr-virtual-pointer-unstable-v1-client-protocol.h
wayland-scanner private-code  wlr-virtual-pointer-unstable-v1.xml wlr-virtual-pointer-unstable-v1-protocol.c
XDG=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
wayland-scanner client-header "$XDG" xdg-shell-client-protocol.h
wayland-scanner private-code  "$XDG" xdg-shell-protocol.c
WL="$(pkg-config --cflags --libs wayland-client)"
gcc -I. -o vptr   "$HERE/vptr.c"   wlr-virtual-pointer-unstable-v1-protocol.c $WL
gcc -I. -o wldrop "$HERE/wldrop.c" xdg-shell-protocol.c $WL
GLFW_LIB="$(find "$BUILD_DIR" -name 'libglfw3.a' | head -1)"
GLFW_INC="$(find "$BUILD_DIR" -type d -path '*glfw-src/include' | head -1)"
if [[ -n "$GLFW_LIB" && -n "$GLFW_INC" ]]; then
  g++ -std=c++17 -DGLFW_INCLUDE_NONE -I"$GLFW_INC" -o droptarget "$HERE/droptarget_x11.cpp" "$GLFW_LIB" -ldl -lpthread
else
  echo "note: GLFW built from source not found under $BUILD_DIR - skipping droptarget (X11 test)"
fi
echo "helpers in $E2E_BIN"
