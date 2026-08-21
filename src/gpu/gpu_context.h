// gpu_context.h - a wgpu-native surface tied to a GLFW window,
// rendering an app-provided UI each frame.
//
// Built against wgpu-native v25.0.2.2 (see third_party/wgpu_native/README.md)
// - the headers in third_party/wgpu_native/include/ are vendored
// verbatim from that tag (webgpu.h from its exact pinned
// webgpu-headers submodule commit), not hand-written, to avoid ABI
// drift between the header and the prebuilt binary.

#ifndef APPSHELL_GPU_GPU_CONTEXT_H
#define APPSHELL_GPU_GPU_CONTEXT_H

#include <cstdint>
#include <functional>
#include <string>

#include "wgpu.h"

struct GLFWwindow;

namespace appshell {

// RAII wrapper around one wgpu-native instance/adapter/device/surface
// bound to a single window. Not copyable - owns GPU resources with a
// release-on-destruction lifetime.
class GpuContext {
public:
    GpuContext() = default;
    ~GpuContext();

    GpuContext(const GpuContext&) = delete;
    GpuContext& operator=(const GpuContext&) = delete;

    // Synchronously sets up instance -> adapter -> device -> surface
    // and configures the surface at (width, height). Returns false
    // (with a message in last_error()) on any failure. `window` is
    // read once here (to pull the native handle for surface
    // creation) and is not retained past this call.
    bool Create(GLFWwindow* window, uint32_t width, uint32_t height);

    // Reconfigures the surface after a resize (e.g. from a
    // framebuffer-size callback). Safe to call before the first
    // successful frame.
    void Resize(uint32_t width, uint32_t height);

    // Acquires the current surface texture, clears it to (r, g, b, a)
    // (each 0.0-1.0), calls draw_ui(pass) - if set - so callers can
    // encode more draw calls into the same render pass, then submits
    // and presents. This is the entire per-frame render path; the
    // included UiRenderer hangs its draw calls off `draw_ui`.
    bool RenderFrame(double r, double g, double b, double a,
                      const std::function<void(WGPURenderPassEncoder)>& draw_ui);

    // Exposed so UiRenderer (or any other renderer) can build its own
    // pipeline against the same device/queue/surface-format without
    // GpuContext needing to know anything about UI rendering itself.
    WGPUDevice device() const { return device_; }
    WGPUQueue queue() const { return queue_; }
    WGPUTextureFormat surface_format() const { return surface_format_; }

    const std::string& last_error() const { return last_error_; }

    // Called from the free-function WGPUUncapturedErrorCallback
    // (device validation/backend errors - e.g. a bad format/alpha-mode
    // combination passed to wgpuSurfaceConfigure).
    void ReportUncapturedDeviceError(const std::string& message);

private:
    void SetError(const std::string& message);

    // Blocks the calling thread until `done` becomes true, by
    // repeatedly pumping wgpuInstanceProcessEvents. Used to make the
    // otherwise-async adapter/device requests behave synchronously
    // for this simple, single-threaded startup path.
    void PumpUntil(const bool& done);

    WGPUInstance instance_ = nullptr;
    WGPUAdapter adapter_ = nullptr;
    WGPUDevice device_ = nullptr;
    WGPUQueue queue_ = nullptr;
    WGPUSurface surface_ = nullptr;

    WGPUTextureFormat surface_format_ = WGPUTextureFormat_BGRA8Unorm;
    WGPUCompositeAlphaMode surface_alpha_mode_ = WGPUCompositeAlphaMode_Opaque;

    // Tracks the status logged last frame, so a stall/failure only
    // prints a new line when the *kind* of failure changes - spamming
    // a line every ~16ms would itself make the failure feel laggy to
    // read.
    WGPUSurfaceGetCurrentTextureStatus last_logged_status_ =
        WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;

    uint32_t width_ = 0;
    uint32_t height_ = 0;

    bool surface_configured_ = false;
    bool render_failure_reported_ = false;
    bool device_error_reported_ = false;

    std::string last_error_;
};

}  // namespace appshell

#endif  // APPSHELL_GPU_GPU_CONTEXT_H
