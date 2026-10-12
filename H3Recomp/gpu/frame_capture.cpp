// H3_GPU_CAPTURE: a text log of one frame's GPU work (frame_capture.h).

#include "gpu/frame_capture.h"
#include "config/env.h"

#include "gpu/command_processor.h"
#include "gpu/registers.h"
#include "gpu/texture.h"
#include "memory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace h3::gpu::capture
{
    namespace
    {
        // The swap number H3_GPU_CAPTURE names (0: capture off).
        uint32_t TargetFrame()
        {
            static uint32_t frame = []
            {
                const char* value = EnvString("H3_GPU_CAPTURE");
                return value ? uint32_t(strtoul(value, nullptr, 0)) : 0u;
            }();
            return frame;
        }

        uint32_t g_frame = 0;  // swaps seen so far
        FILE* g_out = nullptr;
        uint32_t g_draws = 0;
        uint32_t g_vertexShader = 0, g_pixelShader = 0;

        // The capture file while the target frame runs, else null.
        FILE* Out()
        {
            if (TargetFrame() == 0 || g_frame + 1 != TargetFrame())
                return nullptr;
            if (!g_out)
            {
                g_out = fopen("gpu_capture.txt", "w");
                if (g_out)
                    fprintf(g_out, "frame %u\n", TargetFrame());
            }
            return g_out;
        }

        // A register's bits as a float.
        float RegFloat(uint32_t index)
        {
            uint32_t bits = ReadRegister(index);
            float value;
            memcpy(&value, &bits, sizeof(value));
            return value;
        }

        // Name of an RB_MODECONTROL EDRAM mode, for the log.
        const char* EdramModeName(uint32_t mode)
        {
            switch (mode)
            {
            case kEdramModeNone: return "none";
            case kEdramModeColorDepth: return "color+depth";
            case kEdramModeDepthOnly: return "depth-only";
            case kEdramModeCopy: return "copy";
            default: return "?";
            }
        }
    }

    void OnShaderLoad(uint32_t type, uint32_t guestAddress, uint32_t dwords)
    {
        (type == 0 ? g_vertexShader : g_pixelShader) = guestAddress;
        FILE* out = Out();
        if (!out)
            return;

        // The microcode itself (big-endian dwords, as in guest memory), once
        // per distinct shader, for working on the shader translator offline.
        if (guestAddress == 0 || dwords == 0)
            return;
        const uint8_t* code = GuestPtr<uint8_t>(guestAddress);
        uint64_t hash = 0xCBF29CE484222325ull;  // FNV-1a
        for (uint32_t i = 0; i < dwords * 4; i++)
            hash = (hash ^ code[i]) * 0x100000001B3ull;
        std::filesystem::create_directories("gpu_shaders");
        char path[64];
        snprintf(path, sizeof(path), "gpu_shaders/%s_%016llx.bin", type == 0 ? "vs" : "ps", (unsigned long long)hash);
        fprintf(out, "  %s shader at %08X, %u dwords (%s)\n", type == 0 ? "vertex" : "pixel", guestAddress, dwords, path);
        if (!std::filesystem::exists(path))
            if (FILE* file = fopen(path, "wb"))
            {
                fwrite(code, 4, dwords, file);
                fclose(file);
            }
    }

    void OnDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexWords)
    {
        FILE* out = Out();
        if (!out)
            return;

        uint32_t mode = ReadRegister(kRegRbModeControl) & 7;
        uint32_t surfaceInfo = ReadRegister(kRegRbSurfaceInfo);
        uint32_t tl = ReadRegister(kRegPaScWindowScissorTl), br = ReadRegister(kRegPaScWindowScissorBr);
        fprintf(out, "draw #%u prim %u src %u indices %u index %08X+%u edram %s pitch %u msaa %u scissor %u,%u-%u,%u\n",
            g_draws++, initiator & 0x3F, (initiator >> 6) & 3, initiator >> 16, indexBase, indexWords,
            EdramModeName(mode), surfaceInfo & 0x3FFF, (surfaceInfo >> 16) & 3, tl & 0x7FFF, (tl >> 16) & 0x7FFF,
            br & 0x7FFF, (br >> 16) & 0x7FFF);

        if (mode == kEdramModeCopy)
        {
            uint32_t control = ReadRegister(kRegRbCopyControl);
            uint32_t destInfo = ReadRegister(kRegRbCopyDestInfo);
            uint32_t destPitch = ReadRegister(kRegRbCopyDestPitch);
            fprintf(out,
                "  resolve src %u (color info %08X depth info %08X) clear color %u depth %u command %u -> %08X pitch %u "
                "height %u format %u endian %u | clear %08X%08X depth clear %08X\n",
                control & 7, ReadRegister(kRegRbColorInfo), ReadRegister(kRegRbDepthInfo), (control >> 8) & 1,
                (control >> 9) & 1, (control >> 20) & 3, ReadRegister(kRegRbCopyDestBase), destPitch & 0x3FFF,
                (destPitch >> 16) & 0x3FFF, (destInfo >> 7) & 0x3F, destInfo & 7, ReadRegister(kRegRbColorClear),
                ReadRegister(kRegRbColorClearLo), ReadRegister(kRegRbDepthClear));
            return;
        }

        // Small auto-indexed draws (UI quads): vertex fetch constant 95's
        // data as floats (byte-swapped).
        if (((initiator >> 6) & 3) == 2 && (initiator >> 16) <= 6)
        {
            uint32_t w0 = ReadRegister(kRegShaderConstantFetch + 95 * 2), w1 = ReadRegister(kRegShaderConstantFetch + 95 * 2 + 1);
            uint32_t dwords = std::min((w1 >> 2) & 0xFFFFFF, 32u);
            if ((w0 & 3) == 3 && (w0 & ~3u) != 0)
            {
                const uint32_t* data = GuestPtr<uint32_t>(PhysicalToGuest(w0 & ~3u));
                fprintf(out, "  vertices");
                for (uint32_t i = 0; i < dwords; i++)
                {
                    uint32_t raw = __builtin_bswap32(data[i]);
                    float f;
                    memcpy(&f, &raw, 4);
                    if (f == f && std::fabs(f) < 1e6f)
                        fprintf(out, " %g", f);
                    else
                        fprintf(out, " #%08X", raw);
                }
                fprintf(out, "\n");
            }
        }
        fprintf(out, "  vs %08X ps %08X program cntl %08X color info %08X depth info %08X modecontrol %08X\n",
            g_vertexShader, g_pixelShader, ReadRegister(kRegSqProgramCntl), ReadRegister(kRegRbColorInfo),
            ReadRegister(kRegRbDepthInfo), ReadRegister(kRegRbModeControl));
        fprintf(out, "  color1 %08X color2 %08X color3 %08X surface %08X\n", ReadRegister(kRegRbColor1Info), ReadRegister(kRegRbColor2Info),
            ReadRegister(kRegRbColor3Info), ReadRegister(kRegRbSurfaceInfo));
        fprintf(out, "  window offset %08X mode cntl %08X vte cntl %08X depthcontrol %08X colormask %08X blend %08X stencil %08X\n",
            ReadRegister(kRegPaScWindowOffset), ReadRegister(0x2205), ReadRegister(0x2206), ReadRegister(0x2200),
            ReadRegister(0x2104), ReadRegister(0x2201), ReadRegister(0x210D));  // 0x210D: RB_STENCILREFMASK
        fprintf(out, "  viewport scale %g,%g,%g offset %g,%g,%g\n", RegFloat(kRegPaClVportXScale),
            RegFloat(kRegPaClVportYScale), RegFloat(kRegPaClVportZScale), RegFloat(kRegPaClVportXOffset),
            RegFloat(kRegPaClVportYOffset), RegFloat(kRegPaClVportZOffset));
        // Pixel shader constants c0-c7 and c255 (ALU constants 256 on).
        fprintf(out, "  ps constants");
        for (uint32_t c : { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 255u })
            fprintf(out, " c%u(%g %g %g %g)", c, RegFloat(kRegShaderConstantAlu + (256 + c) * 4), RegFloat(kRegShaderConstantAlu + (256 + c) * 4 + 1),
                RegFloat(kRegShaderConstantAlu + (256 + c) * 4 + 2), RegFloat(kRegShaderConstantAlu + (256 + c) * 4 + 3));
        fprintf(out, "\n");
        // Texture fetch constants of type texture:
        // slot:base:format:signs:exp:size:dimension (t = tiled, p = packed
        // mips):mip levels@mip address:swizzle (x/y/z/w, 0, 1).
        fprintf(out, "  textures");
        for (uint32_t t = 0; t < 32; t++)
        {
            uint32_t w0 = ReadRegister(kRegShaderConstantFetch + t * 6), w1 = ReadRegister(kRegShaderConstantFetch + t * 6 + 1);
            uint32_t w3 = ReadRegister(kRegShaderConstantFetch + t * 6 + 3);
            if ((w0 & 3) != 2 || (w1 & 0xFFFFF000) == 0)
                continue;
            uint32_t words[6];
            for (uint32_t i = 0; i < 6; i++)
                words[i] = ReadRegister(kRegShaderConstantFetch + t * 6 + i);
            texture::FetchConstant fetch = texture::ParseFetchConstant(words);
            char swizzle[5] = {};
            for (uint32_t i = 0; i < 4; i++)
                swizzle[i] = "xyzw01??"[(fetch.swizzle >> (3 * i)) & 7];
            fprintf(out, " %u:%08X:%u:%02X:%d:%ux%u:d%u%s%s:m%u-%u@%08X:%s", t, w1 & 0xFFFFF000, w1 & 0x3F, (w0 >> 2) & 0xFF,
                int32_t(w3 << 13) >> 26, fetch.width, fetch.height, fetch.dimension, fetch.tiled ? "t" : "", fetch.packedMips ? "p" : "",
                fetch.mipMin, fetch.mipMax, fetch.mipAddress, swizzle);
        }
        fprintf(out, "\n");
    }

    void OnBinState(uint64_t mask, uint64_t select)
    {
        if (FILE* out = Out())
            fprintf(out, "  bin mask %016llX select %016llX\n", (unsigned long long)mask, (unsigned long long)select);
    }

    void OnPredicatedSkip(uint32_t opcode)
    {
        if (FILE* out = Out())
            fprintf(out, "  skipped predicated opcode %02X\n", opcode);
    }

    void OnSwap(uint32_t frame)
    {
        g_frame = frame;
        if (g_out && g_frame >= TargetFrame())
        {
            fprintf(g_out, "swap (%u draws)\n", g_draws);
            fclose(g_out);
            g_out = nullptr;
            fprintf(stderr, "[gpu] wrote gpu_capture.txt (frame %u)\n", TargetFrame());
        }
    }
}
