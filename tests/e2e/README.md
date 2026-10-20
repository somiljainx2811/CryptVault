# Drag-and-drop end-to-end tests

Drag-out talks to a live display server, so it can't run in `ctest`. These scripts exercise it for real,
with no human and no physical input devices:

* `wayland_drag.sh` - headless **sway** + a virtual pointer injected with `zwlr_virtual_pointer_v1`
  (`vptr`) + a Wayland drop target (`wldrop`). Drags a tile onto CryptVault's own window (must be
  ignored) and then onto the other window (must print `DROPPED: file://...`).
* `x11_drag.sh` - **Xvfb** + `xdotool` + a GLFW drop target on X11 (XDND).

```bash
tests/e2e/build_helpers.sh                      # compiles vptr, wldrop, droptarget
BUILD_DIR=build DEMO_HOME=$HOME tests/e2e/wayland_drag.sh
BUILD_DIR=build DEMO_HOME=$HOME tests/e2e/x11_drag.sh
```

Both need a GUI build (`cmake --build build`) and software Vulkan (e.g. Mesa lavapipe: set
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`). Output includes the app's `CRYPTVAULT_DEBUG`
log; look for `DROPPED:` from the target. Pixel positions assume the default 900x600 window with the
explorer sidebar, i.e. the first tile of the home folder at (236, 82) relative to the window.
