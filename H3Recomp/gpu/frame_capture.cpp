#include "gpu/frame_capture.h"

#include "gpu/command_processor.h"
#include "gpu/registers.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace h3::gpu::capture
{
    namespace
    {
        uint32_t TargetFrame()
        {
            static uint32_t frame = []
            {
                const char* value = getenv("H3_GPU_CAPTURE");
                return value ? uint32_t(strtoul(value, nullptr, 0)) : 0u;
            }();
            return frame;
        }

        uint32_t g_frame = 0;  // swaps seen so far
        FILE* g_out = nullptr;
        uint32_t g_draws = 0;
        uint32_t g_vertexShader = 0, g_pixelShader = 0;

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

        float RegFloat(uint32_t index)
        {
            uint32_t bits = ReadRegister(index);
            float value;
            memcpy(&value, &bits, sizeof(value));
            return value;
        }

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
        if (FILE* out = Out())
            fprintf(out, "  %s shader at %08X, %u dwords\n", type == 0 ? "vertex" : "pixel", guestAddress, dwords);
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

        fprintf(out, "  vs %08X ps %08X program cntl %08X color info %08X depth info %08X modecontrol %08X\n",
            g_vertexShader, g_pixelShader, ReadRegister(kRegSqProgramCntl), ReadRegister(kRegRbColorInfo),
            ReadRegister(kRegRbDepthInfo), ReadRegister(kRegRbModeControl));
        fprintf(out, "  viewport scale %g,%g,%g offset %g,%g,%g\n", RegFloat(kRegPaClVportXScale),
            RegFloat(kRegPaClVportYScale), RegFloat(kRegPaClVportZScale), RegFloat(kRegPaClVportXOffset),
            RegFloat(kRegPaClVportYOffset), RegFloat(kRegPaClVportZOffset));
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
