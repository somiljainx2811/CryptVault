// Tiny test tool: injects pointer events into a wlroots compositor (sway) through
// zwlr_virtual_pointer_v1. Reads commands from stdin, one per line:
//   move X Y        absolute motion (output pixels; output assumed 1280x800)
//   press | release left button
//   sleep MS
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
static struct wl_seat* seat; static struct zwlr_virtual_pointer_manager_v1* mgr;
static void reg_global(void* d, struct wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    if (!strcmp(iface, "wl_seat") && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, "zwlr_virtual_pointer_manager_v1")) mgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void reg_remove(void* d, struct wl_registry* r, uint32_t n) {}
static const struct wl_registry_listener rl = {reg_global, reg_remove};
static uint32_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000 + t.tv_nsec / 1000000; }
int main(void) {
    struct wl_display* d = wl_display_connect(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    struct wl_registry* r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &rl, NULL);
    wl_display_roundtrip(d);
    if (!mgr || !seat) { fprintf(stderr, "no virtual pointer manager (%p) or seat (%p)\n", (void*)mgr, (void*)seat); return 1; }
    struct zwlr_virtual_pointer_v1* p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    wl_display_roundtrip(d);
    printf("ready\n"); fflush(stdout);
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
        int x, y, ms;
        if (sscanf(line, "move %d %d", &x, &y) == 2) {
            zwlr_virtual_pointer_v1_motion_absolute(p, now_ms(), x, y, 1280, 800);
            zwlr_virtual_pointer_v1_frame(p);
        } else if (!strncmp(line, "press", 5)) {
            zwlr_virtual_pointer_v1_button(p, now_ms(), 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(p);
        } else if (!strncmp(line, "release", 7)) {
            zwlr_virtual_pointer_v1_button(p, now_ms(), 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(p);
        } else if (sscanf(line, "sleep %d", &ms) == 1) {
            wl_display_flush(d); usleep(ms * 1000); wl_display_roundtrip(d); continue;
        }
        wl_display_flush(d);
    }
    wl_display_roundtrip(d);
    return 0;
}
