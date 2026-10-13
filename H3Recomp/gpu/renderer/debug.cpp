// Renderer debugging dumps, all switched on by environment variables (the
// README lists them): render targets after draws, frames, guest memory.
// Dumping waits for the GPU, so it's slow; it's meant for one frame at a
// time.

#include "gpu/renderer/internal.h"

#include "config/env.h"
#include "gpu/frame_capture.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <unordered_map>
#include <vector>
#include <string>
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

    namespace
    {
        // A texture's pixels as RGBA8 (half floats clamped to 0..1). Waits
        // for the GPU (debugging only).
        std::vector<uint32_t> ReadbackRgba8(SDL_GPUTexture* texture, uint32_t width, uint32_t height, SDL_GPUTextureFormat format)
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
            std::vector<uint32_t> out(size_t(width) * height);
            const void* pixels = SDL_MapGPUTransferBuffer(g_device, download, false);
            if (half)
            {
                const uint16_t* halves = static_cast<const uint16_t*>(pixels);
                uint8_t* bytes8 = reinterpret_cast<uint8_t*>(out.data());
                for (size_t i = 0; i < out.size() * 4; i++)
                    bytes8[i] = uint8_t(std::clamp(HalfToFloat(halves[i]), 0.0f, 1.0f) * 255.0f);
            }
            else
            {
                memcpy(out.data(), pixels, out.size() * 4);
                if (format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
                    ToRgba8(out.data(), uint32_t(out.size()), format);
            }
            SDL_UnmapGPUTransferBuffer(g_device, download);
            SDL_ReleaseGPUTransferBuffer(g_device, download);
            return out;
        }

        // F10 deep captures: after each draw, compares color target 0 with
        // its contents after the previous draw that used it and saves only
        // the changed rectangle (draw_<frame>_<n>_<x>_<y>.png), listing each
        // in deep_capture.txt. Full images per draw were ~1 GB a frame and
        // took minutes.
        void DeepCaptureDraw(SDL_GPUTexture* texture, SDL_GPUTextureFormat format, uint32_t frame)
        {
            static std::unordered_map<SDL_GPUTexture*, std::vector<uint32_t>> previous;
            static uint32_t previousFrame = 0;
            if (previousFrame != frame)
            {
                previous.clear();
                previousFrame = frame;
            }
            uint32_t width = kDumpTargetWidth * g_renderScale, height = kDumpTargetHeight * g_renderScale;
            std::vector<uint32_t> pixels = ReadbackRgba8(texture, width, height, format);
            std::vector<uint32_t>& last = previous[texture];
            FILE* log = fopen(capture::CapturePath("deep_capture.txt").c_str(), "a");
            if (last.empty())
            {
                if (log)
                    fprintf(log, "draw %u: target %p format %d: first use (baseline)\n", g_frameDraws, (void*)texture, int(format));
                // Also keep the baseline image (downscaled 2x) for context.
                last = std::move(pixels);
                if (log)
                    fclose(log);
                return;
            }
            uint32_t x0 = width, y0 = height, x1 = 0, y1 = 0;
            for (uint32_t y = 0; y < height; y++)
                for (uint32_t x = 0; x < width; x++)
                    if (pixels[size_t(y) * width + x] != last[size_t(y) * width + x])
                        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1), y1 = std::max(y1, y + 1);
            if (x0 < x1)
            {
                uint32_t w = x1 - x0, h = y1 - y0;
                std::vector<uint32_t> crop(size_t(w) * h);
                for (uint32_t y = 0; y < h; y++)
                    memcpy(&crop[size_t(y) * w], &pixels[size_t(y0 + y) * width + x0], size_t(w) * 4);
                char name[96];
                snprintf(name, sizeof(name), "draw_%u_%04u_%u_%u.png", frame, g_frameDraws, x0, y0);
                SDL_Surface* surface = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_ABGR8888, crop.data(), int(w * 4));
                if (surface)
                {
                    SDL_SavePNG(surface, capture::CapturePath(name).c_str());
                    SDL_DestroySurface(surface);
                }
                if (log)
                    fprintf(log, "draw %u: target %p format %d: changed %u,%u-%u,%u (%ux%u) -> %s\n", g_frameDraws, (void*)texture,
                        int(format), x0, y0, x1, y1, w, h, name);
            }
            if (log)
                fclose(log);
            last = std::move(pixels);
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
        // F10 deep captures: every draw of the captured frame.
        bool deep = capture::HotkeyDeep() && capture::HotkeyFrame() && g_swaps + 1 == capture::HotkeyFrame();
        if (!deep && (!parsed || g_swaps + 1 != dumpFrame || g_frameDraws < first || g_frameDraws > last))
            return;
        if (!targets.color[index])
            return;
        Flush();
        if (deep)
        {
            DeepCaptureDraw(targets.color[index], targets.colorFormat[index], capture::HotkeyFrame());
            return;
        }
        char path[64];
        snprintf(path, sizeof(path), "draw_%u_%04u.png", deep ? capture::HotkeyFrame() : dumpFrame, g_frameDraws);
        std::string fullPath = deep ? capture::CapturePath(path) : std::string(path);
        // Host targets are in sample space: a 4x surface covers 2x2 texels
        // per pixel.
        DumpTexture(targets.color[index], kDumpTargetWidth * g_renderScale, kDumpTargetHeight * g_renderScale, fullPath.c_str(),
            targets.colorFormat[index]);
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

    SDL_GPUShader* HighlightFragmentShader()
    {
        static SDL_GPUShader* shader = CompileInternal(R"(#version 450
layout(location = 0) out vec4 xe_color;
void main() { xe_color = vec4(1.0, 0.0, 1.0, 1.0); }
)", false, 0, 2);
        return shader;
    }

    bool HighlightDraw(const Shader& vs)
    {
        // Matched by the capture's microcode hash (gpu_shaders/vs_<hash>.bin:
        // FNV-1a over the big-endian dwords as in guest memory).
        static const char* wanted = EnvString("H3_HIGHLIGHT_VS");
        if (!wanted)
            return false;
        uint64_t hash = vs.captureHash;
        char text[17];
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);
        if (strcmp(wanted, "list") == 0)
        {
            static std::set<std::string> seen;
            if (seen.insert(text).second)
                fprintf(stderr, "[render] H3_HIGHLIGHT_VS: draws use vs_%s\n", text);
            return false;
        }
        bool match = strncmp(text, wanted, strlen(wanted)) == 0;
        static uint32_t logged = 0;
        if (match && logged++ < 5)
            fprintf(stderr, "[render] H3_HIGHLIGHT_VS: drawing vs_%s in magenta (frame %u)\n", text, g_swaps);
        return match;
    }

    std::vector<VsDataCheck> g_vsDataChecks;

    void VerifyVsData(SDL_GPUBuffer* buffer, const std::unordered_map<uint64_t, uint32_t>& staged)
    {
        for (const VsDataCheck& check : g_vsDataChecks)
        {
            auto it = staged.find((uint64_t(check.address) << 32) | check.dwords);
            if (it == staged.end())
            {
                fprintf(stderr, "[vsverify] f%u draw %u: range not staged in this flush\n", check.frame, check.draw);
                continue;
            }
            uint32_t bytes = check.dwords * 4;
            SDL_GPUTransferBufferCreateInfo info{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
            SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(g_device, &info);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            SDL_GPUBufferRegion region{ buffer, it->second * 4, bytes };
            SDL_GPUTransferBufferLocation destination{ download, 0 };
            SDL_DownloadFromGPUBuffer(copy, &region, &destination);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_commands);
            g_commands = nullptr;
            SDL_WaitForGPUFences(g_device, true, &fence, 1);
            SDL_ReleaseGPUFence(g_device, fence);
            const uint32_t* gpu = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(g_device, download, false));
            const uint32_t* guest = GuestPtr<uint32_t>(check.address);
            uint32_t zero = 0, differ = 0;
            for (uint32_t r : check.records)
            {
                if ((r + 1) * check.stride > check.dwords)
                    continue;
                const uint32_t* g = gpu + r * check.stride;
                bool allZero = !g[0] && !g[1] && !g[2] && !g[3];
                bool same = memcmp(g, guest + r * check.stride, check.stride * 4) == 0;
                zero += allZero;
                differ += !same;
                float xyzw[4];
                for (uint32_t i = 0; i < 4; i++)
                {
                    uint32_t bits = __builtin_bswap32(g[i]);
                    memcpy(&xyzw[i], &bits, 4);
                }
                fprintf(stderr, "[vsverify] f%u draw %u: record %u gpu %g %g %g %g%s\n", check.frame, check.draw, r, xyzw[0], xyzw[1], xyzw[2],
                    xyzw[3], allZero ? " ZERO" : "");
            }
            // A point far off its neighbours' spacing (a stray point
            // stretches the ribbon across the screen).
            std::vector<float> steps;
            for (size_t i = 1; i < check.records.size(); i++)
            {
                const uint32_t* a = gpu + check.records[i - 1] * check.stride;
                const uint32_t* b = gpu + check.records[i] * check.stride;
                float distance = 0;
                for (uint32_t c = 0; c < 3; c++)
                {
                    uint32_t ba = __builtin_bswap32(a[c]), bb = __builtin_bswap32(b[c]);
                    float fa, fb;
                    memcpy(&fa, &ba, 4);
                    memcpy(&fb, &bb, 4);
                    distance += (fa - fb) * (fa - fb);
                }
                steps.push_back(std::sqrt(distance));
            }
            float largest = steps.empty() ? 0 : *std::max_element(steps.begin(), steps.end());
            std::vector<float> sorted = steps;
            std::sort(sorted.begin(), sorted.end());
            float median = sorted.empty() ? 0 : sorted[sorted.size() / 2];
            bool jump = largest > 1.0f && largest > 8 * std::max(median, 0.01f);
            fprintf(stderr, "[vsverify] f%u draw %u: %zu records, %u zero on GPU, %u differ from guest memory, step median %g max %g%s\n",
                check.frame, check.draw, check.records.size(), zero, differ, median, largest, jump ? " JUMP" : "");
            SDL_UnmapGPUTransferBuffer(g_device, download);
            SDL_ReleaseGPUTransferBuffer(g_device, download);
        }
        g_vsDataChecks.clear();
    }

    void LogVertexData(const Shader& vs, uint32_t initiator)
    {
        static const char* wanted = EnvString("H3_LOG_VS_DATA");
        if (!wanted)
            return;
        char text[17];
        snprintf(text, sizeof(text), "%016llx", (unsigned long long)vs.captureHash);
        const char* colon = strchr(wanted, ':');
        size_t length = colon ? size_t(colon - wanted) : strlen(wanted);
        if (strncmp(text, wanted, length) != 0)
            return;
        uint32_t stride = colon ? uint32_t(strtoul(colon + 1, nullptr, 0)) : 16;
        // H3_LOG_VS_DATA_FRAMES=<first>-<last>: only those frames.
        static const char* frames = EnvString("H3_LOG_VS_DATA_FRAMES");
        static const uint32_t firstFrame = frames ? uint32_t(strtoul(frames, nullptr, 0)) : 0;
        static const uint32_t lastFrame = frames && strchr(frames, '-') ? uint32_t(strtoul(strchr(frames, '-') + 1, nullptr, 0)) : ~0u;
        if (g_swaps < firstFrame || g_swaps > lastFrame)
            return;
        static uint32_t logged = 0;
        if (logged++ > 100000)
            return;
        constexpr uint32_t kPrimitiveMask = 0x3F, kSourceShift = 6, kCountShift = 16;
        fprintf(stderr, "[vsdata] f%u draw %u vs_%.8s prim %u src %u count %u clip cntl %08X\n", g_swaps, g_frameDraws, text,
            initiator & kPrimitiveMask, (initiator >> kSourceShift) & 3, initiator >> kCountShift, ReadRegister(kRegPaClClipCntl));
        // VS constants (the ones contrail/particle shaders index).
        fprintf(stderr, "[vsdata]   c151 %g c253 %g %g %g %g c255 %g %g %g %g | c158..165.x", RegisterFloat(kRegShaderConstantAlu + 151 * 4),
            RegisterFloat(kRegShaderConstantAlu + 253 * 4), RegisterFloat(kRegShaderConstantAlu + 253 * 4 + 1),
            RegisterFloat(kRegShaderConstantAlu + 253 * 4 + 2), RegisterFloat(kRegShaderConstantAlu + 253 * 4 + 3),
            RegisterFloat(kRegShaderConstantAlu + 255 * 4), RegisterFloat(kRegShaderConstantAlu + 255 * 4 + 1),
            RegisterFloat(kRegShaderConstantAlu + 255 * 4 + 2), RegisterFloat(kRegShaderConstantAlu + 255 * 4 + 3));
        for (uint32_t c : { 32u, 157u, 230u })
        {
            uint32_t bits = ReadRegister(kRegShaderConstantAlu + c * 4);
            fprintf(stderr, " | c%u.x %g (%08X) y %g z %g w %g", c, RegisterFloat(kRegShaderConstantAlu + c * 4), bits,
                RegisterFloat(kRegShaderConstantAlu + c * 4 + 1), RegisterFloat(kRegShaderConstantAlu + c * 4 + 2),
                RegisterFloat(kRegShaderConstantAlu + c * 4 + 3));
        }
        fprintf(stderr, " |");
        for (uint32_t c = 158; c < 166; c++)
            fprintf(stderr, " %g", RegisterFloat(kRegShaderConstantAlu + c * 4));
        fprintf(stderr, "\n");
        static const bool positions = EnvString("H3_DEBUG_VS_POSITIONS") != nullptr;
        if (positions)
            DebugVertexPositions(vs, initiator);
        for (uint32_t index : vs.info.vertexFetchConstants)
        {
            VertexFetch fetch = ReadVertexFetch(index);
            if (!fetch.valid)
                continue;
            fprintf(stderr, "[vsdata]   vfetch %u: %08X dwords %u endian %u; this frame's exports:%s\n", index, fetch.address, fetch.dwords,
                fetch.endian, FrameExportsOverlapping(fetch.address, fetch.address + fetch.dwords * 4).c_str());
            // Contrails (vs_e35cf352): point p of the strip is record
            // c[158 + p / 16].x * 16 + p % 16, for p < c151.x.
            std::vector<uint32_t> records;
            if (strncmp(text, "e35cf352", 8) == 0)
            {
                constexpr uint32_t kBlock = 16;
                uint32_t points = uint32_t(RegisterFloat(kRegShaderConstantAlu + 151 * 4));
                for (uint32_t point = 0; point < std::min(points, 256u); point++)
                    records.push_back(uint32_t(RegisterFloat(kRegShaderConstantAlu + (158 + point / kBlock) * 4)) * kBlock + point % kBlock);
            }
            else
                for (uint32_t r = 0; r < std::min(fetch.dwords / std::max(stride, 1u), 64u); r++)
                    records.push_back(r);
            static const bool verify = EnvFlag("H3_VERIFY_VS_DATA");
            if (verify)
                g_vsDataChecks.push_back({ g_swaps, g_frameDraws, PhysicalToGuest(fetch.address), fetch.dwords, std::max(stride, 1u), records });
            for (uint32_t r : records)
            {
                if (r >= fetch.dwords / std::max(stride, 1u))
                {
                    fprintf(stderr, "[vsdata]     %3u: out of range\n", r);
                    continue;
                }
                fprintf(stderr, "[vsdata]     %3u:", r);
                for (uint32_t i = 0; i < std::min(stride, 4u); i++)
                {
                    uint32_t bits = __builtin_bswap32(GuestRef<uint32_t>(PhysicalToGuest(fetch.address) + (r * stride + i) * 4).value);
                    float f;
                    memcpy(&f, &bits, 4);
                    fprintf(stderr, " %g", f);
                }
                // The rest of the record as raw words.
                fprintf(stderr, " |");
                for (uint32_t i = 4; i < stride; i++)
                    fprintf(stderr, " %08X", __builtin_bswap32(GuestRef<uint32_t>(PhysicalToGuest(fetch.address) + (r * stride + i) * 4).value));
                fprintf(stderr, "\n");
            }
        }
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
        // H3_SNAPSHOT_FROM on, up to H3_SNAPSHOT_TO if set). Skipped when
        // resolves are being dumped.
        static const uint32_t dumpFrame = EnvNumber("H3_DUMP_FRAME");
        static const uint32_t snapshotEvery = EnvNumber("H3_SNAPSHOT_EVERY");
        static const uint32_t snapshotFrom = EnvNumber("H3_SNAPSHOT_FROM");
        static const uint32_t snapshotTo = EnvNumber("H3_SNAPSHOT_TO", ~0u);
        static const bool dumpResolves = EnvFlag("H3_DUMP_RESOLVE");
        bool snapshot = snapshotEvery != 0 && g_swaps % snapshotEvery == 0 && g_swaps >= snapshotFrom &&
            g_swaps <= snapshotTo;
        // An F9 capture also saves its frame (into the capture's folder).
        bool hotkey = capture::HotkeyFrame() != 0 && g_swaps == capture::HotkeyFrame();
        if (!((dumpFrame != 0 && g_swaps == dumpFrame) || snapshot || hotkey) || (dumpResolves && !hotkey) || !texture)
            return;
        if (const char* address = EnvString("H3_DUMP_ADDRESS"))
            if (ResolvedTexture* resolved = FindResolved(uint32_t(strtoul(address, nullptr, 16))))
                texture = resolved->texture, width = resolved->pitch * g_renderScale, height = resolved->height * g_renderScale,
                format = resolved->hostFormat;
        char path[64];
        snprintf(path, sizeof(path), "frame_%u.png", g_swaps);
        std::string fullPath = hotkey ? capture::CapturePath(path) : std::string(path);
        DumpTexture(texture, width, height, fullPath.c_str(), format);
    }
}
