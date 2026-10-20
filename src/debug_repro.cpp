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
}

int main() {
    cryptvault::WindowConfig cfg;
    cfg.title = "repro";
    cfg.width = 400;
    cfg.height = 300;
    cfg.resizable = false;
    cfg.decorated = false;
    cryptvault::Window window(cfg);
    if (!window.Create()) {
        std::fprintf(stderr, "window create failed\n");
        return 1;
    }
    cryptvault::GpuContext gpu;
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
<<<<<<< HEAD
    UiAppSetStart(app, nullptr, 1); 
    
=======
    UiAppSetStart(app, nullptr, 1);  // its scripted input targets the vault side

>>>>>>> 409ed34 (Drag n Drop)
    double t = 0.0;
    auto RunFrame = [&](UiAppInput in) {
        in.now_seconds = t;
        t += 1.0 / 60.0;
        window.GetFramebufferSize(&fb_w, &fb_h);
        UiAppBeginFrame(app, fb_w, fb_h, &in);
        gpu.RenderFrame(0.1f, 0.1f, 0.1f, 1.0f,
                         [&](WGPURenderPassEncoder pass) { UiAppEndFrame(app, pass); });
    };

    RunFrame(MakeInput());
    Dump(app, "start");

    float grid_y = 35.0f;
    float cell_w = (400.0f - 2 * kBorder - 8.0f * 2) / 5.0f;
    float tile_cx = 8.0f + cell_w * 0.5f;
    float tile_cy = grid_y + 6.0f + 20.0f;

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

    PressEnter();
    Dump(app, "after enter on location (stage->password)");

    Type("hunter2");
    PressEnter();
    Dump(app, "after typing password (stage->confirm)");

    Type("hunter2");
    PressEnter();
    Dump(app, "after confirm password (should create vault)");

    DoubleClick(tile_cx, tile_cy);
    Dump(app, "after double-click the new vault tile");

    float folder_grid_y = 35.0f + 30.0f;
    float folder_tile_cy = folder_grid_y + 6.0f + 20.0f;
    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after double-click ADD FOLDER tile");

    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after SECOND double-click ADD FOLDER tile (repro attempt)");

    float settings_x = 295.0f, settings_y = 17.5f;
    Click(settings_x, settings_y);
    Dump(app, "after clicking Settings icon (once - should open)");
    Click(settings_x, settings_y);
    Dump(app, "after clicking Settings icon again (should close)");

    float search_x = 315.0f, search_y = 17.5f;
    Click(search_x, search_y);
    Dump(app, "after clicking Search icon (once - should open)");
    Click(search_x, search_y);
    Dump(app, "after clicking Search icon again (should close)");

    float newtab_x = 115.0f, newtab_y = 50.0f;
    Click(newtab_x, newtab_y);
    Dump(app, "after clicking vault-tab '+' (New Tab)");

    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after double-click into 'New Folder' (should navigate in)");

    DoubleClick(tile_cx, folder_tile_cy);
    Dump(app, "after ADD FOLDER while nested inside 'New Folder'");

    Click(56.0f, 17.5f);
    Dump(app, "after clicking Back (should pop back to vault root)");

    UiAppDestroy(app);
    std::printf("done\n");
    return 0;
}
