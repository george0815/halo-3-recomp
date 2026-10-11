// Resolves: Xenos copies a rectangle of an EDRAM render target out to main
// memory, where the game then uses it as a texture (or the swap presents
// it). Here the destination memory is stood in for by a host texture
// (ResolvedTexture) and the copy happens on the GPU; texture fetches from
// that memory sample the host texture instead of decoding memory.
//
// Small resolves are also copied back into guest memory, because the CPU
// reads some of them (Halo's auto exposure reads back the scene luminance).
//
// With a render scale above 1 the render targets, and so the resolved
// textures, are larger than the guest's: resolved textures keep their guest
// layout (pitch, height, offsets in guest texels) and every host position
// is that times g_renderScale. Write-backs are scaled down first.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // D3D lays resolve destinations out in 32x32-texel macro tiles, and
        // destination pitches and heights are multiples of the tile size.
        constexpr uint32_t kMacroTileSize = 32;
        // Resolve rectangles cover whole 8x8-pixel blocks.
        constexpr int32_t kResolveAlignment = 8;
        // Resolves of at most this many pixels are written back to guest
        // memory for the CPU (exposure readbacks are tiny; big resolves are
        // textures only the GPU reads).
        constexpr uint32_t kMaxWriteBackPixels = 64 * 64;
        // D3D puts the resolve rectangle's three corners (x, y) in vertex
        // fetch constant 0 (Xenia's GetResolveInfo).
        constexpr uint32_t kResolveRectangleFetch = 0;
        constexpr uint32_t kResolveRectangleDwords = 6;

        // RB_COPY_CONTROL source select: 0-3 color targets, 4 depth.
        constexpr uint32_t kCopySourceDepth = 4;

        std::vector<ResolvedTexture> g_resolved;

        // Texel position of an address inside a resolved texture: D3D
        // advances the base in 32x32 macro tiles.
        void ResolvedOffset(const ResolvedTexture& texture, uint32_t address, uint32_t& x, uint32_t& y)
        {
            uint32_t delta = GuestToPhysical(address) - texture.base;
            uint32_t macroTileBytes = kMacroTileSize * kMacroTileSize * texture.bpp;
            uint32_t macroRowBytes = texture.pitch * kMacroTileSize * texture.bpp;
            y = (delta / macroRowBytes) * kMacroTileSize;
            x = ((delta % macroRowBytes) / macroTileBytes) * kMacroTileSize;
        }

        // Bytes per texel and host format of a resolve destination texture
        // format (xenos::TextureFormat). False if unsupported.
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

        // --- Write-backs -------------------------------------------------

        // A resolve being copied back to guest memory for the CPU. The
        // download completes asynchronously: waiting for it would drain the
        // GPU several times a frame and serialize command processing with
        // GPU execution. The CPU sees the data a frame or so late, which
        // exposure adaption doesn't mind. H3_SYNC_WRITEBACK=1 waits instead
        // (debugging).
        struct PendingWriteBack
        {
            SDL_GPUFence* fence = nullptr;
            SDL_GPUTransferBuffer* download = nullptr;
            uint32_t base = 0, pitch = 0, size = 0, bpp = 0;  // the resolved texture's memory
            uint32_t x0 = 0, y0 = 0, w = 0, h = 0, endian = 0;  // the rectangle, RB_COPY_DEST_INFO endian
        };
        std::vector<PendingWriteBack> g_pendingWriteBacks;  // oldest first

        // Byte-swaps one element in place for an RB_COPY_DEST_INFO endian
        // mode: 1 8in16, 2 8in32, 3 16in32, 4 8in64.
        void SwapElement(uint8_t* element, uint32_t bpp, uint32_t endian)
        {
            switch (endian)
            {
            case 1:
                for (uint32_t i = 0; i + 1 < bpp; i += 2)
                    std::swap(element[i], element[i + 1]);
                break;
            case 2:
            case 3:
                for (uint32_t i = 0; i + 3 < bpp; i += 4)
                {
                    uint32_t value;
                    memcpy(&value, element + i, 4);
                    value = SwapEndian32(value, endian);
                    memcpy(element + i, &value, 4);
                }
                break;
            case 4:
                for (uint32_t i = 0; i + 7 < bpp; i += 8)
                {
                    uint64_t value;
                    memcpy(&value, element + i, 8);
                    value = __builtin_bswap64(value);
                    memcpy(element + i, &value, 8);
                }
                break;
            default:
                break;
            }
        }

        // Tiles and byte-swaps the downloaded pixels into guest memory, as
        // the console stores them.
        void CompleteWriteBack(const PendingWriteBack& pending)
        {
            uint32_t bpp = pending.bpp, w = pending.w, h = pending.h;
            const uint8_t* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(g_device, pending.download, false));
            uint8_t* guest = GuestPtr<uint8_t>(PhysicalToGuest(pending.base));
            uint8_t element[16];
            for (uint32_t y = 0; y < h; y++)
            {
                for (uint32_t x = 0; x < w; x++)
                {
                    memcpy(element, pixels + (size_t(y) * w + x) * bpp, bpp);
                    SwapElement(element, bpp, pending.endian);
                    size_t offset = size_t(texture::TiledOffset(pending.x0 + x, pending.y0 + y, pending.pitch, bpp)) * bpp;
                    if (offset + bpp <= pending.size)
                        memcpy(guest + offset, element, bpp);
                }
            }
            SDL_UnmapGPUTransferBuffer(g_device, pending.download);
            SDL_ReleaseGPUTransferBuffer(g_device, pending.download);
            SDL_ReleaseGPUFence(g_device, pending.fence);
        }

        // Guest-sized textures that scaled write-backs are averaged into,
        // kept for reuse: (format, width, height) -> texture.
        std::unordered_map<uint64_t, SDL_GPUTexture*> g_writeBackScratch;

        // A width x height scratch texture of a format for WriteBackResolve.
        SDL_GPUTexture* WriteBackScratch(SDL_GPUTextureFormat format, uint32_t width, uint32_t height)
        {
            SDL_GPUTexture*& texture = g_writeBackScratch[(uint64_t(format) << 32) | (uint64_t(width) << 16) | height];
            if (!texture)
            {
                SDL_GPUTextureCreateInfo info{};
                info.type = SDL_GPU_TEXTURETYPE_2D;
                info.format = format;
                info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
                info.width = width;
                info.height = height;
                info.layer_count_or_depth = 1;
                info.num_levels = 1;
                texture = SDL_CreateGPUTexture(g_device, &info);
            }
            return texture;
        }

        // Starts copying a rectangle (guest texels) of a resolved texture
        // back to guest memory. Submits the command buffer to get a fence.
        // PERF: creates a download transfer buffer each time (a few per
        // frame); pooling them like draw.cpp's upload buffers would save the
        // allocations.
        void WriteBackResolve(const ResolvedTexture& texture, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t endian)
        {
            if (texture.hostFormat == SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT)  // not the guest's 10_11_11 layout
                return;
            // At a render scale, average each guest texel's block of host
            // texels into a guest-sized texture first, and download that.
            SDL_GPUTexture* source = texture.texture;
            uint32_t sourceX = x0, sourceY = y0;
            if (g_renderScale > 1)
            {
                source = WriteBackScratch(texture.hostFormat, w, h);
                if (!source)
                    return;
                int32_t scale = int32_t(g_renderScale);
                ConvertCopy(texture.texture, source, texture.hostFormat, w, h, int32_t(x0) * scale, int32_t(y0) * scale, 0, 0, w, h,
                    CopyMode::Value, false, 1.0f, scale, scale);
                sourceX = sourceY = 0;
            }
            uint32_t bpp = texture.bpp, bytes = w * h * bpp;
            SDL_GPUTransferBufferCreateInfo info{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
            SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(g_device, &info);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            SDL_GPUTextureRegion region{};
            region.texture = source;
            region.x = sourceX;
            region.y = sourceY;
            region.w = w;
            region.h = h;
            region.d = 1;
            SDL_GPUTextureTransferInfo destination{ download, 0, w, h };
            SDL_DownloadFromGPUTexture(copy, &region, &destination);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_commands);
            g_commands = nullptr;
            if (!fence)
            {
                SDL_ReleaseGPUTransferBuffer(g_device, download);
                return;
            }
            g_pendingWriteBacks.push_back({ fence, download, texture.base, texture.pitch, texture.size, bpp, x0, y0, w, h, endian });
            static const bool sync = EnvFlag("H3_SYNC_WRITEBACK");
            PollWriteBacks(sync);
        }

        // --- Views of resolved textures ---------------------------------

        // A texture inside a resolved texture at another size or position
        // (memory reused for smaller surfaces, or a later macro tile): copied
        // out at exactly the size the fetch constant asks for, so UVs and
        // clamping work. Re-copied when the resolved texture changes.
        struct SubTexture
        {
            SDL_GPUTexture* texture = nullptr;
            SDL_GPUTexture* source = nullptr;  // resolved texture it was copied from
            uint32_t generation = ~0u;         // source's generation at the copy
        };
        std::unordered_map<uint64_t, SubTexture> g_subTextures;  // (address, size, format) -> copy

        // Reads the resolve rectangle from vertex fetch constant 0 into
        // pixel bounds [x0, x1) x [y0, y1) (before the window offset).
        bool ResolveRectangle(int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1)
        {
            VertexFetch fetch = ReadVertexFetch(kResolveRectangleFetch);
            if (!fetch.valid || fetch.dwords != kResolveRectangleDwords)
                return false;
            float xs[3], ys[3];
            for (uint32_t i = 0; i < 3; i++)
            {
                xs[i] = VertexFloat(fetch, i * 2);
                ys[i] = VertexFloat(fetch, i * 2 + 1);
            }
            // Corners sit on pixel edges (-0.5 offsets).
            x0 = int32_t(std::floor(std::min({ xs[0], xs[1], xs[2] }) + 0.5f));
            y0 = int32_t(std::floor(std::min({ ys[0], ys[1], ys[2] }) + 0.5f));
            x1 = int32_t(std::floor(std::max({ xs[0], xs[1], xs[2] }) + 0.5f));
            y1 = int32_t(std::floor(std::max({ ys[0], ys[1], ys[2] }) + 0.5f));
            return true;
        }

        // The resolved texture for a destination, created (or recreated at a
        // new size/format) as needed. Null on failure.
        ResolvedTexture* DestinationTexture(uint32_t destBase, uint32_t destPitchReg, uint32_t destFormat, uint32_t bpp,
            SDL_GPUTextureFormat hostFormat, uint32_t minHeight)
        {
            ResolvedTexture* texture = FindResolved(destBase);
            // Color formats of one size share a texture (the copy converts);
            // depth's float texture is kept apart from color.
            bool depthMismatch = texture && (hostFormat == SDL_GPU_TEXTUREFORMAT_R32_FLOAT) !=
                (texture->hostFormat == SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
            if (texture && texture->bpp == bpp && !depthMismatch)
                return texture;

            ResolvedTexture created;
            created.base = destBase;
            created.pitch = AlignUp(destPitchReg & 0x3FFF, kMacroTileSize);
            created.height = AlignUp((destPitchReg >> 16) & 0x3FFF, kMacroTileSize);
            created.height = std::max(created.height, AlignUp(minHeight, kMacroTileSize));
            created.bpp = bpp;
            created.size = created.pitch * created.height * bpp;
            created.xenosFormat = destFormat;
            created.hostFormat = hostFormat;
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = hostFormat;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = created.HostWidth();
            info.height = created.HostHeight();
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            created.texture = SDL_CreateGPUTexture(g_device, &info);
            if (!created.texture)
            {
                LogOnce(std::string("resolve texture creation failed: ") + SDL_GetError());
                return nullptr;
            }
            if (texture)
            {
                // Same memory reinterpreted at another size: replace it.
                SDL_ReleaseGPUTexture(g_device, texture->texture);
                *texture = created;
                return texture;
            }
            g_resolved.push_back(created);
            return &g_resolved.back();
        }

        // H3_DUMP_RESOLVE=<n>|all (with H3_DUMP_FRAME): save the destination
        // of that frame's n-th resolve (1-based), or of all of them, right
        // after it. H3_DUMP_SOURCE: also the source render target.
        void DumpResolve(const ResolvedTexture& texture, const RenderTarget& target, bool depthSource, uint32_t destBase,
            uint32_t w, uint32_t h)
        {
            static const uint32_t dumpFrame = EnvNumber("H3_DUMP_FRAME");
            static const uint32_t dumpResolve = EnvNumber("H3_DUMP_RESOLVE");
            static const bool dumpAll = EnvString("H3_DUMP_RESOLVE") && strcmp(EnvString("H3_DUMP_RESOLVE"), "all") == 0;
            if (!(dumpResolve || dumpAll) || g_swaps + 1 != dumpFrame || !(dumpAll || g_frameResolves == dumpResolve))
                return;
            if (texture.bpp != 4 && texture.hostFormat != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT)
                return;
            char path[96];
            if (dumpAll)
                snprintf(path, sizeof(path), "resolve_%u_%02u_%08X_%ux%u.png", dumpFrame, g_frameResolves, destBase, w, h);
            else
                snprintf(path, sizeof(path), "resolve_%u_%u.png", dumpFrame, dumpResolve);
            DumpTexture(texture.texture, texture.pitch * g_renderScale, texture.height * g_renderScale, path, texture.hostFormat);
            static const bool dumpSource = EnvFlag("H3_DUMP_SOURCE");
            if (dumpSource && !depthSource &&
                (target.format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM || target.format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT))
            {
                snprintf(path, sizeof(path), "source_%u_%02u.png", dumpFrame, g_frameResolves);
                DumpTexture(target.texture, kDumpTargetWidth * g_renderScale, kDumpTargetHeight * g_renderScale, path, target.format);
            }
        }
    }

    ResolvedTexture* FindResolved(uint32_t address)
    {
        // Addresses may come as physical (resolve registers) or through a
        // CPU view of physical memory (0xA/0xC/0xE..., the swap's fetch
        // constant); compare physical.
        // PERF: linear search over every resolved texture, several times
        // per draw (BindTextures); an interval map would scale better.
        address = GuestToPhysical(address);
        for (ResolvedTexture& texture : g_resolved)
            if (address >= texture.base && address < texture.base + texture.size)
                return &texture;
        return nullptr;
    }

    void PollWriteBacks(bool wait)
    {
        size_t done = 0;
        for (; done < g_pendingWriteBacks.size(); done++)
        {
            PendingWriteBack& pending = g_pendingWriteBacks[done];
            if (wait)
                SDL_WaitForGPUFences(g_device, true, &pending.fence, 1);
            else if (!SDL_QueryGPUFence(g_device, pending.fence))
                break;
            CompleteWriteBack(pending);
        }
        g_pendingWriteBacks.erase(g_pendingWriteBacks.begin(), g_pendingWriteBacks.begin() + done);
    }

    SDL_GPUTexture* ResolvedView(ResolvedTexture& resolved, const texture::FetchConstant& fetch, uint32_t address)
    {
        // Positions and sizes in guest texels; host ones are times the scale.
        uint32_t guestWidth = resolved.GuestWidth(), guestHeight = resolved.GuestHeight();
        const uint32_t scale = g_renderScale;
        uint32_t x, y;
        ResolvedOffset(resolved, address, x, y);
        uint32_t width = fetch.width, height = fetch.height;
        // Depth resolved to 8_8_8_8 and sampled as such: the depth word's
        // bytes rather than the float.
        constexpr uint32_t kFormat8888 = 6, kFormat8888A = 14;  // xenos::TextureFormat
        bool depthBytes = resolved.hostFormat == SDL_GPU_TEXTUREFORMAT_R32_FLOAT && fetch.valid &&
            (fetch.format == kFormat8888 || fetch.format == kFormat8888A);
        SDL_GPUTextureFormat format = depthBytes ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM : resolved.hostFormat;
        // The whole texture as is: no copy needed.
        if (!fetch.valid || x >= guestWidth || y >= guestHeight || width == 0 || height == 0 ||
            (x == 0 && y == 0 && width == guestWidth && height == guestHeight && !depthBytes))
            return resolved.texture;
        uint32_t key[] = { GuestToPhysical(address), width, height, uint32_t(format) };
        SubTexture& sub = g_subTextures[Hash(key, sizeof(key))];
        if (sub.texture && sub.source == resolved.texture && sub.generation == resolved.generation)
            return sub.texture;
        if (!sub.texture)
        {
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = format;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
            info.width = width * scale;
            info.height = height * scale;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            sub.texture = SDL_CreateGPUTexture(g_device, &info);
            if (!sub.texture)
                return resolved.texture;
        }
        uint32_t w = std::min(width, guestWidth - x) * scale, h = std::min(height, guestHeight - y) * scale;
        if (depthBytes)
            ConvertCopy(resolved.texture, sub.texture, format, width * scale, height * scale, int32_t(x * scale), int32_t(y * scale), 0, 0,
                w, h, CopyMode::DepthBytes, resolved.depthFloat);
        else
        {
            SDL_GPUBlitInfo blit{};
            blit.source = { resolved.texture, 0, 0, x * scale, y * scale, w, h };
            blit.destination = { sub.texture, 0, 0, 0, 0, w, h };
            blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(Commands(), &blit);
        }
        sub.source = resolved.texture;
        sub.generation = resolved.generation;
        // H3_DUMP_VIEWS (with H3_DUMP_FRAME): save each copied-out view.
        static const bool dumpViews = EnvFlag("H3_DUMP_VIEWS");
        if (dumpViews && g_swaps + 1 == EnvNumber("H3_DUMP_FRAME"))
        {
            char path[96];
            snprintf(path, sizeof(path), "view_%u_%04u_%08X_%ux%u.png", g_swaps + 1, g_frameDraws, address, width, height);
            DumpTexture(sub.texture, width * scale, height * scale, path, format);
        }
        return sub.texture;
    }

    void Resolve()
    {
        if (!g_enabled)
            return;
        uint32_t copyControl = ReadRegister(kRegRbCopyControl);
        uint32_t source = copyControl & 7;
        bool depthSource = source >= kCopySourceDepth;

        int32_t x0, y0, x1, y1;
        if (!ResolveRectangle(x0, y0, x1, y1))
        {
            LogOnce("unexpected resolve vertex buffer");
            return;
        }
        if ((ReadRegister(kRegPaSuScModeCntl) >> 16) & 1)  // window offset enabled
        {
            int32_t ox, oy;
            WindowOffset(ox, oy);
            x0 += ox, x1 += ox, y0 += oy, y1 += oy;
        }
        // The rectangle is in the surface's pixels; the render target holds
        // its samples.
        uint32_t surfaceMsaa = SurfaceMsaa();
        int32_t samplesX = int32_t(SamplesX(surfaceMsaa)), samplesY = int32_t(SamplesY(surfaceMsaa));
        constexpr int32_t kAlignMask = ~(kResolveAlignment - 1);
        x0 = std::clamp(x0 & kAlignMask, 0, int32_t(kRenderTargetWidth) / samplesX);
        y0 = std::clamp(y0 & kAlignMask, 0, int32_t(kRenderTargetHeight) / samplesY);
        x1 = std::clamp((x1 + kResolveAlignment - 1) & kAlignMask, x0, int32_t(kRenderTargetWidth) / samplesX);
        y1 = std::clamp((y1 + kResolveAlignment - 1) & kAlignMask, y0, int32_t(kRenderTargetHeight) / samplesY);
        if (x1 <= x0 || y1 <= y0)
            return;

        // Source: the color or depth target being resolved.
        RenderTarget* target;
        bool depthFloat = false;
        if (depthSource)
        {
            uint32_t depthInfo = ReadRegister(kRegRbDepthInfo);
            depthFloat = DepthIsFloat(depthInfo);
            target = GetRenderTarget(EdramBase(depthInfo), depthFloat, true);
        }
        else
        {
            uint32_t colorInfo = ReadRegister(kRegColorInfo[source]);
            target = GetColorTarget(EdramBase(colorInfo), ColorFormat(colorInfo));
        }
        if (!target)
            return;

        // Destination: the host texture covering the destination memory.
        uint32_t destBase = GuestToPhysical(ReadRegister(kRegRbCopyDestBase));
        uint32_t destPitchReg = ReadRegister(kRegRbCopyDestPitch);
        uint32_t destInfo = ReadRegister(kRegRbCopyDestInfo);
        uint32_t destFormat = (destInfo >> 7) & 0x3F;
        uint32_t destEndian = destInfo & 7;
        int32_t destExpBias = int32_t(destInfo << 10) >> 26;  // signed 6 bits @16
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
        ResolvedTexture* texture = DestinationTexture(destBase, destPitchReg, destFormat, bpp, hostFormat, uint32_t(y1 - y0));
        if (!texture)
            return;

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

        static const bool logResolves = EnvFlag("H3_LOG_RESOLVES");
        if (logResolves)
            fprintf(stderr,
                "[render] f%u r%u resolve rt%u %d,%d-%d,%d -> %08X (pitch reg %08X fmt %u info %08X) tex %08X %ux%u at %u,%u surface %08X\n",
                g_swaps + 1, g_frameResolves + 1, source, x0, y0, x1, y1, destBase, destPitchReg, destFormat, destInfo, texture->base,
                texture->pitch, texture->height, destX, destY, ReadRegister(kRegRbSurfaceInfo));

        // The copy itself, in host texels: the source rectangle is in
        // samples times the render scale, the destination in texels times
        // the render scale; each destination texel averages one pixel's
        // samples.
        Flush();
        const int32_t scale = int32_t(g_renderScale);
        int32_t sampleX0 = x0 * samplesX * scale, sampleY0 = y0 * samplesY * scale;
        int32_t hostDestX = int32_t(destX) * scale, hostDestY = int32_t(destY) * scale;
        uint32_t hostW = w * uint32_t(scale), hostH = h * uint32_t(scale);
        auto copy = [&](CopyMode copyMode, float colorScale) {
            ConvertCopy(target->texture, texture->texture, texture->hostFormat, texture->HostWidth(), texture->HostHeight(), sampleX0,
                sampleY0, hostDestX, hostDestY, hostW, hostH, copyMode, depthFloat, colorScale, samplesX, samplesY);
        };
        if (depthSource)
        {
            copy(CopyMode::Value, 1.0f);
            texture->depthFloat = depthFloat;
        }
        else if (target->xenosFormat == kColorFormat8888Gamma && !IsFloatHostFormat(texture->hostFormat))
            copy(CopyMode::LinearToGamma, 1.0f);
        else if (destExpBias != 0 || samplesX != 1 || samplesY != 1)
        {
            // RB_COPY_DEST_INFO exp bias: the copy scales colors by 2^bias
            // (Halo copies its tone-mapped 7e3 image out with -5).
            copy(CopyMode::Value, std::ldexp(1.0f, destExpBias));
        }
        else
        {
            SDL_GPUBlitInfo blit{};
            blit.source = { target->texture, 0, 0, uint32_t(sampleX0), uint32_t(sampleY0), hostW, hostH };
            blit.destination = { texture->texture, 0, 0, uint32_t(hostDestX), uint32_t(hostDestY), hostW, hostH };
            blit.load_op = SDL_GPU_LOADOP_LOAD;
            blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(Commands(), &blit);
        }

        if (!depthSource && w * h <= kMaxWriteBackPixels && !g_noWriteBack)
            WriteBackResolve(*texture, destX, destY, w, h, destEndian);
        texture->generation++;
        g_frameResolves++;
        DumpResolve(*texture, *target, depthSource, destBase, w, h);
    }
}
