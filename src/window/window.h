// window.h - a thin, cross-platform wrapper around a single GLFW
// window (Windows + Linux; also builds on macOS since GLFW supports
// it, though this boilerplate only targets the first two).
//
// Scope on purpose: window creation, the event loop, and a handful of
// callbacks. No rendering, no layout, no widget logic here - that's
// GpuContext (gpu/gpu_context.h) and UiRenderer (ui/ui_renderer.h).
// Keeping this file small means the GPU surface and the immediate-
// mode UI can each be reasoned about independently.

#ifndef APPSHELL_WINDOW_WINDOW_H
#define APPSHELL_WINDOW_WINDOW_H

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace appshell {

struct WindowConfig {
    std::string title = "App";
    int width = 1100;
    int height = 700;
    bool resizable = true;
    // Frameless + manual drag/resize is a common "modern UI" look but
    // is a lot more platform-specific plumbing than a boilerplate
    // should hardcode - left as a false default. See README.md for
    // notes on adding it.
    bool decorated = true;
};

// Wraps exactly one GLFWwindow*. Not copyable - a window has a single
// identity. GLFW itself is initialized/terminated once, globally, the
// first/last time a Window is created/destroyed (see the .cpp) - safe
// for the common case of exactly one Window in the process; a
// multi-window app should extend this with explicit refcounting if
// needed.
class Window {
public:
    explicit Window(const WindowConfig& config);
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Creates the actual platform window. Returns false on failure
    // (GLFW error details are already printed to stderr via GLFW's
    // own error callback, installed once in the .cpp).
    bool Create();

    // Runs the event loop, calling on_frame once per iteration, until
    // the window is closed. Blocks until then. Every iteration polls
    // OS events (so callbacks below fire promptly) then invokes
    // on_frame - present-mode Fifo in GpuContext is what actually
    // paces this to the display's refresh rate, not a manual sleep
    // here.
    int Run(const std::function<void()>& on_frame);

    GLFWwindow* handle() const { return window_; }

    // Current framebuffer size in pixels (may differ from the size
    // passed to WindowConfig on HiDPI displays) - use this, not
    // WindowConfig::width/height, for anything GPU/pixel-related.
    void GetFramebufferSize(uint32_t* width, uint32_t* height) const;

    // Programmatically resizes the window (used by the top-left
    // resize-toggle button - see app_ui.cpp's DrawResizeToggle). GLFW
    // honors this even when WindowConfig::resizable is false; that
    // hint only blocks OS/user-driven edge-dragging, not calls like
    // this one. Triggers the same on_resize callback below as any
    // other resize, so callers don't need to separately propagate the
    // new size to the GPU surface.
    void SetSize(int width, int height);

    // Toggles the OS-level "always on top" / floating window
    // attribute at runtime (GLFW's GLFW_FLOATING) - see the border
    // right-click menu in app_ui.cpp for where this gets triggered
    // from the UI side.
    void SetAlwaysOnTop(bool enabled);

    void RequestClose();

    // --- Callbacks, set any time before Run() ---------------------

    // Client-area size in pixels.
    std::function<void(uint32_t, uint32_t)> on_resize;

    // GLFW key code (GLFW_KEY_*) and action (GLFW_PRESS/RELEASE/REPEAT).
    std::function<void(int key, int action)> on_key;

    // Client-area pixel coordinates, top-left origin.
    std::function<void(double x, double y)> on_mouse_move;

    // GLFW_MOUSE_BUTTON_* and GLFW_PRESS/GLFW_RELEASE.
    std::function<void(int button, int action)> on_mouse_button;

    // Vertical/horizontal scroll delta.
    std::function<void(double dx, double dy)> on_scroll;

    // A printable character was typed (GLFW's char callback - already
    // layout-aware and repeat-filtered, unlike on_key). Codepoint is
    // Unicode; callers that only render ASCII (like this boilerplate's
    // 5x7 bitmap font) should drop anything outside that range rather
    // than assume it fits.
    std::function<void(unsigned int codepoint)> on_char;

    // Files dropped onto the window from the OS (Explorer/Finder/file
    // manager) - see app_ui.cpp's DrawVaultFolderScreen for what
    // happens to them (each gets read, encrypted, and added to the
    // vault). Absolute filesystem paths, valid only for the duration
    // of this callback - copy them if held past it (main.cpp does,
    // into InputState::dropped_paths).
    std::function<void(int count, const char** paths)> on_files_dropped;

    std::function<void()> on_close;

private:
    static void InstallGlfwCallbacksOnce();

    WindowConfig config_;
    GLFWwindow* window_ = nullptr;
};

}  // namespace appshell

#endif  // APPSHELL_WINDOW_WINDOW_H
