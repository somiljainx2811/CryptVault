// debug_repro.cpp - TEMPORARY diagnostic harness, not part of the
// shipped app. Drives the real UI code (via the same UiAppCreate/
// UiAppBeginFrame/UiAppEndFrame entry points main.cpp uses) with a
// scripted sequence of synthetic input frames instead of real mouse/
// keyboard events, so reported bugs can be reproduced deterministically
// and inspected via UiAppDebugDump rather than guessed at from reading
// code alone.
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gpu/gpu_context.h"
#include "ui/app_ui.h"
#include "window/window.h"

extern "C" void UiAppDebugDump(UiApp* app, char* buf, int buf_size);

namespace {
constexpr float kBorder = 17.0f;

UiAppInput MakeInput() {
    UiAppInput in{};
    in.dt_seconds = 1.0f / 60.0f;
    return in;
}

void Dump(UiApp* app, const char* label) {
    char buf[2048];
    UiAppDebugDump(app, buf, sizeof(buf));
    std::printf("[%s] %s\n", label, buf);
}
}  // namespace

int main() {
    appshell::WindowConfig cfg;
    cfg.title = "repro";
    cfg.width = 400;
    cfg.height = 300;
    cfg.resizable = false;
    cfg.decorated = false;
    appshell::Window window(cfg);
    if (!window.Create()) {
        std::fprintf(stderr, "window create failed\n");
        return 1;
    }
    appshell::GpuContext gpu;
    uint32_t fb_w = 0, fb_h = 0;
    window.GetFramebufferSize(&fb_w, &fb_h);
    if (!gpu.Create(window.handle(), fb_w, fb_h)) {
        std::fprintf(stderr, "gpu create failed: %s\n", gpu.last_error().c_str());
        return 1;
    }

    UiApp* app = UiAppCreate(gpu.device(), gpu.queue(), gpu.surface_format());
    if (!app) {
        std::fprintf(stderr, "UiAppCreate failed\n");
        return 1;
    }

    double t = 0.0;
    auto RunFrame = [&](UiAppInput in) {
        in.now_seconds = t;
        t += 1.0 / 60.0;
        window.GetFramebufferSize(&fb_w, &fb_h);
        UiAppBeginFrame(app, fb_w, fb_h, &in);
        gpu.RenderFrame(0.1f, 0.1f, 0.1f, 1.0f,
                         [&](WGPURenderPassEncoder pass) { UiAppEndFrame(app, pass); });
    };

    // Idle frame so animations/hover state settle.
    RunFrame(MakeInput());
    Dump(app, "start");

    // --- Step 1: click "ADD VAULT" on VaultList (opens the wizard) ---
    // Layout: toolbar height 35, "ADD VAULT" toolbar button sits
    // right after the icon cluster - rather than guess pixel-perfect
    // coordinates for every control, drive the wizard open via the
    // grid's own trailing "Add" tile instead: 5-column grid starting
    // at padding 8, row height 62 - tile 0 (first/only tile when the
    // vault list is empty) is the trailing "ADD VAULT" tile itself.
    float grid_y = 35.0f;         // kToolbarHeight
    float cell_w = (400.0f - 2 * kBorder - 8.0f * 2) / 5.0f;  // usable_cell_w approx (no scrollbar)
    float tile_cx = 8.0f + cell_w * 0.5f;
    float tile_cy = grid_y + 6.0f + 20.0f;  // kPaddingY + icon-ish area

    auto Click = [&](float x, float y) {
        UiAppInput press = MakeInput();
        press.mouse_x = x; press.mouse_y = y; press.mouse_down = 1;
        RunFrame(press);
        UiAppInput release = MakeInput();
        release.mouse_x = x; release.mouse_y = y; release.mouse_down = 0; release.clicked = 1;
        RunFrame(release);
    };
    auto DoubleClick = [&](float x, float y) {
        UiAppInput p1 = MakeInput(); p1.mouse_x = x; p1.mouse_y = y; p1.mouse_down = 1;
        RunFrame(p1);
        UiAppInput r1 = MakeInput(); r1.mouse_x = x; r1.mouse_y = y; r1.clicked = 1;
        RunFrame(r1);
        UiAppInput p2 = MakeInput(); p2.mouse_x = x; p2.mouse_y = y; p2.mouse_down = 1;
        RunFrame(p2);
        UiAppInput r2 = MakeInput(); r2.mouse_x = x; r2.mouse_y = y; r2.clicked = 1; r2.double_clicked = 1;
        RunFrame(r2);
    };
    auto Type = [&](const std::string& s) {
        for (char c : s) {
            UiAppInput in = MakeInput();
            in.text_input[0] = c; in.text_input[1] = 0;
            RunFrame(in);
        }
    };
    auto PressEnter = [&]() {
        UiAppInput in = MakeInput();
        in.key_enter = 1;
        RunFrame(in);
    };

    DoubleClick(tile_cx, tile_cy);
    Dump(app, "after double-click ADD VAULT tile");

    Type("TestVault");
    PressEnter();
    Dump(app, "after typing name + enter (stage->location)");

    // Location field pre-filled with a default; accept as-is.
    PressEnter();
    Dump(app, "after enter on location (stage->password)");

    Type("hunter2");
    PressEnter();
    Dump(app, "after typing password (stage->confirm)");

    Type("hunter2");
    PressEnter();
    Dump(app, "after confirm password (should create vault)");

    // --- Step 2: open the newly created vault (double-click its tile) ---
    DoubleClick(tile_cx, tile_cy);
    Dump(app, "after double-click the new vault tile");

    // --- Step 3: double-click "ADD FOLDER" trailing tile ---
    // Now inside the vault: tab bar adds kTabBarHeight (30) below the
    // toolbar, so the grid starts lower.
    float folder_grid_y = 35.0f + 30.0f;
    float folder_tile_cy = folder_grid_y + 6.0f + 20.0f;
    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after double-click ADD FOLDER tile");

    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after SECOND double-click ADD FOLDER tile (repro attempt)");

    // --- Step 4: Settings icon ---
    float settings_x = 295.0f, settings_y = 17.5f;
    Click(settings_x, settings_y);
    Dump(app, "after clicking Settings icon (once - should open)");
    Click(settings_x, settings_y);
    Dump(app, "after clicking Settings icon again (should close)");

    // --- Step 5: Search icon ---
    float search_x = 315.0f, search_y = 17.5f;
    Click(search_x, search_y);
    Dump(app, "after clicking Search icon (once - should open)");
    Click(search_x, search_y);
    Dump(app, "after clicking Search icon again (should close)");

    // --- Step 6: "New Tab" (+) button on the vault tab bar ---
    float newtab_x = 115.0f, newtab_y = 50.0f;
    Click(newtab_x, newtab_y);
    Dump(app, "after clicking vault-tab '+' (New Tab)");

    // --- Step 7: double-click the "New Folder" tile itself to
    // navigate INTO it (nested folder browsing) ---
    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after double-click into 'New Folder' (should navigate in)");

    // --- Step 8: create a folder while nested - confirms it lands
    // inside the current folder, not back at the vault root ---
    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after ADD FOLDER while nested inside 'New Folder'");

    // --- Step 9: Back should pop up one level ---
    // Back icon sits right after the brand icon: cursor_x starts at
    // 5, brand icon is 30px + 6px gap = 41, then centered within the
    // next 30px-wide slot at 24px nav-icon size -> x=44, y=5.5.
    Click(56.0f, 17.5f);
    Dump(app, "after clicking Back (should pop back to vault root)");

    UiAppDestroy(app);
    std::printf("done\n");
    return 0;
}
