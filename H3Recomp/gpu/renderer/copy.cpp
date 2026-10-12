// Fullscreen copy passes. Xenos copies between EDRAM and memory reinterpret
// bits in ways a plain blit can't express (depth as bytes, float formats as
// another format's bits, gamma encoding, MSAA averaging), so those copies
// run a small fragment shader instead. The same pipeline also fills
// rectangles with a color (D3D's fast clears; see draw.cpp), and others
// write depth and stencil from such bytes (RestoreDepthFromBytes,
// RestoreStencilFromBytes).

#include "gpu/renderer/internal.h"

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // Covers the target with one triangle (vertex IDs 0-2 -> a triangle
        // twice the size of clip space); the scissor limits it to the
        // rectangle being copied.
        const char* kCopyVertexShader = R"(#version 450
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

        // mode.x is a CopyMode (internal.h).
        const char* kCopyFragmentShader = R"(#version 450
layout(set = 2, binding = 0) uniform sampler2D xe_source;
layout(set = 3, binding = 0) uniform XeCopy
{
    ivec4 offset;  // xy: destination origin, zw: source origin (samples)
    ivec4 mode;    // x: copy mode, y: depth is 20e4 float, zw: source samples per pixel
    vec4 scale;    // x: 2^RB_COPY_DEST_INFO exp bias, for color; y: 0 = gamma 2 (H3_APPROX_GAMMA); fill color for mode 5
};
layout(location = 0) out vec4 xe_out;

// Depth as the 24-bit float (20e4) the console stores in D24FS8 buffers.
uint xe_float_to_20e4(float f)
{
    uint u = floatBitsToUint(clamp(f, 0.0, 1.99999988));
    if (u < 0x38800000u)
        u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23), 24u);
    else
        u += 0xC8000000u;
    return ((u + 3u + ((u >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}

// A float as the 10-bit 7e3 float of 2_10_10_10_FLOAT render targets,
// returned as that bit pattern's unorm value.
float xe_float_to_7e3(float f)
{
    f = clamp(f, 0.0, 31.875);
    uint bits;
    if (f < 0.25)
        bits = uint(round(f * 512.0));
    else
    {
        int e;
        float m = frexp(f, e);
        uint exponent = uint(e + 2), mantissa = uint(round((2.0 * m - 1.0) * 128.0));
        if (mantissa == 128u)
            mantissa = 0u, exponent++;
        bits = exponent > 7u ? 1023u : (exponent << 7) | mantissa;
    }
    return float(bits) / 1023.0;
}

void main()
{
    if (mode.x == 5)  // Fill
    {
        xe_out = scale;
        return;
    }
    // Average the pixel's samples (one texel each for 1x sources).
    ivec2 origin = (ivec2(gl_FragCoord.xy) - offset.xy) * mode.zw + offset.zw;
    vec4 v = vec4(0.0);
    for (int y = 0; y < mode.w; y++)
        for (int x = 0; x < mode.z; x++)
            v += texelFetch(xe_source, origin + ivec2(x, y), 0);
    v /= float(mode.z * mode.w);
    if (mode.x != 1)
        v.rgb *= scale.x;
    if (mode.x == 1)  // DepthBytes: the 24_8 word's bytes, stencil (0) low
    {
        uint d = mode.y != 0 ? xe_float_to_20e4(v.r) : uint(round(clamp(v.r, 0.0, 1.0) * 16777215.0));
        xe_out = vec4(0.0, float(d & 255u), float((d >> 8) & 255u), float(d >> 16)) / 255.0;
    }
    // LinearToGamma / GammaToLinear: the hardware's piecewise-linear curve,
    // or gamma 2 with H3_APPROX_GAMMA (scale.y = 0).
    else if (mode.x == 3)
        xe_out = vec4(scale.y != 0.0 ? xe_linear_to_pwl_gamma(v.rgb) : sqrt(clamp(v.rgb, 0.0, 1.0)), v.a);
    else if (mode.x == 4)
        xe_out = vec4(scale.y != 0.0 ? xe_pwl_gamma_to_linear(v.rgb) : v.rgb * v.rgb, v.a);
    else if (mode.x == 2)  // FloatTo7e3 (alpha is 2 bits)
        xe_out = vec4(xe_float_to_7e3(v.r), xe_float_to_7e3(v.g), xe_float_to_7e3(v.b), round(clamp(v.a, 0.0, 1.0) * 3.0) / 3.0);
    else
        xe_out = v;
}
)";

        // Depth from the bytes of 24_8 depth words drawn as 8_8_8_8 color
        // (RestoreDepthFromBytes). Each texel holds (mid, low, stencil,
        // high): the DepthBytes layout (stencil, low, mid, high) after the
        // zyxw swizzle Halo's restore shader applies.
        //
        // EDRAM stores depth tiles with the two 40-sample halves of every
        // 80-sample-wide tile swapped relative to color tiles (Xenia's
        // render target cache does the same swap). Halo's restore shader
        // pre-swaps the columns it writes as color (its constants: 40, -40,
        // 1/80) so they land right when the EDRAM is read as depth; reading
        // the color back here must undo it, or every other 40-pixel column
        // gets its neighbour's depth (blocky fog/sky over the menu scene).
        const char* kDepthRestoreFragmentShader = R"(#version 450
layout(set = 2, binding = 0) uniform sampler2D xe_source;
layout(set = 3, binding = 0) uniform XeRestore
{
    ivec4 mode;  // x: depth is 20e4 float, y: render scale (host texels per sample)
};

// EDRAM tile width in samples, and the half that depth swaps.
const int kTileWidthSamples = 80;
const int kHalfTileSamples = 40;

// The console's 24-bit float depth (20e4) as a float (Xenia's
// Float20e4To32).
float xe_20e4_to_float(uint f24)
{
    if (f24 == 0u)
        return 0.0;
    uint mantissa = f24 & 0xFFFFFu;
    int exponent = int(f24 >> 20);
    if (exponent == 0)
    {
        // Denormal: shift the top set bit up to the implicit bit 20.
        int shift = 20 - findMSB(mantissa);
        exponent = 1 - shift;
        mantissa = (mantissa << uint(shift)) & 0xFFFFFu;
    }
    return uintBitsToFloat((uint(exponent + 112) << 23) | (mantissa << 3));
}

void main()
{
    // Host texel -> sample, swap the tile half, back to a host texel (the
    // same sub-texel at render scales above 1).
    ivec2 texel = ivec2(gl_FragCoord.xy);
    int scale = max(mode.y, 1);
    int sample_x = texel.x / scale;
    sample_x += (sample_x % kTileWidthSamples) < kHalfTileSamples ? kHalfTileSamples : -kHalfTileSamples;
    texel.x = sample_x * scale + texel.x % scale;
    uvec4 bytes = uvec4(round(texelFetch(xe_source, texel, 0) * 255.0));
    uint d = (bytes.a << 16) | (bytes.r << 8) | bytes.g;
    gl_FragDepth = mode.x != 0 ? xe_20e4_to_float(d) : float(d) / 16777215.0;
}
)";

        SDL_GPUGraphicsPipeline* g_depthRestorePipeline = nullptr;

        // Stencil from the red byte of 8_8_8_8 color drawn at the depth
        // buffer's EDRAM base (RestoreStencilFromBytes): the low byte of a
        // D24S8 word is the stencil, which color views call red. Without
        // stencil export a fragment can't write an arbitrary stencil value,
        // so this runs once per bit: mode.x is the bit (fragments whose byte
        // has it clear are discarded; a pass with write mask 1 << bit and
        // reference 0xFF sets it), or -1 to write every fragment (the clear
        // pass before the bits). Same tile-half swap as the depth restore.
        const char* kStencilRestoreFragmentShader = R"(#version 450
layout(set = 2, binding = 0) uniform sampler2D xe_source;
layout(set = 3, binding = 0) uniform XeStencil
{
    ivec4 mode;  // x: bit to keep (-1: all fragments), y: render scale
};

const int kTileWidthSamples = 80;
const int kHalfTileSamples = 40;

void main()
{
    if (mode.x < 0)
        return;
    ivec2 texel = ivec2(gl_FragCoord.xy);
    int scale = max(mode.y, 1);
    int sample_x = texel.x / scale;
    sample_x += (sample_x % kTileWidthSamples) < kHalfTileSamples ? kHalfTileSamples : -kHalfTileSamples;
    texel.x = sample_x * scale + texel.x % scale;
    uint stencil = uint(round(texelFetch(xe_source, texel, 0).r * 255.0));
    if ((stencil & (1u << uint(mode.x))) == 0u)
        discard;
}
)";
        // Stencil bits per byte, and the pipelines writing each bit (index
        // = bit) plus the whole byte (index kStencilBits), made on first use.
        constexpr uint32_t kStencilBits = 8;
        SDL_GPUGraphicsPipeline* g_stencilRestorePipelines[kStencilBits + 1] = {};

        // The display gamma ramp (gpu::GammaRamp) applied to the front
        // buffer, as the console's display controller does at scanout.
        const char* kGammaFragmentShader = R"(#version 450
layout(set = 2, binding = 0) uniform sampler2D xe_source;
layout(set = 3, binding = 0) uniform XeGamma
{
    ivec4 origin;       // xy: the frame's top-left in the source
    uvec4 ramp[128];    // per segment: r, g, b = base | delta << 16
};
layout(location = 0) out vec4 xe_out;

void main()
{
    vec3 color = clamp(texelFetch(xe_source, ivec2(gl_FragCoord.xy) + origin.xy, 0).rgb, 0.0, 1.0);
    uvec3 x = uvec3(round(color * 1023.0));  // 10-bit input, as the display controller sees it
    vec3 result;
    for (int c = 0; c < 3; c++)
    {
        uint entry = ramp[x[c] >> 3][c];
        float base = float(entry & 0xFFFFu), delta = float(entry >> 16);
        result[c] = (base + delta * float(x[c] & 7u) / 8.0) / 65472.0;  // 0xFFC0 = 1.0
    }
    xe_out = vec4(clamp(result, 0.0, 1.0), 1.0);
}
)";
        SDL_GPUGraphicsPipeline* g_gammaPipeline = nullptr;

        // The copy shader's uniform block (XeCopy above).
        struct CopyConstants
        {
            int32_t offset[4];
            int32_t mode[4];
            float scale[4];
        };

        std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline*> g_copyPipelines;  // target format -> pipeline

        // Creates g_pointSampler (nearest, clamped) on first use.
        void EnsurePointSampler()
        {
            if (g_pointSampler)
                return;
            SDL_GPUSamplerCreateInfo sampler{};
            sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            g_pointSampler = SDL_CreateGPUSampler(g_device, &sampler);
        }

        // The copy pipeline rendering into a target format (one per format,
        // created on first use).
        SDL_GPUGraphicsPipeline* CopyPipeline(SDL_GPUTextureFormat format)
        {
            SDL_GPUGraphicsPipeline*& pipeline = g_copyPipelines[uint32_t(format)];
            if (pipeline)
                return pipeline;
            SDL_GPUShader* vertex = CompileInternal(kCopyVertexShader, true, 0, 0);
            // The copy shader uses the shared piecewise-linear gamma GLSL,
            // inserted after its #version line.
            std::string source = kCopyFragmentShader;
            source.insert(source.find('\n') + 1, shader::kPwlGammaGlsl);
            SDL_GPUShader* fragment = CompileInternal(source.c_str(), false, 1, 1);
            if (vertex && fragment)
            {
                SDL_GPUColorTargetDescription target{};
                target.format = format;
                SDL_GPUGraphicsPipelineCreateInfo info{};
                info.vertex_shader = vertex;
                info.fragment_shader = fragment;
                info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                info.target_info.color_target_descriptions = &target;
                info.target_info.num_color_targets = 1;
                pipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
                if (!pipeline)
                    LogOnce(std::string("copy pipeline creation failed: ") + SDL_GetError());
            }
            EnsurePointSampler();
            if (vertex)
                SDL_ReleaseGPUShader(g_device, vertex);
            if (fragment)
                SDL_ReleaseGPUShader(g_device, fragment);
            return pipeline;
        }

        // One copy-shader pass: source into a rectangle of destination
        // (viewport destinationWidth x destinationHeight), with constants.
        void RunCopyPass(SDL_GPUTexture* source, SDL_GPUTexture* destination, SDL_GPUTextureFormat destinationFormat,
            uint32_t destinationWidth, uint32_t destinationHeight, const SDL_Rect& rectangle, const CopyConstants& constants)
        {
            SDL_GPUGraphicsPipeline* pipeline = CopyPipeline(destinationFormat);
            if (!pipeline)
                return;
            SDL_GPUColorTargetInfo color{};
            color.texture = destination;
            color.load_op = SDL_GPU_LOADOP_LOAD;
            color.store_op = SDL_GPU_STOREOP_STORE;
            CountPass(PassKind::Copy, uint64_t(rectangle.w) * uint64_t(rectangle.h));
            SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(Commands(), &color, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_GPUViewport viewport{ 0.0f, 0.0f, float(destinationWidth), float(destinationHeight), 0.0f, 1.0f };
            SDL_SetGPUViewport(pass, &viewport);
            SDL_SetGPUScissor(pass, &rectangle);
            SDL_GPUTextureSamplerBinding binding{ source, g_pointSampler };
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            SDL_PushGPUFragmentUniformData(Commands(), 0, &constants, sizeof(constants));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
    }

    void ConvertCopy(SDL_GPUTexture* source, SDL_GPUTexture* destination, SDL_GPUTextureFormat destinationFormat,
        uint32_t destinationWidth, uint32_t destinationHeight, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w,
        uint32_t h, CopyMode mode, bool depthFloat, float scale, int32_t samplesX, int32_t samplesY)
    {
        CopyConstants constants = { { dx, dy, sx, sy }, { int32_t(mode), depthFloat ? 1 : 0, samplesX, samplesY },
            { scale, ApproxGamma() ? 0.0f : 1.0f, 1.0f, 1.0f } };
        RunCopyPass(source, destination, destinationFormat, destinationWidth, destinationHeight, SDL_Rect{ dx, dy, int(w), int(h) },
            constants);
    }

    void FillRect(SDL_GPUTexture* destination, SDL_GPUTextureFormat format, int32_t x, int32_t y, uint32_t w, uint32_t h,
        const float color[4])
    {
        if (!w || !h)
            return;
        // The source isn't read in Fill mode; bind something valid.
        CopyConstants constants = { { 0, 0, 0, 0 }, { int32_t(CopyMode::Fill), 0, 1, 1 }, { color[0], color[1], color[2], color[3] } };
        RunCopyPass(g_placeholder2D, destination, format, TargetWidth(), TargetHeight(), SDL_Rect{ x, y, int(w), int(h) },
            constants);
    }

    void RestoreDepthFromBytes(SDL_GPUTexture* source, SDL_GPUTexture* depth, uint32_t width, uint32_t height, const SDL_Rect& rect,
        bool depthFloat)
    {
        if (!g_depthRestorePipeline)
        {
            SDL_GPUShader* vertex = CompileInternal(kCopyVertexShader, true, 0, 0);
            SDL_GPUShader* fragment = CompileInternal(kDepthRestoreFragmentShader, false, 1, 1);
            if (vertex && fragment)
            {
                SDL_GPUGraphicsPipelineCreateInfo info{};
                info.vertex_shader = vertex;
                info.fragment_shader = fragment;
                info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                info.depth_stencil_state.enable_depth_test = true;  // needed for depth writes
                info.depth_stencil_state.enable_depth_write = true;
                info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
                info.target_info.has_depth_stencil_target = true;
                info.target_info.depth_stencil_format = g_depthFormat;
                g_depthRestorePipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
                if (!g_depthRestorePipeline)
                    LogOnce(std::string("depth restore pipeline creation failed: ") + SDL_GetError());
            }
            if (vertex)
                SDL_ReleaseGPUShader(g_device, vertex);
            if (fragment)
                SDL_ReleaseGPUShader(g_device, fragment);
        }
        EnsurePointSampler();
        if (!g_depthRestorePipeline || !g_pointSampler)
            return;
        SDL_GPUDepthStencilTargetInfo target{};
        target.texture = depth;
        target.load_op = SDL_GPU_LOADOP_LOAD;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        target.stencil_store_op = SDL_GPU_STOREOP_STORE;
        CountPass(PassKind::DepthRestore, uint64_t(rect.w) * uint64_t(rect.h));
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(Commands(), nullptr, 0, &target);
        SDL_BindGPUGraphicsPipeline(pass, g_depthRestorePipeline);
        SDL_GPUViewport viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        SDL_SetGPUViewport(pass, &viewport);
        SDL_SetGPUScissor(pass, &rect);
        SDL_GPUTextureSamplerBinding binding{ source, g_pointSampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        int32_t mode[4] = { depthFloat ? 1 : 0, int32_t(g_renderScale), 0, 0 };
        SDL_PushGPUFragmentUniformData(Commands(), 0, mode, sizeof(mode));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }

    void RestoreStencilFromBytes(SDL_GPUTexture* source, SDL_GPUTexture* depth, uint32_t width, uint32_t height, const SDL_Rect& rect)
    {
        if (!g_stencilRestorePipelines[0])
        {
            SDL_GPUShader* vertex = CompileInternal(kCopyVertexShader, true, 0, 0);
            SDL_GPUShader* fragment = CompileInternal(kStencilRestoreFragmentShader, false, 1, 1);
            for (uint32_t i = 0; vertex && fragment && i <= kStencilBits; i++)
            {
                SDL_GPUGraphicsPipelineCreateInfo info{};
                info.vertex_shader = vertex;
                info.fragment_shader = fragment;
                info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                info.depth_stencil_state.enable_stencil_test = true;  // depth test off: depth stays as is
                SDL_GPUStencilOpState replace{};
                replace.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
                replace.pass_op = SDL_GPU_STENCILOP_REPLACE;
                replace.fail_op = replace.depth_fail_op = SDL_GPU_STENCILOP_KEEP;
                info.depth_stencil_state.front_stencil_state = info.depth_stencil_state.back_stencil_state = replace;
                info.depth_stencil_state.compare_mask = 0xFF;
                info.depth_stencil_state.write_mask = i < kStencilBits ? uint8_t(1u << i) : uint8_t(0xFF);
                info.target_info.has_depth_stencil_target = true;
                info.target_info.depth_stencil_format = g_depthFormat;
                g_stencilRestorePipelines[i] = SDL_CreateGPUGraphicsPipeline(g_device, &info);
                if (!g_stencilRestorePipelines[i])
                {
                    LogOnce(std::string("stencil restore pipeline creation failed: ") + SDL_GetError());
                    break;
                }
            }
            if (vertex)
                SDL_ReleaseGPUShader(g_device, vertex);
            if (fragment)
                SDL_ReleaseGPUShader(g_device, fragment);
        }
        EnsurePointSampler();
        for (SDL_GPUGraphicsPipeline* pipeline : g_stencilRestorePipelines)
            if (!pipeline || !g_pointSampler)
                return;
        SDL_GPUDepthStencilTargetInfo target{};
        target.texture = depth;
        target.load_op = SDL_GPU_LOADOP_LOAD;
        target.store_op = SDL_GPU_STOREOP_STORE;
        target.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        target.stencil_store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(Commands(), nullptr, 0, &target);
        SDL_GPUViewport viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        SDL_SetGPUViewport(pass, &viewport);
        SDL_SetGPUScissor(pass, &rect);
        SDL_GPUTextureSamplerBinding binding{ source, g_pointSampler };
        // The clear (reference 0, every fragment), then one pass per bit
        // (reference 0xFF through that bit's write mask).
        for (int32_t bit = -1; bit < int32_t(kStencilBits); bit++)
        {
            CountPass(PassKind::StencilRestore, uint64_t(rect.w) * uint64_t(rect.h));
            SDL_BindGPUGraphicsPipeline(pass, g_stencilRestorePipelines[bit < 0 ? kStencilBits : uint32_t(bit)]);
            SDL_SetGPUStencilReference(pass, bit < 0 ? 0 : 0xFF);
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            int32_t mode[4] = { bit, int32_t(g_renderScale), 0, 0 };
            SDL_PushGPUFragmentUniformData(Commands(), 0, mode, sizeof(mode));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);
    }

    void ApplyGammaRamp(SDL_GPUTexture* source, int32_t sourceX, int32_t sourceY, SDL_GPUTexture* destination, uint32_t width,
        uint32_t height, const GammaRamp& ramp)
    {
        if (!g_gammaPipeline)
        {
            SDL_GPUShader* vertex = CompileInternal(kCopyVertexShader, true, 0, 0);
            SDL_GPUShader* fragment = CompileInternal(kGammaFragmentShader, false, 1, 1);
            if (vertex && fragment)
            {
                SDL_GPUColorTargetDescription target{};
                target.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                SDL_GPUGraphicsPipelineCreateInfo info{};
                info.vertex_shader = vertex;
                info.fragment_shader = fragment;
                info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                info.target_info.color_target_descriptions = &target;
                info.target_info.num_color_targets = 1;
                g_gammaPipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
                if (!g_gammaPipeline)
                    LogOnce(std::string("gamma pipeline creation failed: ") + SDL_GetError());
            }
            if (vertex)
                SDL_ReleaseGPUShader(g_device, vertex);
            if (fragment)
                SDL_ReleaseGPUShader(g_device, fragment);
        }
        EnsurePointSampler();
        if (!g_gammaPipeline || !g_pointSampler)
            return;
        struct
        {
            int32_t origin[4];
            uint32_t ramp[GammaRamp::kSegments][4];
        } constants{};
        constants.origin[0] = sourceX;
        constants.origin[1] = sourceY;
        for (uint32_t i = 0; i < GammaRamp::kSegments; i++)
            for (uint32_t c = 0; c < 3; c++)
                constants.ramp[i][c] = uint32_t(ramp.base[i][c]) | (uint32_t(ramp.delta[i][c]) << 16);
        SDL_GPUColorTargetInfo color{};
        color.texture = destination;
        color.load_op = SDL_GPU_LOADOP_DONT_CARE;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(Commands(), &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, g_gammaPipeline);
        SDL_GPUViewport viewport{ 0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f };
        SDL_SetGPUViewport(pass, &viewport);
        SDL_GPUTextureSamplerBinding binding{ source, g_pointSampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_PushGPUFragmentUniformData(Commands(), 0, &constants, sizeof(constants));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
    }
}
