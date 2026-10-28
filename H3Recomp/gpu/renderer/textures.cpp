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
        // Those checks hash kSampleCount evenly spread kSampleBytes pieces
        // (a texture streamed in over another differs everywhere); the whole
        // memory is hashed every kFullCheckInterval frames, for changes in a
        // corner. Hashing every check in full was ~7.5 MB a frame at the
        // Sierra start, none of it changed.
        constexpr uint32_t kSampleCount = 32;
        constexpr uint32_t kSampleBytes = 64;
        constexpr uint32_t kFullCheckInterval = 64;
        // Kept bases (KeptBase): pieces of every 4 KB page are hashed, so
        // any write over the memory (I/O and decompression write whole
        // pages) shows the next frame.
        constexpr uint32_t kPageHashStride = 4096;
        constexpr uint32_t kPageHashBytes = 16;
        // Textures that changed within this many frames are checked every
        // frame: Bink double-buffers its frames, and a buffer also sampled
        // while it holds the previous frame must not count as settled (stale
        // frames flickered in the splash videos).
        constexpr uint32_t kRecentFrames = 60;
        // Textures whose contents changed this many times are dynamic (Bink
        // planes, not memory a texture was streamed over once) and are
        // checked every frame for good: a video can hold a frame longer than
        // kRecentFrames (the Halo 3 logo stands ~100 frames before fading
        // out), and the 8-frame interval then showed the other Bink buffer
        // stale on alternate frames through the first part of the fade.
        constexpr uint32_t kDynamicChanges = 2;
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
        // Dwords per texture fetch constant.
        constexpr uint32_t kTextureFetchDwords = 6;
        // Hash seeds for g_frontTextures' keys: binds as 2D/cube, and as
        // layers (3D or stacked).
        constexpr uint64_t kFrontSeed = 0x46524F4E54ull, kFrontLayersSeed = 0x4C41594552ull;

        // A texture decoded from guest memory.
        struct CachedTexture
        {
            SDL_GPUTexture* texture = nullptr;
            uint64_t contentHash = 0;      // hash of the guest memory it was decoded from
            uint64_t sampleHash = 0;       // SampleHash of it
            uint32_t fullCheckedFrame = kNever;  // last frame its memory was hashed in full
            uint32_t checkedFrame = kNever;  // last frame its memory was hashed
            uint32_t changedFrame = kNever;  // last frame its contents changed (videos, render-to-memory)
            uint32_t changes = 0;            // times its contents changed after the first decode
            uint32_t usedFrame = 0;          // last frame it was bound
            uint64_t bytes = 0;              // host memory it holds (approximate: the decoded data)
            // KeptBase: the largest mip level's memory (physical) when this
            // full-size entry was decoded, and its PageHash and ContentHash.
            uint32_t mipAddress = 0, mipBytes = 0;
            uint64_t mipPageHash = 0, mipContentHash = 0;
            uint32_t keptCheckedFrame = kNever;  // KeptBase: last frame the memory was checked
            uint32_t keptFullCheckedFrame = kNever;  // ... and hashed in full
            bool keptValid = false;          // KeptBase: the memory still holds what was decoded
        };
        std::unordered_map<uint64_t, CachedTexture> g_textures;  // fetch constant layout -> texture

        // Whether a texture is checked on every use rather than every
        // kCheckInterval frames: it changed recently, or keeps changing.
        bool CheckEveryUse(const CachedTexture& cached)
        {
            return cached.changes >= kDynamicChanges ||
                (cached.changedFrame != kNever && g_swaps - cached.changedFrame < kRecentFrames);
        }
        // The entry a fetch constant's raw dwords (and the layers flag)
        // lead to, so a bind skips working out its layout: the largest
        // usable level, mip chain and sizes took ~8% of the command
        // processor thread for ~5,000 binds a frame. Cleared whenever an
        // entry of g_textures is erased (TrimTextureCache).
        std::unordered_map<uint64_t, CachedTexture*> g_frontTextures;
        // KeptBase's entries by the demoted fetch constant's front key, and
        // the frame they were looked up (null: no decoded base to keep yet;
        // looked up again every kCheckInterval frames). Cleared with
        // g_frontTextures.
        struct KeptLookup
        {
            CachedTexture* entry = nullptr;
            uint32_t frame = 0;
        };
        std::unordered_map<uint64_t, KeptLookup> g_keptFront;
        // Full-size entries (base level resident, with mips) by what a
        // demotion leaves alone: the mip address, format, size and whether
        // it's bound as layers (FullKey). Demoting moves the fetch's base
        // address onto the mip address, so the base's own address is only
        // known from when it was full size. Holds g_textures keys, so trims
        // don't invalidate it.
        std::unordered_map<uint64_t, uint64_t> g_fullByMips;
        uint64_t g_textureBytes = 0;  // sum of g_textures' bytes
        std::unordered_map<uint32_t, SDL_GPUSampler*> g_samplers;  // clamp/filter modes -> sampler

        // Hash of kSampleCount evenly spread kSampleBytes pieces of size
        // bytes (all of them when that's smaller).
        uint64_t SampleHash(const uint8_t* data, size_t size)
        {
            if (size <= size_t(kSampleCount) * kSampleBytes)
                return ContentHash(data, size);
            uint8_t samples[kSampleCount * kSampleBytes];
            for (uint32_t i = 0; i < kSampleCount; i++)
                memcpy(samples + i * kSampleBytes, data + (size - kSampleBytes) * i / (kSampleCount - 1), kSampleBytes);
            return ContentHash(samples, sizeof(samples));
        }

        // Hash of kPageHashBytes from the middle of every kPageHashStride
        // bytes of size bytes.
        uint64_t PageHash(const uint8_t* data, size_t size)
        {
            constexpr size_t kOffset = kPageHashStride / 2;
            std::vector<uint8_t> samples;
            samples.reserve((size / kPageHashStride + 1) * kPageHashBytes);
            for (size_t page = 0; page < size; page += kPageHashStride)
            {
                size_t at = std::min(page + kOffset, size - std::min<size_t>(size, kPageHashBytes));
                samples.insert(samples.end(), data + at, data + at + std::min<size_t>(kPageHashBytes, size - at));
            }
            return ContentHash(samples.data(), samples.size());
        }

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
        // texture of the base level's size can hold. base may be a smaller
        // level standing in for a base that isn't resident yet
        // (LargestUsableLevel): the levels after it, found by size.
        std::vector<texture::FetchConstant> UsableMips(const texture::FetchConstant& constant, const texture::FetchConstant& base)
        {
            std::vector<texture::FetchConstant> mips = texture::MipLevels(constant);
            if (base.width != constant.width || base.height != constant.height)
            {
                // H3_NO_STANDIN_MIPS=1 (testing): a stand-in level alone,
                // as before 2026-10-27.
                static const bool none = EnvFlag("H3_NO_STANDIN_MIPS");
                if (none)
                    return {};
                size_t level = 0;
                while (level < mips.size() && (mips[level].width != base.width || mips[level].height != base.height))
                    level++;
                mips.erase(mips.begin(), mips.begin() + std::min(level + 1, mips.size()));
            }
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
            GpuTimedScope timed{ "texture upload" };  // H3_GPU_TIMING
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

        // g_fullByMips' key for a fetch constant.
        uint64_t FullKey(const texture::FetchConstant& constant, bool layers)
        {
            uint32_t key[] = { constant.mipAddress, constant.format, constant.width, constant.height, constant.dimension, uint32_t(layers) };
            return Hash(key, sizeof(key));
        }

        // The g_textures key: everything that decides how the memory
        // decodes (fetch: the level decoded as the base, levels: with its
        // mips).
        uint64_t EntryKey(const texture::FetchConstant& constant, const texture::FetchConstant& fetch, bool cube, bool volume, bool array,
            uint32_t faces, uint32_t levels)
        {
            uint32_t layout[] = { fetch.address, fetch.format, fetch.endian, fetch.width, fetch.height, fetch.pitch,
                uint32_t(fetch.tiled) | (uint32_t(fetch.packedMips) << 1) | (uint32_t(cube) << 2) | (uint32_t(volume) << 3) |
                    (uint32_t(array) << 4) |
                    (faces << 8) | (levels << 20),
                levels > 1 ? constant.mipAddress : 0 };
            return Hash(layout, sizeof(layout));
        }

        // Halo's texture cache demotes textures under memory pressure: it
        // binds them at mip_min_level 1 while their base level sits in
        // memory untouched, then reads each base back in, about one texture
        // every two frames (Sierra 117: ~95 at once as the cave's textures
        // stream in, 2-3 s of half-resolution textures popping back one by
        // one). Like MCC, which has the memory not to, draw the full texture
        // through that: a demoted texture that was drawn at full size before
        // keeps its full-size entry, as long as it's still the same texture
        // (its mips unchanged, every page checked each frame). Demoting moves
        // the fetch's base onto its mips, so the entry is found by those
        // (g_fullByMips). Textures streaming in for the first time have no
        // such entry.
        // H3_NO_KEPT_BASES=1: the 360's behaviour.
        SDL_GPUTexture* KeptBase(const texture::FetchConstant& constant, bool layers, uint64_t frontKey)
        {
            auto [it, inserted] = g_keptFront.try_emplace(frontKey);
            if (inserted || (!it->second.entry && g_swaps - it->second.frame >= kCheckInterval))
            {
                it->second.frame = g_swaps;
                it->second.entry = nullptr;
                if (auto full = g_fullByMips.find(FullKey(constant, layers)); full != g_fullByMips.end())
                {
                    if (auto entry = g_textures.find(full->second); entry != g_textures.end())
                        it->second.entry = &entry->second;
                    else
                        g_fullByMips.erase(full);
                }
            }
            CachedTexture* cached = it->second.entry;
            if (!cached || !cached->texture || !cached->mipBytes)
                return nullptr;
            if (cached->keptCheckedFrame != g_swaps)
            {
                // Still the same texture: its mips (which the demoted fetch
                // draws from, so they stay resident) hold what they held when
                // the full-size entry was decoded. The base's memory doesn't
                // matter: the host copy holds that level, and Halo reuses the
                // memory for what it streams in (the base comes back
                // elsewhere).
                cached->keptCheckedFrame = g_swaps;
                const uint8_t* guest = GuestPtr<uint8_t>(PhysicalToGuest(cached->mipAddress));
                cached->keptValid = PageHash(guest, cached->mipBytes) == cached->mipPageHash;
                static const bool logKept = EnvFlag("H3_LOG_TEXTURE_DECODES");
                if (logKept && !cached->keptValid)
                    fprintf(stderr, "[kept] f%u %08X: mip memory changed\n", g_swaps, cached->mipAddress);
                if (cached->keptValid &&
                    (cached->keptFullCheckedFrame == kNever || g_swaps - cached->keptFullCheckedFrame >= kFullCheckInterval))
                {
                    cached->keptFullCheckedFrame = g_swaps;
                    cached->keptValid = ContentHash(guest, cached->mipBytes) == cached->mipContentHash;
                }
            }
            if (!cached->keptValid)
                return nullptr;
            cached->usedFrame = g_swaps;
            // H3_LOG_TEXTURE_DECODES: textures drawn at full size through a
            // demotion, per frame.
            static const bool log = EnvFlag("H3_LOG_TEXTURE_DECODES");
            if (log)
            {
                static std::set<uint32_t> kept;
                static uint32_t frame = 0;
                if (frame != g_swaps)
                {
                    if (!kept.empty())
                        fprintf(stderr, "[tex] f%u: %zu demoted textures kept at full size\n", frame, kept.size());
                    kept.clear();
                    frame = g_swaps;
                }
                kept.insert(cached->mipAddress);
            }
            return cached->texture;
        }

        // The host texture for a fetch constant, decoded from guest memory
        // (and re-decoded when the memory changes). layers: a stacked
        // texture's layers as a 2D array texture with mips (for shaders that
        // sample it as 3D-or-stacked); otherwise its first layer as 2D. True
        // 3D textures are 3D host textures (base level). Null if the texture
        // can't be decoded.
        SDL_GPUTexture* MemoryTexture(const texture::FetchConstant& constant, bool layers, uint64_t frontKey)
        {
            static const bool noKeptBases = EnvFlag("H3_NO_KEPT_BASES");
            if (!noKeptBases && constant.mipMin > 0 && constant.mipAddress != 0)
                if (SDL_GPUTexture* kept = KeptBase(constant, layers, frontKey))
                    return kept;
            // Known fetch constant: its entry, if it needs no check now.
            if (auto front = g_frontTextures.find(frontKey); front != g_frontTextures.end())
            {
                CachedTexture& cached = *front->second;
                cached.usedFrame = g_swaps;
                bool recentlyChanged = CheckEveryUse(cached);
                if (cached.texture && (cached.checkedFrame == g_swaps || (!recentlyChanged && g_swaps - cached.checkedFrame < kCheckInterval)))
                    return cached.texture;
            }
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
            // level were as sharp as level 0. A smaller level standing in
            // for a base that isn't resident yet gets the levels after it:
            // with none, every texture still streaming in (most of a
            // level's ground and water just after it loads) drew from one
            // level and shimmered grainy until its base arrived (The
            // Covenant's beach). A 2D view of a stacked texture gets each
            // level's first layer.
            std::vector<texture::FetchConstant> mips;
            if (!volume)
                mips = UsableMips(constant, fetch);  // (none for 3D textures yet)
            uint32_t levels = uint32_t(mips.size()) + 1;

            uint64_t entryKey = EntryKey(constant, fetch, cube, volume, array, faces, levels);
            CachedTexture& cached = g_textures[entryKey];
            g_frontTextures[frontKey] = &cached;
            cached.usedFrame = g_swaps;

            bool recentlyChanged = CheckEveryUse(cached);
            if (cached.texture && (cached.checkedFrame == g_swaps ||
                                      (!recentlyChanged && g_swaps - cached.checkedFrame < kCheckInterval)))
                return cached.texture;
            cached.checkedFrame = g_swaps;
            const uint8_t* guest = GuestPtr<uint8_t>(PhysicalToGuest(fetch.address));
            // Settled textures: the samples first, in full now and then.
            uint64_t sampleHash = SampleHash(guest, size);
            bool full = !cached.texture || recentlyChanged || cached.fullCheckedFrame == kNever ||
                g_swaps - cached.fullCheckedFrame >= kFullCheckInterval || sampleHash != cached.sampleHash;
            if (!full)
                return cached.texture;
            cached.fullCheckedFrame = g_swaps;
            uint64_t contentHash = ContentHash(guest, size);
            bool samplesChanged = sampleHash != cached.sampleHash;
            cached.sampleHash = sampleHash;
            if (cached.texture && contentHash != cached.contentHash)
            {
                cached.changedFrame = g_swaps;
                cached.changes++;
            }
            if (cached.texture && contentHash == cached.contentHash)
                return cached.texture;

            // H3_LOG_TEXTURE_DECODES: every decode (new entry, or memory
            // that changed; found by the samples or only by a full check),
            // and whether a smaller level stands in for the base.
            static const bool logDecodes = EnvFlag("H3_LOG_TEXTURE_DECODES");
            if (logDecodes)
                fprintf(stderr, "[tex] f%u %s %08X %ux%u fmt %u levels %u%s (fetch %08X %ux%u mips %u-%u @%08X)\n", g_swaps,
                    !cached.texture ? "new" : samplesChanged ? "changed" : "changed-full", fetch.address, fetch.width,
                    fetch.height, fetch.format, levels, fetch.width != constant.width ? " stand-in" : "", constant.address, constant.width,
                    constant.height, constant.mipMin, constant.mipMax, constant.mipAddress);

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
            // decoded levels and layers (RGBA8 ones, single-channel R8 / BC4
            // ones as gray, DXT1 / DXT5 ones expanded to RGBA) as
            // tex_<address>_l<level>_<layer>.png when it's decoded.
            static const char* dumpTexture = EnvString("H3_DUMP_TEXTURE");
            if (dumpTexture && uint32_t(strtoul(dumpTexture, nullptr, 16)) == constant.address)
            {
                auto save = [&](const texture::Decoded& decoded, uint32_t level, uint32_t layer) {
                    std::vector<uint8_t> expanded;
                    const uint8_t* rgba = decoded.data.data();
                    if (decoded.format == SDL_GPU_TEXTUREFORMAT_R8_UNORM || decoded.format == SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM)
                    {
                        // One channel: expand to opaque gray RGBA8 rows of
                        // rowTexels.
                        expanded.assign(size_t(decoded.rowTexels) * decoded.rows * 4, 255);
                        auto put = [&](uint32_t x, uint32_t y, uint8_t value) {
                            uint8_t* texel = &expanded[(size_t(y) * decoded.rowTexels + x) * 4];
                            texel[0] = texel[1] = texel[2] = value;
                        };
                        if (decoded.format == SDL_GPU_TEXTUREFORMAT_R8_UNORM)
                        {
                            for (uint32_t y = 0; y < decoded.rows; y++)
                                for (uint32_t x = 0; x < decoded.rowTexels; x++)
                                    put(x, y, decoded.data[size_t(y) * decoded.rowTexels + x]);
                        }
                        else
                        {
                            // BC4 blocks (8 bytes: two endpoints, 16 3-bit
                            // indices), blocks in rows.
                            uint32_t blocksX = decoded.rowTexels / 4;
                            for (uint32_t by = 0; by < decoded.rows / 4; by++)
                                for (uint32_t bx = 0; bx < blocksX; bx++)
                                {
                                    const uint8_t* block = &decoded.data[(size_t(by) * blocksX + bx) * 8];
                                    uint32_t a0 = block[0], a1 = block[1];
                                    uint8_t palette[8] = { uint8_t(a0), uint8_t(a1) };
                                    for (uint32_t i = 2; i < 8; i++)
                                        palette[i] = a0 > a1 ? uint8_t(((8 - i) * a0 + (i - 1) * a1) / 7)
                                            : i < 6         ? uint8_t(((6 - i) * a0 + (i - 1) * a1) / 5)
                                                            : uint8_t(i == 6 ? 0 : 255);
                                    uint64_t bits = 0;
                                    for (uint32_t i = 0; i < 6; i++)
                                        bits |= uint64_t(block[2 + i]) << (8 * i);
                                    for (uint32_t i = 0; i < 16; i++)
                                        put(bx * 4 + i % 4, by * 4 + i / 4, palette[(bits >> (3 * i)) & 7]);
                                }
                        }
                        rgba = expanded.data();
                    }
                    else if (decoded.format == SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM || decoded.format == SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM)
                    {
                        // DXT1 / DXT5 blocks (BC3: an 8-byte BC4-style alpha
                        // block first), blocks in rows: decoded to RGBA8 so
                        // lightmaps and such can be looked at.
                        bool bc3 = decoded.format == SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
                        uint32_t blockBytes = bc3 ? 16 : 8;
                        expanded.assign(size_t(decoded.rowTexels) * decoded.rows * 4, 255);
                        uint32_t blocksX = decoded.rowTexels / 4;
                        auto expand565 = [](uint32_t c, uint8_t out[3]) {
                            out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
                            out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
                            out[2] = uint8_t((c & 31) * 255 / 31);
                        };
                        for (uint32_t by = 0; by < decoded.rows / 4; by++)
                            for (uint32_t bx = 0; bx < blocksX; bx++)
                            {
                                const uint8_t* block = &decoded.data[(size_t(by) * blocksX + bx) * blockBytes];
                                uint8_t alpha[16];
                                std::fill(alpha, alpha + 16, uint8_t(255));
                                if (bc3)
                                {
                                    uint32_t a0 = block[0], a1 = block[1];
                                    uint8_t palette[8] = { uint8_t(a0), uint8_t(a1) };
                                    for (uint32_t i = 2; i < 8; i++)
                                        palette[i] = a0 > a1 ? uint8_t(((8 - i) * a0 + (i - 1) * a1) / 7)
                                            : i < 6         ? uint8_t(((6 - i) * a0 + (i - 1) * a1) / 5)
                                                            : uint8_t(i == 6 ? 0 : 255);
                                    uint64_t bits = 0;
                                    for (uint32_t i = 0; i < 6; i++)
                                        bits |= uint64_t(block[2 + i]) << (8 * i);
                                    for (uint32_t i = 0; i < 16; i++)
                                        alpha[i] = palette[(bits >> (3 * i)) & 7];
                                    block += 8;
                                }
                                uint32_t c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
                                uint8_t colors[4][4];
                                expand565(c0, colors[0]);
                                expand565(c1, colors[1]);
                                bool fourColors = bc3 || c0 > c1;
                                for (uint32_t k = 0; k < 3; k++)
                                {
                                    colors[2][k] = fourColors ? uint8_t((2 * colors[0][k] + colors[1][k]) / 3) : uint8_t((colors[0][k] + colors[1][k]) / 2);
                                    colors[3][k] = fourColors ? uint8_t((colors[0][k] + 2 * colors[1][k]) / 3) : 0;
                                }
                                colors[0][3] = colors[1][3] = colors[2][3] = 255;
                                colors[3][3] = fourColors ? 255 : 0;
                                uint32_t indices = block[4] | (block[5] << 8) | (block[6] << 16) | (uint32_t(block[7]) << 24);
                                for (uint32_t i = 0; i < 16; i++)
                                {
                                    const uint8_t* color = colors[(indices >> (2 * i)) & 3];
                                    uint8_t* texel = &expanded[((size_t(by) * 4 + i / 4) * decoded.rowTexels + bx * 4 + i % 4) * 4];
                                    texel[0] = color[0], texel[1] = color[1], texel[2] = color[2];
                                    texel[3] = bc3 ? alpha[i] : color[3];
                                }
                            }
                        rgba = expanded.data();
                    }
                    else if (decoded.format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
                        return;
                    SDL_Surface* surface = SDL_CreateSurfaceFrom(int(decoded.width), int(decoded.height), SDL_PIXELFORMAT_ABGR8888,
                        const_cast<uint8_t*>(rgba), int(decoded.rowTexels * 4));
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
            cached.mipBytes = 0;
            if (constant.mipMin == 0 && constant.mipAddress != 0 && !mips.empty() && fetch.address == constant.address &&
                fetch.width == constant.width)
            {
                // A full-size entry: what KeptBase may stand in for this
                // texture while it's demoted, identified by its mips.
                const texture::FetchConstant& mip = mips[0];
                cached.mipAddress = mip.address;
                cached.mipBytes = texture::FaceStride(mip) * (faces - 1) + texture::GuestSize(mip);
                const uint8_t* mipMemory = GuestPtr<uint8_t>(PhysicalToGuest(mip.address));
                cached.mipPageHash = PageHash(mipMemory, cached.mipBytes);
                cached.mipContentHash = ContentHash(mipMemory, cached.mipBytes);
                g_fullByMips[FullKey(constant, layers)] = entryKey;
            }
            cached.keptCheckedFrame = cached.keptFullCheckedFrame = kNever;
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
        // frontKey: Hash of the fetch constant's raw dwords and layers
        // (g_frontTextures).
        bool BindMemoryTexture(const texture::FetchConstant& fetch, bool layers, uint64_t frontKey, SDL_GPUTexture*& texture,
            SDL_GPUSampler*& sampler, uint32_t* constants)
        {
            SDL_GPUTexture* decoded = MemoryTexture(fetch, layers, frontKey);
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
            uint32_t words[kTextureFetchDwords];
            for (uint32_t i = 0; i < kTextureFetchDwords; i++)
                words[i] = ReadRegister(kRegShaderConstantFetch + binding.fetchConstant * kTextureFetchDwords + i);
            texture::FetchConstant fetch = texture::ParseFetchConstant(words);
            // H3_LOG_TEXTURE_DECODES also counts, per frame, the textures
            // bound without their base level (mip_min_level above 0: not
            // resident, streaming in or moved), to time a streaming dip.
            static const bool logStandIns = EnvFlag("H3_LOG_TEXTURE_DECODES");
            if (logStandIns)
            {
                static std::set<uint32_t> standIns;
                static uint32_t frame = 0;
                if (frame != g_swaps)
                {
                    if (!standIns.empty())
                        fprintf(stderr, "[tex] f%u: %zu textures bound without their base level\n", frame, standIns.size());
                    standIns.clear();
                    frame = g_swaps;
                }
                if (fetch.valid && fetch.mipMin > 0 && fetch.mipAddress != 0)
                    standIns.insert(fetch.address);
            }
            // g_frontTextures' keys: 2D and cube binds, and stacked layers.
            uint64_t frontKey = Hash(words, sizeof(words), kFrontSeed);
            uint64_t layersKey = Hash(words, sizeof(words), kFrontLayersSeed);
            uint32_t* constants = system.textures[binding.fetchConstant & 31];
            bool usable = fetch.valid && !g_noTextures;
            switch (binding.dimension)
            {
            case ucode::TextureDimension::Cube:
                if (usable && fetch.dimension == kDimensionCube && !FindResolved(fetch.address))
                    BindMemoryTexture(fetch, false, frontKey, texture, sampler, constants);
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
                    if (BindMemoryTexture(fetch, true, layersKey, texture, sampler, constants) && binding.array)
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
                    // Depth read as 8_8_8_8 bytes is the exception: its view
                    // (ResolvedView, CopyMode::DepthBytes) is built in
                    // memory's component order (stencil, low, mid, high), so
                    // the fetch's swizzle applies as for memory textures. The
                    // sniper rifle's scope screen reads the stencil byte
                    // through zyxw as .z (characters drawn in its heat
                    // color); unswizzled, .z was the depth's middle byte,
                    // nonzero nearly everywhere, and the whole screen filled
                    // with the heat color. H3_NO_DEPTH_BYTES_SWIZZLE=1: the
                    // old behaviour, to compare.
                    static const bool noDepthBytesSwizzle = EnvFlag("H3_NO_DEPTH_BYTES_SWIZZLE");
                    constexpr uint32_t kFormat8888 = 6, kFormat8888A = 14;  // xenos::TextureFormat
                    bool depthBytes = fetch.valid && resolved->hostFormat == SDL_GPU_TEXTUREFORMAT_R32_FLOAT &&
                        (fetch.format == kFormat8888 || fetch.format == kFormat8888A);
                    if (depthBytes && !noDepthBytesSwizzle)
                    {
                        constants[0] = fetch.swizzle;
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
                    if (!BindMemoryTexture(fetch, false, frontKey, texture, sampler, constants))
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
                g_frontTextures.clear();
                g_keptFront.clear();
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
            g_frontTextures.clear();
            g_keptFront.clear();
        }
    }
}
