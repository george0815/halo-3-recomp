// Renderer debugging dumps, all switched on by environment variables (the
// README lists them): render targets after draws, frames, guest memory.
// Dumping waits for the GPU, so it's slow; it's meant for one frame at a
// time.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // IEEE half float to float (for RGBA16F dumps).
        float HalfToFloat(uint16_t half)
        {
            constexpr float kHalfMax = 65504.0f;
            uint32_t sign = half >> 15, exponent = (half >> 10) & 31, mantissa = half & 1023;
            float value = exponent == 0 ? std::ldexp(float(mantissa), -24)          // subnormal
                        : exponent == 31 ? kHalfMax                                  // inf/NaN: clamp
                                         : std::ldexp(float(mantissa | 1024), int(exponent) - 25);
            return sign ? -value : value;
        }

        // Converts downloaded RGBA16F pixels to RGBA8 in place (the front
        // half of the buffer) and prints the average, so HDR values beyond 1
        // show up in the log.
        void HalfToRgba8(void* pixels, uint32_t width, uint32_t height, const char* path)
        {
            // H3_DUMP_SCALE=<n>: divide colors by n first (render targets
            // with an exponent bias hold values far above 1).
            static const float dumpScale = EnvString("H3_DUMP_SCALE") ? 1.0f / float(atof(EnvString("H3_DUMP_SCALE"))) : 1.0f;
            const uint16_t* halves = static_cast<const uint16_t*>(pixels);
            uint8_t* out = static_cast<uint8_t*>(pixels);
            double sum[4] = {};
            for (uint32_t i = 0; i < width * height; i++)
                for (int c = 0; c < 4; c++)
                {
                    float value = HalfToFloat(halves[i * 4 + c]);
                    sum[c] += value;
                    out[i * 4 + c] = uint8_t(std::clamp(c < 3 ? value * dumpScale : value, 0.0f, 1.0f) * 255.0f);
                }
            double pixels_ = double(width) * height;
            fprintf(stderr, "[render] %s average %g %g %g %g\n", path, sum[0] / pixels_, sum[1] / pixels_, sum[2] / pixels_,
                sum[3] / pixels_);
        }

        // Converts other 4-byte formats to RGBA8 in place.
        void ToRgba8(uint32_t* texels, uint32_t count, SDL_GPUTextureFormat format)
        {
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t v = texels[i];
                if (format == SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM)
                    v = ((v >> 2) & 0xFF) | (((v >> 12) & 0xFF) << 8) | (((v >> 22) & 0xFF) << 16) | ((v >> 30) * 85u << 24);
                else if (format == SDL_GPU_TEXTUREFORMAT_R32_FLOAT)  // as gray
                {
                    float f;
                    memcpy(&f, &v, 4);
                    uint32_t g = uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f);
                    v = g | (g << 8) | (g << 16) | 0xFF000000u;
                }
                texels[i] = v;
            }
        }
    }

    void DumpTexture(SDL_GPUTexture* texture, uint32_t width, uint32_t height, const char* path, SDL_GPUTextureFormat format)
    {
        FenceMemExports();  // this submits g_commands
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
        // R32F (depth) values are mostly tiny with reversed depth and show
        // as black: also save the raw floats as <path>.f32 for inspection.
        if (format == SDL_GPU_TEXTUREFORMAT_R32_FLOAT)
        {
            std::string rawPath = std::string(path) + ".f32";
            if (FILE* raw = fopen(rawPath.c_str(), "wb"))
            {
                fwrite(pixels, 4, size_t(width) * height, raw);
                fclose(raw);
            }
        }
        if (half)
            HalfToRgba8(pixels, width, height, path);
        else if (format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
            ToRgba8(static_cast<uint32_t*>(pixels), width * height, format);
        SDL_Surface* surface = SDL_CreateSurfaceFrom(int(width), int(height), SDL_PIXELFORMAT_ABGR8888, pixels, int(width * 4));
        if (surface && SDL_SavePNG(surface, path))
            fprintf(stderr, "[render] wrote %s (%ux%u)\n", path, width, height);
        else
            fprintf(stderr, "[render] couldn't save %s: %s\n", path, SDL_GetError());
        SDL_DestroySurface(surface);
        SDL_UnmapGPUTransferBuffer(g_device, download);
        SDL_ReleaseGPUTransferBuffer(g_device, download);
    }

    void DumpAfterDraw()
    {
        // H3_DUMP_DRAWS=<first>-<last> (with H3_DUMP_FRAME): save color
        // target 0 (or H3_DUMP_TARGET=<n>) after each of those draws as
        // draw_<frame>_<n>.png.
        static const char* setting = EnvString("H3_DUMP_DRAWS");
        static uint32_t first = 0, last = 0;
        static bool parsed = setting && sscanf(setting, "%u-%u", &first, &last) == 2;
        static const uint32_t dumpFrame = EnvNumber("H3_DUMP_FRAME");
        static const uint32_t index = std::min(EnvNumber("H3_DUMP_TARGET"), 3u);  // which color target
        const TargetSet& targets = CurrentTargetSet();
        if (!parsed || g_swaps + 1 != dumpFrame || g_frameDraws < first || g_frameDraws > last || !targets.color[index])
            return;
        Flush();
        char path[64];
        snprintf(path, sizeof(path), "draw_%u_%04u.png", dumpFrame, g_frameDraws);
        // Host targets are in sample space: a 4x surface covers 2x2 texels
        // per pixel.
        DumpTexture(targets.color[index], kDumpTargetWidth * g_renderScale, kDumpTargetHeight * g_renderScale, path, targets.colorFormat[index]);
    }

    void DumpAroundPrimitive(uint32_t primitive, bool after)
    {
        // H3_DUMP_PRIM=<primitive type> (with H3_DUMP_FRAME): save color
        // target 0 right before and after every draw of that primitive type
        // (draw_<frame>_<n>_before/after.png), e.g. 17 for the tessellated
        // water, which may be hidden on screen.
        static const char* setting = EnvString("H3_DUMP_PRIM");
        static const uint32_t wanted = setting ? uint32_t(atoi(setting)) : 0;
        static const uint32_t dumpFrame = EnvNumber("H3_DUMP_FRAME");
        if (!setting || primitive != wanted || g_swaps + 1 != dumpFrame)
            return;
        Flush();
        const TargetSet& targets = CurrentTargetSet();
        if (!targets.color[0])
            return;
        char path[64];
        snprintf(path, sizeof(path), "draw_%u_%04u_%s.png", dumpFrame, g_frameDraws, after ? "after" : "before");
        DumpTexture(targets.color[0], kDumpTargetWidth * g_renderScale, kDumpTargetHeight * g_renderScale, path, targets.colorFormat[0]);
    }

    void DumpDepthAfterRestore(SDL_GPUTexture* bytes, SDL_GPUTextureFormat bytesFormat, SDL_GPUTexture* depth)
    {
        // H3_DUMP_RESTORE=<frame>: after each depth restore in that frame,
        // save the color target holding the depth bytes
        // (restore_<frame>_<n>_bytes.png) and the depth target as floats
        // (restore_<frame>_<n>_depth.png + .f32), both in sample space.
        static const uint32_t dumpFrame = EnvNumber("H3_DUMP_RESTORE");
        if (!dumpFrame || g_swaps + 1 != dumpFrame)
            return;
        static SDL_GPUTexture* floats = nullptr;
        if (!floats)
        {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = TargetWidth();
            info.height = TargetHeight();
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            floats = SDL_CreateGPUTexture(g_device, &info);
        }
        char path[64];
        snprintf(path, sizeof(path), "restore_%u_%04u_bytes.png", dumpFrame, g_frameDraws);
        DumpTexture(bytes, kDumpTargetWidth * g_renderScale, kDumpTargetHeight * g_renderScale, path, bytesFormat);
        ConvertCopy(depth, floats, SDL_GPU_TEXTUREFORMAT_R32_FLOAT, TargetWidth(), TargetHeight(), 0, 0, 0, 0,
            kDumpTargetWidth * g_renderScale, kDumpTargetHeight * g_renderScale, CopyMode::Value);
        snprintf(path, sizeof(path), "restore_%u_%04u_depth.png", dumpFrame, g_frameDraws);
        DumpTexture(floats, kDumpTargetWidth * g_renderScale, kDumpTargetHeight * g_renderScale, path, SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
    }

    void PokeMemoryAtSwap()
    {
        // H3_POKE=<frame>:<address>:<hex bytes>[,...]: write those bytes
        // (big-endian, as in memory) to guest memory at that swap, for
        // experiments (e.g. changing a game variable mid-run). Addresses
        // as for H3_DUMP_MEMORY.
        static const char* spec = EnvString("H3_POKE");
        if (!spec)
            return;
        for (const char* p = spec; *p;)
        {
            char* end;
            uint32_t frameNumber = uint32_t(strtoul(p, &end, 0));
            uint32_t address = *end == ':' ? uint32_t(strtoul(end + 1, &end, 16)) : 0;
            if (*end != ':')
                break;
            const char* hex = end + 1;
            constexpr uint32_t kFirstVirtual = 0x80000000;
            uint8_t* guest = GuestPtr<uint8_t>(address >= kFirstVirtual ? address : PhysicalToGuest(address));
            size_t i = 0;
            for (; isxdigit(uint8_t(hex[i])) && isxdigit(uint8_t(hex[i + 1])); i += 2)
            {
                if (frameNumber == g_swaps)
                {
                    char byte[3] = { hex[i], hex[i + 1], 0 };
                    guest[i / 2] = uint8_t(strtoul(byte, nullptr, 16));
                }
            }
            if (frameNumber == g_swaps)
                fprintf(stderr, "[debug] poked %zu bytes at %08X\n", i / 2, address);
            p = hex + i;
            if (*p == ',')
                p++;
            else
                break;
        }
    }

    void DumpMemoryAtSwap()
    {
        // H3_DUMP_MEMORY=<frame>:<address>:<bytes>[,...]: save guest memory
        // at that swap as mem_<frame>_<address>.bin. Addresses below
        // 0x80000000 are physical; others are guest addresses as is (the
        // executable's data at 0x82..., the physical views).
        static const char* spec = EnvString("H3_DUMP_MEMORY");
        if (!spec)
            return;
        for (const char* p = spec; *p;)
        {
            char* end;
            uint32_t frameNumber = uint32_t(strtoul(p, &end, 0));
            uint32_t address = *end == ':' ? uint32_t(strtoul(end + 1, &end, 16)) : 0;
            uint32_t bytes = *end == ':' ? uint32_t(strtoul(end + 1, &end, 0)) : 0;
            if (frameNumber == g_swaps && bytes)
            {
                char path[64];
                snprintf(path, sizeof(path), "mem_%u_%08X.bin", g_swaps, address);
                if (FILE* file = fopen(path, "wb"))
                {
                    constexpr uint32_t kFirstVirtual = 0x80000000;
                    uint32_t guest = address >= kFirstVirtual ? address : PhysicalToGuest(address);
                    fwrite(GuestPtr<uint8_t>(guest), 1, bytes, file);
                    fclose(file);
                }
            }
            p = *end == ',' ? end + 1 : end;
            if (end == p && *p)  // malformed: stop rather than loop forever
                break;
        }
    }

    void DumpFrameAtSwap(uint32_t frontbuffer, SDL_GPUTexture* texture, uint32_t width, uint32_t height, SDL_GPUTextureFormat format)
    {
        // H3_DUMP_FRAME=<n>: save the n-th presented frame as frame_<n>.png
        // (or, with H3_DUMP_ADDRESS, the resolved texture at that address).
        // H3_SNAPSHOT_EVERY=<n>: save every n-th presented frame (from frame
        // H3_SNAPSHOT_FROM on). Skipped when resolves are being dumped.
        static const uint32_t dumpFrame = EnvNumber("H3_DUMP_FRAME");
        static const uint32_t snapshotEvery = EnvNumber("H3_SNAPSHOT_EVERY");
        static const uint32_t snapshotFrom = EnvNumber("H3_SNAPSHOT_FROM");
        static const bool dumpResolves = EnvFlag("H3_DUMP_RESOLVE");
        bool snapshot = snapshotEvery != 0 && g_swaps % snapshotEvery == 0 && g_swaps >= snapshotFrom;
        if (!((dumpFrame != 0 && g_swaps == dumpFrame) || snapshot) || dumpResolves || !texture)
            return;
        if (const char* address = EnvString("H3_DUMP_ADDRESS"))
            if (ResolvedTexture* resolved = FindResolved(uint32_t(strtoul(address, nullptr, 16))))
                texture = resolved->texture, width = resolved->pitch * g_renderScale, height = resolved->height * g_renderScale,
                format = resolved->hostFormat;
        char path[64];
        snprintf(path, sizeof(path), "frame_%u.png", g_swaps);
        DumpTexture(texture, width, height, path, format);
    }
}
