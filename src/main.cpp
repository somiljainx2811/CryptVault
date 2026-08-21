// main.cpp - process shell for CryptVault.
//
// Owns the window and GPU device and drives the UI (app_ui.cpp, compiled
// directly into this binary - see UiAppCreate/UiAppBeginFrame/UiAppEndFrame
// in ui/app_ui.h) once per frame.

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "gpu/gpu_context.h"
#include "ui/app_ui.h"
#include "ui/ui_renderer.h"
#include "window/window.h"

namespace {

// Two fixed window sizes the top-left resize-toggle button (see
// app_ui.cpp's DrawResizeToggle) switches between; 400x300 is the
// startup default.
//
// NOTE: app_ui.cpp keeps its own copies of these same numbers (it's
// a separate translation unit and can't depend on this exe's
// headers) - the two must be changed together or the border
// hit-testing here and the layout there will disagree about where
// content-space starts.
constexpr int kSmallWindowWidth = 400;
constexpr int kSmallWindowHeight = 300;
constexpr int kLargeWindowWidth = 900;
constexpr int kLargeWindowHeight = 600;
constexpr float kBorder = 17.0f;
constexpr appshell::UiColor kBorderColor{0.200f, 0.247f, 0.302f, 1.0f};

struct InputState {
    float mouse_x = -1.0f;
    float mouse_y = -1.0f;
    bool mouse_down = false;
    bool clicked = false;
    bool double_clicked = false;
    // Right mouse button release, one-shot like `clicked` - opens a
    // tile's context menu (see DrawTileGrid/DrawContextMenu). Doesn't
    // participate in the border-drag or double-click logic below;
    // right-click has no OS-chrome-drag or double-click meaning here.
    bool right_clicked = false;
    // Accumulated since the last frame was sent to the UI side (see
    // on_scroll below and the reset at the bottom of DrawFrame) -
    // GLFW can deliver more than one scroll event between frames, so
    // this sums them rather than keeping only the latest.
    float scroll_delta_y = 0.0f;

    // Text typed since the last frame (see on_char below) - reset the
    // same way as the click flags. ASCII-filtered here rather than in
    // the UI side, since that's where GLFW's raw codepoints arrive.
    std::string text_input;
    bool key_backspace = false;
    bool key_enter = false;
    bool key_up = false;
    bool key_down = false;

    // Absolute filesystem paths dropped onto the window since the
    // last frame (see window.on_files_dropped below) - reset the same
    // way as the click flags, after being copied into UiAppInput's
    // fixed-size array for this frame.
    std::vector<std::string> dropped_paths;
};

}  // namespace

int main() {
    appshell::WindowConfig window_config;
    window_config.title = "CryptVault";
    window_config.width = kSmallWindowWidth;
    window_config.height = kSmallWindowHeight;
    window_config.resizable = false;
    window_config.decorated = false;

    appshell::Window window(window_config);
    appshell::GpuContext gpu;

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

        if (drag.active) {
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

            // Content bounds depend on the *current* window size,
            // which can now change at runtime (see the resize-toggle
            // button) - so this is computed fresh here rather than
            // from a fixed compile-time constant.
            uint32_t fb_width = 0, fb_height = 0;
            window.GetFramebufferSize(&fb_width, &fb_height);
            float content_width = static_cast<float>(fb_width) - 2.0f * kBorder;
            float content_height = static_cast<float>(fb_height) - 2.0f * kBorder;

            bool in_border = input.mouse_x < 0.0f
                              || input.mouse_x > content_width
                              || input.mouse_y < 0.0f
                              || input.mouse_y > content_height;
            if (in_border) {
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

    window.on_key = [&window, &input](int key, int action) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
            window.RequestClose();
        }
        // PRESS or REPEAT so holding Backspace/Up/Down behaves like
        // any normal text field; Enter only fires on the initial
        // press (holding it down repeatedly re-submitting isn't
        // useful for the command palette's "execute selected" case).
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
        // Drop anything outside printable ASCII - the 5x7 bitmap font
        // (see font5x7.h) only has glyphs for that range anyway, and
        // silently keeping wider Unicode here would just mean it gets
        // typed into the buffer and then rendered as blank glyphs.
        if (codepoint >= 32 && codepoint < 127) {
            input.text_input += static_cast<char>(codepoint);
        }
    };

    window.on_scroll = [&input](double /*dx*/, double dy) {
        input.scroll_delta_y += static_cast<float>(dy);
    };

    // GLFW's drop callback is already cross-platform (OLE on Windows,
    // XDND on X11) - no per-OS code needed on this side for "drag
    // files from the OS into CryptVault". The reverse direction
    // (dragging a file *out* of CryptVault to the OS) is a
    // fundamentally different, genuinely platform-specific thing -
    // GLFW has no API for initiating an OS-level drag - and isn't
    // implemented here.
    window.on_files_dropped = [&input](int count, const char** paths) {
        for (int i = 0; i < count; ++i) {
            input.dropped_paths.emplace_back(paths[i]);
        }
    };

    // Elapsed-time clamp for dt_seconds (see UiAppInput's comment):
    // caps any single frame's delta so a debugger pause, or the OS
    // briefly starving this process, can't hand the UI side a multi-
    // second dt that would make an in-flight animation jump straight
    // to its end state instead of animating.
    constexpr double kMaxDtSeconds = 0.1;
    double last_frame_time = glfwGetTime();

    // A resize requested by the UI side (see DrawResizeToggle) during
    // *this* frame's BeginFrame/EndFrame. Applying window.SetSize()
    // (and therefore gpu.Resize()) immediately - between this frame's
    // BeginFrame and its RenderFrame, as the code used to do - meant
    // the render pass at the end of the frame ran against a surface
    // that had just been reconfigured to a new size, while the vertex/
    // scissor data the UI side just built in BeginFrame was still
    // sized for the *old* width/height. wgpu validates scissor/viewport
    // rects against the current surface texture, so a rect computed
    // for 900x600 submitted against a freshly-reconfigured 400x300
    // surface (or vice versa) is invalid; that single invalid call
    // poisons the whole command encoder, so wgpuCommandEncoderFinish/
    // wgpuQueueSubmit both fail with "Command encoder is invalid" -
    // exactly the crash reported when toggling the window size back
    // and forth.
    //
    // The fix: never resize mid-frame. Stash the request and apply it
    // at the very top of the *next* frame, before that frame reads
    // the framebuffer size or calls BeginFrame - so every frame's
    // BeginFrame/RenderFrame pair agrees on one consistent size, and
    // the resize itself costs one frame of latency (imperceptible)
    // rather than a crash.
    bool has_pending_resize = false;
    uint32_t pending_resize_width = 0;
    uint32_t pending_resize_height = 0;

    // Tracks the "always on top" state actually applied to the real
    // GLFW window, so it's only re-applied when the UI side's desired
    // state (see UiAppGetAlwaysOnTop) actually changes, not every
    // single frame.
    bool applied_always_on_top = false;

    auto DrawFrame = [&]() {
        if (has_pending_resize) {
            has_pending_resize = false;
            // Triggers on_resize -> gpu.Resize() synchronously, so by
            // the time GetFramebufferSize() runs just below, the GPU
            // surface and the framebuffer size this frame reads are
            // already consistent with each other.
            window.SetSize(static_cast<int>(pending_resize_width),
                            static_cast<int>(pending_resize_height));
        }

        uint32_t width = 0, height = 0;
        window.GetFramebufferSize(&width, &height);
        if (width == 0 || height == 0) return;

        double now = glfwGetTime();
        double dt = now - last_frame_time;
        if (dt < 0.0) dt = 0.0;  // glfwGetTime() is monotonic, but be defensive.
        if (dt > kMaxDtSeconds) dt = kMaxDtSeconds;
        last_frame_time = now;

        UiAppInput ui_input{};
        ui_input.mouse_x = input.mouse_x;
        ui_input.mouse_y = input.mouse_y;
        ui_input.mouse_down = input.mouse_down ? 1 : 0;
        ui_input.clicked = input.clicked ? 1 : 0;
        ui_input.double_clicked = input.double_clicked ? 1 : 0;
        ui_input.right_clicked = input.right_clicked ? 1 : 0;
        ui_input.now_seconds = now;
        ui_input.dt_seconds = static_cast<float>(dt);
        ui_input.scroll_delta_y = input.scroll_delta_y;

        // Truncate to the fixed buffer's capacity (see UiAppInput's
        // comment) - a single frame's typing is realistically 0-1
        // characters, so this is just a defensive cap, not something
        // expected to actually trim real input.
        std::snprintf(ui_input.text_input, sizeof(ui_input.text_input), "%s",
                       input.text_input.c_str());
        ui_input.key_backspace = input.key_backspace ? 1 : 0;
        ui_input.key_enter = input.key_enter ? 1 : 0;
        ui_input.key_up = input.key_up ? 1 : 0;
        ui_input.key_down = input.key_down ? 1 : 0;

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

        // The UI side (see app_ui.cpp's DrawResizeToggle) can request
        // the window be resized - only this side owns the actual GLFW
        // window, so it's the one that has to act on it. Don't apply
        // it now, though: this frame's RenderFrame call below still
        // needs to run against the *current* surface size, matching
        // what BeginFrame just built. Stash the request and apply it
        // at the top of next frame instead (see the comment on
        // has_pending_resize above).
        uint32_t requested_width = 0, requested_height = 0;
        if (UiAppConsumeResizeRequest(ui_app, &requested_width, &requested_height)) {
            has_pending_resize = true;
            pending_resize_width = requested_width;
            pending_resize_height = requested_height;
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
        input.dropped_paths.clear();
    };

    int result = window.Run(DrawFrame);
    UiAppDestroy(ui_app);
    return result;
}