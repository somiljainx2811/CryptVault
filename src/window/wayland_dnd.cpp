// Wayland drag source - see drag_out.h. Compiled only with libwayland-client's
// development headers (CRYPTVAULT_HAVE_WAYLAND_DND).
#ifdef CRYPTVAULT_HAVE_WAYLAND_DND

#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <poll.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "window/drag_out.h"

// GLFW's native-handle accessors, declared by hand (see gpu_context.cpp) so no
// Wayland headers are pulled in through <GLFW/glfw3native.h>; weak so the app
// still links against a GLFW built without Wayland.
extern "C" {
void* glfwGetWaylandDisplay(void) __attribute__((weak));
void* glfwGetWaylandWindow(GLFWwindow*) __attribute__((weak));
}

namespace cryptvault {
namespace {

double DebugNow() {   // seconds on a monotonic clock, for ordering debug lines
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<double>(t.tv_sec % 1000) + t.tv_nsec / 1e9;
}

class WaylandDrag : public DragOut::Backend {
public:
    ~WaylandDrag() override {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lock(mu_);
            EndDragLocked();
        }
        DestroyOffer(&incoming_offer_);
        DestroyOffer(&drag_offer_);
        DestroyOffer(&selection_offer_);
        if (pointer_) wl_pointer_destroy(pointer_);
        if (device_) wl_data_device_destroy(device_);
        if (manager_) wl_data_device_manager_destroy(manager_);
        if (shm_) wl_shm_destroy(shm_);
        if (compositor_) wl_compositor_destroy(compositor_);
        if (seat_) wl_seat_destroy(seat_);
        if (registry_) wl_registry_destroy(registry_);
        if (queue_) wl_event_queue_destroy(queue_);
    }

    bool Init(GLFWwindow* window) {
        if (!glfwGetWaylandDisplay || !glfwGetWaylandWindow) return false;
        display_ = static_cast<wl_display*>(glfwGetWaylandDisplay());
        origin_ = static_cast<wl_surface*>(glfwGetWaylandWindow(window));
        if (!display_ || !origin_) return false;

        // A private event queue, so none of our events are ever delivered through
        // (or stolen from) GLFW's own dispatching of the default queue.
        queue_ = wl_display_create_queue(display_);
        wl_display* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display_));
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue_);
        registry_ = wl_display_get_registry(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
        static const wl_registry_listener registry_listener = {&OnGlobal, &OnGlobalRemove};
        wl_registry_add_listener(registry_, &registry_listener, this);
        wl_display_roundtrip_queue(display_, queue_);
        if (!seat_ || !manager_) return false;
        wl_display_roundtrip_queue(display_, queue_);   // seat capabilities arrive after binding

        device_ = wl_data_device_manager_get_data_device(manager_, seat_);
        // We never receive drops or clipboard data through this device (GLFW does that); it exists because
        // start_drag is a request on a data device. But the compositor also sends it offers - above all the
        // offer for OUR OWN drag as it passes over our own window - and destroying such an offer while the
        // drag is running makes the compositor cancel the drag. So offers are kept alive until the
        // compositor says they are done (leave / replaced selection).
        static wl_data_device_listener device_listener;
        std::memset(&device_listener, 0, sizeof device_listener);
        device_listener.data_offer = [](void* d, wl_data_device*, wl_data_offer* offer) {
            static_cast<WaylandDrag*>(d)->incoming_offer_ = offer;
        };
        device_listener.enter = [](void* d, wl_data_device*, uint32_t, wl_surface*, wl_fixed_t, wl_fixed_t, wl_data_offer* offer) {
            auto* self = static_cast<WaylandDrag*>(d);
            self->DestroyOffer(&self->drag_offer_);
            self->drag_offer_ = offer;
            if (offer == self->incoming_offer_) self->incoming_offer_ = nullptr;
        };
        device_listener.leave = [](void* d, wl_data_device*) { auto* self = static_cast<WaylandDrag*>(d); self->DestroyOffer(&self->drag_offer_); };
        device_listener.motion = [](void*, wl_data_device*, uint32_t, wl_fixed_t, wl_fixed_t) {};
        device_listener.drop = [](void*, wl_data_device*) {};
        device_listener.selection = [](void* d, wl_data_device*, wl_data_offer* offer) {
            auto* self = static_cast<WaylandDrag*>(d);
            self->DestroyOffer(&self->selection_offer_);
            self->selection_offer_ = offer;
            if (offer == self->incoming_offer_) self->incoming_offer_ = nullptr;
        };
        wl_data_device_add_listener(device_, &device_listener, this);
        wl_display_flush(display_);
        // Our private queue is dispatched on its own thread. This is not an optimisation: if a drag is
        // dropped back onto OUR window, GLFW (the target) blocks the main thread reading the data, and
        // only our data source's `send` handler can supply it - on the main thread that would deadlock.
        thread_ = std::thread([this] { DispatchLoop(); });
        return true;
    }

    void DispatchLoop() {
        const int fd = wl_display_get_fd(display_);
        while (!stop_.load()) {
            if (wl_display_dispatch_queue_pending(display_, queue_) > 0) continue;
            wl_display_flush(display_);
            if (wl_display_prepare_read_queue(display_, queue_) != 0) continue;   // events arrived meanwhile
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 40) > 0 && (p.revents & POLLIN)) wl_display_read_events(display_);
            else wl_display_cancel_read(display_);
        }
    }

    void Pump() override {
        // Events are dispatched on the helper thread; here only the timeout for old targets.
        std::lock_guard<std::mutex> lock(mu_);
        // Targets older than data-device v3 never send dnd_finished: stop waiting.
        if (source_ && performed_ && std::chrono::steady_clock::now() - performed_at_ > std::chrono::seconds(5)) {
            finished_ = true;
            EndDragLocked();
        }
    }

    bool Begin(const std::vector<std::string>& paths) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (source_) return false;
        if (!pointer_ || !button_down_ || press_serial_ == 0) return false;   // no held button to attach the drag to
        paths_ = paths;
        uri_list_ = DragOut::UriList(paths);
        plain_ = DragOut::PlainPaths(paths);

        source_ = wl_data_device_manager_create_data_source(manager_);
        wl_data_source_offer(source_, "text/uri-list");
        wl_data_source_offer(source_, "text/plain;charset=utf-8");
        wl_data_source_offer(source_, "text/plain");
        if (manager_version_ >= 3) {
            wl_data_source_set_actions(source_, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
        }
        static wl_data_source_listener source_listener;
        std::memset(&source_listener, 0, sizeof source_listener);
        source_listener.target = [](void*, wl_data_source*, const char*) {};
        source_listener.send = &OnSend;
        source_listener.cancelled = &OnCancelled;
        // dnd_drop_performed / dnd_finished / action exist from data-device version 3.
        source_listener.dnd_drop_performed = &OnDropPerformed;
        source_listener.dnd_finished = &OnFinished;
        source_listener.action = [](void*, wl_data_source*, uint32_t) {};
        wl_data_source_add_listener(source_, &source_listener, this);

        MakeIcon(paths.size());
        finished_ = performed_ = dropped_ = false;
        if (std::getenv("CRYPTVAULT_DEBUG")) {
            std::fprintf(stderr, "[cryptvault] origin surface %p, pointer-focus surface %p, button_down %d\n", static_cast<void*>(origin_),
                         static_cast<void*>(pointer_surface_), button_down_ ? 1 : 0);
            std::fprintf(stderr, "[cryptvault] %.3f wayland start_drag: serial %u, manager v%u, icon %s\n", DebugNow(), press_serial_, manager_version_,
                         icon_ ? "yes" : "no");
        }
        active_.store(true);
        wl_data_device_start_drag(device_, source_, origin_, icon_, press_serial_);
        if (icon_) wl_surface_commit(icon_);
        wl_display_flush(display_);
        return true;
    }

    bool Active() const override { return active_.load(); }

    bool TakeFinished(bool* dropped) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (!finished_ || source_) return false;
        finished_ = false;
        if (dropped) *dropped = dropped_;
        return true;
    }

    const char* Name() const override { return "wayland"; }

private:
    // --- registry / seat / pointer ---------------------------------------------------
    static void OnGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
        auto* self = static_cast<WaylandDrag*>(data);
        if (!std::strcmp(interface, "wl_seat") && !self->seat_) {
            // Version 1 on purpose: the pointer then only ever sends enter/leave/motion/button/axis,
            // a small fixed set of events we handle fully.
            self->seat_ = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
            static wl_seat_listener seat_listener;
            std::memset(&seat_listener, 0, sizeof seat_listener);
            seat_listener.capabilities = &OnSeatCapabilities;
            wl_seat_add_listener(self->seat_, &seat_listener, self);
        } else if (!std::strcmp(interface, "wl_data_device_manager")) {
            self->manager_version_ = version < 3 ? version : 3;
            self->manager_ = static_cast<wl_data_device_manager*>(
                wl_registry_bind(registry, name, &wl_data_device_manager_interface, self->manager_version_));
        } else if (!std::strcmp(interface, "wl_compositor")) {
            self->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
        } else if (!std::strcmp(interface, "wl_shm")) {
            self->shm_ = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
        }
    }
    static void OnGlobalRemove(void*, wl_registry*, uint32_t) {}

    static void OnSeatCapabilities(void* data, wl_seat* seat, uint32_t capabilities) {
        auto* self = static_cast<WaylandDrag*>(data);
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !self->pointer_) {
            self->pointer_ = wl_seat_get_pointer(seat);
            static wl_pointer_listener pointer_listener;
            std::memset(&pointer_listener, 0, sizeof pointer_listener);   // v1 pointer: only these five events can arrive
            pointer_listener.enter = [](void* d, wl_pointer*, uint32_t, wl_surface* s, wl_fixed_t, wl_fixed_t) {
                static_cast<WaylandDrag*>(d)->pointer_surface_ = s;
            };
            pointer_listener.leave = [](void* d, wl_pointer*, uint32_t, wl_surface*) {
                static_cast<WaylandDrag*>(d)->pointer_surface_ = nullptr;
            };
            pointer_listener.motion = [](void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {};
            pointer_listener.button = [](void* d, wl_pointer*, uint32_t serial, uint32_t, uint32_t, uint32_t state) {
                auto* s = static_cast<WaylandDrag*>(d);
                if (state == WL_POINTER_BUTTON_STATE_PRESSED) { s->press_serial_ = serial; s->button_down_ = true; }
                else s->button_down_ = false;
            };
            pointer_listener.axis = [](void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {};
            wl_pointer_add_listener(self->pointer_, &pointer_listener, self);
        }
    }

    // --- data source ---------------------------------------------------------------------
    static void OnSend(void* data, wl_data_source*, const char* mime, int32_t fd) {
        auto* self = static_cast<WaylandDrag*>(data);
        if (std::getenv("CRYPTVAULT_DEBUG")) std::fprintf(stderr, "[cryptvault] wayland drag: target asked for %s\n", mime);
        const std::string& payload = !std::strcmp(mime, "text/uri-list") ? self->uri_list_ : self->plain_;
        size_t off = 0;
        while (off < payload.size()) {
            ssize_t n = ::write(fd, payload.data() + off, payload.size() - off);
            if (n <= 0) break;
            off += static_cast<size_t>(n);
        }
        ::close(fd);
    }
    static void OnCancelled(void* data, wl_data_source*) {
        auto* self = static_cast<WaylandDrag*>(data);
        if (std::getenv("CRYPTVAULT_DEBUG")) std::fprintf(stderr, "[cryptvault] %.3f wayland drag cancelled\n", DebugNow());
        std::lock_guard<std::mutex> lock(self->mu_);
        self->dropped_ = false;
        self->finished_ = true;
        self->EndDragLocked();
    }
    static void OnDropPerformed(void* data, wl_data_source*) {
        auto* self = static_cast<WaylandDrag*>(data);
        if (std::getenv("CRYPTVAULT_DEBUG")) std::fprintf(stderr, "[cryptvault] wayland drop performed\n");
        std::lock_guard<std::mutex> lock(self->mu_);
        self->performed_ = true;
        self->performed_at_ = std::chrono::steady_clock::now();
    }
    static void OnFinished(void* data, wl_data_source*) {
        auto* self = static_cast<WaylandDrag*>(data);
        if (std::getenv("CRYPTVAULT_DEBUG")) std::fprintf(stderr, "[cryptvault] wayland drag finished (target took the data)\n");
        std::lock_guard<std::mutex> lock(self->mu_);
        self->dropped_ = true;
        self->finished_ = true;
        self->EndDragLocked();
    }

    void DestroyOffer(wl_data_offer** offer) {
        if (*offer) { wl_data_offer_destroy(*offer); *offer = nullptr; }
    }

    // Caller holds mu_.
    void EndDragLocked() {
        active_.store(false);
        if (source_) { wl_data_source_destroy(source_); source_ = nullptr; }
        if (icon_) { wl_surface_destroy(icon_); icon_ = nullptr; }
        if (icon_buffer_) { wl_buffer_destroy(icon_buffer_); icon_buffer_ = nullptr; }
        if (icon_map_) { munmap(icon_map_, icon_bytes_); icon_map_ = nullptr; }
        wl_display_flush(display_);
    }

    // A small drag image: a page shape (two stacked pages for several files), so the
    // cursor visibly carries something. Drawn into shared memory, ARGB8888.
    void MakeIcon(size_t count) {
        if (!compositor_ || !shm_) return;
        const int w = 40, h = 48;
        icon_bytes_ = static_cast<size_t>(w) * h * 4;
        int fd = memfd_fd_create();
        if (fd < 0) return;
        if (::ftruncate(fd, static_cast<off_t>(icon_bytes_)) != 0) { ::close(fd); return; }
        icon_map_ = mmap(nullptr, icon_bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (icon_map_ == MAP_FAILED) { icon_map_ = nullptr; ::close(fd); return; }
        uint32_t* px = static_cast<uint32_t*>(icon_map_);
        std::memset(px, 0, icon_bytes_);
        auto page = [&](int x0, int y0, uint32_t body) {
            for (int y = 0; y < 38; ++y) {
                for (int x = 0; x < 28; ++x) {
                    const bool edge = x == 0 || y == 0 || x == 27 || y == 37;
                    px[(y0 + y) * w + (x0 + x)] = edge ? 0xFF2B3A3F : body;
                }
            }
            for (int line = 0; line < 4; ++line) {   // text lines on the page
                for (int x = 5; x < 23; ++x) px[(y0 + 12 + line * 6) * w + (x0 + x)] = 0xFF8FA3A8;
            }
        };
        if (count > 1) page(10, 8, 0xFFB9C7CA);
        page(2, 2, 0xFFF1F5F6);
        wl_shm_pool* pool = wl_shm_create_pool(shm_, fd, static_cast<int32_t>(icon_bytes_));
        icon_buffer_ = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        ::close(fd);
        icon_ = wl_compositor_create_surface(compositor_);
        wl_surface_attach(icon_, icon_buffer_, 0, 0);
    }

    static int memfd_fd_create() {
        int fd = static_cast<int>(::syscall(319 /* memfd_create (x86_64) */, "cryptvault-dnd-icon", 0));
        if (fd >= 0) return fd;
        char name[] = "/dev/shm/cryptvault-dnd-XXXXXX";   // portable fallback
        fd = ::mkstemp(name);
        if (fd >= 0) ::unlink(name);
        return fd;
    }

    wl_display* display_ = nullptr;
    wl_surface* origin_ = nullptr;
    wl_event_queue* queue_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    wl_shm* shm_ = nullptr;
    wl_data_device_manager* manager_ = nullptr;
    wl_data_device* device_ = nullptr;
    uint32_t manager_version_ = 1;

    wl_data_offer* incoming_offer_ = nullptr;   // announced, not yet entered/selected
    wl_data_offer* drag_offer_ = nullptr;
    wl_data_offer* selection_offer_ = nullptr;
    wl_surface* pointer_surface_ = nullptr;
    uint32_t press_serial_ = 0;
    bool button_down_ = false;

    std::mutex mu_;                 // guards the drag state below (touched by the main and the dispatch thread)
    std::atomic<bool> active_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
    wl_data_source* source_ = nullptr;
    wl_surface* icon_ = nullptr;
    wl_buffer* icon_buffer_ = nullptr;
    void* icon_map_ = nullptr;
    size_t icon_bytes_ = 0;
    std::vector<std::string> paths_;
    std::string uri_list_, plain_;
    bool finished_ = false, performed_ = false, dropped_ = false;
    std::chrono::steady_clock::time_point performed_at_;
};

}  // namespace

std::unique_ptr<DragOut::Backend> CreateWaylandDragBackend(GLFWwindow* window) {
    auto backend = std::make_unique<WaylandDrag>();
    if (!backend->Init(window)) return nullptr;
    return backend;
}

}  // namespace cryptvault

#endif  // CRYPTVAULT_HAVE_WAYLAND_DND
