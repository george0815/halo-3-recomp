#pragma once

#include <cstdint>

// Xenos GPU register indices used for rendering (dword index into the
// register file). Reference: Xenia's src/xenia/gpu/register_table.inc,
// registers.h and xenos.h.
namespace h3::gpu
{
    // Render backend: EDRAM surfaces and resolves.
    constexpr uint32_t kRegRbSurfaceInfo = 0x2000;  // pitch (pixels) : 14, msaa : 2 @16
    constexpr uint32_t kRegRbColorInfo = 0x2001;    // base (tiles) : 12, format : 4 @16, exp bias : 6 @20
    constexpr uint32_t kRegRbDepthInfo = 0x2002;    // base (tiles) : 12, format : 1 @16
    constexpr uint32_t kRegRbColor1Info = 0x2003;
    constexpr uint32_t kRegRbColor2Info = 0x2004;
    constexpr uint32_t kRegRbColor3Info = 0x2005;
    constexpr uint32_t kRegRbModeControl = 0x2208;  // edram mode : 3
    constexpr uint32_t kRegRbCopyControl = 0x2318;  // src : 3, color clear @8, depth clear @9, command : 2 @20
    constexpr uint32_t kRegRbCopyDestBase = 0x2319;
    constexpr uint32_t kRegRbCopyDestPitch = 0x231A;  // pitch : 14, height : 14 @16
    constexpr uint32_t kRegRbCopyDestInfo = 0x231B;   // endian : 3, format : 6 @7, number : 3 @13
    constexpr uint32_t kRegRbDepthClear = 0x231D;
    constexpr uint32_t kRegRbColorClear = 0x231E;
    constexpr uint32_t kRegRbColorClearLo = 0x231F;

    // RB_MODECONTROL edram_mode values.
    constexpr uint32_t kEdramModeNone = 0;
    constexpr uint32_t kEdramModeColorDepth = 4;
    constexpr uint32_t kEdramModeDepthOnly = 5;
    constexpr uint32_t kEdramModeCopy = 6;

    // Setup and clipping.
    constexpr uint32_t kRegPaScWindowOffset = 0x2080;
    constexpr uint32_t kRegPaScWindowScissorTl = 0x2081;
    constexpr uint32_t kRegPaScWindowScissorBr = 0x2082;
    constexpr uint32_t kRegPaClVportXScale = 0x210F;
    constexpr uint32_t kRegPaClVportXOffset = 0x2110;
    constexpr uint32_t kRegPaClVportYScale = 0x2111;
    constexpr uint32_t kRegPaClVportYOffset = 0x2112;
    constexpr uint32_t kRegPaClVportZScale = 0x2113;
    constexpr uint32_t kRegPaClVportZOffset = 0x2114;

    // Shaders and vertex fetch.
    constexpr uint32_t kRegSqProgramCntl = 0x2180;
    constexpr uint32_t kRegVgtDmaBase = 0x21FA;
    constexpr uint32_t kRegVgtDmaSize = 0x21FB;
    constexpr uint32_t kRegVgtDrawInitiator = 0x21FC;

    // Shader constants, written through SET_CONSTANT / LOAD_ALU_CONSTANT.
    constexpr uint32_t kRegShaderConstantAlu = 0x4000;    // 512 float4 (VS 0-255, PS 256-511)
    constexpr uint32_t kRegShaderConstantFetch = 0x4800;  // 32 texture fetch constants of 6 dwords
    constexpr uint32_t kRegShaderConstantBool = 0x4900;
    constexpr uint32_t kRegShaderConstantLoop = 0x4908;
    constexpr uint32_t kRegContextRegisterBase = 0x2000;  // SET_CONSTANT type 4
}
