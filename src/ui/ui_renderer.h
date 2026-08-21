// ui_renderer.h - minimal immediate-mode UI primitives (filled rects,
// text via the 5x7 bitmap font, and one real clickable button) built
// directly on the GpuContext/wgpu plumbing.
//
// Deliberately not a general-purpose UI toolkit: one pipeline
// (solid-color triangles, no textures), no layout system, no
// clipping/scissoring, no z-ordering beyond draw order. Every screen
// is expected to be built by calling these same primitives more
// times, not by replacing them - so the API surface here is kept
// intentionally small and stable. Reach for a real UI library (Dear
// ImGui, etc) once you outgrow this.
//
// Coordinate space: callers work entirely in top-left-origin pixel
// coordinates matching the window's framebuffer size - UiRenderer
// converts to NDC internally per vertex, so there's no shader-side
// uniform buffer or projection matrix to wire up.

#ifndef APPSHELL_UI_UI_RENDERER_H
#define APPSHELL_UI_UI_RENDERER_H

#include <cstdint>
#include <string>
#include <vector>

#include "wgpu.h"

namespace appshell {

struct UiColor {
    float r, g, b, a;
};

// Mouse state for the current frame, filled in by the app from the
// window's mouse callbacks and read by Button(). `clicked` is a
// one-frame edge (true only on the frame containing the button-up
// that released over *some* button) - it's up to the caller to reset
// it after each frame; UiRenderer doesn't own input state itself
// since the app may need the same state to drive non-UI input too.
struct UiInput {
    float mouse_x = -1.0f;
    float mouse_y = -1.0f;
    bool mouse_down = false;
    bool clicked = false;
    // One-frame edge, same contract as `clicked`: true only on the
    // frame containing a release that the app's window-level
    // double-click detection (position + timing) recognized as the
    // second click of a pair. UiRenderer never sets this itself -
    // it's filled in by the app (see main.cpp) from consecutive
    // on_mouse_button releases, same as `clicked`.
    bool double_clicked = false;
    // Right mouse button release, one-shot like `clicked` - drives a
    // tile's context menu (see DrawTileGrid's right_clicked result
    // and DrawContextMenu in app_ui.cpp).
    bool right_clicked = false;
    // Accumulated vertical scroll wheel input since the last frame
    // (positive = scrolled up/away from the user, matching GLFW's
    // yoffset convention - see main.cpp's on_scroll). Not an edge
    // like clicked/double_clicked; it's a per-frame delta the caller
    // (e.g. a scrollable tile grid) consumes directly, and main.cpp
    // resets it to 0 after each frame the same way it does for the
    // click flags.
    float scroll_delta_y = 0.0f;

    // Text typed this frame (already ASCII-filtered) and one-shot
    // navigation-key edges - see UiAppInput's fields of the same name
    // for the full contract. Kept as std::string here (unlike the C
    // struct crossing the DLL boundary) since UiInput never leaves
    // this process.
    std::string text_input;
    bool key_backspace = false;
    bool key_enter = false;
    bool key_up = false;
    bool key_down = false;

    // Absolute filesystem paths dropped onto the window this frame
    // (see UiAppInput's fixed-size version of the same data, and
    // DrawVaultFolderScreen for what happens to them - each gets
    // read, encrypted, and added to the vault). Empty on every frame
    // nothing was dropped. Unlike UiAppInput this can be a real
    // std::vector<std::string> since UiInput never crosses the DLL
    // boundary.
    std::vector<std::string> dropped_paths;
};

// Owns one render pipeline and one growable vertex buffer. Not
// copyable - holds live wgpu resource handles.
class UiRenderer {
public:
    UiRenderer() = default;
    ~UiRenderer();

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    // Builds the shader module, pipeline layout, and pipeline against
    // the given device/surface format. Call once, after
    // GpuContext::Create() succeeds. Returns false (with a message in
    // last_error()) on failure.
    bool Create(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surface_format);

    // Call at the start of each frame, before any Draw*/Button calls.
    // Clears the CPU-side vertex list and records the screen size
    // used to convert pixel coordinates to NDC for this frame.
    void BeginFrame(uint32_t screen_width, uint32_t screen_height);

    // All coordinates are top-left-origin pixels in the current
    // frame's screen space (see BeginFrame).
    void DrawRect(float x, float y, float w, float h, UiColor color);

    // Draws a rectangle with smoothly rounded, anti-aliased corners.
    // Unlike building corners out of several DrawRect() calls, this
    // is a single quad whose fragment shader evaluates the signed
    // distance to a rounded-box outline (see ui_renderer.cpp) and
    // feathers the edge with smoothstep(), so the corner is a true
    // curve at any radius/scale rather than a fixed-step staircase.
    // `radius` is clamped internally to at most half of the smaller
    // of `w`/`h`, so an oversized radius degrades gracefully to a
    // stadium/circle shape instead of producing garbage geometry.
    // Draw order relative to DrawRect/DrawLabel/DrawImage is the true
    // call order (see EndFrame) - draw a background rounded rect
    // before the content that sits on top of it, same as you would
    // with any of the other Draw* calls.
    void DrawRoundedRect(float x, float y, float w, float h, float radius, UiColor color);

    // Renders `text` using the 5x7 bitmap font (see font5x7.h) as
    // plain solid rects - no texture atlas. `pixel_size` is the
    // on-screen size of one font pixel (e.g. 2.0 for small text, 3.0
    // for button labels); glyphs are 5 wide x 7 tall with a 1-pixel
    // gap between characters. Lowercase is upper-cased; unmapped
    // characters render as blank glyphs rather than being skipped, so
    // spacing stays predictable.
    void DrawLabel(float x, float y, const std::string& text, UiColor color, float pixel_size);

    // Returns the on-screen width of `text` at the given pixel_size,
    // without drawing anything - callers use this to center labels
    // inside buttons/panels.
    float MeasureText(const std::string& text, float pixel_size) const;

    // Draws a background rect (brighter on hover), a 1px-equivalent
    // border, and a centered label, then reports whether this button
    // was clicked this frame. `id` only needs to be stable/unique if
    // callers later want per-button state beyond hover (not needed
    // today); it's kept in the signature so adding e.g. press-
    // animation state later doesn't change every call site.
    bool Button(int id, float x, float y, float w, float h,
                const std::string& label, const UiInput& input);

    // --- Textured icons (Phase 2d addition) ------------------------
    //
    // A second, separate pipeline: same NDC-space immediate-mode
    // model as DrawRect, but each vertex carries a UV instead of
    // (relying entirely on) a flat color, and is sampled against one
    // texture bound per draw call. Icons are decoded once up front
    // (LoadIcon) into a persistent GPU texture + bind group; DrawImage
    // just records a textured quad against an already-loaded icon.
    //
    // Kept as a distinct pipeline/vertex-format rather than folded
    // into the solid-rect one so that DrawRect/DrawLabel/Button (the
    // "Phase 2c" surface) don't have to pay for a texture/sampler bind
    // group they don't use.

    // Decodes a PNG (or any stb_image-supported format) from disk and
    // uploads it as an RGBA8 texture with its own bind group. Returns
    // an opaque handle (>= 0) to pass to DrawImage, or -1 on failure
    // (see last_error()). Call after Create(), before the first frame
    // that needs it - decoding + GPU upload happens synchronously, so
    // this is meant to be called during app startup, not per-frame.
    int LoadIcon(const std::string& path);

    // Decodes every frame of an animated GIF at `path` and uploads
    // each as its own RGBA8 texture (same underlying representation
    // LoadIcon produces for a single image - each frame is just
    // another opaque handle DrawImage already knows how to draw).
    // `out_frame_icons`/`out_frame_delays_ms` are filled in parallel,
    // one entry per frame - `out_frame_delays_ms[i]` is how long
    // frame `i` should stay on screen before advancing to the next,
    // as decoded from the GIF itself (not a guess/fixed rate), with
    // any zero/absurdly-small delay (some GIF encoders emit 0ms,
    // which browsers/viewers conventionally treat as "use a sane
    // default" rather than "no delay at all") floored to 20ms so a
    // pathological file can't spin this uselessly fast. Returns false
    // (leaving both vectors empty) on failure, e.g. `path` isn't a
    // decodable GIF at all - see last_error(). Called at startup or
    // when the user picks a new background (see DrawSettingsPanel's
    // background-picker grid), not per-frame - decoding every frame
    // up front means playback is just "pick which already-uploaded
    // texture to draw this frame," no per-frame decode cost.
    bool LoadAnimatedImage(const std::string& path, std::vector<int>& out_frame_icons,
                            std::vector<int>& out_frame_delays_ms);

    // Draws `icon` (as returned by LoadIcon) as a textured quad at
    // (x, y, w, h) in the current frame's pixel space. `tint` is
    // multiplied against the sampled texel (straight alpha) - pass
    // white (1,1,1,1) to draw the icon unmodified, or a dimmer/brighter
    // white to fake hover/disabled states without a second asset.
    // Draw order relative to every other Draw* call (rects, rounded
    // rects, other images) is the true call order (see EndFrame).
    void DrawImage(float x, float y, float w, float h, int icon, UiColor tint = UiColor{1.0f, 1.0f, 1.0f, 1.0f});

    // Restricts all drawing (of every kind - rects, rounded rects,
    // images) to `x,y,w,h` (top-left-origin pixels, same space as
    // every other Draw* call) until the matching PopClipRect. Nests:
    // pushing while already clipped intersects with the current clip
    // rather than replacing it, so an inner PushClipRect can only
    // shrink the visible area, never escape an outer one. Backed by
    // the GPU's scissor test (see EndFrame), not per-primitive math,
    // so it clips every pipeline uniformly with no extra shader work.
    // Every PushClipRect must be matched by a PopClipRect before
    // EndFrame - typical use is a scrollable region: push the
    // region's bounds, draw content that may extend past them, pop.
    void PushClipRect(float x, float y, float w, float h);
    void PopClipRect();

    // Uploads the accumulated vertex data and issues the draw call
    // into the given render pass. Call once per frame, inside the
    // WGPURenderPassEncoder that GpuContext::RenderFrame's draw_ui
    // callback receives, after all Draw*/Button calls for the frame.
    void EndFrame(WGPURenderPassEncoder pass);

    const std::string& last_error() const { return last_error_; }

private:
    // Shared by LoadIcon and LoadAnimatedImage - uploads already-
    // decoded RGBA8 pixel data (width*height*4 bytes, row-major) as a
    // new texture/view/bind-group and appends it to icons_, returning
    // its handle (or -1 on a GPU-side failure, with last_error() set -
    // decode failures are the caller's concern, this only ever sees
    // pixels that already decoded successfully). `label` is used only
    // for the WGPU object labels/error messages, not decoding.
    int UploadTexture(const unsigned char* pixels, int width, int height, const std::string& label);

    struct Vertex {
        float x, y;
        float r, g, b, a;
    };

    struct ImageVertex {
        float x, y;
        float u, v;
        float r, g, b, a;
    };

    // Vertex format for the rounded-rect pipeline. `local` is this
    // vertex's offset in pixels from the rect's center (NOT a 0..1
    // UV) - the fragment shader needs real pixel distances to
    // evaluate the rounded-box SDF against `half_size`/`radius`
    // correctly regardless of the rect's aspect ratio. `half_size` and
    // `radius` are the same on all 4 vertices of a given quad; they're
    // duplicated per-vertex (rather than pulled from a uniform) to
    // keep this pipeline immediate-mode/uniform-free like the other
    // two.
    struct RoundedRectVertex {
        float x, y;            // clip-space (NDC) position
        float local_x, local_y;  // pixel offset from rect center
        float half_w, half_h;     // rect half-size, in pixels
        float radius;
        float r, g, b, a;
    };

    // One decoded icon: its own texture/view/bind-group, created once
    // in LoadIcon and released in ~UiRenderer.
    struct IconTexture {
        WGPUTexture texture = nullptr;
        WGPUTextureView view = nullptr;
        WGPUBindGroup bind_group = nullptr;
    };

    // One recorded DrawImage call for the current frame: which icon's
    // bind group to draw with, and the six vertices of its quad. Kept
    // as a flat, in-call-order list (not batched by texture) so that
    // images drawn over other images - e.g. the "+" badge over the
    // "Add Vault" folder icon - composite in the exact order the app
    // called DrawImage, which per-texture batching would not preserve.
    struct ImageDrawCall {
        int icon = -1;
        ImageVertex verts[6];
    };

    // A contiguous run of same-pipeline geometry, in true call order.
    // DrawRect/DrawLabel, DrawRoundedRect, and DrawImage each append
    // their vertices to their own per-pipeline CPU list (vertices_ /
    // rounded_rect_vertices_ / image_draw_calls_) as before - that
    // part hasn't changed, and still lets EndFrame do one upload per
    // pipeline. What changed is that every call *also* appends (or
    // extends, if the previous call was the same kind) an entry here,
    // so EndFrame can walk pipeline switches in the order the app
    // actually called Draw*, instead of always drawing every rounded
    // rect, then every solid rect, then every image regardless of
    // call order. That fixed ordering was wrong whenever a solid rect
    // needed to paint *over* an already-drawn rounded rect (e.g. a
    // toolbar's background rect, called first, is supposed to sit
    // *under* an address-bar rounded rect called after it - with a
    // fixed rounded-then-solid pass order the background rect ended
    // up on top instead, erasing the rounded box).
    enum class BatchKind { kSolid, kRoundedRect, kImage, kSetScissor };
    struct DrawBatch {
        BatchKind kind;
        // For kSolid/kRoundedRect: range within vertices_ /
        // rounded_rect_vertices_. For kImage: `offset` is the index
        // into image_draw_calls_ and `count` is unused (always 1 -
        // each image call keeps its own bind group, so these aren't
        // coalesced the way solid/rounded-rect runs are). For
        // kSetScissor: offset/count are unused; the rect lives in
        // scissor_x/y/w/h below.
        size_t offset;
        size_t count;
        uint32_t scissor_x = 0, scissor_y = 0, scissor_w = 0, scissor_h = 0;
    };

    void PushVertex(float px, float py, UiColor color);
    void EnsureVertexCapacity(size_t vertex_count);
    bool CreateImagePipeline();
    bool CreateRoundedRectPipeline();
    void EnsureRoundedRectVertexCapacity(size_t vertex_count);

    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUTextureFormat surface_format_ = WGPUTextureFormat_BGRA8Unorm;

    WGPUShaderModule shader_module_ = nullptr;
    WGPUPipelineLayout pipeline_layout_ = nullptr;
    WGPURenderPipeline pipeline_ = nullptr;

    WGPUBuffer vertex_buffer_ = nullptr;
    size_t vertex_buffer_capacity_ = 0;  // in vertices

    std::vector<Vertex> vertices_;

    // --- image pipeline state ---
    WGPUShaderModule image_shader_module_ = nullptr;
    WGPUBindGroupLayout image_bind_group_layout_ = nullptr;
    WGPUPipelineLayout image_pipeline_layout_ = nullptr;
    WGPURenderPipeline image_pipeline_ = nullptr;
    WGPUSampler sampler_ = nullptr;  // shared by every icon's bind group

    WGPUBuffer image_vertex_buffer_ = nullptr;
    size_t image_vertex_buffer_capacity_ = 0;  // in vertices

    std::vector<IconTexture> icons_;
    std::vector<ImageDrawCall> image_draw_calls_;

    // --- rounded-rect pipeline state ---
    WGPUShaderModule rounded_rect_shader_module_ = nullptr;
    WGPUPipelineLayout rounded_rect_pipeline_layout_ = nullptr;
    WGPURenderPipeline rounded_rect_pipeline_ = nullptr;

    WGPUBuffer rounded_rect_vertex_buffer_ = nullptr;
    size_t rounded_rect_vertex_buffer_capacity_ = 0;  // in vertices

    std::vector<RoundedRectVertex> rounded_rect_vertices_;

    // In-call-order record of pipeline switches - see DrawBatch above.
    std::vector<DrawBatch> batches_;

    // Stack of active clip rects in framebuffer pixel coordinates
    // (already intersected with their parent - see PushClipRect), so
    // .back() is always the currently-effective scissor rect. Reset
    // empty each BeginFrame; every push/pop also records a
    // kSetScissor DrawBatch so EndFrame applies it at the right point
    // in true draw-call order (same reasoning as DrawBatch itself).
    struct ScissorRect {
        uint32_t x, y, w, h;
    };
    std::vector<ScissorRect> clip_stack_;

    float screen_width_ = 1.0f;
    float screen_height_ = 1.0f;

    std::string last_error_;
};

}  // namespace appshell

#endif  // APPSHELL_UI_UI_RENDERER_H
