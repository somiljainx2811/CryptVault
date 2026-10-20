#!/usr/bin/env bash
# End-to-end test of drag-out on X11 (XDND): Xvfb, a GLFW drop-target window and xdotool as the pointer.
# Presses on the first tile of the home folder, drags it onto the other window; the target must print
# DROPPED: <path of the first folder>. Needs: Xvfb, xdotool. Build the helpers first: tests/e2e/build_helpers.sh
#   BUILD_DIR=build E2E_BIN=/tmp/cryptvault-e2e DEMO_HOME=/path/with/some/folders tests/e2e/x11_drag.sh
BUILD_DIR="${BUILD_DIR:-build}"; E2E_BIN="${E2E_BIN:-/tmp/cryptvault-e2e}"; DEMO_HOME="${DEMO_HOME:-$HOME}"
cleanup() { pkill -9 Xvfb; pkill -9 cryptvault; pkill -9 droptarget; }
trap cleanup EXIT
cleanup; sleep 0.5
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:99; unset WAYLAND_DISPLAY
(exec env CRYPTVAULT_PLATFORM=x11 $E2E_BIN/droptarget 1000 100 > /tmp/target_x.log 2>&1) &
sleep 1.5
(cd $BUILD_DIR && exec env CRYPTVAULT_PLATFORM=x11 CRYPTVAULT_DEBUG=1 HOME=$DEMO_HOME VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json timeout 120 ./cryptvault > /tmp/gui_x.log 2>&1) &
sleep 8
xdotool mousemove 236 82; sleep 0.4
xdotool mousemove 237 83; sleep 0.4
xdotool mousedown 1; sleep 0.3
for i in 1 2 3 4 5 6; do xdotool mousemove $((236+i*5)) $((82+i*2)); sleep 0.1; done
sleep 0.5
for i in 1 2 3 4 5 6 7 8 9 10; do xdotool mousemove $((236+(1100-236)*i/10)) $((82+(200-82)*i/10)); sleep 0.15; done
sleep 0.8
xdotool mouseup 1; sleep 2
echo "--- target saw:"; cat /tmp/target_x.log
echo "--- app log:"; grep -h "cryptvault\]" /tmp/gui_x.log
