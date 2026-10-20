// drag_out: start a real OS-level drag-and-drop of FILES out of the window, so
// you can drag from CryptVault onto Dolphin, GNOME Files, a terminal, a mail
// composer... GLFW can receive drops but has no way to start a drag, so this
// talks to the display server itself:
//   Wayland: wl_data_device.start_drag, on the same connection as GLFW but on
//            a private event queue (our own seat/pointer objects supply the
//            button serial the protocol requires).
//   X11:     the XDND source protocol, on a private X connection (libX11 is
//            loaded at run time, so nothing links against it).
// Each backend is compiled only when its development headers were present at
// build time; otherwise Begin() just returns false.
//
// Files are offered as `text/uri-list` (file:// URIs) and plain text paths,
// with copy and move actions: the TARGET copies or moves them, exactly as when
// dragging out of any file manager.
#pragma once

#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace cryptvault {

class DragOut {
public:
    DragOut();
    ~DragOut();
    DragOut(const DragOut&) = delete;
    DragOut& operator=(const DragOut&) = delete;

    // Picks the backend for the platform `window` runs on. False if neither is
    // available (compiled out, or the compositor/server lacks what it needs).
    bool Init(GLFWwindow* window, bool wayland);

    // Call once per frame: dispatches this backend's events and drives the drag.
    void Pump();

    // Starts dragging `paths` (absolute). Must be called while the mouse button
    // that began the gesture is still held. False if a drag can't start.
    bool Begin(const std::vector<std::string>& paths);

    bool Active() const;

    // True once after a drag ends; `dropped` says whether a target took it.
    bool TakeFinished(bool* dropped);

    // Short description of what is supported ("wayland", "x11", "none"), for diagnostics.
    const char* BackendName() const;

    // The wire formats, public so they can be unit-tested.
    static std::string UriList(const std::vector<std::string>& paths);       // "file:///a\r\nfile:///b\r\n"
    static std::string PlainPaths(const std::vector<std::string>& paths);    // "/a\n/b\n"

    struct Backend {
        virtual ~Backend() = default;
        virtual void Pump() = 0;
        virtual bool Begin(const std::vector<std::string>& paths) = 0;
        virtual bool Active() const = 0;
        virtual bool TakeFinished(bool* dropped) = 0;
        virtual const char* Name() const = 0;
    };

private:
    std::unique_ptr<Backend> backend_;
};

// Backend factories (defined in wayland_dnd.cpp / x11_dnd.cpp when compiled in).
std::unique_ptr<DragOut::Backend> CreateWaylandDragBackend(GLFWwindow* window);
std::unique_ptr<DragOut::Backend> CreateX11DragBackend(GLFWwindow* window);

}  // namespace cryptvault
