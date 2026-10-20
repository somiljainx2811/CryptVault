#include "window/window.h"

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cryptvault {

    namespace {

    int g_glfw_ref_count = 0;

<<<<<<< HEAD
    void OnGlfwError(int code, const char* description) {
        std::fprintf(stderr, "[glfw] error %d: %s\n", code, description);
=======
void OnGlfwError(int code, const char* description) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, description);
}

Window* WindowFromGlfw(GLFWwindow* w) {
    return static_cast<Window*>(glfwGetWindowUserPointer(w));
}

}  // namespace

Window::Window(const WindowConfig& config) : config_(config) {
    if (g_glfw_ref_count == 0) {
        glfwSetErrorCallback(OnGlfwError);
#if defined(__linux__) && defined(GLFW_PLATFORM_X11)
        // Native Wayland when the session is Wayland, X11 otherwise (GLFW
        // >= 3.4 picks automatically and falls back). Override with
        // CRYPTVAULT_PLATFORM=x11|wayland, e.g. to force XWayland if a
        // compositor misbehaves.
        const char* want = std::getenv("CRYPTVAULT_PLATFORM");
        int platform = GLFW_ANY_PLATFORM;
        if (want && std::strcmp(want, "x11") == 0) platform = GLFW_PLATFORM_X11;
        else if (want && std::strcmp(want, "wayland") == 0) platform = GLFW_PLATFORM_WAYLAND;
        glfwInitHint(GLFW_PLATFORM, platform);
#endif
        if (!glfwInit()) {
            std::fprintf(stderr,
                         "[cryptvault] couldn't start the window system. On Wayland this needs XWayland "
                         "(install xorg-x11-server-Xwayland); over SSH use the terminal commands "
                         "('cryptvault --help').\n");
        }
    }
    ++g_glfw_ref_count;
}

Window::~Window() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    --g_glfw_ref_count;
    if (g_glfw_ref_count == 0) {
        glfwTerminate();
    }
}

bool Window::Create() {
    // GpuContext creates its own device/surface via wgpu-native, so
    // GLFW must not set up a client API (GL/GLES) context on this
    // window - WGPU owns the surface entirely.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, config_.resizable ? GLFW_TRUE : GLFW_FALSE);
#if defined(__linux__) && defined(GLFW_PLATFORM_WAYLAND)
    wayland_ = (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND);
#endif
    // Wayland has no client-side window moving via glfwSetWindowPos, so the
    // custom frameless border can't be dragged there - use decorations.
    glfwWindowHint(GLFW_DECORATED, (wayland_ || config_.decorated) ? GLFW_TRUE : GLFW_FALSE);

    window_ = glfwCreateWindow(config_.width, config_.height, config_.title.c_str(),
                                nullptr, nullptr);
    if (!window_) {
        return false;
>>>>>>> 409ed34 (Drag n Drop)
    }

    Window* WindowFromGlfw(GLFWwindow* w) {
        return static_cast<Window*>(glfwGetWindowUserPointer(w));
    }

    }  // namespace

    Window::Window(const WindowConfig& config) : config_(config) {
        if (g_glfw_ref_count == 0) {
            glfwSetErrorCallback(OnGlfwError);
    #if defined(__linux__) && defined(GLFW_PLATFORM_X11)

            const char* want = std::getenv("CRYPTVAULT_PLATFORM");
            int platform = GLFW_ANY_PLATFORM;
            if (want && std::strcmp(want, "x11") == 0) platform = GLFW_PLATFORM_X11;
            else if (want && std::strcmp(want, "wayland") == 0) platform = GLFW_PLATFORM_WAYLAND;
            glfwInitHint(GLFW_PLATFORM, platform);
    #endif
            if (!glfwInit()) {
                std::fprintf(stderr,
                            "[cryptvault] couldn't start the window system. On Wayland this needs XWayland "
                            "(install xorg-x11-server-Xwayland); over SSH use the terminal commands "
                            "('cryptvault --help').\n");
            }
        }
        ++g_glfw_ref_count;
    }

    Window::~Window() {
        if (window_) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        --g_glfw_ref_count;
        if (g_glfw_ref_count == 0) {
            glfwTerminate();
        }
    }

    bool Window::Create() {

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_RESIZABLE, config_.resizable ? GLFW_TRUE : GLFW_FALSE);
    #if defined(__linux__) && defined(GLFW_PLATFORM_WAYLAND)
        wayland_ = (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND);
    #endif
        glfwWindowHint(GLFW_DECORATED, (wayland_ || config_.decorated) ? GLFW_TRUE : GLFW_FALSE);

        window_ = glfwCreateWindow(config_.width, config_.height, config_.title.c_str(),
                                    nullptr, nullptr);
        if (!window_) {
            return false;
        }

        glfwSetWindowUserPointer(window_, this);

        glfwSetFramebufferSizeCallback(window_, [](GLFWwindow* w, int width, int height) {
            Window* self = WindowFromGlfw(w);
            if (self->on_resize && width > 0 && height > 0) {
                self->on_resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
            }
        });

        glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int /*scancode*/, int action, int /*mods*/) {
            Window* self = WindowFromGlfw(w);
            if (self->on_key) {
                self->on_key(key, action);
            }
        });

        glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
            Window* self = WindowFromGlfw(w);
            if (self->on_mouse_move) {
                self->on_mouse_move(x, y);
            }
        });

        glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int /*mods*/) {
            Window* self = WindowFromGlfw(w);
            if (self->on_mouse_button) {
                self->on_mouse_button(button, action);
            }
        });

        glfwSetScrollCallback(window_, [](GLFWwindow* w, double dx, double dy) {
            Window* self = WindowFromGlfw(w);
            if (self->on_scroll) {
                self->on_scroll(dx, dy);
            }
        });

        glfwSetCharCallback(window_, [](GLFWwindow* w, unsigned int codepoint) {
            Window* self = WindowFromGlfw(w);
            if (self->on_char) {
                self->on_char(codepoint);
            }
        });

        glfwSetDropCallback(window_, [](GLFWwindow* w, int count, const char** paths) {
            Window* self = WindowFromGlfw(w);
            if (self->on_files_dropped) {
                self->on_files_dropped(count, paths);
            }
        });

        glfwSetWindowCloseCallback(window_, [](GLFWwindow* w) {
            Window* self = WindowFromGlfw(w);
            if (self->on_close) {
                self->on_close();
            }
        });

        return true;
    }

<<<<<<< HEAD
    int Window::Run(const std::function<void()>& on_frame) {
        while (!glfwWindowShouldClose(window_)) {
            glfwPollEvents();
            if (on_frame) {
                on_frame();
            }
        }
        return 0;
=======
void Window::GetLogicalSize(uint32_t* width, uint32_t* height) const {
    int w = 0, h = 0;
    if (window_) glfwGetWindowSize(window_, &w, &h);
    *width = w > 0 ? static_cast<uint32_t>(w) : 0;
    *height = h > 0 ? static_cast<uint32_t>(h) : 0;
}

void Window::SetAlwaysOnTop(bool enabled) {
    if (window_ && !wayland_) {  // not available to Wayland clients
        glfwSetWindowAttrib(window_, GLFW_FLOATING, enabled ? GLFW_TRUE : GLFW_FALSE);
>>>>>>> 409ed34 (Drag n Drop)
    }

    void Window::GetFramebufferSize(uint32_t* width, uint32_t* height) const {
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        *width = static_cast<uint32_t>(w);
        *height = static_cast<uint32_t>(h);
    }

    void Window::SetSize(int width, int height) {
        if (window_ && width > 0 && height > 0) {
            glfwSetWindowSize(window_, width, height);
        }
    }

    void Window::GetLogicalSize(uint32_t* width, uint32_t* height) const {
        int w = 0, h = 0;
        if (window_) glfwGetWindowSize(window_, &w, &h);
        *width = w > 0 ? static_cast<uint32_t>(w) : 0;
        *height = h > 0 ? static_cast<uint32_t>(h) : 0;
    }

    void Window::SetAlwaysOnTop(bool enabled) {
        if (window_ && !wayland_) {
            glfwSetWindowAttrib(window_, GLFW_FLOATING, enabled ? GLFW_TRUE : GLFW_FALSE);
        }
    }

    void Window::RequestClose() {
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
    }

}
