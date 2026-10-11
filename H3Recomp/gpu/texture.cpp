#include "gpu/texture.h"

#include <algorithm>
#include <cstring>

namespace h3::gpu::texture
{
    namespace
    {
        // How a guest block becomes host data.
        enum class Convert
        {
            Copy,         // same layout as the host format
            R5G6B5,       // 16-bit packed -> RGBA8 (X in the low bits)
            R6G5B5,
            R5G5B5A1,
            R4G4B4A4,
            R11G11B10,    // unorm, X in the low 11 bits -> RGBA16
            R10G11B11,
            Dxt3a,        // 4-bit alpha block -> 4x4 R8
            Ctx1,         // two RG8 endpoints + 2-bit indices -> 4x4 RG8
        };

        struct Format
        {
            uint32_t blockBytes = 0;  // guest bytes per block (or texel)
            uint32_t blockSize = 1;   // texels per block side: 1, or 4 for compressed
            SDL_GPUTextureFormat host = SDL_GPU_TEXTUREFORMAT_INVALID;
            Convert convert = Convert::Copy;
        };

        Format FormatFor(uint32_t format)
        {
            switch (format)
            {
            case 2:   // k_8
            case 8:   // k_8_A
            case 9:   // k_8_B
                return { 1, 1, SDL_GPU_TEXTUREFORMAT_R8_UNORM };
            case 3: return { 2, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, Convert::R5G5B5A1 };  // k_1_5_5_5
            case 4: return { 2, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, Convert::R5G6B5 };    // k_5_6_5
            case 5: return { 2, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, Convert::R6G5B5 };    // k_6_5_5
            case 6:   // k_8_8_8_8
            case 14:  // k_8_8_8_8_A
            case 50:  // k_8_8_8_8_AS_16_16_16_16
                return { 4, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM };
            case 7:   // k_2_10_10_10
            case 54:  // k_2_10_10_10_AS_16_16_16_16
                return { 4, 1, SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM };
            case 10: return { 2, 1, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM };  // k_8_8
            case 15: return { 2, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, Convert::R4G4B4A4 };  // k_4_4_4_4
            case 16:  // k_10_11_11
            case 55:
                return { 4, 1, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM, Convert::R10G11B11 };
            case 17:  // k_11_11_10
            case 56:
                return { 4, 1, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM, Convert::R11G11B10 };
            case 18:  // k_DXT1
            case 51:
                return { 8, 4, SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM };
            case 19:  // k_DXT2_3
            case 52:
                return { 16, 4, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM };
            case 20:  // k_DXT4_5
            case 53:
                return { 16, 4, SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM };
            case 24: return { 2, 1, SDL_GPU_TEXTUREFORMAT_R16_UNORM };            // k_16
            case 25: return { 4, 1, SDL_GPU_TEXTUREFORMAT_R16G16_UNORM };         // k_16_16
            case 26: return { 8, 1, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM };   // k_16_16_16_16
            case 27:  // k_16_EXPAND (16-bit floats, as Xenia treats them)
            case 30:  // k_16_FLOAT
                return { 2, 1, SDL_GPU_TEXTUREFORMAT_R16_FLOAT };
            case 28: return { 4, 1, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT };         // k_16_16_EXPAND
            case 29: return { 8, 1, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT };   // k_16_16_16_16_EXPAND
            case 31: return { 4, 1, SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT };         // k_16_16_FLOAT
            case 32: return { 8, 1, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT };   // k_16_16_16_16_FLOAT
            case 36: return { 4, 1, SDL_GPU_TEXTUREFORMAT_R32_FLOAT };            // k_32_FLOAT
            case 37: return { 8, 1, SDL_GPU_TEXTUREFORMAT_R32G32_FLOAT };         // k_32_32_FLOAT
            case 38: return { 16, 1, SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT };  // k_32_32_32_32_FLOAT
            case 49: return { 16, 4, SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM };        // k_DXN
            case 58: return { 8, 4, SDL_GPU_TEXTUREFORMAT_R8_UNORM, Convert::Dxt3a };  // k_DXT3A
            case 59: return { 8, 4, SDL_GPU_TEXTUREFORMAT_BC4_R_UNORM };          // k_DXT5A
            case 60: return { 8, 4, SDL_GPU_TEXTUREFORMAT_R8G8_UNORM, Convert::Ctx1 };  // k_CTX1
            default: return {};
            }
        }

        // Host bytes per guest block after conversion.
        uint32_t HostBlockBytes(const Format& f)
        {
            switch (f.convert)
            {
            case Convert::Copy: return f.blockBytes;
            case Convert::R11G11B10:
            case Convert::R10G11B11: return 8;
            case Convert::Dxt3a: return 16;
            case Convert::Ctx1: return 32;
            default: return 4;
            }
        }

        uint32_t Log2Ceil(uint32_t value)
        {
            uint32_t log2 = 0;
            while ((1u << log2) < value)
                log2++;
            return log2;
        }

        void Swap(uint8_t* data, uint32_t bytes, uint32_t endian)
        {
            switch (endian)
            {
            case 1:  // 8in16
                for (uint32_t i = 0; i + 1 < bytes; i += 2)
                    std::swap(data[i], data[i + 1]);
                break;
            case 2:  // 8in32
                for (uint32_t i = 0; i + 3 < bytes; i += 4)
                {
                    std::swap(data[i], data[i + 3]);
                    std::swap(data[i + 1], data[i + 2]);
                }
                break;
            case 3:  // 16in32
                for (uint32_t i = 0; i + 3 < bytes; i += 4)
                {
                    std::swap(data[i], data[i + 2]);
                    std::swap(data[i + 1], data[i + 3]);
                }
                break;
            default:
                break;
            }
        }

        uint8_t Expand(uint32_t value, uint32_t bits) { return uint8_t(value * 255 / ((1u << bits) - 1)); }
        uint16_t Expand16(uint32_t value, uint32_t bits) { return uint16_t(value * 65535ull / ((1u << bits) - 1)); }

        // Converts one guest block (already byte-swapped) into host texels.
        // Uncompressed conversions write one texel; block conversions write a
        // 4x4 block row by row (rowBytes apart).
        void ConvertBlock(Convert convert, const uint8_t* src, uint8_t* dst, uint32_t rowBytes)
        {
            uint32_t v16 = src[0] | (src[1] << 8);
            uint32_t v32 = v16 | (uint32_t(src[2]) << 16) | (uint32_t(src[3]) << 24);
            switch (convert)
            {
            case Convert::R5G6B5:
                dst[0] = Expand(v16 & 31, 5), dst[1] = Expand((v16 >> 5) & 63, 6), dst[2] = Expand(v16 >> 11, 5), dst[3] = 255;
                break;
            case Convert::R6G5B5:
                dst[0] = Expand(v16 & 63, 6), dst[1] = Expand((v16 >> 6) & 31, 5), dst[2] = Expand(v16 >> 11, 5), dst[3] = 255;
                break;
            case Convert::R5G5B5A1:
                dst[0] = Expand(v16 & 31, 5), dst[1] = Expand((v16 >> 5) & 31, 5), dst[2] = Expand((v16 >> 10) & 31, 5);
                dst[3] = (v16 >> 15) ? 255 : 0;
                break;
            case Convert::R4G4B4A4:
                for (int c = 0; c < 4; c++)
                    dst[c] = Expand((v16 >> (4 * c)) & 15, 4);
                break;
            case Convert::R11G11B10:
            case Convert::R10G11B11:
            {
                uint32_t bits[3] = { 11, 11, 10 };
                if (convert == Convert::R10G11B11)
                    bits[0] = 10, bits[2] = 11;
                uint16_t out[4];
                uint32_t shift = 0;
                for (int c = 0; c < 3; c++)
                {
                    out[c] = Expand16((v32 >> shift) & ((1u << bits[c]) - 1), bits[c]);
                    shift += bits[c];
                }
                out[3] = 65535;
                memcpy(dst, out, 8);
                break;
            }
            case Convert::Dxt3a:
                // 16 4-bit alphas, row by row, low nibble first.
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                    {
                        uint32_t i = y * 4 + x;
                        dst[y * rowBytes + x] = Expand((src[i >> 1] >> ((i & 1) * 4)) & 15, 4);
                    }
                break;
            case Convert::Ctx1:
            {
                // Endpoints (R, G) x2, then 2 bits per texel, as in Xenia.
                uint8_t r[4] = { src[0], src[2] }, g[4] = { src[1], src[3] };
                r[2] = uint8_t((2 * r[0] + r[1]) / 3), g[2] = uint8_t((2 * g[0] + g[1]) / 3);
                r[3] = uint8_t((r[0] + 2 * r[1]) / 3), g[3] = uint8_t((g[0] + 2 * g[1]) / 3);
                uint32_t indices = src[4] | (src[5] << 8) | (src[6] << 16) | (uint32_t(src[7]) << 24);
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                    {
                        uint32_t index = (indices >> (2 * (y * 4 + x))) & 3;
                        dst[y * rowBytes + x * 2] = r[index];
                        dst[y * rowBytes + x * 2 + 1] = g[index];
                    }
                break;
            }
            case Convert::Copy:
                break;
            }
        }

        bool IsBlockConversion(Convert convert) { return convert == Convert::Dxt3a || convert == Convert::Ctx1; }

        // Base level placement: blocks per row in memory, and where the level
        // starts (packed small textures share a 32x32 tile with their mips).
        void Layout(const FetchConstant& fetch, const Format& f, uint32_t& pitchBlocks, uint32_t& offsetX, uint32_t& offsetY)
        {
            pitchBlocks = std::max(fetch.pitch, fetch.width) / f.blockSize;
            if (fetch.tiled)
                pitchBlocks = (pitchBlocks + 31) & ~31u;
            offsetX = offsetY = 0;
            if (fetch.explicitOffset)
            {
                offsetX = fetch.offsetX;
                offsetY = fetch.offsetY;
                return;
            }
            uint32_t log2w = Log2Ceil(fetch.width), log2h = Log2Ceil(fetch.height);
            if (fetch.packedMips && std::min(log2w, log2h) <= 4)
            {
                // Xenia's GetPackedTileOffset for the first packed level.
                if (log2w > log2h)
                    offsetY = 16 / f.blockSize;
                else
                    offsetX = 16 / f.blockSize;
            }
        }
    }

    // XGAddress2DTiledOffset: element index of block (x, y) in a tiled
    // surface pitch blocks wide (a multiple of 32).
    uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t blockBytes)
    {
        uint32_t logBpp = (blockBytes >> 2) + ((blockBytes >> 1) >> (blockBytes >> 2));
        uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (logBpp + 7);
        uint32_t micro = ((x & 7) + ((y & 6) << 2)) << logBpp;
        uint32_t offset = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + logBpp)) + ((y & 1) << 4);
        return (((offset & ~511u) << 3) + ((offset & 448) << 2) + (offset & 63) + ((y & 16) << 7) +
                   (((((y & 8) >> 2) + (x >> 3)) & 3) << 6)) >>
            logBpp;
    }

    FetchConstant ParseFetchConstant(const uint32_t words[6])
    {
        FetchConstant fetch;
        fetch.valid = (words[0] & 3) == 2;
        fetch.signs = (words[0] >> 2) & 0xFF;
        fetch.clampX = (words[0] >> 10) & 7;
        fetch.clampY = (words[0] >> 13) & 7;
        fetch.clampZ = (words[0] >> 16) & 7;
        fetch.pitch = ((words[0] >> 22) & 0x1FF) << 5;
        fetch.tiled = (words[0] >> 31) & 1;
        fetch.format = words[1] & 0x3F;
        fetch.endian = (words[1] >> 6) & 3;
        fetch.address = words[1] & 0xFFFFF000;
        fetch.stacked = (words[1] >> 10) & 1;
        fetch.dimension = (words[5] >> 9) & 3;
        fetch.packedMips = (words[5] >> 11) & 1;
        fetch.mipAddress = words[5] & 0xFFFFF000;
        fetch.mipMin = (words[4] >> 2) & 0xF;
        fetch.mipMax = (words[4] >> 6) & 0xF;
        switch (fetch.dimension)
        {
        case 0:
            fetch.width = (words[2] & 0xFFFFFF) + 1;
            fetch.height = 1;
            break;
        case 2:
            fetch.width = (words[2] & 0x7FF) + 1;
            fetch.height = ((words[2] >> 11) & 0x7FF) + 1;
            fetch.depth = ((words[2] >> 22) & 0x3FF) + 1;
            break;
        default:
            fetch.width = (words[2] & 0x1FFF) + 1;
            fetch.height = ((words[2] >> 13) & 0x1FFF) + 1;
            if (fetch.dimension == 1 && fetch.stacked)
                fetch.depth = ((words[2] >> 26) & 0x3F) + 1;
            break;
        }
        fetch.swizzle = (words[3] >> 1) & 0xFFF;
        fetch.expAdjust = int32_t(words[3] << 13) >> 26;
        fetch.magFilter = (words[3] >> 19) & 3;
        fetch.minFilter = (words[3] >> 21) & 3;
        fetch.mipFilter = (words[3] >> 23) & 3;
        fetch.anisoFilter = (words[3] >> 25) & 7;
        return fetch;
    }

    uint32_t GuestSize(const FetchConstant& fetch)
    {
        Format f = FormatFor(fetch.format);
        if (!f.blockBytes)
            return 0;
        uint32_t pitchBlocks, offsetX, offsetY;
        Layout(fetch, f, pitchBlocks, offsetX, offsetY);
        uint32_t rows = (fetch.height + f.blockSize - 1) / f.blockSize + offsetY;
        if (fetch.tiled)
            rows = (rows + 31) & ~31u;
        uint32_t rowBytes = pitchBlocks * f.blockBytes;
        if (!fetch.tiled)
            rowBytes = (rowBytes + 255) & ~255u;
        return rows * rowBytes;
    }

    bool Decode(const FetchConstant& fetch, const uint8_t* guest, Decoded& out)
    {
        Format f = FormatFor(fetch.format);
        if (!f.blockBytes || fetch.width == 0 || fetch.height == 0)
            return false;
        uint32_t pitchBlocks, offsetX, offsetY;
        Layout(fetch, f, pitchBlocks, offsetX, offsetY);
        uint32_t blocksX = (fetch.width + f.blockSize - 1) / f.blockSize;
        uint32_t blocksY = (fetch.height + f.blockSize - 1) / f.blockSize;
        uint32_t linearRowBytes = (pitchBlocks * f.blockBytes + 255) & ~255u;

        // Host layout: compressed formats stay in blocks (rows of blocks);
        // block conversions expand to texels.
        bool expand = IsBlockConversion(f.convert);
        uint32_t hostBlockBytes = HostBlockBytes(f);
        out.format = f.host;
        out.width = blocksX * f.blockSize;
        out.height = blocksY * f.blockSize;
        out.rowTexels = out.width;
        uint32_t hostRowBytes = expand ? out.width * (hostBlockBytes / 16) : blocksX * hostBlockBytes;
        uint32_t hostBlockRows = expand ? 4 : 1;
        out.data.assign(size_t(hostRowBytes) * blocksY * hostBlockRows, 0);

        uint8_t block[16];
        for (uint32_t y = 0; y < blocksY; y++)
        {
            for (uint32_t x = 0; x < blocksX; x++)
            {
                uint32_t gx = x + offsetX, gy = y + offsetY;
                size_t source = fetch.tiled ? size_t(TiledOffset(gx, gy, pitchBlocks, f.blockBytes)) * f.blockBytes
                                            : size_t(gy) * linearRowBytes + size_t(gx) * f.blockBytes;
                memcpy(block, guest + source, f.blockBytes);
                Swap(block, f.blockBytes, fetch.endian);
                uint8_t* dst = out.data.data() + size_t(y) * hostRowBytes * hostBlockRows;
                if (expand)
                    ConvertBlock(f.convert, block, dst + x * 4 * (hostBlockBytes / 16), hostRowBytes);
                else if (f.convert == Convert::Copy)
                    memcpy(dst + size_t(x) * hostBlockBytes, block, hostBlockBytes);
                else
                    ConvertBlock(f.convert, block, dst + size_t(x) * hostBlockBytes, 0);
            }
        }
        return true;
    }

    std::vector<FetchConstant> MipLevels(const FetchConstant& fetch)
    {
        std::vector<FetchConstant> levels;
        Format f = FormatFor(fetch.format);
        if (!f.blockBytes || fetch.dimension != 1 || fetch.stacked || fetch.mipAddress == 0 || fetch.mipMax == 0)
            return levels;
        uint32_t log2w = Log2Ceil(fetch.width), log2h = Log2Ceil(fetch.height);
        uint32_t maxLevel = std::min(fetch.mipMax, std::max(log2w, log2h));
        // Levels from packedLevel on share one tile (Xenia's
        // GetPackedMipLevel / GetPackedMipOffset).
        uint32_t log2Min = std::min(log2w, log2h);
        uint32_t packedLevel = fetch.packedMips ? (log2Min > 4 ? log2Min - 4 : 0) : ~0u;
        auto alignUp = [](uint32_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; };
        // Level l's storage: power-of-two dimensions, 32x32-block aligned,
        // linear rows 256-byte aligned, each level 4 KB aligned.
        auto stridePitch = [&](uint32_t level) { return std::max(1u, (1u << log2w) >> level); };
        auto strideBytes = [&](uint32_t level) {
            uint32_t pitchBlocks = alignUp(alignUp(stridePitch(level), f.blockSize) / f.blockSize, 32);
            uint32_t rowBytes = pitchBlocks * f.blockBytes;
            if (!fetch.tiled)
                rowBytes = alignUp(rowBytes, 256);
            uint32_t rows = alignUp(alignUp(std::max(1u, (1u << log2h) >> level), f.blockSize) / f.blockSize, 32);
            return alignUp(rowBytes * rows, 4096);
        };
        uint32_t address = fetch.mipAddress;
        for (uint32_t level = 1; level <= maxLevel; level++)
        {
            FetchConstant mip = fetch;
            mip.packedMips = false;
            mip.mipMin = mip.mipMax = 0;
            mip.width = std::max(1u, fetch.width >> level);
            mip.height = std::max(1u, fetch.height >> level);
            if (level >= packedLevel)
            {
                // The packed tail lives at the packed level's address with
                // that level's pitch; each level at its own block offset.
                uint32_t tailAddress = fetch.mipAddress;
                for (uint32_t l = 1; l < std::max(packedLevel, 1u); l++)
                    tailAddress += strideBytes(l);
                mip.address = tailAddress;
                mip.pitch = stridePitch(std::max(packedLevel, 1u));
                uint32_t baseLevel = log2Min > 4 ? log2Min - 4 : 0;
                uint32_t packedMip = level - baseLevel;
                uint32_t x = 0, y = 0;
                if (packedMip < 3)
                {
                    if (log2w > log2h)
                        y = 16 >> packedMip;
                    else
                        x = 16 >> packedMip;
                }
                else if (log2w > log2h)
                    x = (1u << (log2w - baseLevel)) >> (packedMip - 2);
                else
                    y = (1u << (log2h - baseLevel)) >> (packedMip - 2);
                mip.explicitOffset = true;
                mip.offsetX = x / f.blockSize;
                mip.offsetY = y / f.blockSize;
                // Tail levels need the tile's full 32-texel pitch.
                mip.pitch = std::max(mip.pitch, 32u);
            }
            else
            {
                mip.address = address;
                mip.pitch = stridePitch(level);
                address += strideBytes(level);
            }
            levels.push_back(mip);
        }
        return levels;
    }

    FetchConstant LargestUsableLevel(const FetchConstant& fetch)
    {
        if (fetch.mipMin == 0 || fetch.mipAddress == 0 || fetch.dimension != 1 || fetch.stacked)
            return fetch;
        // Levels 1 on start at the mip address, each with power-of-two
        // dimensions and 4 KB aligned. Levels of 16 texels or less share a
        // packed tile; stop above those.
        auto levelSize = [&](uint32_t size, uint32_t level) { return std::max(1u, (1u << Log2Ceil(size)) >> level); };
        uint32_t level = fetch.mipMin;
        while (level > 1 && fetch.packedMips && std::min(levelSize(fetch.width, level), levelSize(fetch.height, level)) <= 16)
            level--;
        if (fetch.packedMips && std::min(levelSize(fetch.width, level), levelSize(fetch.height, level)) <= 16)
            return fetch;

        FetchConstant result = fetch;
        result.packedMips = false;
        result.mipMin = 0;
        result.address = fetch.mipAddress;
        for (uint32_t l = 1; l <= level; l++)
        {
            result.width = result.pitch = levelSize(fetch.width, l);
            result.height = levelSize(fetch.height, l);
            if (l < level)
                result.address += (GuestSize(result) + 4095) & ~4095u;
        }
        return result;
    }

    uint32_t HostSwizzle(const FetchConstant& fetch)
    {
        uint32_t last;  // the host texture's last component
        switch (fetch.format)
        {
        case 2: case 8: case 9:     // k_8, k_8_A, k_8_B
        case 24: case 27: case 30:  // k_16, k_16_EXPAND, k_16_FLOAT
        case 36:                    // k_32_FLOAT
        case 58: case 59:           // k_DXT3A, k_DXT5A
            last = 0;
            break;
        case 10:                    // k_8_8
        case 25: case 28: case 31:  // k_16_16, k_16_16_EXPAND, k_16_16_FLOAT
        case 37:                    // k_32_32_FLOAT
        case 49: case 60:           // k_DXN, k_CTX1
            last = 1;
            break;
        default:
            return fetch.swizzle;
        }
        uint32_t swizzle = fetch.swizzle;
        for (uint32_t i = 0; i < 4; i++)
        {
            uint32_t component = (swizzle >> (3 * i)) & 7;
            if (component < 4 && component > last)
                swizzle = (swizzle & ~(7u << (3 * i))) | (last << (3 * i));
        }
        return swizzle;
    }

    uint32_t SignPrecision(const FetchConstant& fetch)
    {
        switch (fetch.format)
        {
        case 27: case 28: case 29:  // k_16_EXPAND, k_16_16_EXPAND, k_16_16_16_16_EXPAND
        case 30: case 31: case 32:  // k_16_FLOAT, k_16_16_FLOAT, k_16_16_16_16_FLOAT
        case 36: case 37: case 38:  // k_32_FLOAT, k_32_32_FLOAT, k_32_32_32_32_FLOAT
        case 57:                    // k_32_32_32_FLOAT
            return 0;
        case 24: case 25: case 26:  // k_16, k_16_16, k_16_16_16_16
            return 2;
        default:
            return 1;
        }
    }
}
