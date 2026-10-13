// EDRAM render targets. Xenos renders into 10 MB of embedded memory (EDRAM)
// addressed by a tile base; the same EDRAM can be viewed in different
// formats and sample counts. Here each (base, format) pair gets its own host
// texture in sample space, and switching formats at one base carries the
// contents over (EDRAM aliasing).

#include "gpu/renderer/internal.h"

#include <algorithm>
#include <cstdio>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // EDRAM base used for the dummy target bound in unused color slots.
        constexpr uint32_t kScratchBase = kEdramBaseMask;

        std::unordered_map<uint64_t, RenderTarget> g_renderTargets;  // (base, format, depth) -> texture

        // EDRAM aliasing: the same EDRAM base can be drawn to or resolved
        // from in another format (Halo draws its final image as
        // 2_10_10_10_FLOAT and resolves it as 2_10_10_10). Each format has
        // its own host texture, so on a switch the contents move over to the
        // texture now in use. The console reinterprets the bits; this
        // converts values instead, which is close but not exact.
        std::unordered_map<uint32_t, RenderTarget*> g_colorAtBase;  // EDRAM base -> target last used there

        // Carrying the whole target over on every switch was most of the GPU
        // time at a render scale: Halo switches formats ~50 times a frame at
        // two bases (8_8_8_8 <-> 8_8_8_8_GAMMA and others), and each switch
        // converted all 2560x1280 sample-space texels (x9 at 3x: 2 Gpx a
        // frame). So each base keeps a log of the rectangles written to it,
        // numbered in order, and each target remembers the write number it
        // was last in sync at (RenderTarget::syncedWrite). On a switch, the
        // new target gets just the bounding box of what was written since
        // it was in sync, from the current target, which always holds
        // everything.
        struct BaseWrites
        {
            uint64_t next = 1;  // number of the next write
            std::vector<std::pair<uint64_t, SDL_Rect>> rects;  // (write number, rectangle), oldest first
        };
        std::unordered_map<uint32_t, BaseWrites> g_baseWrites;
        // Writes kept per base before the oldest are merged into one
        // (conservative: the merged rectangle covers them all).
        constexpr size_t kMaxBaseWrites = 256;

        // Union of two rectangles (an empty one is ignored).
        SDL_Rect Union(const SDL_Rect& a, const SDL_Rect& b)
        {
            if (a.w <= 0 || a.h <= 0)
                return b;
            if (b.w <= 0 || b.h <= 0)
                return a;
            int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
            int x1 = std::max(a.x + a.w, b.x + b.w), y1 = std::max(a.y + a.h, b.y + b.h);
            return SDL_Rect{ x0, y0, x1 - x0, y1 - y0 };
        }

        // The rectangle a target at a base is missing: everything written
        // there after its last sync (the whole target if it never synced).
        SDL_Rect StaleRect(uint32_t base, const RenderTarget& target)
        {
            SDL_Rect full{ 0, 0, int(TargetWidth()), int(TargetHeight()) };
            if (!target.synced)
                return full;
            SDL_Rect stale{};
            for (const auto& [write, rect] : g_baseWrites[base].rects)
                if (write > target.syncedWrite)
                    stale = Union(stale, rect);
            return stale;
        }

        // Host format for a ColorRenderTargetFormat.
        SDL_GPUTextureFormat ColorTargetFormat(uint32_t xenosFormat)
        {
            switch (xenosFormat)
            {
            case kColorFormat8888:
                return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            case kColorFormat8888Gamma:
                // Linear values at 8 bits lost the darks (posterized shadows;
                // Halo's lighting relies on this target for them). The
                // console stores them gamma encoded; resolves encode.
                return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
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

        // Moves what `from` holds in a rectangle into `to` (an EDRAM format
        // switch).
        void CarryOver(const RenderTarget& from, const RenderTarget& to, const SDL_Rect& rect)
        {
            GpuTimedScope timed{ "carry over" };  // H3_GPU_TIMING
            // 8_8_8_8_GAMMA keeps linear values on the host (as 16-bit
            // floats), other 8-bit formats the stored (encoded) ones.
            bool fromGamma = from.xenosFormat == kColorFormat8888Gamma, toGamma = to.xenosFormat == kColorFormat8888Gamma;
            SDL_GPUTextureFormat encodedFormat = fromGamma ? to.format : from.format;
            auto convert = [&](CopyMode mode) {
                ConvertCopy(from.texture, to.texture, to.format, TargetWidth(), TargetHeight(), rect.x, rect.y, rect.x, rect.y,
                    uint32_t(rect.w), uint32_t(rect.h), mode);
            };
            if (Is7e3(from.xenosFormat) && !IsFloatHostFormat(to.format))
                convert(CopyMode::FloatTo7e3);
            else if (fromGamma != toGamma && !IsFloatHostFormat(encodedFormat))
                convert(fromGamma ? CopyMode::LinearToGamma : CopyMode::GammaToLinear);
            else
            {
                SDL_GPUBlitInfo blit{};
                uint32_t x = uint32_t(rect.x), y = uint32_t(rect.y), w = uint32_t(rect.w), h = uint32_t(rect.h);
                blit.source = { from.texture, 0, 0, x, y, w, h };
                blit.destination = { to.texture, 0, 0, x, y, w, h };
                // Only part of the target is replaced: keep the rest.
                blit.load_op = SDL_GPU_LOADOP_LOAD;
                blit.filter = SDL_GPU_FILTER_NEAREST;
                CountPass(PassKind::Blit, uint64_t(w) * h);
                SDL_BlitGPUTexture(Commands(), &blit);
            }
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
        info.width = TargetWidth();
        info.height = TargetHeight();
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

    RenderTarget* GetColorTarget(uint32_t base, uint32_t xenosFormat)
    {
        RenderTarget* target = GetRenderTarget(base, xenosFormat, false);
        if (!target)
            return nullptr;
        RenderTarget*& current = g_colorAtBase[base];
        BaseWrites& writes = g_baseWrites[base];
        if (current && current != target)
        {
            // H3_FULL_CARRYOVER=1: copy whole targets on a switch (the old
            // behaviour), to compare.
            static const bool fullCarry = EnvFlag("H3_FULL_CARRYOVER");
            SDL_Rect stale = fullCarry ? SDL_Rect{ 0, 0, int(TargetWidth()), int(TargetHeight()) } : StaleRect(base, *target);
            static const bool logCarry = EnvFlag("H3_LOG_CARRYOVER");
            if (logCarry)
                fprintf(stderr, "[carry] frame %u base %03X %u -> %u: %dx%d at %d,%d\n", g_swaps, base, current->xenosFormat, xenosFormat,
                    stale.w, stale.h, stale.x, stale.y);
            if (stale.w > 0 && stale.h > 0)
            {
                Flush();
                CarryOver(*current, *target, stale);
            }
            // The outgoing target holds everything up to now too.
            current->synced = true;
            current->syncedWrite = writes.next - 1;
        }
        target->synced = true;
        target->syncedWrite = writes.next - 1;
        current = target;
        return target;
    }

    void RecordColorWrite(uint32_t base, const SDL_Rect& rect)
    {
        if (rect.w <= 0 || rect.h <= 0)
            return;
        BaseWrites& writes = g_baseWrites[base];
        writes.rects.push_back({ writes.next++, rect });
        if (writes.rects.size() > kMaxBaseWrites)
        {
            // Merge the older half into one write numbered as the newest of
            // them: anything that needed one of them gets all of them.
            size_t half = writes.rects.size() / 2;
            SDL_Rect merged{};
            for (size_t i = 0; i < half; i++)
                merged = Union(merged, writes.rects[i].second);
            uint64_t number = writes.rects[half - 1].first;
            writes.rects.erase(writes.rects.begin(), writes.rects.begin() + half);
            writes.rects.insert(writes.rects.begin(), { number, merged });
        }
    }

    RenderTarget* ColorTargetAt(uint32_t base)
    {
        auto it = g_colorAtBase.find(base);
        return it != g_colorAtBase.end() ? it->second : nullptr;
    }

    VideoMemoryUse RenderTargetMemory()
    {
        VideoMemoryUse use;
        for (const auto& [key, target] : g_renderTargets)
        {
            use.count++;
            use.bytes += HostTextureBytes(target.format, TargetWidth(), TargetHeight());
        }
        return use;
    }

    bool IsFloatHostFormat(SDL_GPUTextureFormat format)
    {
        return format == SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT || format == SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT ||
            format == SDL_GPU_TEXTUREFORMAT_R32_FLOAT || format == SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT ||
            format == SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
    }

    TargetSet CurrentTargets(const Shader& ps, uint32_t colorMask, uint32_t depthControl, uint32_t edramMode)
    {
        TargetSet targets;
        if (edramMode == kEdramModeColorDepth)
        {
            for (uint32_t i = 0; i < 4; i++)
            {
                // Skip targets that are masked off or that the shader doesn't write.
                if (!((colorMask >> (4 * i)) & 0xF) || !(ps.info.colorTargetMask & (1u << i)))
                    continue;
                uint32_t info = ReadRegister(kRegColorInfo[i]);
                RenderTarget* target = GetColorTarget(EdramBase(info), ColorFormat(info));
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
                    RenderTarget* scratch = GetRenderTarget(kScratchBase, kColorFormat8888, false);
                    targets.color[i] = scratch->texture;
                    targets.colorFormat[i] = scratch->format;
                }
            }
        }
        if (depthControl & (kDepthControlStencilEnable | kDepthControlZEnable | kDepthControlZWriteEnable))
        {
            uint32_t info = ReadRegister(kRegRbDepthInfo);
            if (RenderTarget* target = GetRenderTarget(EdramBase(info), DepthIsFloat(info), true))
                targets.depth = target->texture;
        }
        return targets;
    }
}
