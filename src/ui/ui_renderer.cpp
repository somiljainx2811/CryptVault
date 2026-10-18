// ui_renderer.cpp - see ui_renderer.h for scope notes.

#include "ui/bidi.h"
#include "ui/unicode_font.h"
#include "ui/ui_renderer.h"

#include <algorithm>
#include <memory>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
// PNG/JPEG (icons/most backgrounds) and GIF (animated backgrounds -
// see LoadAnimatedImage) - every other stb_image-supported decoder
// this app doesn't use (BMP, PSD, TGA, HDR, PIC, PNM, ...) stays
// compiled out. JPEG is here specifically because the background
// picker's file filter (see DrawSettingsPanel/platform::PickFile)
// offers .jpg/.jpeg - offering a format the decoder itself couldn't
// actually read would be its own bug.
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO_WARNING
#include "stb_image.h"

#include "ui/font5x7.h"

namespace cryptvault {

namespace {

WGPUStringView ToStringView(const char* text) {
    return WGPUStringView{text, WGPU_STRLEN};
}

// One pipeline, no textures: position already in NDC (converted on
// the CPU side per-vertex in PushVertex) plus a per-vertex color,
// passed straight through to the fragment shader. This is deliberately
// as simple as a render pipeline gets - Phase 2c's job is "get
// rects/text/a button on screen", not build a general shader system.
constexpr const char* kShaderSource = R"(
struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) color: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.clip_position = vec4<f32>(in.position, 0.0, 1.0);
    out.color = in.color;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    return in.color;
}
)";

}  // namespace

UiRenderer::~UiRenderer() {
    if (vertex_buffer_) {
        wgpuBufferRelease(vertex_buffer_);
    }
    if (pipeline_) {
        wgpuRenderPipelineRelease(pipeline_);
    }
    if (pipeline_layout_) {
        wgpuPipelineLayoutRelease(pipeline_layout_);
    }
    if (shader_module_) {
        wgpuShaderModuleRelease(shader_module_);
    }

    for (IconTexture& icon : icons_) {
        if (icon.bind_group) wgpuBindGroupRelease(icon.bind_group);
        if (icon.view) wgpuTextureViewRelease(icon.view);
        if (icon.texture) wgpuTextureRelease(icon.texture);
    }
    if (sampler_) {
        wgpuSamplerRelease(sampler_);
    }
    if (image_vertex_buffer_) {
        wgpuBufferRelease(image_vertex_buffer_);
    }
    if (image_pipeline_) {
        wgpuRenderPipelineRelease(image_pipeline_);
    }
    if (image_pipeline_layout_) {
        wgpuPipelineLayoutRelease(image_pipeline_layout_);
    }
    if (image_bind_group_layout_) {
        wgpuBindGroupLayoutRelease(image_bind_group_layout_);
    }
    if (image_shader_module_) {
        wgpuShaderModuleRelease(image_shader_module_);
    }

    if (rounded_rect_vertex_buffer_) {
        wgpuBufferRelease(rounded_rect_vertex_buffer_);
    }
    if (rounded_rect_pipeline_) {
        wgpuRenderPipelineRelease(rounded_rect_pipeline_);
    }
    if (rounded_rect_pipeline_layout_) {
        wgpuPipelineLayoutRelease(rounded_rect_pipeline_layout_);
    }
    if (rounded_rect_shader_module_) {
        wgpuShaderModuleRelease(rounded_rect_shader_module_);
    }
}

bool UiRenderer::Create(WGPUDevice device, WGPUQueue queue, WGPUTextureFormat surface_format) {
    device_ = device;
    queue_ = queue;
    surface_format_ = surface_format;

    WGPUShaderSourceWGSL wgsl_source{};
    wgsl_source.chain.next = nullptr;
    wgsl_source.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_source.code = ToStringView(kShaderSource);

    WGPUShaderModuleDescriptor shader_descriptor{};
    shader_descriptor.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgsl_source);
    shader_descriptor.label = ToStringView("CryptVaultUiShader");

    shader_module_ = wgpuDeviceCreateShaderModule(device_, &shader_descriptor);
    if (!shader_module_) {
        last_error_ = "wgpuDeviceCreateShaderModule failed for the UI shader";
        return false;
    }

    WGPUPipelineLayoutDescriptor layout_descriptor{};
    layout_descriptor.nextInChain = nullptr;
    layout_descriptor.label = ToStringView("CryptVaultUiPipelineLayout");
    layout_descriptor.bindGroupLayoutCount = 0;
    layout_descriptor.bindGroupLayouts = nullptr;

    pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device_, &layout_descriptor);
    if (!pipeline_layout_) {
        last_error_ = "wgpuDeviceCreatePipelineLayout failed for the UI pipeline";
        return false;
    }

    WGPUVertexAttribute attributes[2];
    attributes[0].format = WGPUVertexFormat_Float32x2;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Float32x4;
    attributes[1].offset = sizeof(float) * 2;
    attributes[1].shaderLocation = 1;

    WGPUVertexBufferLayout vertex_buffer_layout{};
    vertex_buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_buffer_layout.arrayStride = sizeof(Vertex);
    vertex_buffer_layout.attributeCount = 2;
    vertex_buffer_layout.attributes = attributes;

    WGPUBlendComponent color_blend{};
    color_blend.operation = WGPUBlendOperation_Add;
    color_blend.srcFactor = WGPUBlendFactor_SrcAlpha;
    color_blend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUBlendComponent alpha_blend{};
    alpha_blend.operation = WGPUBlendOperation_Add;
    alpha_blend.srcFactor = WGPUBlendFactor_SrcAlpha;
    alpha_blend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUBlendState blend_state{};
    blend_state.color = color_blend;
    blend_state.alpha = alpha_blend;

    WGPUColorTargetState color_target{};
    color_target.nextInChain = nullptr;
    color_target.format = surface_format_;
    color_target.blend = &blend_state;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment_state{};
    fragment_state.nextInChain = nullptr;
    fragment_state.module = shader_module_;
    fragment_state.entryPoint = ToStringView("fs_main");
    fragment_state.constantCount = 0;
    fragment_state.constants = nullptr;
    fragment_state.targetCount = 1;
    fragment_state.targets = &color_target;

    WGPURenderPipelineDescriptor pipeline_descriptor{};
    pipeline_descriptor.nextInChain = nullptr;
    pipeline_descriptor.label = ToStringView("CryptVaultUiPipeline");
    pipeline_descriptor.layout = pipeline_layout_;
    pipeline_descriptor.vertex.nextInChain = nullptr;
    pipeline_descriptor.vertex.module = shader_module_;
    pipeline_descriptor.vertex.entryPoint = ToStringView("vs_main");
    pipeline_descriptor.vertex.constantCount = 0;
    pipeline_descriptor.vertex.constants = nullptr;
    pipeline_descriptor.vertex.bufferCount = 1;
    pipeline_descriptor.vertex.buffers = &vertex_buffer_layout;
    pipeline_descriptor.primitive.nextInChain = nullptr;
    pipeline_descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_descriptor.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
    pipeline_descriptor.primitive.frontFace = WGPUFrontFace_CCW;
    pipeline_descriptor.primitive.cullMode = WGPUCullMode_None;
    pipeline_descriptor.primitive.unclippedDepth = false;
    pipeline_descriptor.depthStencil = nullptr;
    pipeline_descriptor.multisample.nextInChain = nullptr;
    pipeline_descriptor.multisample.count = 1;
    pipeline_descriptor.multisample.mask = ~0u;
    pipeline_descriptor.multisample.alphaToCoverageEnabled = false;
    pipeline_descriptor.fragment = &fragment_state;

    pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &pipeline_descriptor);
    if (!pipeline_) {
        last_error_ = "wgpuDeviceCreateRenderPipeline failed for the UI pipeline";
        return false;
    }

    // Small initial capacity - EnsureVertexCapacity grows it (by
    // recreating the buffer) the first time a frame needs more than
    // this, so this number only affects how many frames pay a
    // reallocation cost early on, not correctness.
    EnsureVertexCapacity(1024);

    if (!CreateImagePipeline()) {
        return false;
    }

    if (!CreateRoundedRectPipeline()) {
        return false;
    }

    return true;
}

namespace {

// Same NDC-passthrough approach as the solid-color shader, but each
// vertex also carries a UV; the fragment shader samples `tex` and
// multiplies by the per-vertex color so DrawImage can tint (e.g. for
// hover states) without a second texture.
constexpr const char* kImageShaderSource = R"(
struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) color: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.clip_position = vec4<f32>(in.position, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    return out;
}

@group(0) @binding(0) var icon_texture: texture_2d<f32>;
@group(0) @binding(1) var icon_sampler: sampler;

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    let sampled = textureSample(icon_texture, icon_sampler, in.uv);
    return sampled * in.color;
}
)";

}  // namespace

bool UiRenderer::CreateImagePipeline() {
    WGPUShaderSourceWGSL wgsl_source{};
    wgsl_source.chain.next = nullptr;
    wgsl_source.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_source.code = ToStringView(kImageShaderSource);

    WGPUShaderModuleDescriptor shader_descriptor{};
    shader_descriptor.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgsl_source);
    shader_descriptor.label = ToStringView("CryptVaultUiImageShader");

    image_shader_module_ = wgpuDeviceCreateShaderModule(device_, &shader_descriptor);
    if (!image_shader_module_) {
        last_error_ = "wgpuDeviceCreateShaderModule failed for the image shader";
        return false;
    }

    WGPUBindGroupLayoutEntry bind_layout_entries[2]{};
    bind_layout_entries[0].binding = 0;
    bind_layout_entries[0].visibility = WGPUShaderStage_Fragment;
    bind_layout_entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    bind_layout_entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    bind_layout_entries[0].texture.multisampled = false;

    bind_layout_entries[1].binding = 1;
    bind_layout_entries[1].visibility = WGPUShaderStage_Fragment;
    bind_layout_entries[1].sampler.type = WGPUSamplerBindingType_Filtering;

    WGPUBindGroupLayoutDescriptor bind_group_layout_descriptor{};
    bind_group_layout_descriptor.nextInChain = nullptr;
    bind_group_layout_descriptor.label = ToStringView("CryptVaultUiImageBindGroupLayout");
    bind_group_layout_descriptor.entryCount = 2;
    bind_group_layout_descriptor.entries = bind_layout_entries;

    image_bind_group_layout_ = wgpuDeviceCreateBindGroupLayout(device_, &bind_group_layout_descriptor);
    if (!image_bind_group_layout_) {
        last_error_ = "wgpuDeviceCreateBindGroupLayout failed for the image pipeline";
        return false;
    }

    WGPUPipelineLayoutDescriptor layout_descriptor{};
    layout_descriptor.nextInChain = nullptr;
    layout_descriptor.label = ToStringView("CryptVaultUiImagePipelineLayout");
    layout_descriptor.bindGroupLayoutCount = 1;
    layout_descriptor.bindGroupLayouts = &image_bind_group_layout_;

    image_pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device_, &layout_descriptor);
    if (!image_pipeline_layout_) {
        last_error_ = "wgpuDeviceCreatePipelineLayout failed for the image pipeline";
        return false;
    }

    WGPUVertexAttribute attributes[3];
    attributes[0].format = WGPUVertexFormat_Float32x2;
    attributes[0].offset = 0;
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Float32x2;
    attributes[1].offset = sizeof(float) * 2;
    attributes[1].shaderLocation = 1;
    attributes[2].format = WGPUVertexFormat_Float32x4;
    attributes[2].offset = sizeof(float) * 4;
    attributes[2].shaderLocation = 2;

    WGPUVertexBufferLayout vertex_buffer_layout{};
    vertex_buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_buffer_layout.arrayStride = sizeof(ImageVertex);
    vertex_buffer_layout.attributeCount = 3;
    vertex_buffer_layout.attributes = attributes;

    // Same straight-alpha over-blend as the solid pipeline, so PNGs
    // with transparent backgrounds (every icon here) composite
    // correctly over whatever was drawn underneath.
    WGPUBlendComponent color_blend{};
    color_blend.operation = WGPUBlendOperation_Add;
    color_blend.srcFactor = WGPUBlendFactor_SrcAlpha;
    color_blend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUBlendComponent alpha_blend{};
    alpha_blend.operation = WGPUBlendOperation_Add;
    alpha_blend.srcFactor = WGPUBlendFactor_SrcAlpha;
    alpha_blend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUBlendState blend_state{};
    blend_state.color = color_blend;
    blend_state.alpha = alpha_blend;

    WGPUColorTargetState color_target{};
    color_target.nextInChain = nullptr;
    color_target.format = surface_format_;
    color_target.blend = &blend_state;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment_state{};
    fragment_state.nextInChain = nullptr;
    fragment_state.module = image_shader_module_;
    fragment_state.entryPoint = ToStringView("fs_main");
    fragment_state.constantCount = 0;
    fragment_state.constants = nullptr;
    fragment_state.targetCount = 1;
    fragment_state.targets = &color_target;

    WGPURenderPipelineDescriptor pipeline_descriptor{};
    pipeline_descriptor.nextInChain = nullptr;
    pipeline_descriptor.label = ToStringView("CryptVaultUiImagePipeline");
    pipeline_descriptor.layout = image_pipeline_layout_;
    pipeline_descriptor.vertex.nextInChain = nullptr;
    pipeline_descriptor.vertex.module = image_shader_module_;
    pipeline_descriptor.vertex.entryPoint = ToStringView("vs_main");
    pipeline_descriptor.vertex.constantCount = 0;
    pipeline_descriptor.vertex.constants = nullptr;
    pipeline_descriptor.vertex.bufferCount = 1;
    pipeline_descriptor.vertex.buffers = &vertex_buffer_layout;
    pipeline_descriptor.primitive.nextInChain = nullptr;
    pipeline_descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_descriptor.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
    pipeline_descriptor.primitive.frontFace = WGPUFrontFace_CCW;
    pipeline_descriptor.primitive.cullMode = WGPUCullMode_None;
    pipeline_descriptor.primitive.unclippedDepth = false;
    pipeline_descriptor.depthStencil = nullptr;
    pipeline_descriptor.multisample.nextInChain = nullptr;
    pipeline_descriptor.multisample.count = 1;
    pipeline_descriptor.multisample.mask = ~0u;
    pipeline_descriptor.multisample.alphaToCoverageEnabled = false;
    pipeline_descriptor.fragment = &fragment_state;

    image_pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &pipeline_descriptor);
    if (!image_pipeline_) {
        last_error_ = "wgpuDeviceCreateRenderPipeline failed for the image pipeline";
        return false;
    }

    WGPUSamplerDescriptor sampler_descriptor{};
    sampler_descriptor.nextInChain = nullptr;
    sampler_descriptor.label = ToStringView("CryptVaultUiIconSampler");
    sampler_descriptor.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler_descriptor.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler_descriptor.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler_descriptor.magFilter = WGPUFilterMode_Linear;
    sampler_descriptor.minFilter = WGPUFilterMode_Linear;
    sampler_descriptor.mipmapFilter = WGPUMipmapFilterMode_Linear;
    sampler_descriptor.lodMinClamp = 0.0f;
    sampler_descriptor.lodMaxClamp = 1.0f;
    sampler_descriptor.compare = WGPUCompareFunction_Undefined;
    sampler_descriptor.maxAnisotropy = 1;

    sampler_ = wgpuDeviceCreateSampler(device_, &sampler_descriptor);
    if (!sampler_) {
        last_error_ = "wgpuDeviceCreateSampler failed";
        return false;
    }

    return true;
}

namespace {

// Renders one rectangle-with-rounded-corners quad per draw call's
// worth of geometry. Instead of assembling the corners out of
// several axis-aligned DrawRect() calls (which only ever produces a
// staircase, no matter how many strips you add), each vertex carries
// its offset from the rect's center in pixels plus the rect's
// half-size and radius; the fragment shader evaluates a signed
// distance to a true rounded-box outline (Inigo Quilez's
// round-box SDF: shrink the box by `radius`, measure distance to
// that inner box, then subtract `radius` back out) and feathers a
// ~1px band around the zero-crossing with smoothstep() for
// anti-aliasing. That gives a smooth curve at any radius/scale, not
// an approximation built from rectangles.
constexpr const char* kRoundedRectShaderSource = R"(
struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) local_pos: vec2<f32>,
    @location(2) half_size: vec2<f32>,
    @location(3) radius: f32,
    @location(4) color: vec4<f32>,
};

struct VertexOutput {
    @builtin(position) clip_position: vec4<f32>,
    @location(0) local_pos: vec2<f32>,
    @location(1) half_size: vec2<f32>,
    @location(2) radius: f32,
    @location(3) color: vec4<f32>,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    out.clip_position = vec4<f32>(in.position, 0.0, 1.0);
    out.local_pos = in.local_pos;
    out.half_size = in.half_size;
    out.radius = in.radius;
    out.color = in.color;
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    // Distance from this pixel to the rounded-box outline: shrink
    // the half-extents by the radius, clamp the pixel's offset into
    // that inner box, then the remaining distance (inside or
    // outside) minus the radius is the signed distance to the
    // rounded outline. Negative = inside, positive = outside.
    let inner_half_size = in.half_size - vec2<f32>(in.radius, in.radius);
    let q = abs(in.local_pos) - inner_half_size;
    let outside_dist = length(max(q, vec2<f32>(0.0, 0.0)));
    let inside_dist = min(max(q.x, q.y), 0.0);
    let dist = outside_dist + inside_dist - in.radius;

    // ~1px feather centered on the zero-crossing turns the binary
    // inside/outside test into anti-aliased coverage.
    let aa = 1.0;
    let coverage = 1.0 - smoothstep(-aa, aa, dist);
    return vec4<f32>(in.color.rgb, in.color.a * coverage);
}
)";

}  // namespace

bool UiRenderer::CreateRoundedRectPipeline() {
    WGPUShaderSourceWGSL wgsl_source{};
    wgsl_source.chain.next = nullptr;
    wgsl_source.chain.sType = WGPUSType_ShaderSourceWGSL;
    wgsl_source.code = ToStringView(kRoundedRectShaderSource);

    WGPUShaderModuleDescriptor shader_descriptor{};
    shader_descriptor.nextInChain = reinterpret_cast<WGPUChainedStruct*>(&wgsl_source);
    shader_descriptor.label = ToStringView("CryptVaultUiRoundedRectShader");

    rounded_rect_shader_module_ = wgpuDeviceCreateShaderModule(device_, &shader_descriptor);
    if (!rounded_rect_shader_module_) {
        last_error_ = "wgpuDeviceCreateShaderModule failed for the rounded-rect shader";
        return false;
    }

    // No textures/uniforms - same empty layout as the solid pipeline.
    WGPUPipelineLayoutDescriptor layout_descriptor{};
    layout_descriptor.nextInChain = nullptr;
    layout_descriptor.label = ToStringView("CryptVaultUiRoundedRectPipelineLayout");
    layout_descriptor.bindGroupLayoutCount = 0;
    layout_descriptor.bindGroupLayouts = nullptr;

    rounded_rect_pipeline_layout_ = wgpuDeviceCreatePipelineLayout(device_, &layout_descriptor);
    if (!rounded_rect_pipeline_layout_) {
        last_error_ = "wgpuDeviceCreatePipelineLayout failed for the rounded-rect pipeline";
        return false;
    }

    WGPUVertexAttribute attributes[5];
    attributes[0].format = WGPUVertexFormat_Float32x2;
    attributes[0].offset = offsetof(RoundedRectVertex, x);
    attributes[0].shaderLocation = 0;
    attributes[1].format = WGPUVertexFormat_Float32x2;
    attributes[1].offset = offsetof(RoundedRectVertex, local_x);
    attributes[1].shaderLocation = 1;
    attributes[2].format = WGPUVertexFormat_Float32x2;
    attributes[2].offset = offsetof(RoundedRectVertex, half_w);
    attributes[2].shaderLocation = 2;
    attributes[3].format = WGPUVertexFormat_Float32;
    attributes[3].offset = offsetof(RoundedRectVertex, radius);
    attributes[3].shaderLocation = 3;
    attributes[4].format = WGPUVertexFormat_Float32x4;
    attributes[4].offset = offsetof(RoundedRectVertex, r);
    attributes[4].shaderLocation = 4;

    WGPUVertexBufferLayout vertex_buffer_layout{};
    vertex_buffer_layout.stepMode = WGPUVertexStepMode_Vertex;
    vertex_buffer_layout.arrayStride = sizeof(RoundedRectVertex);
    vertex_buffer_layout.attributeCount = 5;
    vertex_buffer_layout.attributes = attributes;

    WGPUBlendComponent color_blend{};
    color_blend.operation = WGPUBlendOperation_Add;
    color_blend.srcFactor = WGPUBlendFactor_SrcAlpha;
    color_blend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUBlendComponent alpha_blend{};
    alpha_blend.operation = WGPUBlendOperation_Add;
    alpha_blend.srcFactor = WGPUBlendFactor_SrcAlpha;
    alpha_blend.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;

    WGPUBlendState blend_state{};
    blend_state.color = color_blend;
    blend_state.alpha = alpha_blend;

    WGPUColorTargetState color_target{};
    color_target.nextInChain = nullptr;
    color_target.format = surface_format_;
    color_target.blend = &blend_state;
    color_target.writeMask = WGPUColorWriteMask_All;

    WGPUFragmentState fragment_state{};
    fragment_state.nextInChain = nullptr;
    fragment_state.module = rounded_rect_shader_module_;
    fragment_state.entryPoint = ToStringView("fs_main");
    fragment_state.constantCount = 0;
    fragment_state.constants = nullptr;
    fragment_state.targetCount = 1;
    fragment_state.targets = &color_target;

    WGPURenderPipelineDescriptor pipeline_descriptor{};
    pipeline_descriptor.nextInChain = nullptr;
    pipeline_descriptor.label = ToStringView("CryptVaultUiRoundedRectPipeline");
    pipeline_descriptor.layout = rounded_rect_pipeline_layout_;
    pipeline_descriptor.vertex.nextInChain = nullptr;
    pipeline_descriptor.vertex.module = rounded_rect_shader_module_;
    pipeline_descriptor.vertex.entryPoint = ToStringView("vs_main");
    pipeline_descriptor.vertex.constantCount = 0;
    pipeline_descriptor.vertex.constants = nullptr;
    pipeline_descriptor.vertex.bufferCount = 1;
    pipeline_descriptor.vertex.buffers = &vertex_buffer_layout;
    pipeline_descriptor.primitive.nextInChain = nullptr;
    pipeline_descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_descriptor.primitive.stripIndexFormat = WGPUIndexFormat_Undefined;
    pipeline_descriptor.primitive.frontFace = WGPUFrontFace_CCW;
    pipeline_descriptor.primitive.cullMode = WGPUCullMode_None;
    pipeline_descriptor.primitive.unclippedDepth = false;
    pipeline_descriptor.depthStencil = nullptr;
    pipeline_descriptor.multisample.nextInChain = nullptr;
    pipeline_descriptor.multisample.count = 1;
    pipeline_descriptor.multisample.mask = ~0u;
    pipeline_descriptor.multisample.alphaToCoverageEnabled = false;
    pipeline_descriptor.fragment = &fragment_state;

    rounded_rect_pipeline_ = wgpuDeviceCreateRenderPipeline(device_, &pipeline_descriptor);
    if (!rounded_rect_pipeline_) {
        last_error_ = "wgpuDeviceCreateRenderPipeline failed for the rounded-rect pipeline";
        return false;
    }

    return true;
}

int UiRenderer::UploadTexture(const unsigned char* pixels, int width, int height, const std::string& label) {
    WGPUTextureDescriptor texture_descriptor{};
    texture_descriptor.nextInChain = nullptr;
    texture_descriptor.label = ToStringView("CryptVaultUiIconTexture");
    texture_descriptor.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    texture_descriptor.dimension = WGPUTextureDimension_2D;
    texture_descriptor.size = WGPUExtent3D{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    texture_descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    texture_descriptor.mipLevelCount = 1;
    texture_descriptor.sampleCount = 1;
    texture_descriptor.viewFormatCount = 0;
    texture_descriptor.viewFormats = nullptr;

    WGPUTexture texture = wgpuDeviceCreateTexture(device_, &texture_descriptor);
    if (!texture) {
        last_error_ = "wgpuDeviceCreateTexture failed for '" + label + "'";
        return -1;
    }

    WGPUTexelCopyTextureInfo copy_destination{};
    copy_destination.texture = texture;
    copy_destination.mipLevel = 0;
    copy_destination.origin = WGPUOrigin3D{0, 0, 0};
    copy_destination.aspect = WGPUTextureAspect_All;

    WGPUTexelCopyBufferLayout data_layout{};
    data_layout.offset = 0;
    data_layout.bytesPerRow = static_cast<uint32_t>(width) * 4;
    data_layout.rowsPerImage = static_cast<uint32_t>(height);

    WGPUExtent3D write_size{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};

    size_t data_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    wgpuQueueWriteTexture(queue_, &copy_destination, pixels, data_size, &data_layout, &write_size);

    WGPUTextureViewDescriptor view_descriptor{};
    view_descriptor.nextInChain = nullptr;
    view_descriptor.label = ToStringView("CryptVaultUiIconTextureView");
    view_descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    view_descriptor.dimension = WGPUTextureViewDimension_2D;
    view_descriptor.baseMipLevel = 0;
    view_descriptor.mipLevelCount = 1;
    view_descriptor.baseArrayLayer = 0;
    view_descriptor.arrayLayerCount = 1;
    view_descriptor.aspect = WGPUTextureAspect_All;
    view_descriptor.usage = WGPUTextureUsage_TextureBinding;

    WGPUTextureView view = wgpuTextureCreateView(texture, &view_descriptor);
    if (!view) {
        wgpuTextureRelease(texture);
        last_error_ = "wgpuTextureCreateView failed for '" + label + "'";
        return -1;
    }

    WGPUBindGroupEntry bind_entries[2]{};
    bind_entries[0].binding = 0;
    bind_entries[0].textureView = view;
    bind_entries[1].binding = 1;
    bind_entries[1].sampler = sampler_;

    WGPUBindGroupDescriptor bind_group_descriptor{};
    bind_group_descriptor.nextInChain = nullptr;
    bind_group_descriptor.label = ToStringView("CryptVaultUiIconBindGroup");
    bind_group_descriptor.layout = image_bind_group_layout_;
    bind_group_descriptor.entryCount = 2;
    bind_group_descriptor.entries = bind_entries;

    WGPUBindGroup bind_group = wgpuDeviceCreateBindGroup(device_, &bind_group_descriptor);
    if (!bind_group) {
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(texture);
        last_error_ = "wgpuDeviceCreateBindGroup failed for '" + label + "'";
        return -1;
    }

    IconTexture icon;
    icon.width = width;
    icon.height = height;
    icon.texture = texture;
    icon.view = view;
    icon.bind_group = bind_group;
    if (!free_icon_slots_.empty()) {  // reuse a released slot so handles don't grow without bound
        int slot = free_icon_slots_.back();
        free_icon_slots_.pop_back();
        icons_[slot] = icon;
        return slot;
    }
    icons_.push_back(icon);
    return static_cast<int>(icons_.size()) - 1;
}

bool DecodeThumbnailRgba(const std::string& path, int box, std::vector<unsigned char>& out) {
    int width = 0, height = 0, comp = 0;
    // Check the header first so a huge or non-image file is rejected cheaply.
    if (!stbi_info(path.c_str(), &width, &height, &comp) || width <= 0 || height <= 0
        || static_cast<int64_t>(width) * height > 60000000) {
        return false;
    }
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &comp, 4);
    if (!pixels) return false;

    const float scale = static_cast<float>(box) / static_cast<float>(std::max(width, height));
    const int tw = std::max(1, std::min(box, static_cast<int>(width * scale + 0.5f)));
    const int th = std::max(1, std::min(box, static_cast<int>(height * scale + 0.5f)));
    out.assign(static_cast<size_t>(box) * box * 4, 0);
    const int ox = (box - tw) / 2, oy = (box - th) / 2;
    for (int y = 0; y < th; ++y) {
        const int sy0 = y * height / th;
        const int sy1 = std::max(sy0 + 1, (y + 1) * height / th);
        for (int x = 0; x < tw; ++x) {
            const int sx0 = x * width / tw;
            const int sx1 = std::max(sx0 + 1, (x + 1) * width / tw);
            // Box filter, alpha-weighted so transparent pixels don't darken edges.
            uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int sy = sy0; sy < sy1 && sy < height; ++sy) {
                for (int sx = sx0; sx < sx1 && sx < width; ++sx) {
                    const stbi_uc* p = pixels + (static_cast<size_t>(sy) * width + sx) * 4;
                    r += static_cast<uint64_t>(p[0]) * p[3];
                    g += static_cast<uint64_t>(p[1]) * p[3];
                    b += static_cast<uint64_t>(p[2]) * p[3];
                    a += p[3];
                    ++n;
                }
            }
            unsigned char* d = &out[(static_cast<size_t>(y + oy) * box + (x + ox)) * 4];
            if (a > 0) {
                d[0] = static_cast<unsigned char>(r / a);
                d[1] = static_cast<unsigned char>(g / a);
                d[2] = static_cast<unsigned char>(b / a);
                d[3] = static_cast<unsigned char>(a / std::max<uint64_t>(n, 1));
            }
        }
    }
    stbi_image_free(pixels);
    return true;
}

int UiRenderer::LoadThumbnail(const std::string& path, int box) {
    std::vector<unsigned char> rgba;
    if (!DecodeThumbnailRgba(path, box, rgba)) return -1;
    return UploadTexture(rgba.data(), box, box, "thumb:" + path);
}

int UiRenderer::CreateTexture(const unsigned char* rgba, int width, int height, const std::string& label) {
    return UploadTexture(rgba, width, height, label);
}

bool UiRenderer::UpdateTexture(int handle, const unsigned char* rgba, int width, int height) {
    if (handle < 0 || handle >= static_cast<int>(icons_.size())) return false;
    IconTexture& icon = icons_[handle];
    if (!icon.texture || icon.width != width || icon.height != height) return false;
    WGPUTexelCopyTextureInfo dst{};
    dst.texture = icon.texture;
    dst.mipLevel = 0;
    dst.origin = WGPUOrigin3D{0, 0, 0};
    dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout{};
    layout.offset = 0;
    layout.bytesPerRow = static_cast<uint32_t>(width) * 4;
    layout.rowsPerImage = static_cast<uint32_t>(height);
    WGPUExtent3D size{static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    wgpuQueueWriteTexture(queue_, &dst, rgba, static_cast<size_t>(width) * height * 4, &layout, &size);
    return true;
}

void UiRenderer::ReleaseTexture(int handle) {
    if (handle < 0 || handle >= static_cast<int>(icons_.size())) return;
    IconTexture& icon = icons_[handle];
    if (!icon.texture) return;  // already free
    if (icon.bind_group) wgpuBindGroupRelease(icon.bind_group);
    if (icon.view) wgpuTextureViewRelease(icon.view);
    wgpuTextureRelease(icon.texture);
    icon = IconTexture{};
    free_icon_slots_.push_back(handle);
}

int UiRenderer::LoadIcon(const std::string& path) {
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!pixels) {
        last_error_ = "failed to decode icon '" + path + "': " + stbi_failure_reason();
        std::fprintf(stderr, "[ui] %s\n", last_error_.c_str());
        return -1;
    }
    int handle = UploadTexture(pixels, width, height, path);
    stbi_image_free(pixels);
    return handle;
}

bool UiRenderer::LoadAnimatedImage(const std::string& path, std::vector<int>& out_frame_icons,
                                    std::vector<int>& out_frame_delays_ms) {
    out_frame_icons.clear();
    out_frame_delays_ms.clear();

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        last_error_ = "failed to open '" + path + "'";
        return false;
    }
    std::streamsize size = file.tellg();
    if (size <= 0) {
        last_error_ = "'" + path + "' is empty or unreadable";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<unsigned char> raw(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(raw.data()), size)) {
        last_error_ = "failed to read '" + path + "'";
        return false;
    }

    int width = 0, height = 0, frame_count = 0, channels = 0;
    int* delays_ms = nullptr;
    stbi_uc* pixels = stbi_load_gif_from_memory(raw.data(), static_cast<int>(raw.size()), &delays_ms, &width,
                                                 &height, &frame_count, &channels, 4);
    if (!pixels) {
        last_error_ = "failed to decode animated image '" + path + "': " + stbi_failure_reason();
        std::fprintf(stderr, "[ui] %s\n", last_error_.c_str());
        return false;
    }

    size_t frame_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    bool ok = true;
    for (int i = 0; i < frame_count; ++i) {
        int handle = UploadTexture(pixels + static_cast<size_t>(i) * frame_bytes, width, height,
                                    path + " frame " + std::to_string(i));
        if (handle < 0) {
            ok = false;
            break;
        }
        out_frame_icons.push_back(handle);
        // Floored to 20ms (50fps) - some GIF encoders emit a 0ms
        // delay for a frame, which every mainstream browser/viewer
        // treats as "use a sane default" rather than "no delay," not
        // as a literal instruction to flip frames as fast as
        // possible; a pathological file full of 0ms delays shouldn't
        // be able to spin this uselessly fast either way.
        out_frame_delays_ms.push_back(std::max(20, delays_ms[i]));
    }

    stbi_image_free(pixels);
    STBI_FREE(delays_ms);

    if (!ok) {
        out_frame_icons.clear();
        out_frame_delays_ms.clear();
        return false;
    }
    return true;
}

void UiRenderer::DrawImage(float x, float y, float w, float h, int icon, UiColor tint) {
    if (icon < 0 || icon >= static_cast<int>(icons_.size()) || !icons_[icon].bind_group) {
        return;
    }

    auto Ndc = [this](float px, float py) {
        float ndc_x = (px / screen_width_) * 2.0f - 1.0f;
        float ndc_y = 1.0f - (py / screen_height_) * 2.0f;
        return std::pair<float, float>{ndc_x, ndc_y};
    };

    auto [x0, y0] = Ndc(x, y);
    auto [x1, y1] = Ndc(x + w, y);
    auto [x2, y2] = Ndc(x, y + h);
    auto [x3, y3] = Ndc(x + w, y + h);

    ImageDrawCall call;
    call.icon = icon;
    call.verts[0] = ImageVertex{x0, y0, 0.0f, 0.0f, tint.r, tint.g, tint.b, tint.a};
    call.verts[1] = ImageVertex{x1, y1, 1.0f, 0.0f, tint.r, tint.g, tint.b, tint.a};
    call.verts[2] = ImageVertex{x2, y2, 0.0f, 1.0f, tint.r, tint.g, tint.b, tint.a};
    call.verts[3] = ImageVertex{x1, y1, 1.0f, 0.0f, tint.r, tint.g, tint.b, tint.a};
    call.verts[4] = ImageVertex{x3, y3, 1.0f, 1.0f, tint.r, tint.g, tint.b, tint.a};
    call.verts[5] = ImageVertex{x2, y2, 0.0f, 1.0f, tint.r, tint.g, tint.b, tint.a};
    image_draw_calls_.push_back(call);
    batches_.push_back(DrawBatch{BatchKind::kImage, image_draw_calls_.size() - 1, 1});
}

void UiRenderer::EnsureVertexCapacity(size_t vertex_count) {
    if (vertex_count <= vertex_buffer_capacity_) {
        return;
    }

    if (vertex_buffer_) {
        wgpuBufferRelease(vertex_buffer_);
        vertex_buffer_ = nullptr;
    }

    // Round up generously so a slowly-growing UI doesn't reallocate
    // the vertex buffer every single frame.
    size_t new_capacity = std::max(vertex_count, vertex_buffer_capacity_ * 2);
    if (new_capacity < 1024) {
        new_capacity = 1024;
    }

    WGPUBufferDescriptor buffer_descriptor{};
    buffer_descriptor.nextInChain = nullptr;
    buffer_descriptor.label = ToStringView("CryptVaultUiVertexBuffer");
    buffer_descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    buffer_descriptor.size = new_capacity * sizeof(Vertex);
    buffer_descriptor.mappedAtCreation = false;

    vertex_buffer_ = wgpuDeviceCreateBuffer(device_, &buffer_descriptor);
    vertex_buffer_capacity_ = new_capacity;
}

void UiRenderer::EnsureRoundedRectVertexCapacity(size_t vertex_count) {
    if (vertex_count <= rounded_rect_vertex_buffer_capacity_) {
        return;
    }

    if (rounded_rect_vertex_buffer_) {
        wgpuBufferRelease(rounded_rect_vertex_buffer_);
        rounded_rect_vertex_buffer_ = nullptr;
    }

    size_t new_capacity = std::max(vertex_count, rounded_rect_vertex_buffer_capacity_ * 2);
    if (new_capacity < 256) {
        new_capacity = 256;
    }

    WGPUBufferDescriptor buffer_descriptor{};
    buffer_descriptor.nextInChain = nullptr;
    buffer_descriptor.label = ToStringView("CryptVaultUiRoundedRectVertexBuffer");
    buffer_descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    buffer_descriptor.size = new_capacity * sizeof(RoundedRectVertex);
    buffer_descriptor.mappedAtCreation = false;

    rounded_rect_vertex_buffer_ = wgpuDeviceCreateBuffer(device_, &buffer_descriptor);
    rounded_rect_vertex_buffer_capacity_ = new_capacity;
}

void UiRenderer::BeginFrame(uint32_t screen_width, uint32_t screen_height, float pixel_scale) {
    pixel_scale_ = pixel_scale >= 1.0f ? pixel_scale : 1.0f;
    vertices_.clear();
    image_draw_calls_.clear();
    rounded_rect_vertices_.clear();
    batches_.clear();
    clip_stack_.clear();
    screen_width_ = screen_width > 0 ? static_cast<float>(screen_width) : 1.0f;
    screen_height_ = screen_height > 0 ? static_cast<float>(screen_height) : 1.0f;
}

void UiRenderer::PushClipRect(float x, float y, float w, float h) {
    // Clamp to non-negative pixel coords/sizes and to the framebuffer
    // itself before intersecting - out-of-range values (a rect partly
    // off the top-left edge, or bigger than the screen) are common
    // for scroll regions near the viewport edge, and should clip to
    // what's actually visible rather than wrapping/overflowing the
    // uint32_t scissor rect the GPU API expects.
    float x0 = std::max(0.0f, x);
    float y0 = std::max(0.0f, y);
    float x1 = std::min(screen_width_, x + w);
    float y1 = std::min(screen_height_, y + h);

    if (!clip_stack_.empty()) {
        const ScissorRect& parent = clip_stack_.back();
        x0 = std::max(x0, static_cast<float>(parent.x));
        y0 = std::max(y0, static_cast<float>(parent.y));
        x1 = std::min(x1, static_cast<float>(parent.x + parent.w));
        y1 = std::min(y1, static_cast<float>(parent.y + parent.h));
    }

    ScissorRect rect;
    rect.x = static_cast<uint32_t>(x0);
    rect.y = static_cast<uint32_t>(y0);
    rect.w = x1 > x0 ? static_cast<uint32_t>(x1 - x0) : 0;
    rect.h = y1 > y0 ? static_cast<uint32_t>(y1 - y0) : 0;
    clip_stack_.push_back(rect);

    DrawBatch batch{BatchKind::kSetScissor, 0, 0};
    batch.scissor_x = rect.x;
    batch.scissor_y = rect.y;
    batch.scissor_w = rect.w;
    batch.scissor_h = rect.h;
    batches_.push_back(batch);
}

void UiRenderer::PopClipRect() {
    if (clip_stack_.empty()) {
        return;  // unmatched Pop - ignore rather than underflow.
    }
    clip_stack_.pop_back();

    DrawBatch batch{BatchKind::kSetScissor, 0, 0};
    if (clip_stack_.empty()) {
        // Back to the full framebuffer.
        batch.scissor_x = 0;
        batch.scissor_y = 0;
        batch.scissor_w = static_cast<uint32_t>(screen_width_);
        batch.scissor_h = static_cast<uint32_t>(screen_height_);
    } else {
        const ScissorRect& rect = clip_stack_.back();
        batch.scissor_x = rect.x;
        batch.scissor_y = rect.y;
        batch.scissor_w = rect.w;
        batch.scissor_h = rect.h;
    }
    batches_.push_back(batch);
}

void UiRenderer::PushVertex(float px, float py, UiColor color) {
    // Top-left-origin pixel space -> NDC (-1..1, Y flipped since
    // pixel Y grows downward but NDC Y grows upward).
    float ndc_x = (px / screen_width_) * 2.0f - 1.0f;
    float ndc_y = 1.0f - (py / screen_height_) * 2.0f;
    vertices_.push_back(Vertex{ndc_x, ndc_y, color.r, color.g, color.b, color.a});

    // Extend the current batch if the previous vertex was also part
    // of a Solid run (the common case - DrawRect/DrawLabel calls
    // usually come several in a row), otherwise start a new one. See
    // DrawBatch's comment in the header for why this exists.
    if (!batches_.empty() && batches_.back().kind == BatchKind::kSolid) {
        ++batches_.back().count;
    } else {
        batches_.push_back(DrawBatch{BatchKind::kSolid, vertices_.size() - 1, 1});
    }
}

void UiRenderer::DrawRect(float x, float y, float w, float h, UiColor color) {
    // Two triangles, six vertices - simplest possible approach for an
    // immediate-mode renderer with no index buffer yet.
    PushVertex(x, y, color);
    PushVertex(x + w, y, color);
    PushVertex(x, y + h, color);

    PushVertex(x + w, y, color);
    PushVertex(x + w, y + h, color);
    PushVertex(x, y + h, color);
}

void UiRenderer::DrawRoundedRect(float x, float y, float w, float h, float radius, UiColor color) {
    // Degenerate/negative sizes: nothing sensible to draw.
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }

    // Clamp so the two "inner half extents" the shader computes
    // (half_w - radius, half_h - radius) never go negative - beyond
    // this point the shape is already a full stadium/circle, so
    // clamping just prevents the SDF math from producing a corner
    // that pokes back out past the opposite edge.
    float half_w = w * 0.5f;
    float half_h = h * 0.5f;
    float clamped_radius = std::max(0.0f, std::min(radius, std::min(half_w, half_h)));

    auto Ndc = [this](float px, float py) {
        float ndc_x = (px / screen_width_) * 2.0f - 1.0f;
        float ndc_y = 1.0f - (py / screen_height_) * 2.0f;
        return std::pair<float, float>{ndc_x, ndc_y};
    };

    // Four corners, in both NDC (for clip_position) and local pixel
    // offset from center (for the fragment shader's SDF).
    struct Corner {
        float px, py;      // pixel-space position
        float local_x, local_y;  // offset from center, in pixels
    };
    Corner corners[4] = {
        {x, y, -half_w, -half_h},
        {x + w, y, half_w, -half_h},
        {x, y + h, -half_w, half_h},
        {x + w, y + h, half_w, half_h},
    };

    RoundedRectVertex verts[4];
    for (int i = 0; i < 4; ++i) {
        auto [ndc_x, ndc_y] = Ndc(corners[i].px, corners[i].py);
        verts[i] = RoundedRectVertex{
            ndc_x, ndc_y,
            corners[i].local_x, corners[i].local_y,
            half_w, half_h,
            clamped_radius,
            color.r, color.g, color.b, color.a,
        };
    }

    // Two triangles: (0,1,2) and (1,3,2), matching the same
    // top-left/top-right/bottom-left/bottom-right winding DrawRect
    // and DrawImage use.
    rounded_rect_vertices_.push_back(verts[0]);
    rounded_rect_vertices_.push_back(verts[1]);
    rounded_rect_vertices_.push_back(verts[2]);
    rounded_rect_vertices_.push_back(verts[1]);
    rounded_rect_vertices_.push_back(verts[3]);
    rounded_rect_vertices_.push_back(verts[2]);

    if (!batches_.empty() && batches_.back().kind == BatchKind::kRoundedRect) {
        batches_.back().count += 6;
    } else {
        batches_.push_back(DrawBatch{BatchKind::kRoundedRect, rounded_rect_vertices_.size() - 6, 6});
    }
}

namespace {

// One drawable piece of a label, in left-to-right layout order: a plain ASCII
// character (drawn with the 5x7 pixel font) or a shaped run of other text.
struct LabelItem {
    bool ascii = true;
    char ch = 0;
    std::shared_ptr<const ShapedRun> run;
    float width = 6.0f;   // in base pixels, spacing included
};

char MirrorBracket(char c) {
    switch (c) {
        case '(': return ')'; case ')': return '(';
        case '[': return ']'; case ']': return '[';
        case '{': return '}'; case '}': return '{';
        case '<': return '>'; case '>': return '<';
        default: return c;
    }
}

// Lays out text[begin, end) as items. `rtl`: this stretch is right-to-left (an odd
// bidi level), so its pieces are placed in reverse and its text is shaped RTL.
void LayoutSegment(const std::string& text, size_t begin, size_t end, bool rtl, std::vector<LabelItem>& out) {
    std::vector<LabelItem> pieces;
    for (size_t i = begin; i < end;) {
        if (static_cast<unsigned char>(text[i]) < 0x80) {
            LabelItem item;
            item.ch = rtl ? MirrorBracket(text[i]) : text[i];
            pieces.push_back(std::move(item));
            ++i;
            continue;
        }
        // A run of non-ASCII text, including single spaces between its words:
        // shaped as one unit from system fonts (unicode_font.h).
        size_t j = i;
        while (j < end) {
            if (static_cast<unsigned char>(text[j]) >= 0x80) DecodeUtf8(text, j);
            else if (text[j] == ' ' && j + 1 < end && static_cast<unsigned char>(text[j + 1]) >= 0x80) ++j;
            else break;
        }
        LabelItem item;
        item.ascii = false;
        item.run = ShapeRun(text.substr(i, j - i), rtl ? 2 : 1);
        item.width = item.run->advance + 1.0f;   // +1: the same 1px gap the bitmap font leaves
        pieces.push_back(std::move(item));
        i = j;
    }
    if (rtl) std::reverse(pieces.begin(), pieces.end());
    for (LabelItem& item : pieces) out.push_back(std::move(item));
}

// Whole label -> items in left-to-right order. Text with no right-to-left
// characters (the overwhelmingly common case) skips bidi entirely.
std::vector<LabelItem> LayoutLabel(const std::string& text) {
    std::vector<LabelItem> items;
    if (!TextNeedsBidi(text)) {
        LayoutSegment(text, 0, text.size(), false, items);
        return items;
    }
    const std::vector<BidiRun> runs = BidiResolveRuns(text);
    for (size_t index : BidiVisualOrder(runs)) {
        LayoutSegment(text, runs[index].begin, runs[index].end, (runs[index].level & 1) != 0, items);
    }
    return items;
}

}  // namespace

void UiRenderer::DrawLabel(float x, float y, const std::string& text, UiColor color, float pixel_size) {
    float cursor_x = x;
    for (const LabelItem& item : LayoutLabel(text)) {
        if (!item.ascii) {
            // Shaped text: coverage becomes per-pixel alpha, drawn with the same
            // rectangles the bitmap font uses; colour emoji bring their own pixels.
            const float baseline = y + 7.0f * pixel_size;
            for (const ShapedGlyph& sg : item.run->glyphs) {
                const UnicodeGlyph* g = sg.glyph;
                if (!g) continue;
                if (g->color) {
                    for (int row = 0; row < g->height; ++row) {
                        for (int col = 0; col < g->width; ++col) {
                            const uint8_t* px = &g->rgba[(static_cast<size_t>(row) * g->width + col) * 4];
                            if (px[3] < 24) continue;
                            DrawRect(cursor_x + (sg.x + g->xoff + col) * pixel_size,
                                     baseline + (sg.y + g->yoff + row) * pixel_size, pixel_size, pixel_size,
                                     UiColor{px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f, color.a * px[3] / 255.0f});
                        }
                    }
                    continue;
                }
                for (int row = 0; row < g->height; ++row) {
                    for (int col = 0; col < g->width; ++col) {
                        const uint8_t a = g->alpha[static_cast<size_t>(row) * g->width + col];
                        if (a < 40) continue;
                        UiColor c = color;
                        c.a *= static_cast<float>(a) / 255.0f;
                        DrawRect(cursor_x + (sg.x + g->xoff + col) * pixel_size,
                                 baseline + (sg.y + g->yoff + row) * pixel_size, pixel_size, pixel_size, c);
                    }
                }
            }
            cursor_x += item.width * pixel_size;
            continue;
        }
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(item.ch)));
        const Glyph5x7* glyph = FindGlyph5x7(ch);
        if (glyph) {
            for (int row = 0; row < 7; ++row) {
                const uint8_t row_bits = glyph->rows[row];
                for (int col = 0; col < 5; ++col) {
                    if (row_bits & (0x10 >> col)) {
                        DrawRect(cursor_x + col * pixel_size, y + row * pixel_size, pixel_size, pixel_size, color);
                    }
                }
            }
        }
        // 5 columns + 1 column of spacing between characters.
        cursor_x += 6.0f * pixel_size;
    }
}

float UiRenderer::MeasureText(const std::string& text, float pixel_size) const {
    if (text.empty()) {
        return 0.0f;
    }
    // ASCII: 5 glyph columns + 1 spacer column per char (the spacer after the
    // last char isn't counted); shaped runs add their advance + 1. Reordering
    // by bidi doesn't change the total.
    float width = 0.0f;
    for (const LabelItem& item : LayoutLabel(text)) width += item.width;
    return (width - 1.0f) * pixel_size;
}

bool UiRenderer::Button(int /*id*/, float x, float y, float w, float h,
                        const std::string& label, const UiInput& input) {
    bool hovered = input.mouse_x >= x && input.mouse_x <= x + w
                   && input.mouse_y >= y && input.mouse_y <= y + h;

    // Border drawn as a slightly larger rect behind the fill, since
    // this pipeline has no dedicated stroke/outline primitive yet.
    UiColor border_color{0.35f, 0.42f, 0.55f, 1.0f};
    DrawRect(x - 1, y - 1, w + 2, h + 2, border_color);

    UiColor fill_color = hovered
        ? UiColor{0.22f, 0.28f, 0.38f, 1.0f}
        : UiColor{0.16f, 0.20f, 0.28f, 1.0f};
    DrawRect(x, y, w, h, fill_color);

    constexpr float kLabelPixelSize = 3.0f;
    float label_width = MeasureText(label, kLabelPixelSize);
    float label_height = 7.0f * kLabelPixelSize;
    float label_x = x + (w - label_width) * 0.5f;
    float label_y = y + (h - label_height) * 0.5f;
    UiColor text_color{0.92f, 0.94f, 0.98f, 1.0f};
    DrawLabel(label_x, label_y, label, text_color, kLabelPixelSize);

    return hovered && input.clicked;
}

void UiRenderer::EndFrame(WGPURenderPassEncoder pass) {
    // Upload each pipeline's vertex data once. Upload order doesn't
    // matter - only the draw-call order below does, since that's
    // what determines what paints over what.
    if (!vertices_.empty()) {
        EnsureVertexCapacity(vertices_.size());
        wgpuQueueWriteBuffer(queue_, vertex_buffer_, 0, vertices_.data(),
                              vertices_.size() * sizeof(Vertex));
    }

    if (!rounded_rect_vertices_.empty()) {
        EnsureRoundedRectVertexCapacity(rounded_rect_vertices_.size());
        wgpuQueueWriteBuffer(queue_, rounded_rect_vertex_buffer_, 0, rounded_rect_vertices_.data(),
                              rounded_rect_vertices_.size() * sizeof(RoundedRectVertex));
    }

    if (!image_draw_calls_.empty()) {
        size_t total_image_vertices = image_draw_calls_.size() * 6;
        if (total_image_vertices > image_vertex_buffer_capacity_) {
            if (image_vertex_buffer_) {
                wgpuBufferRelease(image_vertex_buffer_);
                image_vertex_buffer_ = nullptr;
            }
            size_t new_capacity = std::max(total_image_vertices, image_vertex_buffer_capacity_ * 2);
            if (new_capacity < 256) {
                new_capacity = 256;
            }
            WGPUBufferDescriptor buffer_descriptor{};
            buffer_descriptor.nextInChain = nullptr;
            buffer_descriptor.label = ToStringView("CryptVaultUiImageVertexBuffer");
            buffer_descriptor.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
            buffer_descriptor.size = new_capacity * sizeof(ImageVertex);
            buffer_descriptor.mappedAtCreation = false;
            image_vertex_buffer_ = wgpuDeviceCreateBuffer(device_, &buffer_descriptor);
            image_vertex_buffer_capacity_ = new_capacity;
        }

        std::vector<ImageVertex> image_vertices;
        image_vertices.reserve(total_image_vertices);
        for (const ImageDrawCall& call : image_draw_calls_) {
            for (int i = 0; i < 6; ++i) {
                image_vertices.push_back(call.verts[i]);
            }
        }
        wgpuQueueWriteBuffer(queue_, image_vertex_buffer_, 0, image_vertices.data(),
                              image_vertices.size() * sizeof(ImageVertex));
    }

    // Walk the batches in the exact order the app called
    // Draw*/DrawImage/Push|PopClipRect this frame (see DrawBatch's
    // comment in the header), switching pipeline + vertex buffer only
    // when the kind actually changes rather than once per batch,
    // since consecutive same-kind batches are common (e.g. several
    // DrawRect calls in a row for one panel's background/border/
    // highlight).
    //
    // Explicitly set a full-framebuffer scissor up front rather than
    // relying on whatever the pass encoder's default happens to be -
    // scissor state isn't otherwise touched by anything in this file,
    // but leaving it implicit would mean correctness depends on an
    // unstated backend default.
    // Scissor rects are physical pixels; everything else is logical.
    const uint32_t fb_w = static_cast<uint32_t>(screen_width_ * pixel_scale_ + 0.5f);
    const uint32_t fb_h = static_cast<uint32_t>(screen_height_ * pixel_scale_ + 0.5f);
    wgpuRenderPassEncoderSetScissorRect(pass, 0, 0, fb_w, fb_h);

    bool pipeline_set = false;
    BatchKind current_kind = BatchKind::kSolid;

    for (const DrawBatch& batch : batches_) {
        if (batch.kind == BatchKind::kSetScissor) {
            // Orthogonal to pipeline state - doesn't touch
            // current_kind/pipeline_set, so the next kSolid/etc.
            // batch after this one doesn't pay for a redundant
            // pipeline rebind.
            const uint32_t sx = std::min(fb_w, static_cast<uint32_t>(batch.scissor_x * pixel_scale_));
            const uint32_t sy = std::min(fb_h, static_cast<uint32_t>(batch.scissor_y * pixel_scale_));
            const uint32_t sw = std::min(fb_w - sx, static_cast<uint32_t>(batch.scissor_w * pixel_scale_ + 0.5f));
            const uint32_t sh = std::min(fb_h - sy, static_cast<uint32_t>(batch.scissor_h * pixel_scale_ + 0.5f));
            wgpuRenderPassEncoderSetScissorRect(pass, sx, sy, sw, sh);
            continue;
        }

        if (!pipeline_set || batch.kind != current_kind) {
            switch (batch.kind) {
                case BatchKind::kSolid:
                    wgpuRenderPassEncoderSetPipeline(pass, pipeline_);
                    wgpuRenderPassEncoderSetVertexBuffer(
                        pass, 0, vertex_buffer_, 0, vertices_.size() * sizeof(Vertex));
                    break;
                case BatchKind::kRoundedRect:
                    wgpuRenderPassEncoderSetPipeline(pass, rounded_rect_pipeline_);
                    wgpuRenderPassEncoderSetVertexBuffer(
                        pass, 0, rounded_rect_vertex_buffer_, 0,
                        rounded_rect_vertices_.size() * sizeof(RoundedRectVertex));
                    break;
                case BatchKind::kImage:
                    wgpuRenderPassEncoderSetPipeline(pass, image_pipeline_);
                    wgpuRenderPassEncoderSetVertexBuffer(
                        pass, 0, image_vertex_buffer_, 0,
                        image_draw_calls_.size() * 6 * sizeof(ImageVertex));
                    break;
                case BatchKind::kSetScissor:
                    break;  // unreachable - handled by the `continue` above
            }
            current_kind = batch.kind;
            pipeline_set = true;
        }

        if (batch.kind == BatchKind::kImage) {
            // Each image call needs its own bind group (different
            // icon), so it can't be folded into one Draw() the way a
            // run of solid/rounded-rect vertices can.
            const IconTexture& icon = icons_[image_draw_calls_[batch.offset].icon];
            wgpuRenderPassEncoderSetBindGroup(pass, 0, icon.bind_group, 0, nullptr);
            wgpuRenderPassEncoderDraw(pass, 6, 1, static_cast<uint32_t>(batch.offset * 6), 0);
        } else {
            wgpuRenderPassEncoderDraw(pass, static_cast<uint32_t>(batch.count), 1,
                                       static_cast<uint32_t>(batch.offset), 0);
        }
    }
}

}  // namespace cryptvault
