// Post-processing of the finished frame, after the display gamma ramp:
// anti-aliasing. Halo 3 renders without any (its deferred renderer and
// EDRAM budget ruled out MSAA at 1152x640), so jagged edges are original;
// this is an optional improvement (settings.ini [video] anti_aliasing,
// H3_AA=off|fxaa overrides).
//
// FXAA (Timothy Lottes' Fast Approximate Anti-Aliasing, the 3.11 "quality"
// algorithm): per pixel, find a luma edge through it, walk along the edge
// to both ends, and blend toward the neighbour across the edge by how far
// the pixel is from the nearer end; plus a sub-pixel blend for single-pixel
// features. Runs on the gamma-encoded frame, where luma differences match
// what the eye sees, and so also smooths the HUD's diagonal lines.

#include "gpu/renderer/internal.h"

#include "config/settings.h"

#include <string>
#include <unordered_map>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        const char* kFullscreenVertexShader = R"(#version 450
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

        const char* kFxaaFragmentShader = R"(#version 450
layout(set = 2, binding = 0) uniform sampler2D xe_source;
layout(set = 3, binding = 0) uniform XeFxaa
{
    vec4 frame;  // xy: the frame's top-left in the source (texels), zw: 1 / source size
};
layout(location = 0) out vec4 xe_out;

// Edges need a local contrast of at least max(kEdgeMin, kEdgeMax * the
// brightest luma) (FXAA's "quality" preset values).
const float kEdgeMin = 0.0312;
const float kEdgeMax = 0.125;
// How much single-pixel detail is softened (0 off, 1 full).
const float kSubpixel = 0.75;
// Step sizes (in pixels) of the walk along an edge.
const int kSteps = 12;
const float kStepSize[kSteps] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float Luma(vec3 color) { return dot(color, vec3(0.299, 0.587, 0.114)); }
vec3 Color(vec2 uv) { return textureLod(xe_source, uv, 0.0).rgb; }
float LumaAt(vec2 uv, vec2 texels) { return Luma(Color(uv + texels * frame.zw)); }

void main()
{
    vec2 texel = frame.zw;
    vec2 uv = (gl_FragCoord.xy + frame.xy) * texel;
    vec3 center = Color(uv);
    float lumaCenter = Luma(center);
    float lumaDown = LumaAt(uv, vec2(0.0, -1.0)), lumaUp = LumaAt(uv, vec2(0.0, 1.0));
    float lumaLeft = LumaAt(uv, vec2(-1.0, 0.0)), lumaRight = LumaAt(uv, vec2(1.0, 0.0));
    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;
    if (lumaRange < max(kEdgeMin, lumaMax * kEdgeMax))
    {
        xe_out = vec4(center, 1.0);
        return;
    }

    // Edge direction from the 3x3 neighbourhood.
    float lumaDownLeft = LumaAt(uv, vec2(-1.0, -1.0)), lumaUpRight = LumaAt(uv, vec2(1.0, 1.0));
    float lumaUpLeft = LumaAt(uv, vec2(-1.0, 1.0)), lumaDownRight = LumaAt(uv, vec2(1.0, -1.0));
    float lumaDownUp = lumaDown + lumaUp, lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners = lumaDownLeft + lumaUpLeft, lumaDownCorners = lumaDownLeft + lumaDownRight;
    float lumaRightCorners = lumaDownRight + lumaUpRight, lumaUpCorners = lumaUpRight + lumaUpLeft;
    float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) + abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 +
        abs(-2.0 * lumaRight + lumaRightCorners);
    float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) + abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 +
        abs(-2.0 * lumaDown + lumaDownCorners);
    bool horizontal = edgeHorizontal >= edgeVertical;

    // Which side of the pixel the edge lies on (the steeper gradient).
    float luma1 = horizontal ? lumaDown : lumaLeft, luma2 = horizontal ? lumaUp : lumaRight;
    float gradient1 = luma1 - lumaCenter, gradient2 = luma2 - lumaCenter;
    bool side1 = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));
    float stepLength = horizontal ? texel.y : texel.x;
    float lumaLocalAverage;
    if (side1)
    {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    }
    else
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);

    // Walk both ways along the edge (half a pixel toward it) until the luma
    // leaves the edge's average.
    vec2 edgeUv = uv;
    if (horizontal)
        edgeUv.y += stepLength * 0.5;
    else
        edgeUv.x += stepLength * 0.5;
    vec2 offset = horizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uv1 = edgeUv - offset, uv2 = edgeUv + offset;
    float lumaEnd1 = Luma(Color(uv1)) - lumaLocalAverage, lumaEnd2 = Luma(Color(uv2)) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled, reached2 = abs(lumaEnd2) >= gradientScaled;
    if (!reached1)
        uv1 -= offset;
    if (!reached2)
        uv2 += offset;
    for (int i = 2; i < kSteps && !(reached1 && reached2); i++)
    {
        if (!reached1)
            lumaEnd1 = Luma(Color(uv1)) - lumaLocalAverage;
        if (!reached2)
            lumaEnd2 = Luma(Color(uv2)) - lumaLocalAverage;
        reached1 = abs(lumaEnd1) >= gradientScaled;
        reached2 = abs(lumaEnd2) >= gradientScaled;
        if (!reached1)
            uv1 -= offset * kStepSize[i];
        if (!reached2)
            uv2 += offset * kStepSize[i];
    }

    // Blend across the edge by the distance to its nearer end (only if the
    // luma at that end varies the way the center does).
    float distance1 = horizontal ? uv.x - uv1.x : uv.y - uv1.y;
    float distance2 = horizontal ? uv2.x - uv.x : uv2.y - uv.y;
    bool nearer1 = distance1 < distance2;
    float pixelOffset = 0.5 - min(distance1, distance2) / (distance1 + distance2);
    bool centerSmaller = lumaCenter < lumaLocalAverage;
    bool correctVariation = ((nearer1 ? lumaEnd1 : lumaEnd2) < 0.0) != centerSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel aliasing: blend by how much the center differs from its
    // neighbourhood's average.
    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
    float subpixel = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
    subpixel = (-2.0 * subpixel + 3.0) * subpixel * subpixel;
    finalOffset = max(finalOffset, subpixel * subpixel * kSubpixel);

    vec2 finalUv = uv;
    if (horizontal)
        finalUv.y += finalOffset * stepLength;
    else
        finalUv.x += finalOffset * stepLength;
    xe_out = vec4(Color(finalUv), 1.0);
}
)";
        SDL_GPUGraphicsPipeline* g_fxaaPipeline = nullptr;

        // Whether to run FXAA: settings.ini, H3_AA overrides.
        bool FxaaEnabled()
        {
            static const bool enabled = [] {
                if (const char* override = EnvString("H3_AA"))
                    return std::string(override) != "off";
                return config::Active().video.antiAliasing == config::AntiAliasing::Fxaa;
            }();
            return enabled;
        }

        SDL_GPUGraphicsPipeline* FxaaPipeline()
        {
            if (g_fxaaPipeline)
                return g_fxaaPipeline;
            SDL_GPUShader* vertex = CompileInternal(kFullscreenVertexShader, true, 0, 0);
            SDL_GPUShader* fragment = CompileInternal(kFxaaFragmentShader, false, 1, 1);
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
                g_fxaaPipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
                if (!g_fxaaPipeline)
                    LogOnce(std::string("FXAA pipeline creation failed: ") + SDL_GetError());
            }
            if (vertex)
                SDL_ReleaseGPUShader(g_device, vertex);
            if (fragment)
                SDL_ReleaseGPUShader(g_device, fragment);
            return g_fxaaPipeline;
        }
    }

    void ApplyAntiAliasing(Frame& frame, SDL_GPUTextureFormat& format)
    {
        if (!frame.texture || !FxaaEnabled())
            return;
        SDL_GPUGraphicsPipeline* pipeline = FxaaPipeline();
        if (!pipeline || !g_sampler)
            return;
        // Output textures by size, two alternating (the window may still be
        // showing the last one); never released, as the window keeps
        // wrappers of them (like the gamma ramp's, renderer.cpp).
        struct Outputs
        {
            SDL_GPUTexture* textures[2] = {};
            uint32_t next = 0;
        };
        static std::unordered_map<uint64_t, Outputs> outputs;
        Outputs& set = outputs[(uint64_t(frame.width) << 32) | frame.height];
        SDL_GPUTexture*& target = set.textures[set.next];
        set.next ^= 1;
        if (!target)
        {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = frame.width;
            info.height = frame.height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            target = SDL_CreateGPUTexture(g_device, &info);
            if (!target)
                return;
        }
        float constants[4] = { float(frame.x), float(frame.y), 1.0f / float(frame.textureWidth), 1.0f / float(frame.textureHeight) };
        SDL_GPUColorTargetInfo color{};
        color.texture = target;
        color.load_op = SDL_GPU_LOADOP_DONT_CARE;
        color.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(Commands(), &color, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_GPUViewport viewport{ 0.0f, 0.0f, float(frame.width), float(frame.height), 0.0f, 1.0f };
        SDL_SetGPUViewport(pass, &viewport);
        SDL_GPUTextureSamplerBinding binding{ frame.texture, g_sampler };
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_PushGPUFragmentUniformData(Commands(), 0, constants, sizeof(constants));
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        CountPass(PassKind::Blit, uint64_t(frame.width) * frame.height);
        frame.texture = target;
        frame.textureWidth = frame.width;
        frame.textureHeight = frame.height;
        frame.x = frame.y = 0;
        format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }
}
