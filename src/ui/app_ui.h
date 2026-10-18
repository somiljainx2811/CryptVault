#ifndef CRYPTVAULT_UI_APP_UI_H
#define CRYPTVAULT_UI_APP_UI_H

#include <cstdint>
#include "wgpu.h"

#if defined(_WIN32)
  #if defined(CRYPTVAULT_UI_BUILD)
    #define CRYPTVAULT_UI_API __declspec(dllexport)
  #else
    #define CRYPTVAULT_UI_API __declspec(dllimport)
  #endif
#else
  #define CRYPTVAULT_UI_API __attribute__((visibility("default")))
#endif

struct UiApp;

// C-compatible data crossing the app_ui.cpp entry-point boundary.
struct UiAppInput {
    float mouse_x;
    float mouse_y;
    uint8_t mouse_down;
    uint8_t clicked;
    uint8_t double_clicked;

    // Right mouse button release, one-shot like `clicked` - see
    // UiInput::right_clicked for what it drives (a tile's context
    // menu).
    uint8_t right_clicked;

    // Wall-clock time (glfwGetTime(), seconds since process start) as
    // of this frame, and the elapsed time since the previous frame.
    // Added so the UI side can drive time-based animations (see
    // ui/animation.h) without needing its own clock - main.cpp owns
    // the one true clock; the UI side just reads it. `dt_seconds` is
    // pre-clamped by main.cpp (see kMaxDtSeconds) so a debugger pause
    // can't produce a giant single-frame jump in an in-flight
    // animation.
    double now_seconds;
    float dt_seconds;

    // Physical pixels per logical unit (framebuffer size / window size):
    // 1.0 on a normal display, 2.0 on 2x HiDPI. `width`/`height` passed to
    // UiAppBeginFrame and all mouse coordinates are LOGICAL; this only
    // tells the renderer how to size scissor rectangles. 0 is treated as 1.
    float pixel_scale;

    // Accumulated vertical scroll wheel delta since the last frame
    // (see main.cpp's on_scroll) - forwarded as-is to UiInput's field
    // of the same name; see that struct for the sign convention.
    float scroll_delta_y;

    // Text typed since the last frame (see main.cpp's on_char), as a
    // null-terminated ASCII string - non-ASCII codepoints are dropped
    // before reaching here, since the 5x7 bitmap font can't render
    // them anyway. Fixed-size (not std::string) so this struct stays
    // a plain, ABI-stable C struct; 15 chars is more than a single
    // frame's typing ever produces in practice.
    char text_input[16];

    // One-shot edges (same contract as `clicked`) for the handful of
    // keys the command palette's list navigation cares about - see
    // main.cpp's on_key.
    uint8_t key_backspace;
    uint8_t key_enter;
    uint8_t key_up;
    uint8_t key_down;

    // Files dropped onto the window from the OS this frame (see
    // main.cpp's on_files_dropped) - see UiInput::dropped_paths for
    // what happens to them once they reach the UI side. Same fixed-
    // size-array approach as text_input above, and for the same
    // reason: this struct has to stay a plain, ABI-stable C struct.
    // Capped at kMaxDroppedFiles paths of up to
    // kMaxDroppedPathLen-1 characters each - dropping more files than
    // that in one go, or a single path longer than that, is rare
    // enough that truncating (main.cpp still reports the real count
    // so the UI side can toast about it) is an acceptable trade for
    // keeping this struct fixed-size.
    static constexpr int kMaxDroppedFiles = 8;
    static constexpr int kMaxDroppedPathLen = 512;
    uint8_t dropped_paths_count;         // how many of dropped_paths[] below are valid
    uint8_t dropped_paths_count_actual;  // how many the OS actually reported (may exceed kMaxDroppedFiles)
    char dropped_paths[kMaxDroppedFiles][kMaxDroppedPathLen];

    // Explorer keyboard support. key_ctrl/key_shift are HELD state; the
    // rest are one-shot edges: Ctrl+A/C/X/V/F/L (select all, copy, cut,
    // paste, find, edit path), Delete, F2 (rename).
    uint8_t key_ctrl;
    uint8_t key_shift;
    uint8_t key_delete;
    uint8_t key_f2;
    uint8_t key_a;
    uint8_t key_c;
    uint8_t key_x;
    uint8_t key_v;
    uint8_t key_f;
    uint8_t key_l;
};

extern "C" {
CRYPTVAULT_UI_API UiApp* UiAppCreate(WGPUDevice device, WGPUQueue queue,
                                    WGPUTextureFormat surface_format);
CRYPTVAULT_UI_API void UiAppDestroy(UiApp* app);

// Optional, call once after UiAppCreate: open the explorer in
// `start_path` (NULL/empty = $HOME), or start on the restricted vault
// side when `vault_side` is non-zero. Used for `cryptvault ~/Downloads`
// and `cryptvault gui --vaults` from a terminal.
// Esc pressed: closes the topmost popup/prompt/filter if there is one and
// returns 1; returns 0 when nothing was open (main.cpp then quits).
CRYPTVAULT_UI_API uint8_t UiAppHandleEscape(UiApp* app);

// Text the UI wants on the system clipboard ("COPY PATH"). Returns 1 and
// copies it (NUL-terminated, truncated to `capacity`) when a request is
// pending, else 0. main.cpp hands it to GLFW's clipboard.
CRYPTVAULT_UI_API uint8_t UiAppConsumeClipboardRequest(UiApp* app, char* out, uint32_t capacity);

CRYPTVAULT_UI_API void UiAppSetStart(UiApp* app, const char* start_path, uint8_t vault_side);
CRYPTVAULT_UI_API void UiAppBeginFrame(UiApp* app, uint32_t width, uint32_t height,
                                     const UiAppInput* input);
CRYPTVAULT_UI_API void UiAppEndFrame(UiApp* app, WGPURenderPassEncoder pass);

// One-shot: returns 1 and writes the requested window size to
// *out_width/*out_height if the resize-toggle button (top-left
// corner of the border, see DrawResizeToggle in app_ui.cpp) was
// clicked during the most recent UiAppBeginFrame; returns 0
// otherwise. The request is cleared as soon as it's read, so this
// should be polled once per frame (see main.cpp) - only main.cpp can
// act on it, since it's the only side that owns the actual OS window.
CRYPTVAULT_UI_API uint8_t UiAppConsumeResizeRequest(UiApp* app, uint32_t* out_width, uint32_t* out_height);

// One-shot: returns 1 if the app should quit (Settings' QUIT button,
// or the border right-click menu's CLOSE) - cleared as soon as it's
// read, same "poll once per frame" contract as
// UiAppConsumeResizeRequest above. Only main.cpp can act on it
// (Window::RequestClose owns the real GLFW window).
CRYPTVAULT_UI_API uint8_t UiAppConsumeQuitRequest(UiApp* app);

// NOT one-shot, unlike the two functions above - returns the
// currently-desired "always on top" state (see the border right-click
// menu's ALWAYS ON TOP row) so main.cpp can poll it every frame and
// call Window::SetAlwaysOnTop only when it actually changes.
CRYPTVAULT_UI_API uint8_t UiAppGetAlwaysOnTop(UiApp* app);
}

#endif