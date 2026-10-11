// EDRAM render targets. Xenos renders into 10 MB of embedded memory (EDRAM)
// addressed by a tile base; the same EDRAM can be viewed in different
// formats and sample counts. Here each (base, format) pair gets its own host
// texture in sample space, and switching formats at one base carries the
// contents over (EDRAM aliasing).

#include "gpu/renderer/internal.h"

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

        // Moves what `from` holds into `to` (an EDRAM format switch).
        void CarryOver(const RenderTarget& from, const RenderTarget& to)
        {
            // 8_8_8_8_GAMMA keeps linear values on the host (as 16-bit
            // floats), other 8-bit formats the stored (encoded) ones.
            bool fromGamma = from.xenosFormat == kColorFormat8888Gamma, toGamma = to.xenosFormat == kColorFormat8888Gamma;
            SDL_GPUTextureFormat encodedFormat = fromGamma ? to.format : from.format;
            auto convert = [&](CopyMode mode) {
                ConvertCopy(from.texture, to.texture, to.format, kRenderTargetWidth, kRenderTargetHeight, 0, 0, 0, 0,
                    kRenderTargetWidth, kRenderTargetHeight, mode);
            };
            if (Is7e3(from.xenosFormat) && !IsFloatHostFormat(to.format))
                convert(CopyMode::FloatTo7e3);
            else if (fromGamma != toGamma && !IsFloatHostFormat(encodedFormat))
                convert(fromGamma ? CopyMode::LinearToGamma : CopyMode::GammaToLinear);
            else
            {
                SDL_GPUBlitInfo blit{};
                blit.source = { from.texture, 0, 0, 0, 0, kRenderTargetWidth, kRenderTargetHeight };
                blit.destination = { to.texture, 0, 0, 0, 0, kRenderTargetWidth, kRenderTargetHeight };
                blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                blit.filter = SDL_GPU_FILTER_NEAREST;
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

    RenderTarget* GetColorTarget(uint32_t base, uint32_t xenosFormat)
    {
        RenderTarget* target = GetRenderTarget(base, xenosFormat, false);
        if (!target)
            return nullptr;
        RenderTarget*& current = g_colorAtBase[base];
        if (current && current != target)
        {
            Flush();
            CarryOver(*current, *target);
        }
        current = target;
        return target;
    }

    RenderTarget* ColorTargetAt(uint32_t base)
    {
        auto it = g_colorAtBase.find(base);
        return it != g_colorAtBase.end() ? it->second : nullptr;
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
        if (depthControl & 7)  // stencil enable, depth test or depth write
        {
            uint32_t info = ReadRegister(kRegRbDepthInfo);
            if (RenderTarget* target = GetRenderTarget(EdramBase(info), DepthIsFloat(info), true))
                targets.depth = target->texture;
        }
        return targets;
    }
}
