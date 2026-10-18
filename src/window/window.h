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

        std::function<void(uint32_t, uint32_t)> on_resize;
        std::function<void(int key, int action)> on_key;
        std::function<void(double x, double y)> on_mouse_move;
        std::function<void(int button, int action)> on_mouse_button;
        std::function<void(double dx, double dy)> on_scroll;
        std::function<void(unsigned int codepoint)> on_char;
        std::function<void(int count, const char** paths)> on_files_dropped;
        std::function<void()> on_close;

    private:
        bool wayland_ = false;
        static void InstallGlfwCallbacksOnce();

        WindowConfig config_;
        GLFWwindow* window_ = nullptr;
    };

}

#endif
