#!/usr/bin/env bash
# End-to-end test of drag-out on WAYLAND, with no real input devices: a headless sway, a virtual
# pointer (vptr) and a drop-target client (wldrop). Presses on the first tile of the home folder,
# drags it (1) back over CryptVault's own window - must be ignored, no freeze - and (2) onto the other
# window, which must print DROPPED: file:///.../<first folder>. Needs: sway, grim, python3,
# xdotool-free. Build the helpers first: tests/e2e/build_helpers.sh
#   BUILD_DIR=build E2E_BIN=/tmp/cryptvault-e2e DEMO_HOME=/path/with/some/folders tests/e2e/wayland_drag.sh
BUILD_DIR="${BUILD_DIR:-build}"; E2E_BIN="${E2E_BIN:-/tmp/cryptvault-e2e}"; DEMO_HOME="${DEMO_HOME:-$HOME}"
cleanup() { pkill -9 sway; pkill -9 vptr; pkill -9 cryptvault; pkill -9 wldrop; }
trap cleanup EXIT
cleanup; sleep 0.5
export XDG_RUNTIME_DIR=/tmp/xdg; rm -rf $XDG_RUNTIME_DIR; mkdir -p $XDG_RUNTIME_DIR; chmod 700 $XDG_RUNTIME_DIR
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
cat > /tmp/sway.conf <<'CONF'
output HEADLESS-1 resolution 1280x800
for_window [app_id="droptarget"] floating enable, move position 1110 40
CONF
sway -d -c /tmp/sway.conf >/tmp/sway.log 2>&1 &
sleep 3
export SWAYSOCK=$(ls $XDG_RUNTIME_DIR/sway-ipc.* | head -1)
export WAYLAND_DISPLAY=$(ls $XDG_RUNTIME_DIR | grep '^wayland-[0-9]*$' | head -1)
rm -f /tmp/vptr.in; mkfifo /tmp/vptr.in
$E2E_BIN/vptr < /tmp/vptr.in > /tmp/vptr.out 2>&1 &
exec 7>/tmp/vptr.in
sleep 1
(exec env -u DISPLAY $E2E_BIN/wldrop > /tmp/target.log 2>&1) &
sleep 1.5
(cd $BUILD_DIR && exec env -u DISPLAY CRYPTVAULT_DEBUG=1 HOME=$DEMO_HOME VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json timeout 120 ./cryptvault > /tmp/gui_wl.log 2>&1) &
sleep 8
swaymsg -t get_tree > /tmp/tree.json
read CX CY TX TY < <(python3 - <<'PY'
import json
t=json.load(open('/tmp/tree.json'))
def walk(n):
    yield n
    for c in n.get('nodes',[])+n.get('floating_nodes',[]): yield from walk(c)
found={}
for n in walk(t):
    if n.get('type') in ('con','floating_con') and n.get('name'):
        r=n['rect']; w=n.get('window_rect',{})
        found[n['name']]=(r['x']+w.get('x',0), r['y']+w.get('y',0), r['width'], r['height'])
c=found.get('CryptVault',(190,100,900,600)); d=found.get('droptarget',(940,40,300,300))
print(c[0],c[1],d[0]+d[2]//2,d[1]+d[3]//2)
PY
)
TX=1190; TY=200   # inside the visible part of the target window, clear of the CryptVault window
echo "CryptVault client origin: $CX,$CY   drop point: $TX,$TY"
SX=$((CX+236)); SY=$((CY+82))     # the DOCUMENTS tile
drag_to() {   # drag from the tile to the given screen point, then release
  echo "move $SX $SY" >&7; sleep 0.5; echo "move $((SX+1)) $((SY+1))" >&7; sleep 0.4; echo "move $SX $SY" >&7; sleep 0.6
  echo "press" >&7; sleep 0.3
  for i in 1 2 3 4 5 6; do echo "move $((SX+i*4)) $((SY+i*2))" >&7; sleep 0.1; done; sleep 0.5
  for i in 1 2 3 4 5 6 7 8 9 10; do echo "move $((SX+($1-SX)*i/10)) $((SY+($2-SY)*i/10))" >&7; sleep 0.15; done
  sleep 0.8; echo "release" >&7; sleep 2
}
echo "### drag 1: released over CryptVault's own window"; drag_to 700 450
echo "### drag 2: released over the other window"; drag_to 1190 200
echo "--- target saw:"; cat /tmp/target.log
echo "--- app log:"; grep -h "start_drag\|cancelled\|finished\|performed\|asked\|Begin" /tmp/gui_wl.log
echo "--- copies created in home: $(ls /tmp/demo/home | grep -c copy)"
