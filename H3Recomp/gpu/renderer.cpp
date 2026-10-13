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
#include <deque>
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
        // H3_LOG_FPS also logs video memory use whenever it changes by this
        // much between frames.
        constexpr uint64_t kVramLogStepBytes = 64ull << 20;

        // The last presented frame, read by the window on the main thread.
        std::mutex g_frameMutex;
        Frame g_frame;

        // Frames the GPU may lag behind this thread. Nothing else holds the
        // command processor back: when the GPU fell behind (render scale 3
        // in the menu), every frame's cycled buffers and upload buffers
        // still in use made SDL create new instances, which it never frees -
        // gigabytes of video and host memory in seconds, until the driver
        // ran out. Fewer than draw.cpp's kTransferAge, so upload buffers it
        // reuses are always idle.
        constexpr size_t kMaxFramesInFlight = 2;
        std::deque<SDL_GPUFence*> g_frameFences;  // oldest first

        // Submits the frame's last commands and waits until at most
        // kMaxFramesInFlight frames are still running on the GPU.
        void SubmitFrame()
        {
            // Always submit (even an empty command buffer) for a fence that
            // marks the end of this frame's work; submissions finish in order.
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(Commands());
            g_commands = nullptr;
            if (fence)
                g_frameFences.push_back(fence);
            while (g_frameFences.size() > kMaxFramesInFlight)
            {
                SDL_GPUFence* oldest = g_frameFences.front();
                SDL_WaitForGPUFences(g_device, true, &oldest, 1);
                SDL_ReleaseGPUFence(g_device, oldest);
                g_frameFences.pop_front();
            }
        }

        // Creates the 1x1 gray placeholder textures (2D, 3D, cube, array) and
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
            g_placeholderArray = placeholder(SDL_GPU_TEXTURETYPE_2D_ARRAY, 1);

            // One gray texel per layer (the cube's six faces at most).
            SDL_GPUTransferBufferCreateInfo transferInfo{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kCubeFaces * 4, 0 };
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
            uint32_t* gray = static_cast<uint32_t*>(SDL_MapGPUTransferBuffer(device, transfer, false));
            for (uint32_t i = 0; i < kCubeFaces; i++)
                gray[i] = kPlaceholderColor;
            SDL_UnmapGPUTransferBuffer(device, transfer);
            SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            for (auto [texture, layers] : { std::pair{ g_placeholder2D, 1u }, { g_placeholder3D, 1u }, { g_placeholderCube, kCubeFaces },
                     { g_placeholderArray, 1u } })
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

        // Shader compilation (pipelines.cpp): asynchronous if asked
        // (H3_ASYNC_SHADERS=1/0 overrides), and warm-up from the manifest
        // unless turned off (H3_NO_WARMUP).
        const config::VideoSettings& video = config::Active().video;
        g_asyncShaders = EnvNumber("H3_ASYNC_SHADERS", video.asyncShaders ? 1 : 0) != 0;
        if (video.precompileShaders && !EnvFlag("H3_NO_WARMUP"))
            StartShaderWarmup();

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

    namespace
    {
        // The console's display controller applies the gamma ramp D3D
        // programmed (gpu::GammaRamp) to the front buffer at scanout. Do the
        // same: run the frame through it into a presentation texture (two,
        // alternating, so the window can still be showing the last one) and
        // show that instead. H3_NO_GAMMA=1 shows the front buffer as is.
        void ApplyDisplayGamma(Frame& frame, SDL_GPUTextureFormat& format)
        {
            static const bool disabled = EnvFlag("H3_NO_GAMMA");
            if (disabled || !frame.texture)
                return;
            GammaRamp ramp = CurrentGammaRamp();
            if (!ramp.valid)
                return;
            struct Presentation
            {
                SDL_GPUTexture* textures[2] = {};
                uint32_t next = 0;
            };
            // By size; textures are never released, since the window keeps
            // wrappers of them (render scale or mode changes add a set).
            static std::unordered_map<uint64_t, Presentation> presentations;
            Presentation& presentation = presentations[(uint64_t(frame.width) << 32) | frame.height];
            SDL_GPUTexture*& target = presentation.textures[presentation.next];
            presentation.next ^= 1;
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
            ApplyGammaRamp(frame.texture, int32_t(frame.x), int32_t(frame.y), target, frame.width, frame.height, ramp);
            frame.texture = target;
            frame.textureWidth = frame.width;
            frame.textureHeight = frame.height;
            frame.x = frame.y = 0;
            format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        }
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
        // H3_SKIP_DRAWS=<first>-<last>[,<first>-<last>...]: drop those draws
        // (numbered like H3_GPU_CAPTURE's "draw #n") in every frame, to
        // bisect which draw produces an artifact.
        static const std::vector<std::pair<uint32_t, uint32_t>> skipRanges = [] {
            std::vector<std::pair<uint32_t, uint32_t>> ranges;
            for (const char* p = EnvString("H3_SKIP_DRAWS"); p && *p;)
            {
                uint32_t first = 0, last = 0;
                int consumed = 0;
                if (sscanf(p, "%u-%u%n", &first, &last, &consumed) != 2)
                    break;
                ranges.emplace_back(first, last);
                p += consumed;
                if (*p == ',')
                    p++;
            }
            return ranges;
        }();
        for (const auto& [first, last] : skipRanges)
            if (g_frameDraws >= first && g_frameDraws <= last)
            {
                g_frameDraws++;
                return;
            }
        DumpAroundPrimitive(initiator & 0x3F, false);
        Draw(initiator, indexBase, dmaSize);
        DumpAroundPrimitive(initiator & 0x3F, true);
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
        SDL_GPUTextureFormat frameFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
        if (ResolvedTexture* texture = FindResolved(frontbuffer))
        {
            // The host texture is at the render scale.
            frame.texture = texture->texture;
            frame.textureWidth = texture->HostWidth();
            frame.textureHeight = texture->HostHeight();
            frame.width = std::min(width ? width : texture->pitch, texture->pitch) * g_renderScale;
            frame.height = std::min(height ? height : texture->height, texture->height) * g_renderScale;
            frameFormat = texture->hostFormat;
            ApplyDisplayGamma(frame, frameFormat);
            ApplyAntiAliasing(frame, frameFormat);
        }

        g_swaps++;
        g_frameResolves = 0;
        DumpMemoryAtSwap();
        PokeMemoryAtSwap();
        EndFrameBuffers();
        PollWriteBacks();
        PollMemExports();
        TrimTextureCache();
        TrimResolvedViews();
        // H3_LOG_FPS: how many pipelines and shaders exist (creating them
        // stalls this thread).
        static const bool logCounts = EnvFlag("H3_LOG_FPS");
        if (logCounts && g_swaps % kCountLogInterval == 0)
            fprintf(stderr,
                "[render] frame %u: %zu pipelines, %zu shaders, %zu textures (%llu MB), %zu resolve views, %llu texture uploads (%llu MB) so far\n",
                g_swaps, PipelineCount(), ShaderCount(), TextureCount(), (unsigned long long)(TextureBytes() >> 20), ResolvedViewCount(),
                (unsigned long long)g_textureUploads, (unsigned long long)(g_textureUploadBytes >> 20));
        if (logCounts)
        {
            // Where video memory goes, to catch resources that pile up:
            // every kCountLogInterval frames, and whenever the total moved
            // by kVramLogStepBytes since the last line.
            auto mb = [](const VideoMemoryUse& use) { return (unsigned long long)(use.bytes >> 20); };
            VideoMemoryUse targets = RenderTargetMemory(), resolved = ResolvedMemory(), views = ResolvedViewMemory(),
                           buffers = DrawBufferMemory(), uploads = UploadBufferMemory();
            uint64_t total = targets.bytes + resolved.bytes + views.bytes + TextureBytes() + buffers.bytes + uploads.bytes;
            static uint64_t loggedTotal = 0;
            uint64_t moved = total > loggedTotal ? total - loggedTotal : loggedTotal - total;
            if (g_swaps % kCountLogInterval == 0 || moved >= kVramLogStepBytes)
            {
                loggedTotal = total;
                fprintf(stderr,
                "[vram] frame %u: targets %zu (%llu MB), resolved %zu (%llu MB), views %zu (%llu MB), textures %zu (%llu MB), "
                "draw buffers %llu MB, upload buffers %zu (%llu MB)\n",
                g_swaps, targets.count, mb(targets), resolved.count, mb(resolved), views.count, mb(views), TextureCount(),
                    (unsigned long long)(TextureBytes() >> 20), mb(buffers), uploads.count, mb(uploads));
            }
        }
        if (logCounts && g_swaps % kCountLogInterval == 0)
        {
            // Per-frame averages since the last line, in host pixels.
            static const char* kNames[] = { "draw", "copy", "depth restore", "stencil restore", "blit" };
            static_assert(std::size(kNames) == size_t(PassKind::Count));
            fprintf(stderr, "[passes] frame %u, per frame:", g_swaps);
            for (size_t i = 0; i < size_t(PassKind::Count); i++)
            {
                fprintf(stderr, " %s %.1f (%.1f Mpx)", kNames[i], double(g_passCounters[i].passes) / kCountLogInterval,
                    double(g_passCounters[i].pixels) / kCountLogInterval / 1e6);
                g_passCounters[i] = {};
            }
            static uint64_t loggedSyncs = 0;
            fprintf(stderr, " memexport syncs %.1f\n", double(g_memExportSyncs - loggedSyncs) / kCountLogInterval);
            loggedSyncs = g_memExportSyncs;
        }
        DumpFrameAtSwap(frontbuffer, frame.texture, frame.width, frame.height, frameFormat);

        EndFrameQueries();
        PollOcclusionQueries();
        FenceMemExports();
        SubmitFrame();
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

    bool OnOcclusionReport(uint32_t address)
    {
        if (!g_enabled)
            return false;
        internal::OnOcclusionReport(address);
        return true;
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
