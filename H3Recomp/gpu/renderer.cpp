// The renderer's public entry points (gpu/renderer.h): initialization, the
// command processor's draw/resolve/shader/swap notifications, and handing
// finished frames to the window. The work itself is in gpu/renderer/*.cpp;
// renderer/internal.h describes how it's split.

#include "gpu/renderer.h"

#include "config/env.h"
#include "config/settings.h"
#include "gpu/renderer/internal.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace h3::gpu::renderer
{
    using namespace internal;

    namespace
    {
        // Gray: what placeholder textures read as.
        constexpr uint32_t kPlaceholderColor = 0xFF808080;
        // Mip levels the default sampler may use.
        constexpr float kDefaultMaxLod = 16.0f;
        // How often H3_LOG_FPS prints pipeline/shader/texture counts.
        constexpr uint32_t kCountLogInterval = 150;

        // The last presented frame, read by the window on the main thread.
        std::mutex g_frameMutex;
        Frame g_frame;

        // Creates the 1x1 gray placeholder textures (2D, 3D, cube) and
        // uploads their texels.
        void CreatePlaceholders(SDL_GPUDevice* device)
        {
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
            constexpr uint32_t kCubeFaces = 6;
            g_placeholder2D = placeholder(SDL_GPU_TEXTURETYPE_2D, 1);
            g_placeholder3D = placeholder(SDL_GPU_TEXTURETYPE_3D, 1);
            g_placeholderCube = placeholder(SDL_GPU_TEXTURETYPE_CUBE, kCubeFaces);

            // One gray texel per layer (the cube's six faces at most).
            SDL_GPUTransferBufferCreateInfo transferInfo{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kCubeFaces * 4, 0 };
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
            uint32_t* gray = static_cast<uint32_t*>(SDL_MapGPUTransferBuffer(device, transfer, false));
            for (uint32_t i = 0; i < kCubeFaces; i++)
                gray[i] = kPlaceholderColor;
            SDL_UnmapGPUTransferBuffer(device, transfer);
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            for (auto [texture, layers] : { std::pair{ g_placeholder2D, 1u }, { g_placeholder3D, 1u }, { g_placeholderCube, kCubeFaces } })
            {
                for (uint32_t layer = 0; layer < layers; layer++)
                {
                    SDL_GPUTextureTransferInfo source{ transfer, layer * 4, 1, 1 };
                    SDL_GPUTextureRegion destination{};
                    destination.texture = texture;
                    destination.layer = layer;
                    destination.w = destination.h = destination.d = 1;
                    SDL_UploadToGPUTexture(copy, &source, &destination, false);
                }
            }
            SDL_EndGPUCopyPass(copy);
            SDL_SubmitGPUCommandBuffer(commands);
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
    }

    bool Initialize(SDL_GPUDevice* device)
    {
        const char* setting = EnvString("H3_RENDER");
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
        sampler.max_lod = kDefaultMaxLod;
        g_sampler = SDL_CreateGPUSampler(device, &sampler);
        // Render scale: settings.ini, or H3_RENDER_SCALE.
        int scale = int(EnvNumber("H3_RENDER_SCALE", uint32_t(config::Active().video.renderScale)));
        g_renderScale = uint32_t(std::clamp(scale, 1, config::kMaxRenderScale));
        if (g_renderScale > 1)
            fprintf(stderr, "[render] render scale %ux\n", g_renderScale);
        g_noTextures = EnvFlag("H3_NO_TEXTURES");
        g_noWriteBack = EnvFlag("H3_NO_WRITEBACK");
        CreatePlaceholders(device);

        g_enabled = true;
        return true;
    }

    void SetShaderCacheDirectory(const std::filesystem::path& directory)
    {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        g_shaderCacheDirectory = error ? std::filesystem::path() : directory;
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
        Draw(initiator, indexBase, dmaSize);
        DumpAfterDraw();
        g_frameDraws++;
    }

    void OnResolve()
    {
        Resolve();
        g_frameDraws++;
    }

    void OnSwap(uint32_t frontbuffer, uint32_t width, uint32_t height)
    {
        g_frameDraws = 0;
        if (!g_enabled)
            return;
        Flush();

        // The frame to show: whatever was resolved to the front buffer.
        Frame frame;
        if (ResolvedTexture* texture = FindResolved(frontbuffer))
        {
            // The host texture is at the render scale.
            frame.texture = texture->texture;
            frame.textureWidth = texture->HostWidth();
            frame.textureHeight = texture->HostHeight();
            frame.width = std::min(width ? width : texture->pitch, texture->pitch) * g_renderScale;
            frame.height = std::min(height ? height : texture->height, texture->height) * g_renderScale;
        }

        g_swaps++;
        g_frameResolves = 0;
        DumpMemoryAtSwap();
        EndFrameBuffers();
        PollWriteBacks();
        // H3_LOG_FPS: how many pipelines and shaders exist (creating them
        // stalls this thread).
        static const bool logCounts = EnvFlag("H3_LOG_FPS");
        if (logCounts && g_swaps % kCountLogInterval == 0)
            fprintf(stderr, "[render] frame %u: %zu pipelines, %zu shaders, %zu textures\n", g_swaps, PipelineCount(), ShaderCount(),
                TextureCount());
        DumpFrameAtSwap(frontbuffer, frame.texture, frame.width, frame.height);

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
