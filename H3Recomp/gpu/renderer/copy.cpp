// Fullscreen copy passes. Xenos copies between EDRAM and memory reinterpret
// bits in ways a plain blit can't express (depth as bytes, float formats as
// another format's bits, gamma encoding, MSAA averaging), so those copies
// run a small fragment shader instead. The same pipeline also fills
// rectangles with a color (D3D's fast clears; see draw.cpp).

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
    vec4 scale;    // x: 2^RB_COPY_DEST_INFO exp bias, for color; fill color for mode 5
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
    else if (mode.x == 3)  // LinearToGamma: Halo decodes by squaring
        xe_out = vec4(sqrt(clamp(v.rgb, 0.0, 1.0)), v.a);
    else if (mode.x == 4)  // GammaToLinear
        xe_out = vec4(v.rgb * v.rgb, v.a);
    else if (mode.x == 2)  // FloatTo7e3 (alpha is 2 bits)
        xe_out = vec4(xe_float_to_7e3(v.r), xe_float_to_7e3(v.g), xe_float_to_7e3(v.b), round(clamp(v.a, 0.0, 1.0) * 3.0) / 3.0);
    else
        xe_out = v;
}
)";

        // The copy shader's uniform block (XeCopy above).
        struct CopyConstants
        {
            int32_t offset[4];
            int32_t mode[4];
            float scale[4];
        };

        std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline*> g_copyPipelines;  // target format -> pipeline

        // The copy pipeline rendering into a target format (one per format,
        // created on first use).
        SDL_GPUGraphicsPipeline* CopyPipeline(SDL_GPUTextureFormat format)
        {
            SDL_GPUGraphicsPipeline*& pipeline = g_copyPipelines[uint32_t(format)];
            if (pipeline)
                return pipeline;
            SDL_GPUShader* vertex = CompileInternal(kCopyVertexShader, true, 0, 0);
            SDL_GPUShader* fragment = CompileInternal(kCopyFragmentShader, false, 1, 1);
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
            if (!g_pointSampler)
            {
                SDL_GPUSamplerCreateInfo sampler{};
                sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                g_pointSampler = SDL_CreateGPUSampler(g_device, &sampler);
            }
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
            { scale, 1.0f, 1.0f, 1.0f } };
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
}
