#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <vector>

// Xenos textures in guest memory: the texture fetch constant, and decoding
// the base level into a host texture's layout (untiled, byte-swapped,
// converted where the host has no matching format).
namespace h3::gpu::texture
{
    // Texture fetch constant (6 dwords), as Xenia's xe_gpu_texture_fetch_t.
    struct FetchConstant
    {
        bool valid = false;        // type 2 (texture)
        uint32_t address = 0;      // physical, base level
        uint32_t format = 0;       // xenos::TextureFormat
        uint32_t endian = 0;       // 0 none, 1 8in16, 2 8in32, 3 16in32
        uint32_t width = 0, height = 0;
        uint32_t depth = 1;        // 3D depth, or layers of a stacked (2D array) texture
        bool stacked = false;      // 2D dimension holding several layers (Halo's lightmaps)
        uint32_t pitch = 0;        // texels per row
        bool tiled = false;
        bool packedMips = false;
        uint32_t mipAddress = 0;   // physical, level 1 on
        uint32_t mipMin = 0, mipMax = 0;  // levels the sampler may use
        uint32_t dimension = 0;    // 0 1D, 1 2D, 2 3D/stacked, 3 cube
        uint32_t clampX = 0, clampY = 0, clampZ = 0;
        uint32_t magFilter = 0, minFilter = 0;
        uint32_t swizzle = 0;      // 3 bits per component: 0-3 = XYZW, 4 = 0, 5 = 1
        uint32_t signs = 0;        // 2 bits per component: 0 unsigned, 1 signed, 2 biased, 3 gamma
        int32_t expAdjust = 0;
        uint32_t mipFilter = 0;    // 0 point, 1 linear, 2 base map only
        uint32_t anisoFilter = 0;  // 0 off, n = 2^(n-1) samples
        // A mip level inside a packed mip tail: its block offset in the
        // tail's tile (set by MipLevels; otherwise Layout derives it).
        bool explicitOffset = false;
        uint32_t offsetX = 0, offsetY = 0;
    };

    FetchConstant ParseFetchConstant(const uint32_t words[6]);

    // XGAddress2DTiledOffset: element index of block (x, y) in a tiled
    // surface pitch blocks wide (a multiple of 32).
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t blockBytes);

    // The base level of a 2D texture, ready to upload.
    struct Decoded
    {
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        uint32_t width = 0, height = 0;  // host texture size (whole blocks for compressed formats)
        uint32_t rowTexels = 0;          // upload row length in texels
        std::vector<uint8_t> data;
    };

    // Guest bytes the base level covers, for change detection; 0 if the
    // format isn't supported.
    uint32_t GuestSize(const FetchConstant& fetch);

    // Distance between the faces of a cube map, or the layers of a stacked
    // texture (each a 2D level, 4 KB aligned).
    inline uint32_t FaceStride(const FetchConstant& fetch) { return (GuestSize(fetch) + 4095) & ~4095u; }

    bool Decode(const FetchConstant& fetch, const uint8_t* guest, Decoded& out);

    // The fetch constant's swizzle for the host texture: formats with one
    // or two components come back as RRRR / RGGG (as on the console, per
    // Xenia), where the host texture has zeros.
    uint32_t HostSwizzle(const FetchConstant& fetch);

    // How the shader applies the fetch constant's sign modes to the host
    // texture's values: 0 = not at all (float formats, which have no sign
    // modes), 1 = 8-bit components, 2 = 16-bit components.
    uint32_t SignPrecision(const FetchConstant& fetch);

    // Fetch constants for mip levels 1 on of a 2D texture (each decodes
    // like a base level), as many as the fetch constant and the dimensions
    // allow; empty if it has none.
    std::vector<FetchConstant> MipLevels(const FetchConstant& fetch);

    // The largest level the sampler may use, as a base level of its own:
    // the fetch constant itself, or (when the sampler is limited to smaller
    // levels, as for textures whose full size is still streaming in) that
    // level's address and size in the mip chain.
    FetchConstant LargestUsableLevel(const FetchConstant& fetch);
}
