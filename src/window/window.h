#ifndef CRYPTVAULT_WINDOW_WINDOW_H
#define CRYPTVAULT_WINDOW_WINDOW_H

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace cryptvault {

    struct WindowConfig {
        std::string title = "App";
        int width = 1100;
        int height = 700;
        bool resizable = true;
        bool decorated = true;
    };

    class Window {
    public:
        explicit Window(const WindowConfig& config);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        bool Create();
        int Run(const std::function<void()>& on_frame);

        GLFWwindow* handle() const { return window_; }

        void GetFramebufferSize(uint32_t* width, uint32_t* height) const;
        void GetLogicalSize(uint32_t* width, uint32_t* height) const;
        bool IsWayland() const { return wayland_; }
        void SetSize(int width, int height);
        void SetAlwaysOnTop(bool enabled);

        void RequestClose();

<<<<<<< HEAD
        std::function<void(uint32_t, uint32_t)> on_resize;
        std::function<void(int key, int action)> on_key;
        std::function<void(double x, double y)> on_mouse_move;
        std::function<void(int button, int action)> on_mouse_button;
        std::function<void(double dx, double dy)> on_scroll;
        std::function<void(unsigned int codepoint)> on_char;
        std::function<void(int count, const char** paths)> on_files_dropped;
        std::function<void()> on_close;
=======
    // Window size in logical units (what mouse coordinates and the UI
    // layout use). framebuffer / logical = the HiDPI scale factor.
    void GetLogicalSize(uint32_t* width, uint32_t* height) const;

    // True when running as a native Wayland client. On Wayland the app
    // cannot reposition its own window or force it on top (the protocol
    // doesn't allow either), so callers skip the hand-rolled drag and the
    // always-on-top toggle there, and the window uses compositor/GLFW
    // decorations instead of the custom frameless border.
    bool IsWayland() const { return wayland_; }

    // Programmatically resizes the window (used by the top-left
    // resize-toggle button - see app_ui.cpp's DrawResizeToggle). GLFW
    // honors this even when WindowConfig::resizable is false; that
    // hint only blocks OS/user-driven edge-dragging, not calls like
    // this one. Triggers the same on_resize callback below as any
    // other resize, so callers don't need to separately propagate the
    // new size to the GPU surface.
    void SetSize(int width, int height);
>>>>>>> 409ed34 (Drag n Drop)

    private:
        bool wayland_ = false;
        static void InstallGlfwCallbacksOnce();

        WindowConfig config_;
        GLFWwindow* window_ = nullptr;
    };

}

<<<<<<< HEAD
#endif
=======
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
    bool wayland_ = false;
    static void InstallGlfwCallbacksOnce();

    WindowConfig config_;
    GLFWwindow* window_ = nullptr;
};

}  // namespace cryptvault

#endif  // CRYPTVAULT_WINDOW_WINDOW_H
>>>>>>> 409ed34 (Drag n Drop)
