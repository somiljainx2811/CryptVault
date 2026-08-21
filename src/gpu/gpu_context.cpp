// gpu_context.cpp - see gpu_context.h for scope notes.

#include "gpu/gpu_context.h"

#include <GLFW/glfw3.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
// GLFW_EXPOSE_NATIVE_X11 works for both a native X11 session and an
// XWayland one. If you specifically need a native Wayland surface
// (e.g. GLFW built with -DGLFW_BUILD_WAYLAND=ON and no X11 fallback),
// swap this for GLFW_EXPOSE_NATIVE_WAYLAND and the matching
// WGPUSurfaceSourceWaylandSurface block below is already written -
// just flip which one is `#if 1`.
#define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>

#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

namespace appshell {

namespace {

WGPUStringView ToStringView(const char* text) {
    return WGPUStringView{text, WGPU_STRLEN};
}

// wgpu-native's public C status codes are deliberately coarse (e.g.
// wgpuSurfacePresent only ever reports Success or Error, with no
// detail on *why*). The actual reason is only available through
// wgpu-native's internal logging hook, exposed here via
// wgpuSetLogCallback, so a failure's last_error() can include the
// real underlying reason instead of a bare status number.
std::string g_recent_wgpu_log;

const char* LogLevelName(WGPULogLevel level) {
    switch (level) {
        case WGPULogLevel_Error: return "error";
        case WGPULogLevel_Warn:  return "warn";
        case WGPULogLevel_Info:  return "info";
        case WGPULogLevel_Debug: return "debug";
        case WGPULogLevel_Trace: return "trace";
        default: return "?";
    }
}

void OnWgpuLog(WGPULogLevel level, WGPUStringView message, void* /*userdata*/) {
    std::string text(
        message.data ? message.data : "",
        message.length == WGPU_STRLEN
            ? (message.data ? strlen(message.data) : 0)
            : message.length);

    std::fprintf(stderr, "[wgpu:%s] %s\n", LogLevelName(level), text.c_str());

    g_recent_wgpu_log += "[";
    g_recent_wgpu_log += LogLevelName(level);
    g_recent_wgpu_log += "] ";
    g_recent_wgpu_log += text;
    g_recent_wgpu_log += "\n";

    constexpr size_t kMaxLogChars = 1500;
    if (g_recent_wgpu_log.size() > kMaxLogChars) {
        g_recent_wgpu_log.erase(0, g_recent_wgpu_log.size() - kMaxLogChars);
    }
}

std::string WithRecentLog(const std::string& message) {
    if (g_recent_wgpu_log.empty()) {
        return message + "\n\n(no wgpu-native log output was captured before this failure)";
    }
    return message + "\n\n--- recent wgpu-native log ---\n" + g_recent_wgpu_log;
}

// Bundles what a callback needs without capturing state in a
// std::function (wgpu-native's C callbacks are plain function
// pointers with a void* userdata pair, not std::function-compatible).
struct AdapterRequestState {
    WGPUAdapter adapter = nullptr;
    bool done = false;
    bool ok = false;
    std::string error;
};

struct DeviceRequestState {
    WGPUDevice device = nullptr;
    bool done = false;
    bool ok = false;
    std::string error;
};

void OnAdapterRequestEnded(
    WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message,
    void* userdata1, void* /*userdata2*/) {
    auto* state = static_cast<AdapterRequestState*>(userdata1);
    state->done = true;
    state->ok = (status == WGPURequestAdapterStatus_Success);
    state->adapter = adapter;
    if (!state->ok && message.data != nullptr) {
        state->error.assign(message.data, message.length == WGPU_STRLEN
                                               ? strlen(message.data)
                                               : message.length);
    }
}

void OnDeviceRequestEnded(
    WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message,
    void* userdata1, void* /*userdata2*/) {
    auto* state = static_cast<DeviceRequestState*>(userdata1);
    state->done = true;
    state->ok = (status == WGPURequestDeviceStatus_Success);
    state->device = device;
    if (!state->ok && message.data != nullptr) {
        state->error.assign(message.data, message.length == WGPU_STRLEN
                                               ? strlen(message.data)
                                               : message.length);
    }
}

void OnUncapturedError(
    WGPUDevice const* /*device*/, WGPUErrorType /*type*/, WGPUStringView message,
    void* userdata1, void* /*userdata2*/) {
    std::string text(
        message.data ? message.data : "",
        message.length == WGPU_STRLEN
            ? (message.data ? strlen(message.data) : 0)
            : message.length);

    std::fprintf(stderr, "[wgpu] uncaptured device error: %s\n", text.c_str());

    // This is where a bad wgpuSurfaceConfigure() call (wrong format,
    // unsupported alpha mode, etc.) actually reports - a very
    // plausible root cause for a window that creates fine, never
    // crashes, but never shows real content.
    auto* gpu = static_cast<GpuContext*>(userdata1);
    if (gpu) {
        gpu->ReportUncapturedDeviceError(text);
    }
}

}  // namespace

GpuContext::~GpuContext() {
    if (surface_) wgpuSurfaceRelease(surface_);
    if (queue_) wgpuQueueRelease(queue_);
    if (device_) wgpuDeviceRelease(device_);
    if (adapter_) wgpuAdapterRelease(adapter_);
    if (instance_) wgpuInstanceRelease(instance_);
}

void GpuContext::SetError(const std::string& message) {
    last_error_ = message;
}

void GpuContext::ReportUncapturedDeviceError(const std::string& message) {
    last_error_ = "wgpu device error: " + message;
    if (device_error_reported_) {
        return;
    }
    device_error_reported_ = true;
    std::fprintf(stderr, "%s\n", WithRecentLog(last_error_).c_str());
}

void GpuContext::PumpUntil(const bool& done) {
    while (!done) {
        wgpuInstanceProcessEvents(instance_);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool GpuContext::Create(GLFWwindow* window, uint32_t width, uint32_t height) {
    // Enable wgpu-native's internal logging before anything else -
    // this is what actually explains a bare WGPUStatus_Error, and it
    // has to be set before wgpuCreateInstance to catch everything.
    wgpuSetLogLevel(WGPULogLevel_Info);
    wgpuSetLogCallback(OnWgpuLog, nullptr);

    WGPUInstanceDescriptor instance_descriptor{};
    instance_descriptor.nextInChain = nullptr;
    instance_descriptor.features.nextInChain = nullptr;
    // Deliberately NOT requesting timedWaitAnyEnable: this
    // wgpu-native build panics with "Unsupported timed WaitAny
    // features specified" if asked for it. Using
    // WGPUCallbackMode_AllowProcessEvents + a wgpuInstanceProcessEvents
    // polling loop (see PumpUntil) sidesteps wgpuInstanceWaitAny
    // entirely, so this isn't needed regardless.
    instance_descriptor.features.timedWaitAnyEnable = false;
    instance_descriptor.features.timedWaitAnyMaxCount = 0;

    instance_ = wgpuCreateInstance(&instance_descriptor);
    if (!instance_) {
        SetError("wgpuCreateInstance returned null");
        return false;
    }

    // --- Platform-specific surface source -------------------------
    //
    // Everything from here to `surface_ = wgpuInstanceCreateSurface`
    // is the one part of this file that genuinely differs per OS -
    // wgpu-native needs a native window handle, and GLFW's handle
    // type (and the accessor for it) is platform-specific.
#if defined(_WIN32)
    WGPUSurfaceSourceWindowsHWND platform_source{};
    platform_source.chain.next = nullptr;
    platform_source.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
    platform_source.hinstance = GetModuleHandleW(nullptr);
    platform_source.hwnd = glfwGetWin32Window(window);
#elif defined(__linux__)
    WGPUSurfaceSourceXlibWindow platform_source{};
    platform_source.chain.next = nullptr;
    platform_source.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
    platform_source.display = glfwGetX11Display();
    platform_source.window = static_cast<uint64_t>(glfwGetX11Window(window));
#else
#error "gpu_context.cpp only implements Windows and Linux surface creation"
#endif

    WGPUSurfaceDescriptor surface_descriptor{};
    surface_descriptor.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&platform_source);
    surface_descriptor.label = ToStringView("AppShellSurface");

    surface_ = wgpuInstanceCreateSurface(instance_, &surface_descriptor);
    if (!surface_) {
        SetError("wgpuInstanceCreateSurface returned null");
        return false;
    }

    WGPURequestAdapterOptions adapter_options{};
    adapter_options.nextInChain = nullptr;
    adapter_options.featureLevel = WGPUFeatureLevel_Core;
    adapter_options.powerPreference = WGPUPowerPreference_HighPerformance;
    adapter_options.forceFallbackAdapter = false;
    adapter_options.backendType = WGPUBackendType_Undefined;
    adapter_options.compatibleSurface = surface_;

    AdapterRequestState adapter_state;

    WGPURequestAdapterCallbackInfo adapter_callback_info{};
    adapter_callback_info.nextInChain = nullptr;
    adapter_callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    adapter_callback_info.callback = OnAdapterRequestEnded;
    adapter_callback_info.userdata1 = &adapter_state;
    adapter_callback_info.userdata2 = nullptr;

    // Prefer the platform's most mature backend first (D3D12 on
    // Windows, Vulkan on Linux); fall back to whatever wgpu-native
    // picks automatically if that specific backend genuinely isn't
    // available (older GPU/driver, missing Vulkan ICD, etc).
#if defined(_WIN32)
    adapter_options.backendType = WGPUBackendType_D3D12;
#elif defined(__linux__)
    adapter_options.backendType = WGPUBackendType_Vulkan;
#endif
    wgpuInstanceRequestAdapter(instance_, &adapter_options, adapter_callback_info);
    PumpUntil(adapter_state.done);

    if (!adapter_state.ok) {
        adapter_state = AdapterRequestState{};
        adapter_options.backendType = WGPUBackendType_Undefined;
        wgpuInstanceRequestAdapter(instance_, &adapter_options, adapter_callback_info);
        PumpUntil(adapter_state.done);
    }

    if (!adapter_state.ok) {
        SetError("wgpuInstanceRequestAdapter failed: " + adapter_state.error);
        return false;
    }

    adapter_ = adapter_state.adapter;

    DeviceRequestState device_state;

    WGPUUncapturedErrorCallbackInfo error_callback_info{};
    error_callback_info.nextInChain = nullptr;
    error_callback_info.callback = OnUncapturedError;
    error_callback_info.userdata1 = this;
    error_callback_info.userdata2 = nullptr;

    WGPUDeviceDescriptor device_descriptor{};
    device_descriptor.nextInChain = nullptr;
    device_descriptor.label = ToStringView("AppShellDevice");
    device_descriptor.requiredFeatureCount = 0;
    device_descriptor.requiredFeatures = nullptr;
    device_descriptor.requiredLimits = nullptr;
    device_descriptor.defaultQueue.nextInChain = nullptr;
    device_descriptor.defaultQueue.label = ToStringView("AppShellQueue");
    device_descriptor.uncapturedErrorCallbackInfo = error_callback_info;
    // Leave deviceLostCallbackInfo zero-initialized (no callback) -
    // fine to start with; wire it up once you have somewhere real to
    // surface "the GPU device died" to the user.

    WGPURequestDeviceCallbackInfo device_callback_info{};
    device_callback_info.nextInChain = nullptr;
    device_callback_info.mode = WGPUCallbackMode_AllowProcessEvents;
    device_callback_info.callback = OnDeviceRequestEnded;
    device_callback_info.userdata1 = &device_state;
    device_callback_info.userdata2 = nullptr;

    wgpuAdapterRequestDevice(adapter_, &device_descriptor, device_callback_info);
    PumpUntil(device_state.done);

    if (!device_state.ok) {
        SetError("wgpuAdapterRequestDevice failed: " + device_state.error);
        return false;
    }

    device_ = device_state.device;
    queue_ = wgpuDeviceGetQueue(device_);

    WGPUSurfaceCapabilities capabilities{};
    if (wgpuSurfaceGetCapabilities(surface_, adapter_, &capabilities) == WGPUStatus_Success) {
        if (capabilities.formatCount > 0) {
            surface_format_ = capabilities.formats[0];
        }
        if (capabilities.alphaModeCount > 0) {
            surface_alpha_mode_ = capabilities.alphaModes[0];
        }
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    }
    // Falls back to the WGPUTextureFormat_BGRA8Unorm /
    // WGPUCompositeAlphaMode_Opaque defaults set above if
    // capabilities couldn't be queried - safe, widely-supported bets.

    width_ = width;
    height_ = height;

    Resize(width_, height_);

    return surface_configured_;
}

void GpuContext::Resize(uint32_t width, uint32_t height) {
    if (!surface_ || !device_ || width == 0 || height == 0) {
        return;
    }

    width_ = width;
    height_ = height;

    WGPUSurfaceConfiguration configuration{};
    configuration.nextInChain = nullptr;
    configuration.device = device_;
    configuration.format = surface_format_;
    configuration.usage = WGPUTextureUsage_RenderAttachment;
    configuration.width = width_;
    configuration.height = height_;
    configuration.viewFormatCount = 0;
    configuration.viewFormats = nullptr;
    configuration.alphaMode = surface_alpha_mode_;
    configuration.presentMode = WGPUPresentMode_Fifo;

    wgpuSurfaceConfigure(surface_, &configuration);

    surface_configured_ = true;
}

bool GpuContext::RenderFrame(double r, double g, double b, double a,
                              const std::function<void(WGPURenderPassEncoder)>& draw_ui) {
    if (!surface_configured_) {
        SetError("RenderFrame called before a successful Create()/Resize()");
        return false;
    }

    WGPUSurfaceTexture surface_texture{};
    wgpuSurfaceGetCurrentTexture(surface_, &surface_texture);

    if (surface_texture.status == WGPUSurfaceGetCurrentTextureStatus_Outdated) {
        // Standard wgpu recovery pattern: reconfigure at the current
        // size and try exactly once more before treating it as a
        // real failure.
        if (surface_texture.texture) {
            wgpuTextureRelease(surface_texture.texture);
        }
        Resize(width_, height_);
        wgpuSurfaceGetCurrentTexture(surface_, &surface_texture);
    }

    if (surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
        && surface_texture.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        if (surface_texture.status != last_logged_status_) {
            SetError("wgpuSurfaceGetCurrentTexture did not return a usable texture (status="
                      + std::to_string(static_cast<int>(surface_texture.status)) + ")");
            std::fprintf(stderr, "%s\n", last_error_.c_str());
            last_logged_status_ = surface_texture.status;
        }
        if (!render_failure_reported_) {
            render_failure_reported_ = true;
            std::fprintf(stderr, "%s\n", WithRecentLog(last_error_).c_str());
        }
        return false;
    }
    last_logged_status_ = WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal;

    WGPUTextureViewDescriptor view_descriptor{};
    view_descriptor.nextInChain = nullptr;
    view_descriptor.label = ToStringView("AppShellFrameView");
    view_descriptor.format = surface_format_;
    view_descriptor.dimension = WGPUTextureViewDimension_2D;
    view_descriptor.baseMipLevel = 0;
    view_descriptor.mipLevelCount = 1;
    view_descriptor.baseArrayLayer = 0;
    view_descriptor.arrayLayerCount = 1;
    view_descriptor.aspect = WGPUTextureAspect_All;
    view_descriptor.usage = WGPUTextureUsage_RenderAttachment;

    WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, &view_descriptor);

    WGPURenderPassColorAttachment color_attachment{};
    color_attachment.nextInChain = nullptr;
    color_attachment.view = view;
    color_attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color_attachment.resolveTarget = nullptr;
    color_attachment.loadOp = WGPULoadOp_Clear;
    color_attachment.storeOp = WGPUStoreOp_Store;
    color_attachment.clearValue = WGPUColor{r, g, b, a};

    WGPURenderPassDescriptor pass_descriptor{};
    pass_descriptor.nextInChain = nullptr;
    pass_descriptor.label = ToStringView("AppShellUiPass");
    pass_descriptor.colorAttachmentCount = 1;
    pass_descriptor.colorAttachments = &color_attachment;
    pass_descriptor.depthStencilAttachment = nullptr;
    pass_descriptor.occlusionQuerySet = nullptr;
    pass_descriptor.timestampWrites = nullptr;

    WGPUCommandEncoderDescriptor encoder_descriptor{};
    encoder_descriptor.nextInChain = nullptr;
    encoder_descriptor.label = ToStringView("AppShellFrameEncoder");

    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device_, &encoder_descriptor);

    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
    if (draw_ui) {
        draw_ui(pass);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBufferDescriptor command_buffer_descriptor{};
    command_buffer_descriptor.nextInChain = nullptr;
    command_buffer_descriptor.label = ToStringView("AppShellFrameCommands");

    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, &command_buffer_descriptor);

    wgpuQueueSubmit(queue_, 1, &commands);

    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    WGPUStatus present_status = wgpuSurfacePresent(surface_);

    wgpuTextureViewRelease(view);
    wgpuTextureRelease(surface_texture.texture);

    if (present_status != WGPUStatus_Success && !render_failure_reported_) {
        render_failure_reported_ = true;
        SetError("wgpuSurfacePresent failed (status="
                  + std::to_string(static_cast<int>(present_status)) + ")");
        std::fprintf(stderr, "%s\n", WithRecentLog(last_error_).c_str());
    }

    return true;
}

}  // namespace appshell
