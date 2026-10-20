// Test helper: a minimal Wayland window that accepts file drops and prints them.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
static struct wl_compositor* comp; static struct wl_shm* shm; static struct xdg_wm_base* wm;
static struct wl_seat* seat; static struct wl_data_device_manager* ddm; static struct wl_data_device* dd;
static struct wl_surface* surf; static int configured; static struct wl_display* disp;
static struct wl_data_offer* cur_offer; static int offered_uri;
static void wm_ping(void* d, struct xdg_wm_base* w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wml = {wm_ping};
static void xs_conf(void* d, struct xdg_surface* x, uint32_t s) { xdg_surface_ack_configure(x, s); configured = 1; }
static const struct xdg_surface_listener xsl = {xs_conf};
static void tl_conf(void* d, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* s) {}
static void tl_close(void* d, struct xdg_toplevel* t) { exit(0); }
static const struct xdg_toplevel_listener tll = {tl_conf, tl_close};
static void off_offer(void* d, struct wl_data_offer* o, const char* mime) { if (!strcmp(mime, "text/uri-list")) offered_uri = 1; }
static void off_src(void* d, struct wl_data_offer* o, uint32_t a) {}
static void off_act(void* d, struct wl_data_offer* o, uint32_t a) {}
static const struct wl_data_offer_listener offl = {off_offer, off_src, off_act};
static void dd_offer(void* d, struct wl_data_device* dev, struct wl_data_offer* o) { offered_uri = 0; wl_data_offer_add_listener(o, &offl, NULL); }
static void dd_enter(void* d, struct wl_data_device* dev, uint32_t serial, struct wl_surface* s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer* o) {
    cur_offer = o;
    if (!o) return;
    wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_offer_accept(o, serial, "text/uri-list");
    printf("ENTER (uri-list offered: %d)\n", offered_uri); fflush(stdout);
}
static void dd_leave(void* d, struct wl_data_device* dev) { printf("LEAVE\n"); fflush(stdout); }
static void dd_motion(void* d, struct wl_data_device* dev, uint32_t t, wl_fixed_t x, wl_fixed_t y) {}
static void dd_drop(void* d, struct wl_data_device* dev) {
    if (!cur_offer) return;
    int fds[2]; if (pipe(fds)) return;
    wl_data_offer_receive(cur_offer, "text/uri-list", fds[1]);
    close(fds[1]);
    wl_display_flush(disp);
    char buf[4096]; ssize_t n; size_t total = 0; char all[65536];
    while ((n = read(fds[0], buf, sizeof buf)) > 0 && total + n < sizeof all) { memcpy(all + total, buf, n); total += n; }
    all[total] = 0; close(fds[0]);
    for (char* line = strtok(all, "\r\n"); line; line = strtok(NULL, "\r\n")) printf("DROPPED: %s\n", line);
    fflush(stdout);
    wl_data_offer_finish(cur_offer);
}
static void dd_sel(void* d, struct wl_data_device* dev, struct wl_data_offer* o) {}
static const struct wl_data_device_listener ddl = {dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_sel};
static void reg_global(void* d, struct wl_registry* r, uint32_t name, const char* i, uint32_t v) {
    if (!strcmp(i, "wl_compositor")) comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(i, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(i, "xdg_wm_base")) { wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(wm, &wml, NULL); }
    else if (!strcmp(i, "wl_seat") && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(i, "wl_data_device_manager")) ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, v < 3 ? v : 3);
}
static void reg_rm(void* d, struct wl_registry* r, uint32_t n) {}
static const struct wl_registry_listener rl = {reg_global, reg_rm};
int main(void) {
    struct wl_display* d = wl_display_connect(NULL); if (!d) return 1; disp = d;
    struct wl_registry* r = wl_display_get_registry(d); wl_registry_add_listener(r, &rl, NULL); wl_display_roundtrip(d);
    if (!comp || !shm || !wm || !ddm || !seat) { fprintf(stderr, "missing globals\n"); return 1; }
    dd = wl_data_device_manager_get_data_device(ddm, seat);
    // the drop handler needs the display to flush mid-callback
    wl_data_device_add_listener(dd, &(struct wl_data_device_listener){dd_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_sel}, NULL);
    surf = wl_compositor_create_surface(comp);
    struct xdg_surface* xs = xdg_wm_base_get_xdg_surface(wm, surf); xdg_surface_add_listener(xs, &xsl, NULL);
    struct xdg_toplevel* tl = xdg_surface_get_toplevel(xs); xdg_toplevel_add_listener(tl, &tll, NULL);
    xdg_toplevel_set_title(tl, "droptarget"); xdg_toplevel_set_app_id(tl, "droptarget");
    wl_surface_commit(surf); wl_display_roundtrip(d);
    int w = 300, h = 300, stride = w * 4, size = stride * h;
    int fd = syscall(SYS_memfd_create, "wldrop", 0); ftruncate(fd, size);
    uint32_t* px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int i = 0; i < w * h; ++i) px[i] = 0xFF2E7D32;
    struct wl_shm_pool* pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer* buf = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
    wl_surface_attach(surf, buf, 0, 0); wl_surface_commit(surf);
    printf("wldrop ready\n"); fflush(stdout);
    while (wl_display_dispatch(d) != -1) {}
    return 0;
}
