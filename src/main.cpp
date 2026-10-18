#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include "cli/cli.h"
#endif
#include "gpu/gpu_context.h"
#include "ui/app_ui.h"
#include "ui/ui_renderer.h"
#include "window/window.h"

namespace {
    constexpr int kSmallWindowWidth = 400;
    constexpr int kSmallWindowHeight = 300;
    constexpr int kLargeWindowWidth = 900;
    constexpr int kLargeWindowHeight = 600;
    constexpr float kBorder = 17.0f;
    constexpr cryptvault::UiColor kBorderColor{0.200f, 0.247f, 0.302f, 1.0f};

    struct InputState {
        float mouse_x = -1.0f;
        float mouse_y = -1.0f;
        bool mouse_down = false;
        bool clicked = false;
        bool double_clicked = false;
        bool right_clicked = false;
        float scroll_delta_y = 0.0f;

        std::string text_input;
        bool key_backspace = false;
        bool key_enter = false;
        bool key_up = false;
        bool key_down = false;
        bool key_ctrl = false;
        bool key_shift = false;
        bool key_delete = false;
        bool key_f2 = false;
        bool key_a = false;
        bool key_c = false;
        bool key_x = false;
        bool key_v = false;
        bool key_f = false;
        bool key_l = false;

        std::vector<std::string> dropped_paths;
    };

}

int main(int argc, char** argv) {
#ifndef _WIN32
    cvcli::GuiLaunch launch;
    int cli_rc = cvcli::Run(argc, argv, launch);
    if (cli_rc != cvcli::kLaunchGui) return cli_rc;
#else
    (void)argc;
    (void)argv;
#endif

    cryptvault::WindowConfig window_config;
    window_config.title = "CryptVault";
    window_config.width = kLargeWindowWidth;
    window_config.height = kLargeWindowHeight;
    window_config.resizable = false;
    window_config.decorated = false;

    cryptvault::Window window(window_config);
    cryptvault::GpuContext gpu;

    if (!window.Create()) {
        std::fprintf(stderr, "Failed to create window\n");
        return 1;
    }

    uint32_t fb_width = 0, fb_height = 0;
    window.GetFramebufferSize(&fb_width, &fb_height);

    if (!gpu.Create(window.handle(), fb_width, fb_height)) {
        std::fprintf(stderr, "GPU setup failed: %s\n", gpu.last_error().c_str());
        return 1;
    }

    UiApp* ui_app = UiAppCreate(gpu.device(), gpu.queue(), gpu.surface_format());
    if (!ui_app) {
        std::fprintf(stderr, "Failed to create UI app\n");
        return 1;
    }

#ifndef _WIN32
    UiAppSetStart(ui_app, launch.start_path.c_str(), launch.vault_side ? 1 : 0);
#endif

    InputState input;

    struct DragState {
        bool active = false;
        double anchor_x = 0.0;
        double anchor_y = 0.0;
    } drag;

    window.on_resize = [&gpu](uint32_t width, uint32_t height) {
        gpu.Resize(width, height);
    };

    window.on_mouse_move = [&input, &window, &drag](double x, double y) {
        input.mouse_x = static_cast<float>(x) - kBorder;
        input.mouse_y = static_cast<float>(y) - kBorder;

        if (drag.active && !window.IsWayland()) {
            int window_x = 0, window_y = 0;
            glfwGetWindowPos(window.handle(), &window_x, &window_y);
            int dx = static_cast<int>(x - drag.anchor_x);
            int dy = static_cast<int>(y - drag.anchor_y);
            glfwSetWindowPos(window.handle(), window_x + dx, window_y + dy);
        }
    };

    window.on_mouse_button = [&input, &window, &drag](int button, int action) {
        if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            if (action == GLFW_RELEASE) {
                input.right_clicked = true;
            }
            return;
        }
        if (button != GLFW_MOUSE_BUTTON_LEFT) return;

        if (action == GLFW_PRESS) {
            input.mouse_down = true;

            uint32_t win_width = 0, win_height = 0;
            window.GetLogicalSize(&win_width, &win_height);
            float content_width = static_cast<float>(win_width) - 2.0f * kBorder;
            float content_height = static_cast<float>(win_height) - 2.0f * kBorder;

            bool in_border = input.mouse_x < 0.0f
                              || input.mouse_x > content_width
                              || input.mouse_y < 0.0f
                              || input.mouse_y > content_height;
            if (in_border && !window.IsWayland()) {
                drag.active = true;
                glfwGetCursorPos(window.handle(), &drag.anchor_x, &drag.anchor_y);
            }
        } else if (action == GLFW_RELEASE) {
            drag.active = false;
            if (input.mouse_down) {
                input.clicked = true;

                double now = glfwGetTime();
                static double last_release_time = -1.0;
                static float last_release_x = -1.0f;
                static float last_release_y = -1.0f;
                float dx = input.mouse_x - last_release_x;
                float dy = input.mouse_y - last_release_y;
                bool close_enough = (dx * dx + dy * dy) < (6.0f * 6.0f);
                constexpr double kDoubleClickWindowSeconds = 0.4;

                if (last_release_time >= 0.0
                    && (now - last_release_time) < kDoubleClickWindowSeconds
                    && close_enough) {
                    input.double_clicked = true;
                    last_release_time = -1.0;
                } else {
                    last_release_time = now;
                }
                last_release_x = input.mouse_x;
                last_release_y = input.mouse_y;
            }
            input.mouse_down = false;
        }
    };

    window.on_key = [&window, &input, &ui_app](int key, int action) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            if (!UiAppHandleEscape(ui_app)) window.RequestClose();
        }
        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            const bool ctrl = glfwGetKey(window.handle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
                              || glfwGetKey(window.handle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
            if (key == GLFW_KEY_DELETE) input.key_delete = true;
            if (key == GLFW_KEY_F2 && action == GLFW_PRESS) input.key_f2 = true;
            if (ctrl && action == GLFW_PRESS) {
                if (key == GLFW_KEY_A) input.key_a = true;
                if (key == GLFW_KEY_C) input.key_c = true;
                if (key == GLFW_KEY_X) input.key_x = true;
                if (key == GLFW_KEY_V) input.key_v = true;
                if (key == GLFW_KEY_F) input.key_f = true;
                if (key == GLFW_KEY_L) input.key_l = true;
            }
        }
        if (key == GLFW_KEY_BACKSPACE && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
            input.key_backspace = true;
        }
        if (key == GLFW_KEY_ENTER && action == GLFW_PRESS) {
            input.key_enter = true;
        }
        if (key == GLFW_KEY_UP && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
            input.key_up = true;
        }
        if (key == GLFW_KEY_DOWN && (action == GLFW_PRESS || action == GLFW_REPEAT)) {
            input.key_down = true;
        }
    };

    window.on_char = [&input](unsigned int codepoint) {
        if (codepoint >= 32 && codepoint < 127) {
            input.text_input += static_cast<char>(codepoint);
        } else if (codepoint >= 0xA0 && codepoint <= 0x10FFFF && !(codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
            if (codepoint < 0x800) {
                input.text_input += static_cast<char>(0xC0 | (codepoint >> 6));
                input.text_input += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else if (codepoint < 0x10000) {
                input.text_input += static_cast<char>(0xE0 | (codepoint >> 12));
                input.text_input += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                input.text_input += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else {
                input.text_input += static_cast<char>(0xF0 | (codepoint >> 18));
                input.text_input += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                input.text_input += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                input.text_input += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
        }
    };

    window.on_scroll = [&input](double /*dx*/, double dy) {
        input.scroll_delta_y += static_cast<float>(dy);
    };
    window.on_files_dropped = [&input](int count, const char** paths) {
        for (int i = 0; i < count; ++i) {
            input.dropped_paths.emplace_back(paths[i]);
        }
    };
    double last_frame_time = glfwGetTime();
    bool has_pending_resize = false;
    uint32_t pending_resize_width = 0;
    uint32_t pending_resize_height = 0;
    bool applied_always_on_top = false;

    auto DrawFrame = [&]() {
        if (has_pending_resize) {
            has_pending_resize = false;
            window.SetSize(static_cast<int>(pending_resize_width),
                            static_cast<int>(pending_resize_height));
        }

        uint32_t fb_width = 0, fb_height = 0;
        window.GetFramebufferSize(&fb_width, &fb_height);
        uint32_t width = 0, height = 0;
        window.GetLogicalSize(&width, &height);
        if (fb_width == 0 || fb_height == 0 || width == 0 || height == 0) return;
        const float pixel_scale = static_cast<float>(fb_width) / static_cast<float>(width);
        static uint32_t logged_fb_width = 0;
        if (logged_fb_width != fb_width && std::getenv("CRYPTVAULT_DEBUG")) {
            logged_fb_width = fb_width;
            std::fprintf(stderr, "[cryptvault] window %ux%u, framebuffer %ux%u (scale %.3f), platform %s\n", width, height,
                         fb_width, fb_height, pixel_scale, window.IsWayland() ? "wayland" : "x11");
        }

        double now = glfwGetTime();
        double dt = now - last_frame_time;
        if (dt < 0.0) dt = 0.0;
        if (dt > kMaxDtSeconds) dt = kMaxDtSeconds;
        last_frame_time = now;

        UiAppInput ui_input{};
        ui_input.mouse_x = input.mouse_x;
        ui_input.mouse_y = input.mouse_y;
        ui_input.mouse_down = input.mouse_down ? 1 : 0;
        ui_input.clicked = input.clicked ? 1 : 0;
        ui_input.double_clicked = input.double_clicked ? 1 : 0;
        ui_input.right_clicked = input.right_clicked ? 1 : 0;
        ui_input.pixel_scale = pixel_scale;
        ui_input.now_seconds = now;
        ui_input.dt_seconds = static_cast<float>(dt);
        ui_input.scroll_delta_y = input.scroll_delta_y;

        std::snprintf(ui_input.text_input, sizeof(ui_input.text_input), "%s",
                       input.text_input.c_str());
        {
            size_t n = std::strlen(ui_input.text_input);
            size_t k = n;
            while (k > 0 && (static_cast<unsigned char>(ui_input.text_input[k - 1]) & 0xC0) == 0x80) --k;
            if (k > 0 && static_cast<unsigned char>(ui_input.text_input[k - 1]) >= 0xC0) {
                size_t need = static_cast<unsigned char>(ui_input.text_input[k - 1]) >= 0xF0 ? 4
                              : static_cast<unsigned char>(ui_input.text_input[k - 1]) >= 0xE0 ? 3 : 2;
                if (n - (k - 1) < need) ui_input.text_input[k - 1] = '\0';
            }
        }
        ui_input.key_backspace = input.key_backspace ? 1 : 0;
        ui_input.key_enter = input.key_enter ? 1 : 0;
        ui_input.key_up = input.key_up ? 1 : 0;
        ui_input.key_down = input.key_down ? 1 : 0;
        input.key_ctrl = glfwGetKey(window.handle(), GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS
                         || glfwGetKey(window.handle(), GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
        input.key_shift = glfwGetKey(window.handle(), GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS
                          || glfwGetKey(window.handle(), GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;
        ui_input.key_ctrl = input.key_ctrl ? 1 : 0;
        ui_input.key_shift = input.key_shift ? 1 : 0;
        ui_input.key_delete = input.key_delete ? 1 : 0;
        ui_input.key_f2 = input.key_f2 ? 1 : 0;
        ui_input.key_a = input.key_a ? 1 : 0;
        ui_input.key_c = input.key_c ? 1 : 0;
        ui_input.key_x = input.key_x ? 1 : 0;
        ui_input.key_v = input.key_v ? 1 : 0;
        ui_input.key_f = input.key_f ? 1 : 0;
        ui_input.key_l = input.key_l ? 1 : 0;

        ui_input.dropped_paths_count_actual = static_cast<uint8_t>(
            std::min<size_t>(input.dropped_paths.size(), 255));
        int copy_count = static_cast<int>(
            std::min<size_t>(input.dropped_paths.size(), UiAppInput::kMaxDroppedFiles));
        ui_input.dropped_paths_count = static_cast<uint8_t>(copy_count);
        for (int i = 0; i < copy_count; ++i) {
            std::snprintf(ui_input.dropped_paths[i], UiAppInput::kMaxDroppedPathLen, "%s",
                           input.dropped_paths[i].c_str());
        }

        UiAppBeginFrame(ui_app, width, height, &ui_input);

        uint32_t requested_width = 0, requested_height = 0;
        if (UiAppConsumeResizeRequest(ui_app, &requested_width, &requested_height)) {
            has_pending_resize = true;
            pending_resize_width = requested_width;
            pending_resize_height = requested_height;
        }

        {
            static char clip[16384];
            if (UiAppConsumeClipboardRequest(ui_app, clip, sizeof(clip))) {
                glfwSetClipboardString(window.handle(), clip);
            }
        }
        if (UiAppConsumeQuitRequest(ui_app)) {
            window.RequestClose();
        }

        bool desired_always_on_top = UiAppGetAlwaysOnTop(ui_app) != 0;
        if (desired_always_on_top != applied_always_on_top) {
            window.SetAlwaysOnTop(desired_always_on_top);
            applied_always_on_top = desired_always_on_top;
        }

        gpu.RenderFrame(kBorderColor.r, kBorderColor.g, kBorderColor.b, kBorderColor.a,
                        [ui_app](WGPURenderPassEncoder pass) {
                            UiAppEndFrame(ui_app, pass);
                        });

        input.clicked = false;
        input.double_clicked = false;
        input.right_clicked = false;
        input.scroll_delta_y = 0.0f;
        input.text_input.clear();
        input.key_backspace = false;
        input.key_enter = false;
        input.key_up = false;
        input.key_down = false;
        input.key_delete = input.key_f2 = input.key_a = input.key_c = false;
        input.key_x = input.key_v = input.key_f = input.key_l = false;
        input.dropped_paths.clear();
    };

    int result = window.Run(DrawFrame);
    UiAppDestroy(ui_app);
    return result;
}