#!/usr/bin/env bash
# Installs the build + runtime dependencies for CryptVault on common distros.
#   scripts/install-deps.sh            # detects the distro, runs the package manager via sudo
#   scripts/install-deps.sh --print    # just prints the command
#
# Minimal build (terminal tools only, no GPU stack): only the compiler, cmake,
# ninja and libsodium are needed - then use -DCRYPTVAULT_BUILD_GUI=OFF.
set -euo pipefail
PRINT=0; [[ "${1:-}" == "--print" ]] && PRINT=1
. /etc/os-release 2>/dev/null || true
ID_ALL="${ID:-} ${ID_LIKE:-}"

case "$ID_ALL" in
  *fedora*|*rhel*|*centos*)
    # glfw-devel on Fedora has both Wayland and X11 backends; the Vulkan
    # loader + Mesa drivers provide the GPU side; xdg-utils/zenity are the
    # "open with default app" / folder picker helpers.
    CMD=(dnf install -y gcc-c++ cmake ninja-build pkgconf-pkg-config libsodium-devel glfw-devel
<<<<<<< HEAD
         cpp-httplib-devel harfbuzz-devel vulkan-loader mesa-vulkan-drivers xdg-utils zenity) ;;
=======
         cpp-httplib-devel harfbuzz-devel wayland-devel libX11-devel vulkan-loader mesa-vulkan-drivers xdg-utils zenity) ;;
>>>>>>> 409ed34 (Drag n Drop)
  *debian*|*ubuntu*)
    # libglfw3-dev is >= 3.4 on Debian 13+/Ubuntu 24.10+; older releases fall
    # back to building GLFW from source automatically (needs the X11/Wayland
    # dev packages below).
    CMD=(apt-get install -y g++ cmake ninja-build pkg-config libsodium-dev libglfw3-dev libcpp-httplib-dev libharfbuzz-dev
         libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libwayland-dev wayland-protocols
         libxkbcommon-dev libdecor-0-dev libvulkan1 mesa-vulkan-drivers xdg-utils zenity) ;;
  *arch*)
<<<<<<< HEAD
    CMD=(pacman -S --needed --noconfirm base-devel cmake ninja libsodium glfw cpp-httplib harfbuzz
         vulkan-icd-loader vulkan-mesa-layers xdg-utils zenity) ;;
  *suse*)
    CMD=(zypper install -y gcc-c++ cmake ninja pkgconf-pkg-config libsodium-devel glfw3-devel
         cpp-httplib-devel harfbuzz-devel libvulkan1 Mesa-vulkan-device-select xdg-utils zenity) ;;
=======
    CMD=(pacman -S --needed --noconfirm base-devel cmake ninja libsodium glfw cpp-httplib harfbuzz wayland libx11
         vulkan-icd-loader vulkan-mesa-layers xdg-utils zenity) ;;
  *suse*)
    CMD=(zypper install -y gcc-c++ cmake ninja pkgconf-pkg-config libsodium-devel glfw3-devel
         cpp-httplib-devel harfbuzz-devel wayland-devel libX11-devel libvulkan1 Mesa-vulkan-device-select xdg-utils zenity) ;;
>>>>>>> 409ed34 (Drag n Drop)
  *) echo "Unknown distro ('${ID:-?}'). Install: a C++17 compiler, cmake, ninja, libsodium (dev), GLFW >= 3.4," \
          "cpp-httplib, a Vulkan driver, xdg-utils, zenity. Anything missing among the libraries is built" \
          "automatically by CMake." >&2; exit 1 ;;
esac

# Optional runtime tools (CryptVault works without them):
#   ffmpeg        video thumbnails + hover video preview
#   wl-clipboard  (Wayland) / xclip (X11)   copy/paste FILES with other apps
#   Noto fonts for your scripts (e.g. google-noto-sans-arabic-fonts, ...-devanagari-fonts, ...-cjk-fonts)
#   and google-noto-color-emoji-fonts / fonts-noto-color-emoji for colour emoji
# ffmpeg per distro:
#   Fedora: sudo dnf install ffmpeg-free   (or full ffmpeg from RPM Fusion)
#   Debian/Ubuntu: apt install ffmpeg   Arch: pacman -S ffmpeg   openSUSE: zypper install ffmpeg
if [[ $EUID -ne 0 ]]; then CMD=(sudo "${CMD[@]}"); fi
echo "+ ${CMD[*]}"
echo "(optional: ffmpeg for video previews; wl-clipboard or xclip for file copy/paste with other apps; Noto fonts for your scripts)" >&2
[[ $PRINT -eq 1 ]] || "${CMD[@]}"
