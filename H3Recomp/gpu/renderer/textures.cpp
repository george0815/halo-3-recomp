// Textures for shaders. A texture fetch either reads memory a resolve wrote
// (sampled from the resolve's host texture, see resolve.cpp) or ordinary
// texture data, which is decoded from guest memory (gpu/texture.cpp) into a
// host texture and cached. Cached textures are re-checked against memory
// now and then, since the game streams textures in and decodes videos into
// them.

#include "gpu/renderer/internal.h"

#include <algorithm>
#include <set>
#include <functional>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // A cached texture's memory is hashed again when it's used this many
        // frames after its last check: hashing every bound texture every
        // frame cost milliseconds, and streamed-in data showing up a few
        // frames later is harmless. (Checking on frames where
        // (frame + hash) % 8 == 0 instead never refreshed a texture used only
        // every other frame when the hash had the wrong parity: the Halo 3
        // logo screen's second Bink buffer stayed black and flickered.)
        constexpr uint32_t kCheckInterval = 8;
        // Textures that changed within this many frames are checked every
        // frame: Bink double-buffers its frames, and a buffer also sampled
        // while it holds the previous frame must not count as settled (stale
        // frames flickered in the splash videos).
        constexpr uint32_t kRecentFrames = 60;
        // checkedFrame/changedFrame value for "never".
        constexpr uint32_t kNever = ~0u;
        // Fetch constant swizzle for X, Y, Z, W in order (3 bits each).
        constexpr uint32_t kIdentitySwizzle = 0x688;
        // Where the host-to-guest texel ratio goes in a texture's system
        // constant word 3 (translator.h, tfetch.w).
        constexpr uint32_t kTextureScaleShift = 8;
        // A stacked texture's layer count (0: the 3D binding is used) and
        // whether to filter between layers, in the same word.
        constexpr uint32_t kStackedLayersShift = 16, kMaxStackedLayers = 0x7FF;
        constexpr uint32_t kStackedLinearBit = 1u << 27;
        // Fetch constant dimension values (texture.h).
        constexpr uint32_t kDimension2D = 1, kDimension3D = 2, kDimensionCube = 3;
        // Faces of a cube map.
        constexpr uint32_t kCubeFaces = 6;
        // Video memory decoded textures may hold before the least recently
        // used ones are freed. Without a limit the cache grew with every
        // texture the game ever streamed in until the 8 GB card ran out,
        // and the NVIDIA driver didn't recover from that (no Vulkan device
        // could be created until a reboot).
        constexpr uint64_t kTextureBudgetBytes = 2048ull * 1024 * 1024;
        // Textures unused this many frames are freed even under the budget
        // (about a minute at 30 fps): streamed-out data and old levels.
        constexpr uint32_t kMaxIdleFrames = 1800;
        // Textures used within this many frames are never freed (the
        // current frame's command buffer may still reference them).
        constexpr uint32_t kMinIdleFrames = 4;

        // A texture decoded from guest memory.
        struct CachedTexture
        {
            SDL_GPUTexture* texture = nullptr;
            uint64_t contentHash = 0;      // hash of the guest memory it was decoded from
            uint32_t checkedFrame = kNever;  // last frame its memory was hashed
            uint32_t changedFrame = kNever;  // last frame its contents changed (videos, render-to-memory)
            uint32_t usedFrame = 0;          // last frame it was bound
            uint64_t bytes = 0;              // host memory it holds (approximate: the decoded data)
        };
        std::unordered_map<uint64_t, CachedTexture> g_textures;  // fetch constant layout -> texture
        uint64_t g_textureBytes = 0;  // sum of g_textures' bytes
        std::unordered_map<uint32_t, SDL_GPUSampler*> g_samplers;  // clamp/filter modes -> sampler

        // Gray stand-in of the right type for a shader's texture binding.
        SDL_GPUTexture* PlaceholderFor(shader::TextureBinding binding)
        {
            if (binding.array)
                return g_placeholderArray;
            switch (binding.dimension)
            {
            case ucode::TextureDimension::D3OrStacked: return g_placeholder3D;
            case ucode::TextureDimension::Cube: return g_placeholderCube;
            default: return g_placeholder2D;
            }
        }

        // The mip levels of a 2D texture that lie in memory and that a host
        // texture of the base level's size can hold.
        std::vector<texture::FetchConstant> UsableMips(const texture::FetchConstant& constant, const texture::FetchConstant& base)
        {
            std::vector<texture::FetchConstant> mips = texture::MipLevels(constant);
            uint32_t hostLevels = 1;
            while ((std::max(base.width, base.height) >> hostLevels) != 0)
                hostLevels++;
            if (mips.size() + 1 > hostLevels)
                mips.resize(hostLevels - 1);
            for (size_t i = 0; i < mips.size(); i++)
                if (mips[i].address + uint64_t(texture::GuestSize(mips[i])) > kPhysicalMemorySize)
                {
                    mips.resize(i);
                    break;
                }
            return mips;
        }

        // Uploads decoded faces/layers (as layers, or as slices of a 3D
        // texture) and mip levels (each with its faces: one for 2D, six for
        // cube maps) into a host texture.
        void UploadTexture(SDL_GPUTexture* texture, const std::vector<texture::Decoded>& faces,
            const std::vector<std::vector<texture::Decoded>>& mips, bool volume)
        {
            const texture::Decoded& base = faces[0];
            uint32_t faceBytes = uint32_t(base.data.size());
            uint32_t totalBytes = faceBytes * uint32_t(faces.size());
            for (const std::vector<texture::Decoded>& level : mips)
                for (const texture::Decoded& mip : level)
                    totalBytes += uint32_t(mip.data.size());

            // PERF: a transfer buffer per upload; pooling (as for draws) would
            // help when videos re-upload every frame.
            g_textureUploads++;
            g_textureUploadBytes += totalBytes;
            SDL_GPUTransferBufferCreateInfo transferInfo{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, totalBytes, 0 };
            SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(g_device, &transferInfo);
            uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(g_device, transfer, false));
            uint32_t offset = 0;
            for (const texture::Decoded& face : faces)
            {
                memcpy(mapped + offset, face.data.data(), faceBytes);
                offset += faceBytes;
            }
            for (const std::vector<texture::Decoded>& level : mips)
                for (const texture::Decoded& mip : level)
                {
                    memcpy(mapped + offset, mip.data.data(), mip.data.size());
                    offset += uint32_t(mip.data.size());
                }
            SDL_UnmapGPUTransferBuffer(g_device, transfer);

            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            offset = 0;
            for (uint32_t face = 0; face < faces.size(); face++)
            {
                SDL_GPUTextureTransferInfo source{ transfer, offset, base.rowTexels, base.rows };
                SDL_GPUTextureRegion region{};
                region.texture = texture;
                if (volume)
                    region.z = face;
                else
                    region.layer = face;
                region.w = base.width;
                region.h = base.height;
                region.d = 1;
                SDL_UploadToGPUTexture(copy, &source, &region, false);
                offset += faceBytes;
            }
            for (size_t i = 0; i < mips.size(); i++)
            {
                uint32_t level = uint32_t(i) + 1;
                for (uint32_t face = 0; face < mips[i].size(); face++)
                {
                    const texture::Decoded& mip = mips[i][face];
                    SDL_GPUTextureTransferInfo source{ transfer, offset, mip.rowTexels, mip.rows };
                    SDL_GPUTextureRegion region{};
                    region.texture = texture;
                    region.mip_level = level;
                    region.layer = face;
                    // The host level's size (the data may extend past it:
                    // whole compressed blocks).
                    region.w = std::max(1u, base.width >> level);
                    region.h = std::max(1u, base.height >> level);
                    region.d = 1;
                    SDL_UploadToGPUTexture(copy, &source, &region, false);
                    offset += uint32_t(mip.data.size());
                }
            }
            SDL_EndGPUCopyPass(copy);
            SDL_ReleaseGPUTransferBuffer(g_device, transfer);
        }

        // The host texture for a fetch constant, decoded from guest memory
        // (and re-decoded when the memory changes). layers: a stacked
        // texture's layers as a 2D array texture with mips (for shaders that
        // sample it as 3D-or-stacked); otherwise its first layer as 2D. True
        // 3D textures are 3D host textures (base level). Null if the texture
        // can't be decoded.
        SDL_GPUTexture* MemoryTexture(const texture::FetchConstant& constant, bool layers = false)
        {
            const texture::FetchConstant fetch = texture::LargestUsableLevel(constant);
            // Cube maps: six 2D faces one after another; stacked textures the
            // same with their layers. True 3D textures tile in 3D: their
            // slices decode from one block of memory (stride 0 below).
            bool cube = fetch.dimension == kDimensionCube;
            bool true3D = fetch.dimension == kDimension3D;
            bool volume = true3D;
            bool array = layers && !true3D && fetch.dimension == kDimension2D;
            uint32_t faces = cube ? kCubeFaces : volume ? fetch.depth : array ? fetch.depth : 1;
            uint32_t faceSize = texture::GuestSize(fetch);
            uint32_t stride = true3D ? 0 : (cube || array) ? texture::FaceStride(fetch) : faceSize;
            uint32_t size = stride * (faces - 1) + faceSize;
            if (!faceSize || fetch.address + uint64_t(size) > kPhysicalMemorySize)
                return nullptr;
            // Mip levels of 2D, stacked and cube textures: without them
            // distant surfaces aliased into high-contrast noise (Halo's
            // detail maps above all), and reflections sampled at a lower
            // level were as sharp as level 0. Only when the base level is the
            // fetch constant's own (a stacked texture whose base isn't
            // resident starts at level 1, which may share its address). A
            // 2D view of a stacked texture gets each level's first layer.
            std::vector<texture::FetchConstant> mips;
            bool ownBase = fetch.address == constant.address && fetch.width == constant.width && fetch.height == constant.height;
            if (!volume && ownBase)
                mips = UsableMips(constant, fetch);  // (none for 3D textures yet)
            uint32_t levels = uint32_t(mips.size()) + 1;

            // Everything that decides how the memory decodes identifies the
            // cache entry.
            uint32_t layout[] = { fetch.address, fetch.format, fetch.endian, fetch.width, fetch.height, fetch.pitch,
                uint32_t(fetch.tiled) | (uint32_t(fetch.packedMips) << 1) | (uint32_t(cube) << 2) | (uint32_t(volume) << 3) |
                    (uint32_t(array) << 4) |
                    (faces << 8) | (levels << 20),
                levels > 1 ? constant.mipAddress : 0 };
            CachedTexture& cached = g_textures[Hash(layout, sizeof(layout))];
            cached.usedFrame = g_swaps;

            bool recentlyChanged = cached.changedFrame != kNever && g_swaps - cached.changedFrame < kRecentFrames;
            if (cached.texture && (cached.checkedFrame == g_swaps ||
                                      (!recentlyChanged && g_swaps - cached.checkedFrame < kCheckInterval)))
                return cached.texture;
            cached.checkedFrame = g_swaps;
            const uint8_t* guest = GuestPtr<uint8_t>(PhysicalToGuest(fetch.address));
            uint64_t contentHash = ContentHash(guest, size);
            if (cached.texture && contentHash != cached.contentHash)
                cached.changedFrame = g_swaps;
            if (cached.texture && contentHash == cached.contentHash)
                return cached.texture;

            SlowTimer timer{ "texture" };
            std::vector<texture::Decoded> decodedFaces(faces);
            for (uint32_t face = 0; face < faces; face++)
                if (!texture::Decode(fetch, guest + size_t(face) * stride, decodedFaces[face], true3D ? face : 0))
                    return nullptr;
            std::vector<std::vector<texture::Decoded>> decodedMips(mips.size(), std::vector<texture::Decoded>(faces));
            for (size_t i = 0; i < mips.size(); i++)
            {
                uint32_t mipStride = texture::FaceStride(mips[i]);
                for (uint32_t face = 0; face < faces; face++)
                {
                    uint64_t address = uint64_t(mips[i].address) + uint64_t(face) * mipStride;
                    if (address + texture::GuestSize(mips[i]) > kPhysicalMemorySize ||
                        !texture::Decode(mips[i], GuestPtr<uint8_t>(PhysicalToGuest(uint32_t(address))), decodedMips[i][face]))
                        return nullptr;
                }
            }

            // H3_DUMP_TEXTURE=<hex physical address>: save that texture's
            // decoded levels and layers (RGBA8 ones) as
            // tex_<address>_l<level>_<layer>.png when it's decoded.
            static const char* dumpTexture = EnvString("H3_DUMP_TEXTURE");
            if (dumpTexture && uint32_t(strtoul(dumpTexture, nullptr, 16)) == constant.address)
            {
                auto save = [&](const texture::Decoded& decoded, uint32_t level, uint32_t layer) {
                    if (decoded.format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
                        return;
                    SDL_Surface* surface = SDL_CreateSurfaceFrom(int(decoded.width), int(decoded.height), SDL_PIXELFORMAT_ABGR8888,
                        const_cast<uint8_t*>(decoded.data.data()), int(decoded.rowTexels * 4));
                    if (!surface)
                        return;
                    char path[64];
                    snprintf(path, sizeof(path), "tex_%08X_l%u_%u.png", constant.address, level, layer);
                    SDL_SavePNG(surface, path);
                    SDL_DestroySurface(surface);
                };
                for (uint32_t face = 0; face < faces; face++)
                    save(decodedFaces[face], 0, face);
                for (size_t i = 0; i < decodedMips.size(); i++)
                    for (uint32_t face = 0; face < decodedMips[i].size(); face++)
                        save(decodedMips[i][face], uint32_t(i) + 1, face);
            }

            if (!cached.texture)
            {
                const texture::Decoded& decoded = decodedFaces[0];
                SDL_GPUTextureCreateInfo info{};
                info.type = cube   ? SDL_GPU_TEXTURETYPE_CUBE
                    : volume       ? SDL_GPU_TEXTURETYPE_3D
                    : array        ? SDL_GPU_TEXTURETYPE_2D_ARRAY
                                   : SDL_GPU_TEXTURETYPE_2D;
                info.format = decoded.format;
                info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
                info.width = decoded.width;
                info.height = decoded.height;
                info.layer_count_or_depth = faces;
                info.num_levels = levels;
                cached.texture = SDL_CreateGPUTexture(g_device, &info);
                // New textures (a video's first frames, data still streaming
                // in) are watched like ones that just changed.
                cached.changedFrame = g_swaps;
                if (!cached.texture)
                {
                    LogOnce("texture creation failed (format " + std::to_string(fetch.format) + "): " + SDL_GetError());
                    return nullptr;
                }
            }
            cached.contentHash = contentHash;
            if (!cached.bytes)
            {
                for (const texture::Decoded& face : decodedFaces)
                    cached.bytes += face.data.size();
                for (const std::vector<texture::Decoded>& level : decodedMips)
                    for (const texture::Decoded& mip : level)
                        cached.bytes += mip.data.size();
                g_textureBytes += cached.bytes;
            }
            UploadTexture(cached.texture, decodedFaces, decodedMips, volume);
            return cached.texture;
        }

        // The sampler for a fetch constant's clamp and filter modes.
        SDL_GPUSampler* SamplerFor(const texture::FetchConstant& fetch)
        {
            // Aniso filter field: 0 off, 1-5 = 1x-16x.
            uint32_t aniso = fetch.anisoFilter >= 1 && fetch.anisoFilter <= 5 ? 1u << (fetch.anisoFilter - 1) : 1;
            uint32_t key = fetch.clampX | (fetch.clampY << 3) | (fetch.clampZ << 6) | (fetch.magFilter << 9) | (fetch.minFilter << 11) |
                (fetch.mipFilter << 13) | (aniso << 16);
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
            // Mip filter: point, linear, or base level only.
            info.mipmap_mode = fetch.mipFilter == 1 ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            info.max_lod = fetch.mipFilter == 2 ? 0.0f : 1000.0f;
            if (aniso > 1)
            {
                info.enable_anisotropy = true;
                info.max_anisotropy = float(aniso);
            }
            info.address_mode_u = address(fetch.clampX);
            info.address_mode_v = address(fetch.clampY);
            info.address_mode_w = address(fetch.clampZ);
            sampler = SDL_CreateGPUSampler(g_device, &info);
            return sampler ? sampler : g_sampler;
        }

        // How the shader turns a sample of a texture decoded from memory into
        // the guest's value: the fetch constant's swizzle (adapted to the
        // host format), sign modes and exponent (translator.cpp,
        // xe_texture_result).
        void SetMemoryTextureConstants(uint32_t* constants, const texture::FetchConstant& fetch)
        {
            uint32_t signPrecision = texture::SignPrecision(fetch);
            constants[0] = texture::HostSwizzle(fetch);
            constants[1] = signPrecision ? fetch.signs : 0;
            constants[2] = uint32_t(fetch.expAdjust);
            constants[3] = std::max(signPrecision, 1u);
        }

        // Binds a texture decoded from memory, if it decodes.
        bool BindMemoryTexture(const texture::FetchConstant& fetch, bool layers, SDL_GPUTexture*& texture, SDL_GPUSampler*& sampler,
            uint32_t* constants)
        {
            SDL_GPUTexture* decoded = MemoryTexture(fetch, layers);
            if (!decoded)
                return false;
            texture = decoded;
            sampler = SamplerFor(fetch);
            SetMemoryTextureConstants(constants, fetch);
            return true;
        }
    }

    std::vector<SDL_GPUTextureSamplerBinding> BindTextures(const Shader& shader, SystemConstants& system)
    {
        // Resolved render targets first (their host textures hold what the
        // game drew), then textures decoded from memory, else placeholders.
        std::vector<SDL_GPUTextureSamplerBinding> bindings;
        for (const shader::TextureBinding& binding : shader.info.textures)
        {
            SDL_GPUTexture* texture = PlaceholderFor(binding);
            SDL_GPUSampler* sampler = g_sampler;
            texture::FetchConstant fetch = ReadTextureFetch(binding.fetchConstant);
            uint32_t* constants = system.textures[binding.fetchConstant & 31];
            bool usable = fetch.valid && !g_noTextures;
            switch (binding.dimension)
            {
            case ucode::TextureDimension::Cube:
                if (usable && fetch.dimension == kDimensionCube && !FindResolved(fetch.address))
                    BindMemoryTexture(fetch, false, texture, sampler, constants);
                break;
            case ucode::TextureDimension::D3OrStacked:
                // Each 3D-or-stacked fetch has two bindings (translator.cpp):
                // a 3D one for true 3D textures (base level only) and a 2D
                // array one for stacked textures (with mips; a 3D host
                // texture's mips would shrink the layer count). The shader
                // picks by the layer count in tfetch.w (0: 3D).
                if (usable && (binding.array ? fetch.dimension == kDimension2D : fetch.dimension == kDimension3D) &&
                    !FindResolved(fetch.address))
                {
                    // H3_LOG_VOLUMES: each 3D or stacked texture once.
                    static const bool logVolumes = EnvFlag("H3_LOG_VOLUMES");
                    static std::set<uint32_t> logged;
                    if (logVolumes && logged.insert(fetch.address).second)
                        fprintf(stderr, "[render] %s texture %08X %ux%ux%u format %u mips %u-%u at %08X packed %d (frame %u)\n",
                            fetch.dimension == kDimension3D ? "3D" : (fetch.stacked ? "stacked" : "2D-as-3D"), fetch.address, fetch.width,
                            fetch.height, fetch.depth, fetch.format, fetch.mipMin, fetch.mipMax, fetch.mipAddress, fetch.packedMips, g_swaps);
                    if (BindMemoryTexture(fetch, true, texture, sampler, constants) && binding.array)
                        constants[3] |= (std::min(fetch.depth, kMaxStackedLayers) << kStackedLayersShift) |
                            (fetch.volumeLinear ? kStackedLinearBit : 0);
                }
                break;
            case ucode::TextureDimension::D1:
            case ucode::TextureDimension::D2:
                if (ResolvedTexture* resolved = FindResolved(fetch.address))
                {
                    texture = ResolvedView(*resolved, fetch, fetch.address);
                    // The fetch constant's filter and clamp modes, as for
                    // memory textures. A fixed linear sampler blended data
                    // meant to be point sampled: Halo's depth restore reads
                    // depth resolved as 8_8_8_8 bytes, and blending bytes
                    // across a carry gave wrong depth along contour lines
                    // (z-fighting of the sky/fog with the menu scene).
                    if (fetch.valid)
                        sampler = SamplerFor(fetch);
                    // Sign modes (gamma decode above all) apply to resolves
                    // stored as unorm too; float copies already hold the
                    // values. The swizzle and exponent don't: the host copy
                    // keeps the render target's component order and scale,
                    // not memory's.
                    if (fetch.valid && fetch.signs != 0 && !IsFloatHostFormat(resolved->hostFormat))
                    {
                        constants[0] = kIdentitySwizzle;
                        constants[1] = fetch.signs;
                        constants[2] = 0;
                        constants[3] = 1;
                    }
                    // Resolved textures are render-scale times the guest's
                    // size; the shader divides texture sizes by this.
                    constants[3] |= g_renderScale << kTextureScaleShift;
                }
                else if (usable && fetch.dimension <= kDimension2D)
                {
                    if (!BindMemoryTexture(fetch, false, texture, sampler, constants))
                        LogOnce("unsupported texture format " + std::to_string(fetch.format));
                }
                break;
            }
            bindings.push_back({ texture, sampler });
        }
        return bindings;
    }

    size_t TextureCount()
    {
        return g_textures.size();
    }

    uint64_t TextureBytes()
    {
        return g_textureBytes;
    }

    void TrimTextureCache()
    {
        // Free what's been idle too long, and find the rest's ages.
        std::vector<std::pair<uint32_t, uint64_t>> candidates;  // (idle frames, key)
        for (auto it = g_textures.begin(); it != g_textures.end();)
        {
            uint32_t idle = g_swaps - it->second.usedFrame;
            if (idle > kMaxIdleFrames || (!it->second.texture && idle > kMinIdleFrames))
            {
                if (it->second.texture)
                    SDL_ReleaseGPUTexture(g_device, it->second.texture);
                g_textureBytes -= it->second.bytes;
                it = g_textures.erase(it);
                continue;
            }
            if (g_textureBytes > kTextureBudgetBytes && idle > kMinIdleFrames)
                candidates.push_back({ idle, it->first });
            ++it;
        }
        if (g_textureBytes <= kTextureBudgetBytes)
            return;
        // Over budget: least recently used first.
        // PERF: a full sort, but only on frames over budget.
        std::sort(candidates.begin(), candidates.end(), std::greater<>());
        for (const auto& [idle, key] : candidates)
        {
            if (g_textureBytes <= kTextureBudgetBytes)
                break;
            CachedTexture& cached = g_textures[key];
            SDL_ReleaseGPUTexture(g_device, cached.texture);
            g_textureBytes -= cached.bytes;
            g_textures.erase(key);
        }
    }
}
