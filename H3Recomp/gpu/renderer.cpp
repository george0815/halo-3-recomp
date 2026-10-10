#include "gpu/renderer.h"

#include "gpu/command_processor.h"
#include "gpu/registers.h"
#include "gpu/shader/translator.h"
#include "gpu/texture.h"
#include "memory.h"

#include <SDL3/SDL.h>
#include <shaderc/shaderc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace h3::gpu::renderer
{
    namespace
    {
        // --- Registers this file reads beyond registers.h ----------------

        constexpr uint32_t kRegPaSuScModeCntl = 0x2205;
        constexpr uint32_t kRegPaClVteCntl = 0x2206;
        constexpr uint32_t kRegRbDepthControl = 0x2200;
        constexpr uint32_t kRegRbColorMask = 0x2104;
        constexpr uint32_t kRegRbStencilRefMask = 0x210D;
        constexpr uint32_t kRegRbBlendRed = 0x2105;  // then green, blue, alpha
        constexpr uint32_t kRegRbColorControl = 0x2202;
        constexpr uint32_t kRegRbAlphaRef = 0x210E;
        constexpr uint32_t kRegRbBlendControl[4] = { 0x2201, 0x2209, 0x220A, 0x220B };
        constexpr uint32_t kRegColorInfo[4] = { kRegRbColorInfo, kRegRbColor1Info, kRegRbColor2Info, kRegRbColor3Info };
        constexpr uint32_t kRegVgtIndxOffset = 0x2102;
        constexpr uint32_t kRegVgtMultiPrimIbResetIndx = 0x2103;
        constexpr uint32_t kRegSqContextMisc = 0x2181;
        constexpr uint32_t kRegPaSuVtxCntl = 0x2302;  // pix center : 1 (0 = D3D, integer centers)

        // Xenos primitive types (VGT_DRAW_INITIATOR bits 0-5).
        enum : uint32_t
        {
            kPrimPointList = 1,
            kPrimLineList = 2,
            kPrimLineStrip = 3,
            kPrimTriangleList = 4,
            kPrimTriangleFan = 5,
            kPrimTriangleStrip = 6,
            kPrimRectangleList = 8,
            kPrimQuadList = 13,
        };

        // --- State -----------------------------------------------------

        SDL_GPUDevice* g_device = nullptr;
        bool g_enabled = false;
        SDL_GPUTextureFormat g_depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;

        shaderc_compiler_t g_compiler = nullptr;
        SDL_GPUSampler* g_sampler = nullptr;
        bool g_noTextures = false;
        bool g_noWriteBack = false;  // H3_NO_WRITEBACK: small resolves stay on the GPU

        // Swaps seen, and resolves within the current frame (for dumps).
        uint32_t g_swaps = 0;
        uint32_t g_frameResolves = 0;  // H3_NO_TEXTURES: placeholders instead of textures from memory
        SDL_GPUTexture* g_placeholder2D = nullptr;
        SDL_GPUTexture* g_placeholder3D = nullptr;
        SDL_GPUTexture* g_placeholderCube = nullptr;

        uint64_t Hash(const void* data, size_t size, uint64_t hash = 0xCBF29CE484222325ull)
        {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            for (size_t i = 0; i < size; i++)
                hash = (hash ^ bytes[i]) * 0x100000001B3ull;
            return hash;
        }

        void LogOnce(const std::string& message)
        {
            static std::mutex mutex;
            static std::vector<std::string> seen;
            std::lock_guard lock(mutex);
            if (std::find(seen.begin(), seen.end(), message) != seen.end())
                return;
            seen.push_back(message);
            fprintf(stderr, "[render] %s\n", message.c_str());
        }

        // --- Shaders ---------------------------------------------------

        struct Shader
        {
            bool ok = false;
            SDL_GPUShader* shader = nullptr;
            shader::ShaderInfo info;
        };

        std::unordered_map<uint64_t, std::unique_ptr<Shader>> g_shaders;
        Shader* g_vertexShader = nullptr;
        Shader* g_pixelShader = nullptr;

        Shader* LoadShader(bool vertex, const uint32_t* dwords, uint32_t count)
        {
            uint64_t key = Hash(dwords, count * 4, vertex ? 1 : 2);
            auto it = g_shaders.find(key);
            if (it != g_shaders.end())
                return it->second.get();

            auto entry = std::make_unique<Shader>();
            shader::TranslationResult translated = shader::Translate(vertex, dwords, count);
            if (!translated.ok)
            {
                LogOnce("shader translation failed: " + translated.error);
            }
            else
            {
                shaderc_compile_options_t options = shaderc_compile_options_initialize();
                shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
                shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);
                shaderc_compilation_result_t result = shaderc_compile_into_spv(g_compiler, translated.glsl.data(),
                    translated.glsl.size(), vertex ? shaderc_vertex_shader : shaderc_fragment_shader, "xenos", "main", options);
                shaderc_compile_options_release(options);
                if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
                {
                    LogOnce(std::string("shader compilation failed: ") + shaderc_result_get_error_message(result));
                }
                else
                {
                    SDL_GPUShaderCreateInfo info{};
                    info.code_size = shaderc_result_get_length(result);
                    info.code = reinterpret_cast<const Uint8*>(shaderc_result_get_bytes(result));
                    info.entrypoint = "main";
                    info.format = SDL_GPU_SHADERFORMAT_SPIRV;
                    info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
                    info.num_samplers = uint32_t(translated.info.textures.size());
                    info.num_storage_buffers = vertex ? 1 : 0;
                    info.num_uniform_buffers = 2;
                    entry->shader = SDL_CreateGPUShader(g_device, &info);
                    if (!entry->shader)
                        LogOnce(std::string("SDL_CreateGPUShader failed: ") + SDL_GetError());
                    entry->info = translated.info;
                    entry->ok = entry->shader != nullptr;
                }
                shaderc_result_release(result);
            }
            Shader* shader = entry.get();
            g_shaders.emplace(key, std::move(entry));
            return shader;
        }

        // Internal shaders (GLSL compiled at startup), for copies the game
        // does with EDRAM hardware.
        SDL_GPUShader* CompileInternal(const char* glsl, bool vertex, uint32_t samplers, uint32_t uniformBuffers)
        {
            shaderc_compile_options_t options = shaderc_compile_options_initialize();
            shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
            shaderc_compilation_result_t result = shaderc_compile_into_spv(g_compiler, glsl, strlen(glsl),
                vertex ? shaderc_vertex_shader : shaderc_fragment_shader, "internal", "main", options);
            shaderc_compile_options_release(options);
            SDL_GPUShader* shader = nullptr;
            if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
                LogOnce(std::string("internal shader compilation failed: ") + shaderc_result_get_error_message(result));
            else
            {
                SDL_GPUShaderCreateInfo info{};
                info.code_size = shaderc_result_get_length(result);
                info.code = reinterpret_cast<const Uint8*>(shaderc_result_get_bytes(result));
                info.entrypoint = "main";
                info.format = SDL_GPU_SHADERFORMAT_SPIRV;
                info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
                info.num_samplers = samplers;
                info.num_uniform_buffers = uniformBuffers;
                shader = SDL_CreateGPUShader(g_device, &info);
            }
            shaderc_result_release(result);
            return shader;
        }

        // Copies that reinterpret bits the way EDRAM and resolves do on the
        // console, as a fullscreen pass (scissored to the rectangle):
        //   value:  the source as is (depth into R32_FLOAT)
        //   depth:  float depth as the 24_8 word's bytes (stencil in the low
        //           byte), for depth resolved to 8_8_8_8 and sampled so
        //   gamma:  8_8_8_8_GAMMA render targets keep linear values on the
        //           host; memory holds them gamma-encoded (about sqrt)
        //   7e3:    2_10_10_10_FLOAT values as their 7e3 bits, for EDRAM
        //           drawn as 7e3 and then used as 2_10_10_10 (resolves
        //           themselves convert by value)
        enum class CopyMode : int32_t
        {
            Value = 0,
            DepthBytes = 1,
            FloatTo7e3 = 2,
            LinearToGamma = 3,
        };
        std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline*> g_copyPipelines;  // target format -> pipeline

        void ConvertCopy(SDL_GPUTexture* source, SDL_GPUTexture* destination, SDL_GPUTextureFormat destinationFormat,
            uint32_t destinationWidth, uint32_t destinationHeight, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w,
            uint32_t h, CopyMode mode, bool depthFloat = false, float scale = 1.0f);
        SDL_GPUSampler* g_pointSampler = nullptr;

        SDL_GPUGraphicsPipeline* CopyPipeline(SDL_GPUTextureFormat format)
        {
            SDL_GPUGraphicsPipeline*& pipeline = g_copyPipelines[uint32_t(format)];
            if (pipeline)
                return pipeline;
            static const char* kVertex = R"(#version 450
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
            static const char* kFragment = R"(#version 450
layout(set = 2, binding = 0) uniform sampler2D xe_source;
layout(set = 3, binding = 0) uniform XeCopy
{
    ivec4 offset;  // xy: source - destination
    ivec4 mode;    // x: copy mode, y: depth is 20e4 float
    vec4 scale;    // x: 2^RB_COPY_DEST_INFO exp bias, for color
};
layout(location = 0) out vec4 xe_out;

uint xe_float_to_20e4(float f)
{
    uint u = floatBitsToUint(clamp(f, 0.0, 1.99999988));
    if (u < 0x38800000u)
        u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23), 24u);
    else
        u += 0xC8000000u;
    return ((u + 3u + ((u >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}

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
    vec4 v = texelFetch(xe_source, ivec2(gl_FragCoord.xy) + offset.xy, 0);
    if (mode.x != 1)
        v.rgb *= scale.x;
    if (mode.x == 1)
    {
        uint d = mode.y != 0 ? xe_float_to_20e4(v.r) : uint(round(clamp(v.r, 0.0, 1.0) * 16777215.0));
        xe_out = vec4(0.0, float(d & 255u), float((d >> 8) & 255u), float(d >> 16)) / 255.0;
    }
    else if (mode.x == 3)
        xe_out = vec4(sqrt(clamp(v.rgb, 0.0, 1.0)), v.a);  // gamma render targets: Halo decodes by squaring
    else if (mode.x == 2)
        xe_out = vec4(xe_float_to_7e3(v.r), xe_float_to_7e3(v.g), xe_float_to_7e3(v.b), round(clamp(v.a, 0.0, 1.0) * 3.0) / 3.0);
    else
        xe_out = v;
}
)";
            SDL_GPUShader* vertex = CompileInternal(kVertex, true, 0, 0);
            SDL_GPUShader* fragment = CompileInternal(kFragment, false, 1, 1);
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

        // --- Render targets and resolved textures ---------------------

        // Large enough for any EDRAM surface Halo 3 uses (1152 or 1280 wide).
        constexpr uint32_t kRenderTargetWidth = 1280;
        constexpr uint32_t kRenderTargetHeight = 1024;

        struct RenderTarget
        {
            SDL_GPUTexture* texture = nullptr;
            SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
            uint32_t xenosFormat = 0;
        };
        std::unordered_map<uint64_t, RenderTarget> g_renderTargets;  // (base, format, depth) -> texture

        SDL_GPUTextureFormat ColorTargetFormat(uint32_t xenosFormat)
        {
            switch (xenosFormat)
            {
            case 0:   // k_8_8_8_8
            case 1:   // k_8_8_8_8_GAMMA
                return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            case 2:   // k_2_10_10_10
            case 10:  // k_2_10_10_10_AS_10_10_10_10
                return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
            case 4:   // k_16_16 (fixed point, -32..32)
            case 6:   // k_16_16_FLOAT
                return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
            case 14:  // k_32_FLOAT
                return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            case 15:  // k_32_32_FLOAT
                return SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
            default:  // 16_16_16_16, the float variants, 2_10_10_10_FLOAT
                return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            }
        }

        RenderTarget* GetRenderTarget(uint32_t base, uint32_t xenosFormat, bool depth)
        {
            uint64_t key = (uint64_t(base) << 8) | (uint64_t(xenosFormat) << 1) | (depth ? 1 : 0);
            auto it = g_renderTargets.find(key);
            if (it != g_renderTargets.end())
                return &it->second;

            RenderTarget target;
            target.format = depth ? g_depthFormat : ColorTargetFormat(xenosFormat);
            target.xenosFormat = xenosFormat;
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = target.format;
            info.usage = depth ? (SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)
                               : (SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            info.width = kRenderTargetWidth;
            info.height = kRenderTargetHeight;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            info.sample_count = SDL_GPU_SAMPLECOUNT_1;
            target.texture = SDL_CreateGPUTexture(g_device, &info);
            if (!target.texture)
            {
                LogOnce(std::string("render target creation failed: ") + SDL_GetError());
                return nullptr;
            }
            return &g_renderTargets.emplace(key, target).first->second;
        }

        // Host texture standing in for a resolve destination in memory: a
        // tiled pitch x height image of bpp-byte texels at base.
        struct ResolvedTexture
        {
            uint32_t base = 0, size = 0;
            uint32_t pitch = 0, height = 0, bpp = 4;
            uint32_t xenosFormat = 0;
            SDL_GPUTextureFormat hostFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
            SDL_GPUTexture* texture = nullptr;
            uint32_t generation = 0;  // bumped by every resolve into it
            bool depthFloat = false;  // depth resolves: from a 20e4 float depth buffer
        };
        std::vector<ResolvedTexture> g_resolved;

        // Addresses may come as physical (0x0xxxxxxx, resolve registers) or
        // through a CPU view of physical memory (0xA/0xC/0xE..., the swap's
        // fetch constant); compare physical.
        ResolvedTexture* FindResolved(uint32_t address)
        {
            address &= 0x1FFFFFFF;
            for (ResolvedTexture& texture : g_resolved)
                if (address >= texture.base && address < texture.base + texture.size)
                    return &texture;
            return nullptr;
        }

        // Texel position of an address inside a resolved texture: D3D
        // advances the base in 32x32 macro tiles.
        void ResolvedOffset(const ResolvedTexture& texture, uint32_t address, uint32_t& x, uint32_t& y)
        {
            uint32_t delta = (address & 0x1FFFFFFF) - texture.base;
            uint32_t macroTileBytes = 32 * 32 * texture.bpp;
            uint32_t macroRowBytes = texture.pitch * 32 * texture.bpp;
            y = (delta / macroRowBytes) * 32;
            x = ((delta % macroRowBytes) / macroTileBytes) * 32;
        }

        // Bytes per texel and host format of a resolve destination format.
        bool ResolveFormat(uint32_t textureFormat, uint32_t& bpp, SDL_GPUTextureFormat& format)
        {
            switch (textureFormat)
            {
            case 6:   // k_8_8_8_8
            case 14:  // k_8_8_8_8_A
            case 62:  // k_8_8_8_8_GAMMA_EDRAM
                bpp = 4, format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                return true;
            case 7:   // k_2_10_10_10
                bpp = 4, format = SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
                return true;
            case 16:  // k_10_11_11
            case 17:  // k_11_11_10
                bpp = 4, format = SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
                return true;
            case 25:  // k_16_16
            case 31:  // k_16_16_FLOAT
                bpp = 4, format = SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
                return true;
            case 26:  // k_16_16_16_16
            case 32:  // k_16_16_16_16_FLOAT
            case 63:  // k_2_10_10_10_FLOAT_EDRAM
                bpp = 8, format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
                return true;
            case 22:  // k_24_8 (depth resolves)
            case 23:  // k_24_8_FLOAT
            case 36:  // k_32_FLOAT
                bpp = 4, format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
                return true;
            case 37:  // k_32_32_FLOAT
                bpp = 8, format = SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT;
                return true;
            case 4:   // k_5_6_5
            case 24:  // k_16
            case 30:  // k_16_FLOAT
                bpp = 2, format = SDL_GPU_TEXTUREFORMAT_R16_FLOAT;
                return true;
            case 2:   // k_8
                bpp = 1, format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
                return true;
            default:
                return false;
            }
        }

        // --- Frame recording -------------------------------------------

        // System constants (see translator.h for the layout).
        struct SystemConstants
        {
            uint32_t bools[8];
            uint32_t loops[32];
            uint32_t vfetch[96][4];
            float ndcScale[4];
            float ndcOffset[4];
            uint32_t psParamGen;
            uint32_t flags;
            uint32_t rectIndices;
            uint32_t pad;
            uint32_t textures[32][4];  // per texture fetch constant: swizzle, signs, exp adjust, enabled
            float colorExpBias[4];     // 2^RB_COLOR_INFO exp bias, per render target
            float alphaRef;
            uint32_t pad2[3];
        };

        struct TargetSet
        {
            SDL_GPUTexture* color[4] = {};
            SDL_GPUTextureFormat colorFormat[4] = {};
            uint32_t colorCount = 0;  // highest bound slot + 1
            SDL_GPUTexture* depth = nullptr;

            bool operator==(const TargetSet& other) const
            {
                return memcmp(color, other.color, sizeof(color)) == 0 && colorCount == other.colorCount &&
                    depth == other.depth;
            }
        };

        struct PendingDraw
        {
            SDL_GPUGraphicsPipeline* pipeline = nullptr;
            float vertexConstants[256 * 4];
            float pixelConstants[256 * 4];
            SystemConstants system;
            std::vector<SDL_GPUTextureSamplerBinding> vertexTextures, pixelTextures;
            SDL_GPUViewport viewport;
            SDL_Rect scissor;
            uint8_t stencilReference = 0;
            SDL_FColor blendConstants{};
            bool indexed = true;
            uint32_t firstIndex = 0;  // in the frame's index buffer, in indices
            uint32_t count = 0;
        };

        SDL_GPUCommandBuffer* g_commands = nullptr;
        TargetSet g_targets;
        std::vector<std::unique_ptr<PendingDraw>> g_pending;
        std::vector<uint32_t> g_memoryStaging;  // vertex memory buffer contents
        std::vector<uint32_t> g_indexStaging;
        std::unordered_map<uint64_t, uint32_t> g_stagedRanges;

        SDL_GPUBuffer* g_memoryBuffer = nullptr;
        uint32_t g_memoryBufferSize = 0;
        SDL_GPUBuffer* g_indexBuffer = nullptr;
        uint32_t g_indexBufferSize = 0;
        SDL_GPUTransferBuffer* g_transfer = nullptr;
        uint32_t g_transferSize = 0;

        std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline*> g_pipelines;

        SDL_GPUCommandBuffer* Commands()
        {
            if (!g_commands)
                g_commands = SDL_AcquireGPUCommandBuffer(g_device);
            return g_commands;
        }

        void EnsureBuffer(SDL_GPUBuffer*& buffer, uint32_t& capacity, uint32_t size, SDL_GPUBufferUsageFlags usage)
        {
            if (size <= capacity && buffer)
                return;
            if (buffer)
                SDL_ReleaseGPUBuffer(g_device, buffer);
            capacity = std::max<uint32_t>(size, capacity * 2);
            capacity = std::max<uint32_t>(capacity, 1 << 20);
            SDL_GPUBufferCreateInfo info{};
            info.usage = usage;
            info.size = capacity;
            buffer = SDL_CreateGPUBuffer(g_device, &info);
        }

        // Records everything pending: one copy pass for the frame's vertex
        // memory and indices, then one render pass for the draws. Called
        // before anything that can't happen inside a render pass.
        void Flush()
        {
            if (g_pending.empty())
                return;
            SDL_GPUCommandBuffer* commands = Commands();

            uint32_t memoryBytes = uint32_t(std::max<size_t>(g_memoryStaging.size(), 1) * 4);
            uint32_t indexBytes = uint32_t(std::max<size_t>(g_indexStaging.size(), 1) * 4);
            EnsureBuffer(g_memoryBuffer, g_memoryBufferSize, memoryBytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ);
            EnsureBuffer(g_indexBuffer, g_indexBufferSize, indexBytes, SDL_GPU_BUFFERUSAGE_INDEX);
            uint32_t transferBytes = memoryBytes + indexBytes;
            if (transferBytes > g_transferSize || !g_transfer)
            {
                if (g_transfer)
                    SDL_ReleaseGPUTransferBuffer(g_device, g_transfer);
                g_transferSize = std::max(transferBytes, g_transferSize * 2);
                SDL_GPUTransferBufferCreateInfo info{};
                info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                info.size = g_transferSize;
                g_transfer = SDL_CreateGPUTransferBuffer(g_device, &info);
            }
            uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(g_device, g_transfer, true));
            memcpy(mapped, g_memoryStaging.data(), g_memoryStaging.size() * 4);
            memcpy(mapped + memoryBytes, g_indexStaging.data(), g_indexStaging.size() * 4);
            SDL_UnmapGPUTransferBuffer(g_device, g_transfer);

            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUTransferBufferLocation source{ g_transfer, 0 };
            SDL_GPUBufferRegion destination{ g_memoryBuffer, 0, memoryBytes };
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
            source.offset = memoryBytes;
            destination = { g_indexBuffer, 0, indexBytes };
            SDL_UploadToGPUBuffer(copy, &source, &destination, true);
            SDL_EndGPUCopyPass(copy);

            SDL_GPUColorTargetInfo colors[4]{};
            for (uint32_t i = 0; i < g_targets.colorCount; i++)
            {
                colors[i].texture = g_targets.color[i];
                colors[i].load_op = SDL_GPU_LOADOP_LOAD;
                colors[i].store_op = SDL_GPU_STOREOP_STORE;
            }
            SDL_GPUDepthStencilTargetInfo depth{};
            depth.texture = g_targets.depth;
            depth.load_op = SDL_GPU_LOADOP_LOAD;
            depth.store_op = SDL_GPU_STOREOP_STORE;
            depth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
            depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass* pass =
                SDL_BeginGPURenderPass(commands, colors, g_targets.colorCount, g_targets.depth ? &depth : nullptr);

            SDL_GPUBufferBinding indexBinding{ g_indexBuffer, 0 };
            SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            SDL_BindGPUVertexStorageBuffers(pass, 0, &g_memoryBuffer, 1);
            for (const auto& draw : g_pending)
            {
                SDL_BindGPUGraphicsPipeline(pass, draw->pipeline);
                SDL_SetGPUViewport(pass, &draw->viewport);
                SDL_SetGPUScissor(pass, &draw->scissor);
                SDL_SetGPUStencilReference(pass, draw->stencilReference);
                SDL_SetGPUBlendConstants(pass, draw->blendConstants);
                if (!draw->vertexTextures.empty())
                    SDL_BindGPUVertexSamplers(pass, 0, draw->vertexTextures.data(), uint32_t(draw->vertexTextures.size()));
                if (!draw->pixelTextures.empty())
                    SDL_BindGPUFragmentSamplers(pass, 0, draw->pixelTextures.data(), uint32_t(draw->pixelTextures.size()));
                SDL_PushGPUVertexUniformData(commands, 0, draw->vertexConstants, sizeof(draw->vertexConstants));
                SDL_PushGPUVertexUniformData(commands, 1, &draw->system, sizeof(draw->system));
                SDL_PushGPUFragmentUniformData(commands, 0, draw->pixelConstants, sizeof(draw->pixelConstants));
                SDL_PushGPUFragmentUniformData(commands, 1, &draw->system, sizeof(draw->system));
                if (draw->indexed)
                    SDL_DrawGPUIndexedPrimitives(pass, draw->count, 1, draw->firstIndex, 0, 0);
                else
                    SDL_DrawGPUPrimitives(pass, draw->count, 1, 0, 0);
            }
            SDL_EndGPURenderPass(pass);

            g_pending.clear();
            g_memoryStaging.clear();
            g_indexStaging.clear();
            g_stagedRanges.clear();
        }

        // --- Pipelines -------------------------------------------------

        SDL_GPUBlendFactor BlendFactor(uint32_t xenos)
        {
            switch (xenos)
            {
            case 0: return SDL_GPU_BLENDFACTOR_ZERO;
            case 1: return SDL_GPU_BLENDFACTOR_ONE;
            case 4: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
            case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            case 7: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return SDL_GPU_BLENDFACTOR_DST_COLOR;
            case 9: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
            case 10: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
            case 11: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
            case 12:
            case 14: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
            case 13:
            case 15: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 16: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
            default: return SDL_GPU_BLENDFACTOR_ONE;
            }
        }

        SDL_GPUBlendOp BlendOp(uint32_t xenos)
        {
            switch (xenos)
            {
            case 1: return SDL_GPU_BLENDOP_SUBTRACT;
            case 2: return SDL_GPU_BLENDOP_MIN;
            case 3: return SDL_GPU_BLENDOP_MAX;
            case 4: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            default: return SDL_GPU_BLENDOP_ADD;
            }
        }

        SDL_GPUGraphicsPipeline* GetPipeline(Shader* vs, Shader* ps, SDL_GPUPrimitiveType topology, const TargetSet& targets,
            const uint32_t blend[4], uint32_t colorMask, uint32_t depthControl, uint32_t stencilRefMask)
        {
            struct Key
            {
                SDL_GPUShader* vs;
                SDL_GPUShader* ps;
                uint32_t topology, colorCount, depthFormat;
                uint32_t colorFormats[4];
                uint32_t blend[4];
                uint32_t colorMask, depthControl, stencilMasks;
            } key{};
            key.vs = vs->shader;
            key.ps = ps->shader;
            key.topology = topology;
            key.colorCount = targets.colorCount;
            key.depthFormat = targets.depth ? g_depthFormat : 0;
            for (uint32_t i = 0; i < 4; i++)
            {
                key.colorFormats[i] = targets.colorFormat[i];
                key.blend[i] = blend[i];
            }
            key.colorMask = colorMask;
            key.depthControl = depthControl;
            key.stencilMasks = stencilRefMask & 0xFFFF00;
            uint64_t hash = Hash(&key, sizeof(key));
            auto it = g_pipelines.find(hash);
            if (it != g_pipelines.end())
                return it->second;

            SDL_GPUColorTargetDescription descriptions[4]{};
            for (uint32_t i = 0; i < targets.colorCount; i++)
            {
                descriptions[i].format = targets.colorFormat[i];
                SDL_GPUColorTargetBlendState& state = descriptions[i].blend_state;
                uint32_t b = blend[i];
                state.src_color_blendfactor = BlendFactor(b & 0x1F);
                state.color_blend_op = BlendOp((b >> 5) & 7);
                state.dst_color_blendfactor = BlendFactor((b >> 8) & 0x1F);
                state.src_alpha_blendfactor = BlendFactor((b >> 16) & 0x1F);
                state.alpha_blend_op = BlendOp((b >> 21) & 7);
                state.dst_alpha_blendfactor = BlendFactor((b >> 24) & 0x1F);
                state.enable_blend = !((b & 0x1FFF) == 0x0001 && ((b >> 16) & 0x1FFF) == 0x0001);  // not one/zero/add
                // Targets the pixel shader doesn't write keep their contents.
                uint32_t mask = (colorMask >> (4 * i)) & 0xF;
                if (!(ps->info.colorTargetMask & (1u << i)))
                    mask = 0;
                state.color_write_mask = SDL_GPUColorComponentFlags(mask);
                state.enable_color_write_mask = true;
            }

            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vs->shader;
            info.fragment_shader = ps->shader;
            info.primitive_type = topology;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // TODO: PA_SU_SC_MODE_CNTL culling
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
            info.rasterizer_state.enable_depth_clip = true;
            info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            if (targets.depth)
            {
                SDL_GPUDepthStencilState& ds = info.depth_stencil_state;
                ds.enable_depth_test = (depthControl >> 1) & 1;
                ds.enable_depth_write = ds.enable_depth_test && ((depthControl >> 2) & 1);
                ds.compare_op = SDL_GPUCompareOp(((depthControl >> 4) & 7) + 1);  // Xenos order + 1
                ds.enable_stencil_test = depthControl & 1;
                auto stencil = [](uint32_t func, uint32_t fail, uint32_t pass, uint32_t depthFail) {
                    SDL_GPUStencilOpState state;
                    state.compare_op = SDL_GPUCompareOp(func + 1);
                    state.fail_op = SDL_GPUStencilOp(fail + 1);
                    state.pass_op = SDL_GPUStencilOp(pass + 1);
                    state.depth_fail_op = SDL_GPUStencilOp(depthFail + 1);
                    return state;
                };
                ds.front_stencil_state = stencil((depthControl >> 8) & 7, (depthControl >> 11) & 7,
                    (depthControl >> 14) & 7, (depthControl >> 17) & 7);
                ds.back_stencil_state = ((depthControl >> 7) & 1)
                    ? stencil((depthControl >> 20) & 7, (depthControl >> 23) & 7, (depthControl >> 26) & 7, (depthControl >> 29) & 7)
                    : ds.front_stencil_state;
                ds.compare_mask = uint8_t(stencilRefMask >> 8);
                ds.write_mask = uint8_t(stencilRefMask >> 16);
                if (getenv("H3_NO_DEPTH"))  // debugging: no depth or stencil tests
                    ds.enable_depth_test = ds.enable_stencil_test = false;
                if (getenv("H3_NO_STENCIL"))  // debugging: no stencil test
                    ds.enable_stencil_test = false;
                if (getenv("H3_DEPTH_FLIP"))  // debugging: swap less/greater
                {
                    static const SDL_GPUCompareOp kFlip[] = { SDL_GPU_COMPAREOP_INVALID, SDL_GPU_COMPAREOP_NEVER,
                        SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL,
                        SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_NOT_EQUAL, SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
                        SDL_GPU_COMPAREOP_ALWAYS };
                    ds.compare_op = kFlip[ds.compare_op];
                }
            }
            info.target_info.color_target_descriptions = descriptions;
            info.target_info.num_color_targets = targets.colorCount;
            info.target_info.has_depth_stencil_target = targets.depth != nullptr;
            info.target_info.depth_stencil_format = g_depthFormat;

            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
            if (!pipeline)
                LogOnce(std::string("pipeline creation failed: ") + SDL_GetError());
            g_pipelines.emplace(hash, pipeline);
            return pipeline;
        }

        // --- Draw setup ------------------------------------------------

        float RegisterFloat(uint32_t index)
        {
            uint32_t bits = ReadRegister(index);
            float value;
            memcpy(&value, &bits, 4);
            return value;
        }

        int32_t Signed15(uint32_t value) { return int32_t(value << 17) >> 17; }

        // Guest indices of the draw, made host-endian and offset, or 0..n-1
        // for auto-indexed draws.
        std::vector<uint32_t> GuestIndices(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
        {
            uint32_t count = initiator >> 16;
            std::vector<uint32_t> indices(count);
            uint32_t offset = ReadRegister(kRegVgtIndxOffset);
            if (((initiator >> 6) & 3) != 0 || indexBase == 0)
            {
                for (uint32_t i = 0; i < count; i++)
                    indices[i] = i + offset;
                return indices;
            }
            bool wide = (initiator >> 11) & 1;
            uint32_t endian = dmaSize >> 30;
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t value;
                if (wide)
                {
                    uint32_t raw;
                    memcpy(&raw, GuestPtr<uint8_t>(indexBase + i * 4), 4);
                    value = endian == 2 ? __builtin_bswap32(raw) : endian == 3 ? (raw << 16 | raw >> 16) : raw;
                    if (endian == 1)
                        value = ((raw & 0x00FF00FFu) << 8) | ((raw >> 8) & 0x00FF00FFu);
                }
                else
                {
                    uint16_t raw;
                    memcpy(&raw, GuestPtr<uint8_t>(indexBase + i * 2), 2);
                    value = endian == 1 ? __builtin_bswap16(raw) : raw;
                }
                indices[i] = value;
            }
            // The reset index marks strip/fan restarts; keep it recognizable.
            for (uint32_t& index : indices)
                if (index != (wide ? 0xFFFFFFFFu : 0xFFFFu))
                    index = (index + offset) & 0xFFFFFF;
            return indices;
        }

        void AppendTriangles(std::vector<uint32_t>& out, const std::vector<uint32_t>& in, uint32_t primitive,
            uint32_t resetIndex, bool resetEnabled)
        {
            auto isReset = [&](uint32_t index) { return resetEnabled && (index == resetIndex || index == 0xFFFFFFFFu); };
            switch (primitive)
            {
            case kPrimTriangleStrip:
            {
                size_t start = 0;
                for (size_t i = 0; i <= in.size(); i++)
                {
                    if (i < in.size() && !isReset(in[i]))
                        continue;
                    for (size_t j = start; j + 2 < i; j++)
                    {
                        bool odd = (j - start) & 1;
                        out.push_back(in[j]);
                        out.push_back(in[odd ? j + 2 : j + 1]);
                        out.push_back(in[odd ? j + 1 : j + 2]);
                    }
                    start = i + 1;
                }
                break;
            }
            case kPrimTriangleFan:
                for (size_t j = 1; j + 1 < in.size(); j++)
                {
                    out.push_back(in[0]);
                    out.push_back(in[j]);
                    out.push_back(in[j + 1]);
                }
                break;
            case kPrimQuadList:
                for (size_t j = 0; j + 3 < in.size(); j += 4)
                {
                    out.insert(out.end(), { in[j], in[j + 1], in[j + 2], in[j], in[j + 2], in[j + 3] });
                }
                break;
            default:
                out.insert(out.end(), in.begin(), in.end());
                break;
            }
        }

        // Vertex fetch constants the shader uses: copy their data into the
        // frame's memory staging and point the system constants at it.
        void StageVertexData(const Shader& vs, SystemConstants& system)
        {
            for (uint32_t index : vs.info.vertexFetchConstants)
            {
                uint32_t word0 = ReadRegister(kRegShaderConstantFetch + index * 2);
                uint32_t word1 = ReadRegister(kRegShaderConstantFetch + index * 2 + 1);
                uint32_t* entry = system.vfetch[index];
                if ((word0 & 3) != 3)  // not a vertex fetch constant
                {
                    entry[0] = entry[1] = 0;
                    continue;
                }
                uint32_t address = PhysicalToGuest((word0 & ~3u));
                uint32_t dwords = (word1 >> 2) & 0xFFFFFF;
                entry[1] = dwords;
                entry[2] = word1 & 3;
                // Draws share large vertex buffers: stage each range once per
                // flush.
                uint64_t key = (uint64_t(address) << 32) | dwords;
                auto staged = g_stagedRanges.find(key);
                if (staged != g_stagedRanges.end())
                {
                    entry[0] = staged->second;
                    continue;
                }
                entry[0] = uint32_t(g_memoryStaging.size());
                g_stagedRanges.emplace(key, entry[0]);
                size_t start = g_memoryStaging.size();
                g_memoryStaging.resize(start + dwords);
                memcpy(g_memoryStaging.data() + start, GuestPtr<uint8_t>(address), size_t(dwords) * 4);
            }
        }

        SDL_GPUTexture* PlaceholderFor(shader::TextureBinding binding)
        {
            switch (binding.dimension)
            {
            case ucode::TextureDimension::D3OrStacked: return g_placeholder3D;
            case ucode::TextureDimension::Cube: return g_placeholderCube;
            default: return g_placeholder2D;
            }
        }

        // Textures from guest memory, decoded once and re-checked against
        // memory at most once per frame.
        struct CachedTexture
        {
            SDL_GPUTexture* texture = nullptr;
            uint64_t contentHash = 0;
            uint32_t checkedFrame = ~0u;
        };
        std::unordered_map<uint64_t, CachedTexture> g_textures;  // fetch constant layout -> texture
        std::unordered_map<uint32_t, SDL_GPUSampler*> g_samplers;  // clamp/filter modes -> sampler

        uint64_t HashMemory(const uint8_t* data, size_t size)
        {
            uint64_t hash = 0xCBF29CE484222325ull;
            size_t words = size / 8;
            for (size_t i = 0; i < words; i++)
            {
                uint64_t word;
                memcpy(&word, data + i * 8, 8);
                hash = (hash ^ word) * 0x100000001B3ull;
            }
            return Hash(data + words * 8, size - words * 8, hash);
        }

        SDL_GPUTexture* MemoryTexture(const texture::FetchConstant& fetch)
        {
            // Cube maps: six 2D faces one after another.
            bool cube = fetch.dimension == 3;
            uint32_t faces = cube ? 6 : 1;
            uint32_t faceSize = texture::GuestSize(fetch);
            uint32_t stride = cube ? texture::FaceStride(fetch) : faceSize;
            uint32_t size = stride * (faces - 1) + faceSize;
            if (!faceSize || fetch.address + uint64_t(size) > 0x20000000)
                return nullptr;
            uint32_t layout[] = { fetch.address, fetch.format, fetch.endian, fetch.width, fetch.height, fetch.pitch,
                uint32_t(fetch.tiled) | (uint32_t(fetch.packedMips) << 1) | (uint32_t(cube) << 2) };
            CachedTexture& cached = g_textures[Hash(layout, sizeof(layout))];
            if (cached.checkedFrame == g_swaps && cached.texture)
                return cached.texture;
            cached.checkedFrame = g_swaps;
            const uint8_t* guest = GuestPtr<uint8_t>(PhysicalToGuest(fetch.address));
            uint64_t contentHash = HashMemory(guest, size);
            if (cached.texture && contentHash == cached.contentHash)
                return cached.texture;

            texture::Decoded decodedFaces[6];
            for (uint32_t face = 0; face < faces; face++)
                if (!texture::Decode(fetch, guest + size_t(face) * stride, decodedFaces[face]))
                    return nullptr;
            const texture::Decoded& decoded = decodedFaces[0];
            if (!cached.texture)
            {
                SDL_GPUTextureCreateInfo info{};
                info.type = cube ? SDL_GPU_TEXTURETYPE_CUBE : SDL_GPU_TEXTURETYPE_2D;
                info.format = decoded.format;
                info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
                info.width = decoded.width;
                info.height = decoded.height;
                info.layer_count_or_depth = faces;
                info.num_levels = 1;
                cached.texture = SDL_CreateGPUTexture(g_device, &info);
                if (!cached.texture)
                {
                    LogOnce("texture creation failed (format " + std::to_string(fetch.format) + "): " + SDL_GetError());
                    return nullptr;
                }
            }
            cached.contentHash = contentHash;

            uint32_t faceBytes = uint32_t(decoded.data.size());
            SDL_GPUTransferBufferCreateInfo transferInfo{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, faceBytes * faces, 0 };
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g_device, &transferInfo);
            uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(g_device, transfer, false));
            for (uint32_t face = 0; face < faces; face++)
                memcpy(mapped + size_t(face) * faceBytes, decodedFaces[face].data.data(), faceBytes);
            SDL_UnmapGPUTransferBuffer(g_device, transfer);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            for (uint32_t face = 0; face < faces; face++)
            {
                SDL_GPUTextureTransferInfo source{ transfer, face * faceBytes, decoded.rowTexels, decoded.height };
                SDL_GPUTextureRegion region{};
                region.texture = cached.texture;
                region.layer = face;
                region.w = decoded.width;
                region.h = decoded.height;
                region.d = 1;
                SDL_UploadToGPUTexture(copy, &source, &region, false);
            }
            SDL_EndGPUCopyPass(copy);
            SDL_ReleaseGPUTransferBuffer(g_device, transfer);
            return cached.texture;
        }

        SDL_GPUSampler* SamplerFor(const texture::FetchConstant& fetch)
        {
            uint32_t key = fetch.clampX | (fetch.clampY << 3) | (fetch.clampZ << 6) | (fetch.magFilter << 9) | (fetch.minFilter << 11);
            SDL_GPUSampler*& sampler = g_samplers[key];
            if (sampler)
                return sampler;
            // Clamp modes: 0 repeat, 1 mirrored repeat, the rest clamp
            // (edge, half way, border; approximated as clamp to edge).
            auto address = [](uint32_t clamp) {
                return clamp == 0 ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT
                    : clamp == 1  ? SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT
                                  : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            };
            SDL_GPUSamplerCreateInfo info{};
            info.mag_filter = fetch.magFilter == 0 ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
            info.min_filter = fetch.minFilter == 0 ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
            info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            info.address_mode_u = address(fetch.clampX);
            info.address_mode_v = address(fetch.clampY);
            info.address_mode_w = address(fetch.clampZ);
            sampler = SDL_CreateGPUSampler(g_device, &info);
            return sampler ? sampler : g_sampler;
        }

        bool IsFloatHostFormat(SDL_GPUTextureFormat format);

        // Copies a resolved rectangle back to guest memory, tiled and
        // byte-swapped as the console stores it, for the CPU to read (Halo's
        // auto exposure reads back its luminance). Synchronous: waits for the
        // GPU, so only for small resolves.
        void WriteBackResolve(const ResolvedTexture& texture, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t endian)
        {
            if (texture.hostFormat == SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT)  // not the guest's 10_11_11 layout
                return;
            uint32_t bpp = texture.bpp, bytes = w * h * bpp;
            SDL_GPUTransferBufferCreateInfo info{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
            SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(g_device, &info);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            SDL_GPUTextureRegion region{};
            region.texture = texture.texture;
            region.x = x0;
            region.y = y0;
            region.w = w;
            region.h = h;
            region.d = 1;
            SDL_GPUTextureTransferInfo destination{ download, 0, w, h };
            SDL_DownloadFromGPUTexture(copy, &region, &destination);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_commands);
            g_commands = nullptr;
            SDL_WaitForGPUFences(g_device, true, &fence, 1);
            SDL_ReleaseGPUFence(g_device, fence);

            const uint8_t* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(g_device, download, false));
            uint8_t* guest = GuestPtr<uint8_t>(PhysicalToGuest(texture.base));
            uint8_t element[16];
            for (uint32_t y = 0; y < h; y++)
            {
                for (uint32_t x = 0; x < w; x++)
                {
                    memcpy(element, pixels + (size_t(y) * w + x) * bpp, bpp);
                    // RB_COPY_DEST_INFO endian: 1 8in16, 2 8in32, 3 16in32, 4 8in64.
                    switch (endian)
                    {
                    case 1:
                        for (uint32_t i = 0; i + 1 < bpp; i += 2)
                            std::swap(element[i], element[i + 1]);
                        break;
                    case 2:
                        for (uint32_t i = 0; i + 3 < bpp; i += 4)
                            *reinterpret_cast<uint32_t*>(element + i) = __builtin_bswap32(*reinterpret_cast<uint32_t*>(element + i));
                        break;
                    case 3:
                        for (uint32_t i = 0; i + 3 < bpp; i += 4)
                        {
                            uint32_t v = *reinterpret_cast<uint32_t*>(element + i);
                            *reinterpret_cast<uint32_t*>(element + i) = (v << 16) | (v >> 16);
                        }
                        break;
                    case 4:
                        for (uint32_t i = 0; i + 7 < bpp; i += 8)
                            *reinterpret_cast<uint64_t*>(element + i) = __builtin_bswap64(*reinterpret_cast<uint64_t*>(element + i));
                        break;
                    default:
                        break;
                    }
                    size_t offset = size_t(texture::TiledOffset(x0 + x, y0 + y, texture.pitch, bpp)) * bpp;
                    if (offset + bpp <= texture.size)
                        memcpy(guest + offset, element, bpp);
                }
            }
            SDL_UnmapGPUTransferBuffer(g_device, download);
            SDL_ReleaseGPUTransferBuffer(g_device, download);
        }

        // A texture inside a resolved texture at another size or position
        // (memory reused for smaller surfaces, or a later macro tile): copied
        // out at exactly the size the fetch constant asks for, so UVs and
        // clamping work. Re-copied when the resolved texture changes.
        struct SubTexture
        {
            SDL_GPUTexture* texture = nullptr;
            SDL_GPUTexture* source = nullptr;
            uint32_t generation = ~0u;
        };
        std::unordered_map<uint64_t, SubTexture> g_subTextures;

        SDL_GPUTexture* ResolvedView(ResolvedTexture& resolved, const texture::FetchConstant& fetch, uint32_t address)
        {
            uint32_t hostWidth = std::max(resolved.pitch, 32u), hostHeight = std::max(resolved.height, 32u);
            uint32_t x, y;
            ResolvedOffset(resolved, address, x, y);
            uint32_t width = fetch.width, height = fetch.height;
            // Depth resolved to 8_8_8_8 and sampled as such: the depth word's
            // bytes rather than the float.
            bool depthBytes = resolved.hostFormat == SDL_GPU_TEXTUREFORMAT_R32_FLOAT && fetch.valid &&
                (fetch.format == 6 || fetch.format == 14);
            SDL_GPUTextureFormat format = depthBytes ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : resolved.hostFormat;
            if (!fetch.valid || x >= hostWidth || y >= hostHeight || width == 0 || height == 0 ||
                (x == 0 && y == 0 && width == hostWidth && height == hostHeight && !depthBytes))
                return resolved.texture;
            uint32_t key[] = { address & 0x1FFFFFFF, width, height, uint32_t(format) };
            SubTexture& sub = g_subTextures[Hash(key, sizeof(key))];
            if (sub.texture && sub.source == resolved.texture && sub.generation == resolved.generation)
                return sub.texture;
            if (!sub.texture)
            {
                SDL_GPUTextureCreateInfo info{};
                info.type = SDL_GPU_TEXTURETYPE_2D;
                info.format = format;
                info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
                info.width = width;
                info.height = height;
                info.layer_count_or_depth = 1;
                info.num_levels = 1;
                sub.texture = SDL_CreateGPUTexture(g_device, &info);
                if (!sub.texture)
                    return resolved.texture;
            }
            uint32_t w = std::min(width, hostWidth - x), h = std::min(height, hostHeight - y);
            if (depthBytes)
                ConvertCopy(resolved.texture, sub.texture, format, width, height, int32_t(x), int32_t(y), 0, 0, w, h,
                    CopyMode::DepthBytes, resolved.depthFloat);
            else
            {
                SDL_GPUBlitInfo blit{};
                blit.source = { resolved.texture, 0, 0, x, y, w, h };
                blit.destination = { sub.texture, 0, 0, 0, 0, w, h };
                blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                blit.filter = SDL_GPU_FILTER_NEAREST;
                SDL_BlitGPUTexture(Commands(), &blit);
            }
            sub.source = resolved.texture;
            sub.generation = resolved.generation;
            return sub.texture;
        }

        // Resolved render targets first (their host textures hold what the
        // game drew), then textures decoded from memory, else placeholders.
        std::vector<SDL_GPUTextureSamplerBinding> BindTextures(const Shader& shader, SystemConstants& system)
        {
            std::vector<SDL_GPUTextureSamplerBinding> bindings;
            for (const shader::TextureBinding& binding : shader.info.textures)
            {
                SDL_GPUTexture* texture = PlaceholderFor(binding);
                SDL_GPUSampler* sampler = g_sampler;
                bool cubeBinding = binding.dimension == ucode::TextureDimension::Cube;
                if (cubeBinding)
                {
                    uint32_t words[6];
                    for (uint32_t i = 0; i < 6; i++)
                        words[i] = ReadRegister(kRegShaderConstantFetch + binding.fetchConstant * 6 + i);
                    texture::FetchConstant fetch = texture::ParseFetchConstant(words);
                    if (fetch.valid && fetch.dimension == 3 && !g_noTextures && !FindResolved(fetch.address))
                    {
                        if (SDL_GPUTexture* decoded = MemoryTexture(fetch))
                        {
                            texture = decoded;
                            sampler = SamplerFor(fetch);
                            uint32_t* constants = system.textures[binding.fetchConstant & 31];
                            constants[0] = fetch.swizzle;
                            constants[1] = fetch.signs;
                            constants[2] = uint32_t(fetch.expAdjust);
                            constants[3] = 1;
                        }
                    }
                }
                else if (binding.dimension == ucode::TextureDimension::D2 || binding.dimension == ucode::TextureDimension::D1)
                {
                    uint32_t words[6];
                    for (uint32_t i = 0; i < 6; i++)
                        words[i] = ReadRegister(kRegShaderConstantFetch + binding.fetchConstant * 6 + i);
                    texture::FetchConstant fetch = texture::ParseFetchConstant(words);
                    if (ResolvedTexture* resolved = FindResolved(words[1] & 0xFFFFF000))
                    {
                        texture = ResolvedView(*resolved, fetch, words[1] & 0xFFFFF000);
                        // Sign modes (gamma decode above all) apply to resolves
                        // stored as unorm too; float copies already hold the
                        // values. The swizzle and exponent don't: the host copy
                        // keeps the render target's component order and scale,
                        // not memory's.
                        if (fetch.valid && fetch.signs != 0 && !IsFloatHostFormat(resolved->hostFormat))
                        {
                            uint32_t* constants = system.textures[binding.fetchConstant & 31];
                            constants[0] = 0x688;  // identity XYZW
                            constants[1] = fetch.signs;
                            constants[2] = 0;
                            constants[3] = 1;
                        }
                    }
                    else if (fetch.valid && fetch.dimension <= 1 && !g_noTextures)
                    {
                        if (SDL_GPUTexture* decoded = MemoryTexture(fetch))
                        {
                            texture = decoded;
                            sampler = SamplerFor(fetch);
                            uint32_t* constants = system.textures[binding.fetchConstant & 31];
                            constants[0] = fetch.swizzle;
                            constants[1] = fetch.signs;
                            constants[2] = uint32_t(fetch.expAdjust);
                            constants[3] = 1;
                        }
                        else
                            LogOnce("unsupported texture format " + std::to_string(fetch.format));
                    }
                }
                bindings.push_back({ texture, sampler });
            }
            return bindings;
        }

        // A rectangle of source (at sx, sy) into destination (at dx, dy),
        // through CopyPipeline. Not inside a render pass.
        void ConvertCopy(SDL_GPUTexture* source, SDL_GPUTexture* destination, SDL_GPUTextureFormat destinationFormat,
            uint32_t destinationWidth, uint32_t destinationHeight, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w,
            uint32_t h, CopyMode mode, bool depthFloat, float scale)
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
            SDL_Rect scissor{ dx, dy, int(w), int(h) };
            SDL_SetGPUScissor(pass, &scissor);
            SDL_GPUTextureSamplerBinding binding{ source, g_pointSampler };
            SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
            struct
            {
                int32_t offset[4];
                int32_t mode[4];
                float scale[4];
            } constants = { { sx - dx, sy - dy, 0, 0 }, { int32_t(mode), depthFloat ? 1 : 0, 0, 0 }, { scale, 1.0f, 1.0f, 1.0f } };
            SDL_PushGPUFragmentUniformData(Commands(), 0, &constants, sizeof(constants));
            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }

        // 2_10_10_10_FLOAT (and its _AS_16_16_16_16 alias) render targets
        // hold 7e3 floats; the host keeps them as 16-bit floats.
        bool Is7e3(uint32_t xenosFormat) { return xenosFormat == 3 || xenosFormat == 12; }
        bool IsFloatHostFormat(SDL_GPUTextureFormat format)
        {
            return format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT || format == SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT ||
                format == SDL_GPU_TEXTUREFORMAT_R32_FLOAT || format == SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT ||
                format == SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
        }

        // EDRAM aliasing: the same EDRAM base can be drawn to or resolved
        // from in another format (Halo draws its final image as
        // 2_10_10_10_FLOAT and resolves it as 2_10_10_10). Each format has its
        // own host texture, so on a switch the contents move over to the
        // texture now in use. The console reinterprets the bits; this blit
        // converts values instead, which is close but not exact.
        std::unordered_map<uint32_t, RenderTarget*> g_colorAtBase;  // EDRAM base -> target last used there

        RenderTarget* GetColorTarget(uint32_t base, uint32_t xenosFormat)
        {
            RenderTarget* target = GetRenderTarget(base, xenosFormat, false);
            if (!target)
                return nullptr;
            RenderTarget*& current = g_colorAtBase[base];
            if (current && current != target)
            {
                Flush();
                if (Is7e3(current->xenosFormat) && !IsFloatHostFormat(target->format))
                    ConvertCopy(current->texture, target->texture, target->format, kRenderTargetWidth, kRenderTargetHeight, 0, 0, 0,
                        0, kRenderTargetWidth, kRenderTargetHeight, CopyMode::FloatTo7e3);
                else
                {
                    SDL_GPUBlitInfo blit{};
                    blit.source = { current->texture, 0, 0, 0, 0, kRenderTargetWidth, kRenderTargetHeight };
                    blit.destination = { target->texture, 0, 0, 0, 0, kRenderTargetWidth, kRenderTargetHeight };
                    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                    blit.filter = SDL_GPU_FILTER_NEAREST;
                    SDL_BlitGPUTexture(Commands(), &blit);
                }
            }
            current = target;
            return target;
        }

        // Host target set for the current EDRAM state.
        TargetSet CurrentTargets(const Shader& ps, uint32_t colorMask, uint32_t depthControl, uint32_t mode)
        {
            TargetSet targets;
            if (mode == kEdramModeColorDepth)
            {
                for (uint32_t i = 0; i < 4; i++)
                {
                    if (!((colorMask >> (4 * i)) & 0xF) || !(ps.info.colorTargetMask & (1u << i)))
                        continue;
                    uint32_t info = ReadRegister(kRegColorInfo[i]);
                    RenderTarget* target = GetColorTarget(info & 0xFFF, (info >> 16) & 0xF);
                    if (!target)
                        continue;
                    targets.color[i] = target->texture;
                    targets.colorFormat[i] = target->format;
                    targets.colorCount = i + 1;
                }
                // Gaps below the highest target: bind something of the right
                // kind (writes are masked off).
                for (uint32_t i = 0; i < targets.colorCount; i++)
                {
                    if (!targets.color[i])
                    {
                        RenderTarget* scratch = GetRenderTarget(0xFFF, 0, false);
                        targets.color[i] = scratch->texture;
                        targets.colorFormat[i] = scratch->format;
                    }
                }
            }
            if (depthControl & 7)  // stencil, depth test or depth write
            {
                uint32_t info = ReadRegister(kRegRbDepthInfo);
                if (RenderTarget* target = GetRenderTarget(info & 0xFFF, (info >> 16) & 1, true))
                    targets.depth = target->texture;
            }
            return targets;
        }

        // Saves width x height of an RGBA8 texture as a PNG. Submits the
        // current command buffer and waits for it (debugging only).
        void DumpTexture(SDL_GPUTexture* texture, uint32_t width, uint32_t height, const char* path,
            SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
        {
            bool half = format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            uint32_t bytes = width * height * (half ? 8 : 4);
            SDL_GPUTransferBufferCreateInfo info{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
            SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(g_device, &info);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            SDL_GPUTextureRegion region{};
            region.texture = texture;
            region.w = width;
            region.h = height;
            region.d = 1;
            SDL_GPUTextureTransferInfo destination{ download, 0, width, height };
            SDL_DownloadFromGPUTexture(copy, &region, &destination);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_commands);
            g_commands = nullptr;
            SDL_WaitForGPUFences(g_device, true, &fence, 1);
            SDL_ReleaseGPUFence(g_device, fence);
            void* pixels = SDL_MapGPUTransferBuffer(g_device, download, false);
            // 16-bit floats: clamp to RGBA8 in place (front half), and print
            // the average so HDR values beyond 1 show up.
            if (half)
            {
                const uint16_t* halves = static_cast<const uint16_t*>(pixels);
                uint8_t* out = static_cast<uint8_t*>(pixels);
                double sum[4] = {};
                for (uint32_t i = 0; i < width * height; i++)
                    for (int c = 0; c < 4; c++)
                    {
                        uint16_t h = halves[i * 4 + c];
                        uint32_t sign = h >> 15, exponent = (h >> 10) & 31, mantissa = h & 1023;
                        float value = exponent == 0 ? std::ldexp(float(mantissa), -24)
                                    : exponent == 31 ? 65504.0f
                                                     : std::ldexp(float(mantissa | 1024), int(exponent) - 25);
                        if (sign)
                            value = -value;
                        sum[c] += value;
                        out[i * 4 + c] = uint8_t(std::clamp(value, 0.0f, 1.0f) * 255.0f);
                    }
                fprintf(stderr, "[render] %s average %g %g %g %g\n", path, sum[0] / (width * height), sum[1] / (width * height),
                    sum[2] / (width * height), sum[3] / (width * height));
                format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            }
            // 4-byte formats other than RGBA8: convert in place for the PNG.
            uint32_t* texels = static_cast<uint32_t*>(pixels);
            for (uint32_t i = 0; i < width * height && format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM; i++)
            {
                uint32_t v = texels[i];
                if (format == SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM)
                    v = ((v >> 2) & 0xFF) | (((v >> 12) & 0xFF) << 8) | (((v >> 22) & 0xFF) << 16) | 0xFF000000u;
                else if (format == SDL_GPU_TEXTUREFORMAT_R32_FLOAT)
                {
                    float f;
                    memcpy(&f, &v, 4);
                    uint32_t g = uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f);
                    v = g | (g << 8) | (g << 16) | 0xFF000000u;
                }
                texels[i] = v;
            }
            SDL_Surface* surface =
                SDL_CreateSurfaceFrom(int(width), int(height), SDL_PIXELFORMAT_ABGR8888, pixels, int(width * 4));
            if (surface && SDL_SavePNG(surface, path))
                fprintf(stderr, "[render] wrote %s (%ux%u)\n", path, width, height);
            else
                fprintf(stderr, "[render] couldn't save %s: %s\n", path, SDL_GetError());
            SDL_DestroySurface(surface);
            SDL_UnmapGPUTransferBuffer(g_device, download);
            SDL_ReleaseGPUTransferBuffer(g_device, download);
        }

        uint32_t EnvNumber(const char* name)
        {
            const char* value = getenv(name);
            return value ? uint32_t(strtoul(value, nullptr, 0)) : 0u;
        }


        // --- Present ---------------------------------------------------

        std::mutex g_frameMutex;
        Frame g_frame;
    }

    bool Initialize(SDL_GPUDevice* device)
    {
        const char* setting = getenv("H3_RENDER");
        if (setting && strcmp(setting, "0") == 0)
            return false;
        g_device = device;
        g_compiler = shaderc_compiler_initialize();
        if (!g_compiler)
            return false;

        if (!SDL_GPUTextureSupportsFormat(device, g_depthFormat, SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
            g_depthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;

        SDL_GPUSamplerCreateInfo sampler{};
        sampler.min_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
        sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        sampler.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler.max_lod = 16.0f;
        g_sampler = SDL_CreateGPUSampler(device, &sampler);
        g_noTextures = getenv("H3_NO_TEXTURES") != nullptr;
        g_noWriteBack = getenv("H3_NO_WRITEBACK") != nullptr;

        // Gray stand-ins for textures that aren't loaded yet.
        auto placeholder = [&](SDL_GPUTextureType type, uint32_t layers) {
            SDL_GPUTextureCreateInfo info{};
            info.type = type;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = info.height = 1;
            info.layer_count_or_depth = layers;
            info.num_levels = 1;
            return SDL_CreateGPUTexture(device, &info);
        };
        g_placeholder2D = placeholder(SDL_GPU_TEXTURETYPE_2D, 1);
        g_placeholder3D = placeholder(SDL_GPU_TEXTURETYPE_3D, 1);
        g_placeholderCube = placeholder(SDL_GPU_TEXTURETYPE_CUBE, 6);
        SDL_GPUTransferBufferCreateInfo transferInfo{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, 6 * 4, 0 };
        SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
        uint32_t* gray = static_cast<uint32_t*>(SDL_MapGPUTransferBuffer(device, transfer, false));
        for (int i = 0; i < 6; i++)
            gray[i] = 0xFF808080;
        SDL_UnmapGPUTransferBuffer(device, transfer);
        SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        for (auto [texture, layers] : { std::pair{ g_placeholder2D, 1 }, { g_placeholder3D, 1 }, { g_placeholderCube, 6 } })
        {
            for (int layer = 0; layer < layers; layer++)
            {
                SDL_GPUTextureTransferInfo source{ transfer, uint32_t(layer) * 4, 1, 1 };
                SDL_GPUTextureRegion destination{};
                destination.texture = texture;
                destination.layer = uint32_t(layer);
                destination.w = destination.h = destination.d = 1;
                SDL_UploadToGPUTexture(copy, &source, &destination, false);
            }
        }
        SDL_EndGPUCopyPass(copy);
        SDL_SubmitGPUCommandBuffer(commands);
        SDL_ReleaseGPUTransferBuffer(device, transfer);

        g_enabled = true;
        return true;
    }

    bool IsEnabled()
    {
        return g_enabled;
    }

    void OnShaderLoad(uint32_t type, const uint32_t* dwords, uint32_t dwordCount)
    {
        if (!g_enabled)
            return;
        Shader* shader = LoadShader(type == 0, dwords, dwordCount);
        (type == 0 ? g_vertexShader : g_pixelShader) = shader;
    }

    void OnDraw(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
    {
        uint32_t primitive = initiator & 0x3F;
        // 0x10-0x12 are tessellation patches here (Halo's water): not
        // supported, so they're skipped below as unknown primitive types.
        uint32_t mode = ReadRegister(kRegRbModeControl) & 7;
        if (mode != kEdramModeColorDepth && mode != kEdramModeDepthOnly)
            return;

        uint32_t colorMask = ReadRegister(kRegRbColorMask);
        uint32_t depthControl = ReadRegister(kRegRbDepthControl);
        TargetSet targets = CurrentTargets(*g_pixelShader, colorMask, depthControl, mode);
        if (targets.colorCount == 0 && !targets.depth)
            return;

        SDL_GPUPrimitiveType topology;
        switch (primitive)
        {
        case kPrimPointList: topology = SDL_GPU_PRIMITIVETYPE_POINTLIST; break;
        case kPrimLineList: topology = SDL_GPU_PRIMITIVETYPE_LINELIST; break;
        case kPrimLineStrip: topology = SDL_GPU_PRIMITIVETYPE_LINESTRIP; break;
        case kPrimTriangleList:
        case kPrimTriangleFan:
        case kPrimTriangleStrip:
        case kPrimQuadList:
        case kPrimRectangleList:
            topology = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            break;
        default:
            LogOnce("unsupported primitive type " + std::to_string(primitive));
            return;
        }

        if (!(targets == g_targets))
        {
            Flush();
            g_targets = targets;
        }
        uint32_t blend[4];
        for (uint32_t i = 0; i < 4; i++)
            blend[i] = ReadRegister(kRegRbBlendControl[i]);
        uint32_t stencilRefMask = ReadRegister(kRegRbStencilRefMask);
        SDL_GPUGraphicsPipeline* pipeline =
            GetPipeline(g_vertexShader, g_pixelShader, topology, targets, blend, colorMask, depthControl, stencilRefMask);
        if (!pipeline)
            return;

        auto draw = std::make_unique<PendingDraw>();
        draw->pipeline = pipeline;
        draw->stencilReference = uint8_t(stencilRefMask);
        draw->blendConstants = { RegisterFloat(kRegRbBlendRed), RegisterFloat(kRegRbBlendRed + 1), RegisterFloat(kRegRbBlendRed + 2),
            RegisterFloat(kRegRbBlendRed + 3) };

        // Constants: vertex shaders use float constants 0-255, pixel shaders
        // 256-511.
        for (uint32_t i = 0; i < 256 * 4; i++)
        {
            uint32_t v = ReadRegister(kRegShaderConstantAlu + i);
            uint32_t p = ReadRegister(kRegShaderConstantAlu + 256 * 4 + i);
            memcpy(&draw->vertexConstants[i], &v, 4);
            memcpy(&draw->pixelConstants[i], &p, 4);
        }
        SystemConstants& system = draw->system;
        memset(&system, 0, sizeof(system));
        for (uint32_t i = 0; i < 8; i++)
            system.bools[i] = ReadRegister(kRegShaderConstantBool + i);
        for (uint32_t i = 0; i < 32; i++)
            system.loops[i] = ReadRegister(kRegShaderConstantLoop + i);
        uint32_t programControl = ReadRegister(kRegSqProgramCntl);
        uint32_t contextMisc = ReadRegister(kRegSqContextMisc);
        system.psParamGen = ((programControl >> 18) & 1) ? ((contextMisc >> 8) & 0xFF) : 0xFF;

        // Viewport transform into host clip space over the whole render
        // target: Xenos screen position = ndc * scale + offset (each part
        // only when enabled in PA_CL_VTE_CNTL), plus the window offset.
        // Screen Y grows down from row 0; SDL_GPU's NDC has +Y up.
        uint32_t vte = ReadRegister(kRegPaClVteCntl);
        uint32_t modeControl = ReadRegister(kRegPaSuScModeCntl);
        float xScale = (vte & 1) ? RegisterFloat(kRegPaClVportXScale) : 1.0f;
        float xOffset = (vte & 2) ? RegisterFloat(kRegPaClVportXOffset) : 0.0f;
        float yScale = (vte & 4) ? RegisterFloat(kRegPaClVportYScale) : 1.0f;
        float yOffset = (vte & 8) ? RegisterFloat(kRegPaClVportYOffset) : 0.0f;
        float zScale = (vte & 16) ? RegisterFloat(kRegPaClVportZScale) : 1.0f;
        float zOffset = (vte & 32) ? RegisterFloat(kRegPaClVportZOffset) : 0.0f;
        uint32_t windowOffset = ReadRegister(kRegPaScWindowOffset);
        float windowX = 0.0f, windowY = 0.0f;
        if ((modeControl >> 16) & 1)
        {
            windowX = float(Signed15(windowOffset & 0x7FFF));
            windowY = float(Signed15((windowOffset >> 16) & 0x7FFF));
        }
        // D3D-style pixel centers sit on integers; the host's are at +0.5.
        if (!(ReadRegister(kRegPaSuVtxCntl) & 1))
            windowX += 0.5f, windowY += 0.5f;
        float width = float(kRenderTargetWidth), height = float(kRenderTargetHeight);
        system.ndcScale[0] = xScale * 2.0f / width;
        system.ndcOffset[0] = (xOffset + windowX) * 2.0f / width - 1.0f;
        system.ndcScale[1] = -yScale * 2.0f / height;
        system.ndcOffset[1] = 1.0f - (yOffset + windowY) * 2.0f / height;
        system.ndcScale[2] = zScale;
        system.ndcOffset[2] = zOffset;
        draw->viewport = { 0.0f, 0.0f, width, height, 0.0f, 1.0f };

        // Scissor: window scissor, offset by the window offset unless
        // disabled (bit 31 of the top-left).
        uint32_t tl = ReadRegister(kRegPaScWindowScissorTl), br = ReadRegister(kRegPaScWindowScissorBr);
        int32_t x0 = tl & 0x7FFF, y0 = (tl >> 16) & 0x7FFF, x1 = br & 0x7FFF, y1 = (br >> 16) & 0x7FFF;
        if (!(tl >> 31))
        {
            int32_t ox = Signed15(windowOffset & 0x7FFF), oy = Signed15((windowOffset >> 16) & 0x7FFF);
            x0 += ox, x1 += ox, y0 += oy, y1 += oy;
        }
        x0 = std::clamp(x0, 0, int32_t(kRenderTargetWidth));
        x1 = std::clamp(x1, x0, int32_t(kRenderTargetWidth));
        y0 = std::clamp(y0, 0, int32_t(kRenderTargetHeight));
        y1 = std::clamp(y1, y0, int32_t(kRenderTargetHeight));
        draw->scissor = { x0, y0, x1 - x0, y1 - y0 };
        if (getenv("H3_LOG_SCISSOR") && windowOffset != 0)
        {
            static int logged = 0;
            if (logged++ < 8)
                fprintf(stderr, "[render] window offset %08X tl %08X br %08X -> scissor %d,%d %dx%d ndc y %g %g\n", windowOffset,
                    tl, br, x0, y0, x1 - x0, y1 - y0, system.ndcScale[1], system.ndcOffset[1]);
        }

        if (getenv("H3_LOG_DEPTH"))
        {
            static std::unordered_map<uint64_t, int> seen;
            uint64_t key = (uint64_t(depthControl) << 32) | (uint64_t(mode) << 8) | primitive;
            if (seen[key]++ == 0)
                fprintf(stderr, "[render] depth: mode %u prim %u depthcontrol %08X z scale %g offset %g vte %03X depth info %08X\n",
                    mode, primitive, depthControl, zScale, zOffset, vte, ReadRegister(kRegRbDepthInfo));
        }
        // Position formats: VTX_XY_FMT, VTX_Z_FMT, VTX_W0_FMT (bits 8-10).
        system.flags |= ((vte >> 8) & 7) << 2;
        // Render target exponent bias: color outputs scale by 2^bias (Halo
        // writes its final image to 7e3 with a bias of 5).
        for (uint32_t i = 0; i < 4; i++)
        {
            int32_t bias = int32_t((ReadRegister(kRegColorInfo[i]) >> 20) << 26) >> 26;
            system.colorExpBias[i] = std::ldexp(1.0f, bias);
        }
        // Alpha test: RB_COLORCONTROL function (bits 0-2) and enable (bit 3).
        uint32_t colorControl = ReadRegister(kRegRbColorControl);
        if (colorControl & 8)
            system.flags |= ((colorControl & 7) << 5) | 0x100;
        system.alphaRef = RegisterFloat(kRegRbAlphaRef);
        // D24FS8 depth: round to the 24-bit float format.
        if (targets.depth && ((ReadRegister(kRegRbDepthInfo) >> 16) & 1))
            system.flags |= 2;
        StageVertexData(*g_vertexShader, system);
        draw->vertexTextures = BindTextures(*g_vertexShader, system);
        draw->pixelTextures = BindTextures(*g_pixelShader, system);

        std::vector<uint32_t> indices = GuestIndices(initiator, indexBase, dmaSize);
        if (primitive == kPrimRectangleList)
        {
            system.flags |= 1;
            system.rectIndices = uint32_t(g_memoryStaging.size());
            uint32_t rects = uint32_t(indices.size() / 3);
            g_memoryStaging.insert(g_memoryStaging.end(), indices.begin(), indices.begin() + rects * 3);
            draw->indexed = false;
            draw->count = rects * 6;
        }
        else
        {
            uint32_t resetIndex = ReadRegister(kRegVgtMultiPrimIbResetIndx);
            bool resetEnabled = (modeControl >> 21) & 1;
            draw->firstIndex = uint32_t(g_indexStaging.size());
            AppendTriangles(g_indexStaging, indices, primitive, resetIndex, resetEnabled);
            draw->count = uint32_t(g_indexStaging.size()) - draw->firstIndex;
        }
        if (draw->count == 0)
            return;
        g_pending.push_back(std::move(draw));
    }

    void OnResolve()
    {
        if (!g_enabled)
            return;
        uint32_t copyControl = ReadRegister(kRegRbCopyControl);
        uint32_t source = copyControl & 7;
        bool depthSource = source >= 4;

        // The rectangle: D3D puts its three corners in vertex fetch
        // constant 0 (Xenia's GetResolveInfo).
        uint32_t word0 = ReadRegister(kRegShaderConstantFetch), word1 = ReadRegister(kRegShaderConstantFetch + 1);
        if ((word0 & 3) != 3 || ((word1 >> 2) & 0xFFFFFF) != 6)
        {
            LogOnce("unexpected resolve vertex buffer");
            return;
        }
        const uint8_t* vertices = GuestPtr<uint8_t>(PhysicalToGuest(word0 & ~3u));
        float corners[6];
        for (int i = 0; i < 6; i++)
        {
            uint32_t raw;
            memcpy(&raw, vertices + i * 4, 4);
            uint32_t endian = word1 & 3;
            raw = endian == 2 ? __builtin_bswap32(raw) : endian == 3 ? (raw << 16 | raw >> 16) : raw;
            memcpy(&corners[i], &raw, 4);
        }
        int32_t x0 = int32_t(std::floor(std::min({ corners[0], corners[2], corners[4] }) + 0.5f));
        int32_t y0 = int32_t(std::floor(std::min({ corners[1], corners[3], corners[5] }) + 0.5f));
        int32_t x1 = int32_t(std::floor(std::max({ corners[0], corners[2], corners[4] }) + 0.5f));
        int32_t y1 = int32_t(std::floor(std::max({ corners[1], corners[3], corners[5] }) + 0.5f));
        if ((ReadRegister(kRegPaSuScModeCntl) >> 16) & 1)
        {
            uint32_t windowOffset = ReadRegister(kRegPaScWindowOffset);
            int32_t ox = Signed15(windowOffset & 0x7FFF), oy = Signed15((windowOffset >> 16) & 0x7FFF);
            x0 += ox, x1 += ox, y0 += oy, y1 += oy;
        }
        x0 = std::clamp(x0 & ~7, 0, int32_t(kRenderTargetWidth));
        y0 = std::clamp(y0 & ~7, 0, int32_t(kRenderTargetHeight));
        x1 = std::clamp((x1 + 7) & ~7, x0, int32_t(kRenderTargetWidth));
        y1 = std::clamp((y1 + 7) & ~7, y0, int32_t(kRenderTargetHeight));
        if (x1 <= x0 || y1 <= y0)
            return;

        RenderTarget* target;
        bool depthFloat = false;
        if (depthSource)
        {
            uint32_t depthInfo = ReadRegister(kRegRbDepthInfo);
            depthFloat = (depthInfo >> 16) & 1;
            target = GetRenderTarget(depthInfo & 0xFFF, depthFloat, true);
        }
        else
        {
            uint32_t colorInfo = ReadRegister(kRegColorInfo[source]);
            target = GetColorTarget(colorInfo & 0xFFF, (colorInfo >> 16) & 0xF);
        }
        if (!target)
            return;

        // Destination: the host texture covering the destination memory.
        uint32_t destBase = ReadRegister(kRegRbCopyDestBase) & 0x1FFFFFFF;
        uint32_t destPitchReg = ReadRegister(kRegRbCopyDestPitch);
        uint32_t destInfo = ReadRegister(kRegRbCopyDestInfo);
        uint32_t destFormat = (destInfo >> 7) & 0x3F;
        uint32_t bpp;
        SDL_GPUTextureFormat hostFormat;
        if (!ResolveFormat(destFormat, bpp, hostFormat))
        {
            LogOnce("unsupported resolve format " + std::to_string(destFormat));
            return;
        }
        // Depth goes to any 32-bit destination (Halo uses 8_8_8_8) and is
        // sampled back as 24_8: keep it as a float.
        if (depthSource && bpp == 4)
            hostFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        ResolvedTexture* texture = FindResolved(destBase);
        // Color formats of one size share a texture (the blit converts);
        // depth's float texture is kept apart from color.
        bool depthMismatch = texture && (hostFormat == SDL_GPU_TEXTUREFORMAT_R32_FLOAT) !=
            (texture->hostFormat == SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
        if (!texture || texture->bpp != bpp || depthMismatch)
        {
            ResolvedTexture created;
            created.base = destBase;
            created.pitch = ((destPitchReg & 0x3FFF) + 31) & ~31u;
            created.height = ((((destPitchReg >> 16) & 0x3FFF) + 31) & ~31u);
            created.height = std::max(created.height, uint32_t(y1 - y0 + 31) & ~31u);
            created.bpp = bpp;
            created.size = created.pitch * created.height * bpp;
            created.xenosFormat = destFormat;
            created.hostFormat = hostFormat;
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = hostFormat;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = std::max(created.pitch, 32u);
            info.height = std::max(created.height, 32u);
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            created.texture = SDL_CreateGPUTexture(g_device, &info);
            if (!created.texture)
            {
                LogOnce(std::string("resolve texture creation failed: ") + SDL_GetError());
                return;
            }
            if (texture)
            {
                // Same memory reinterpreted at another size: replace it.
                SDL_ReleaseGPUTexture(g_device, texture->texture);
                *texture = created;
            }
            else
            {
                g_resolved.push_back(created);
                texture = &g_resolved.back();
            }
        }

        uint32_t offsetX, offsetY;
        ResolvedOffset(*texture, destBase, offsetX, offsetY);
        uint32_t destX = offsetX + uint32_t(x0), destY = offsetY + uint32_t(y0);
        uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
        if (destX + w > texture->pitch || destY + h > texture->height)
        {
            w = std::min(w, texture->pitch - std::min(destX, texture->pitch));
            h = std::min(h, texture->height - std::min(destY, texture->height));
            if (!w || !h)
                return;
        }

        if (getenv("H3_LOG_RESOLVES"))
            fprintf(stderr, "[render] f%u r%u resolve rt%u %d,%d-%d,%d -> %08X (pitch reg %08X fmt %u info %08X) tex %08X %ux%u at %u,%u\n", g_swaps + 1, g_frameResolves + 1, source, x0, y0,
                x1, y1, destBase, destPitchReg, destFormat, destInfo, texture->base, texture->pitch, texture->height, destX, destY);
        Flush();
        uint32_t textureWidth = std::max(texture->pitch, 32u), textureHeight = std::max(texture->height, 32u);
        if (depthSource)
        {
            ConvertCopy(target->texture, texture->texture, texture->hostFormat, textureWidth, textureHeight, x0, y0, int32_t(destX),
                int32_t(destY), w, h, CopyMode::Value);
            texture->depthFloat = depthFloat;
        }
        else if (target->xenosFormat == 1 && !IsFloatHostFormat(texture->hostFormat))  // k_8_8_8_8_GAMMA
            ConvertCopy(target->texture, texture->texture, texture->hostFormat, textureWidth, textureHeight, x0, y0, int32_t(destX),
                int32_t(destY), w, h, CopyMode::LinearToGamma);
        else if (int32_t expBias = int32_t((destInfo >> 16) << 26) >> 26)
        {
            // RB_COPY_DEST_INFO exp bias: the copy scales colors by 2^bias
            // (Halo copies its tone-mapped 7e3 image out with -5).
            ConvertCopy(target->texture, texture->texture, texture->hostFormat, textureWidth, textureHeight, x0, y0, int32_t(destX),
                int32_t(destY), w, h, CopyMode::Value, false, std::ldexp(1.0f, expBias));
        }
        else
        {
            SDL_GPUBlitInfo blit{};
            blit.source = { target->texture, 0, 0, uint32_t(x0), uint32_t(y0), w, h };
            blit.destination = { texture->texture, 0, 0, destX, destY, w, h };
            blit.load_op = SDL_GPU_LOADOP_LOAD;
            blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(Commands(), &blit);
        }

        // H3_DUMP_RESOLVE=<n> (with H3_DUMP_FRAME): save the destination of
        // that frame's n-th resolve (1-based) right after it.
        if (!depthSource && w * h <= 64 * 64 && !g_noWriteBack)
            WriteBackResolve(*texture, destX, destY, w, h, destInfo & 7);
        texture->generation++;
        g_frameResolves++;
        static const uint32_t dumpFrame = EnvNumber("H3_DUMP_FRAME");
        static const uint32_t dumpResolve = EnvNumber("H3_DUMP_RESOLVE");
        static const bool dumpAll = getenv("H3_DUMP_RESOLVE") && strcmp(getenv("H3_DUMP_RESOLVE"), "all") == 0;
        if ((dumpResolve || dumpAll) && g_swaps + 1 == dumpFrame && (dumpAll || g_frameResolves == dumpResolve) &&
            (texture->bpp == 4 || texture->hostFormat == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT))
        {
            char path[96];
            if (dumpAll)
                snprintf(path, sizeof(path), "resolve_%u_%02u_%08X_%ux%u.png", dumpFrame, g_frameResolves, destBase, w, h);
            else
                snprintf(path, sizeof(path), "resolve_%u_%u.png", dumpFrame, dumpResolve);
            DumpTexture(texture->texture, texture->pitch, texture->height, path, texture->hostFormat);
        }
    }

    void OnSwap(uint32_t frontbuffer, uint32_t width, uint32_t height)
    {
        if (!g_enabled)
            return;
        Flush();
        Frame frame;
        if (ResolvedTexture* texture = FindResolved(frontbuffer))
        {
            frame.texture = texture->texture;
            frame.textureWidth = std::max(texture->pitch, 32u);
            frame.textureHeight = std::max(texture->height, 32u);
            frame.width = std::min(width ? width : texture->pitch, texture->pitch);
            frame.height = std::min(height ? height : texture->height, texture->height);
        }
        // H3_DUMP_FRAME=<n>: save the n-th presented frame as frame_<n>.png
        // (or, with H3_DUMP_ADDRESS, the resolved texture at that address).
        g_swaps++;
        g_frameResolves = 0;
        static const uint32_t dumpFrame = EnvNumber("H3_DUMP_FRAME");
        if (dumpFrame != 0 && g_swaps == dumpFrame && !getenv("H3_DUMP_RESOLVE") && frame.texture)
        {
            uint32_t w = frame.width, h = frame.height;
            SDL_GPUTexture* texture = frame.texture;
            SDL_GPUTextureFormat format = FindResolved(frontbuffer)->hostFormat;
            if (const char* address = getenv("H3_DUMP_ADDRESS"))
                if (ResolvedTexture* resolved = FindResolved(uint32_t(strtoul(address, nullptr, 16))))
                    texture = resolved->texture, w = resolved->pitch, h = resolved->height, format = resolved->hostFormat;
            char path[64];
            snprintf(path, sizeof(path), "frame_%u.png", g_swaps);
            DumpTexture(texture, w, h, path, format);
        }

        if (g_commands)
        {
            SDL_SubmitGPUCommandBuffer(g_commands);
            g_commands = nullptr;
        }
        if (frame.texture)
        {
            std::lock_guard lock(g_frameMutex);
            frame.serial = g_frame.serial + 1;
            g_frame = frame;
        }
        else
        {
            static bool logged = false;
            if (!logged)
            {
                logged = true;
                fprintf(stderr, "[render] nothing resolved at the front buffer %08X yet\n", frontbuffer);
            }
        }
    }

    bool AcquireFrame(Frame& frame)
    {
        std::lock_guard lock(g_frameMutex);
        if (!g_frame.texture)
            return false;
        frame = g_frame;
        return true;
    }
}
