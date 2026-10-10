// xboxkrnl Vd* (video driver) exports, plus the XGet* video queries.
// Reference: Xenia's xboxkrnl_video.cc and xam_video.cc.
//
// The display is fixed at 1280x720, 60 Hz, widescreen, HD. The command
// stream goes to the stand-in command processor (gpu/command_processor.h).

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "../gpu/command_processor.h"
#include "../memory.h"

#include <bit>
#include <cstring>

using namespace h3;
using namespace h3::kernel;

namespace
{
    constexpr uint32_t kDisplayWidth = 1280;
    constexpr uint32_t kDisplayHeight = 720;
    constexpr float kRefreshRate = 60.0f;

    void StoreFloat(uint32_t address, float value)
    {
        GuestRef<uint32_t>(address) = std::bit_cast<uint32_t>(value);
    }

    // X_VIDEO_MODE (0x30 bytes): width, height, interlaced, widescreen,
    // HD, refresh rate (float), video standard (1 = NTSC), two constants
    // the system uses (0x4A, 0x01), reserved.
    void WriteVideoMode(uint32_t mode)
    {
        memset(GuestPtr<uint8_t>(mode), 0, 0x30);
        GuestRef<uint32_t>(mode + 0x00) = kDisplayWidth;
        GuestRef<uint32_t>(mode + 0x04) = kDisplayHeight;
        GuestRef<uint32_t>(mode + 0x08) = 0;
        GuestRef<uint32_t>(mode + 0x0C) = 1;
        GuestRef<uint32_t>(mode + 0x10) = 1;
        StoreFloat(mode + 0x14, kRefreshRate);
        GuestRef<uint32_t>(mode + 0x18) = 1;
        GuestRef<uint32_t>(mode + 0x1C) = 0x4A;
        GuestRef<uint32_t>(mode + 0x20) = 0x01;
    }
}

// --- Display queries -------------------------------------------------------

// void VdQueryVideoMode(X_VIDEO_MODE* mode)
PPC_FUNC(__imp__VdQueryVideoMode)
{
    WriteVideoMode(ctx.r3.u32);
}

// void XGetVideoMode(X_VIDEO_MODE* mode)
PPC_FUNC(__imp__XGetVideoMode)
{
    WriteVideoMode(ctx.r3.u32);
}

// DWORD VdQueryVideoFlags(): 1 widescreen, 2 at least 1024 wide, 4 at least 1920 wide.
PPC_FUNC(__imp__VdQueryVideoFlags)
{
    ctx.r3.u64 = 1 | 2;
}

// DWORD XGetAVPack(): 8 is HDMI, as Xenia reports by default.
PPC_FUNC(__imp__XGetAVPack)
{
    ctx.r3.u64 = 8;
}

// DWORD XGetGameRegion(): all regions.
PPC_FUNC(__imp__XGetGameRegion)
{
    ctx.r3.u64 = 0xFFFF;
}

// void VdGetCurrentDisplayGamma(PDWORD type, float* power)
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    GuestRef<uint32_t>(ctx.r3.u32) = 2;
    StoreFloat(ctx.r4.u32, 2.22222233f);
}

// void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* info)
// Layout after Xenia's X_DISPLAY_INFO: front buffer size and formats, scaler
// parameters (source rect, output size), overscan, display size, refresh
// rate, interlace flag, color format, actual display width.
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    uint32_t info = ctx.r3.u32;
    memset(GuestPtr<uint8_t>(info), 0, 0x58);
    GuestRef<uint16_t>(info + 0x00) = kDisplayWidth;
    GuestRef<uint16_t>(info + 0x02) = kDisplayHeight;
    GuestRef<uint32_t>(info + 0x10) = kDisplayWidth;   // scaler source rect x2
    GuestRef<uint32_t>(info + 0x14) = kDisplayHeight;  // scaler source rect y2
    GuestRef<uint32_t>(info + 0x18) = kDisplayWidth;   // scaled output width
    GuestRef<uint32_t>(info + 0x1C) = kDisplayHeight;  // scaled output height
    GuestRef<uint16_t>(info + 0x40) = kDisplayWidth;
    GuestRef<uint16_t>(info + 0x42) = kDisplayHeight;
    StoreFloat(info + 0x44, kRefreshRate);
    GuestRef<uint16_t>(info + 0x4E) = kDisplayWidth;
}

// --- Engine and display setup ----------------------------------------------

// BOOL VdInitializeEngines(DWORD unknown, PVOID callback, PVOID context, PDWORD pfp, PDWORD me)
PPC_FUNC(__imp__VdInitializeEngines)
{
    ctx.r3.u64 = 1;
}

// void VdShutdownEngines()
PPC_FUNC(__imp__VdShutdownEngines)
{
}

// DWORD VdSetDisplayMode(DWORD mode)
PPC_FUNC(__imp__VdSetDisplayMode)
{
    ctx.r3.u64 = 0;
}

// DWORD VdEnableDisableClockGating(BOOL enable)
PPC_FUNC(__imp__VdEnableDisableClockGating)
{
    ctx.r3.u64 = 0;
}

// DWORD VdCallGraphicsNotificationRoutines(DWORD unknown, PVOID args)
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    ctx.r3.u64 = 0;
}

// DWORD VdPersistDisplay(DWORD unknown, PDWORD unknownOut)
PPC_FUNC(__imp__VdPersistDisplay)
{
    if (ctx.r4.u32 != 0)
        GuestRef<uint32_t>(ctx.r4.u32) = 0;
    ctx.r3.u64 = 1;
}

// EDRAM link training always succeeds.
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__VdRetrainEDRAM)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    ctx.r3.u64 = 0;
}

// void KiApcNormalRoutineNop(): passed around as an APC routine that does nothing.
PPC_FUNC(__imp__KiApcNormalRoutineNop)
{
}

// --- Command buffers --------------------------------------------------------

// void VdInitializeRingBuffer(PVOID physicalAddress, DWORD sizeLog2)
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    gpu::InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

// void VdEnableRingBufferRPtrWriteBack(PVOID physicalAddress, DWORD blockSizeLog2)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    gpu::EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

// void VdSetGraphicsInterruptCallback(PVOID callback, PVOID userData)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    gpu::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
}

// void VdSetSystemCommandBufferGpuIdentifierAddress(PVOID address)
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
}

// void VdGetSystemCommandBuffer(PVOID buffer, PDWORD unknown)
// The values are placeholders the game passes back to VdSwap, as in Xenia.
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    memset(GuestPtr<uint8_t>(ctx.r3.u32), 0, 0x94);
    GuestRef<uint32_t>(ctx.r3.u32) = 0xBEEF0000;
    GuestRef<uint32_t>(ctx.r4.u32) = 0xBEEF0001;
}

// DWORD VdInitializeScalerCommandBuffer(8 scaler parameters, PDWORD dest)
// Fills the scaler commands with NOPs; returns the dword count.
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    constexpr uint32_t kDwords = 0x1CC / 4;
    constexpr uint32_t kType2Nop = 0x80000000;
    uint32_t dest = StackArgument(ctx, 8);
    for (uint32_t i = 0; i < kDwords; i++)
        GuestRef<uint32_t>(dest + i * 4) = kType2Nop;
    ctx.r3.u64 = kDwords;
}

// DWORD VdSwap(PDWORD ringBufferSpace, PVOID fetchConstant, ... 8 more)
// The game reserves 64 dwords in the ring buffer for the swap commands.
// Like Xenia, fill them with one private packet the command processor
// recognizes as a swap.
//
// The second argument is the front buffer's texture fetch constant (6 dwords):
// base address in dword 1 bits 12-31, width - 1 and height - 1 in dword 2
// (13 bits each). The packet carries base, width and height (Xenia's VdSwap).
PPC_FUNC(__imp__VdSwap)
{
    uint32_t buffer = ctx.r3.u32;
    uint32_t fetch = ctx.r4.u32;
    memset(GuestPtr<uint8_t>(buffer), 0, gpu::kSwapPacketDwords * 4);
    GuestRef<uint32_t>(buffer) = gpu::MakePacketType3(gpu::kOpcodeHostSwap, gpu::kSwapPacketDwords - 1);
    if (fetch != 0)
    {
        uint32_t word1 = GuestRef<uint32_t>(fetch + 4);
        uint32_t word2 = GuestRef<uint32_t>(fetch + 8);
        GuestRef<uint32_t>(buffer + 4) = word1 & 0xFFFFF000;
        GuestRef<uint32_t>(buffer + 8) = (word2 & 0x1FFF) + 1;
        GuestRef<uint32_t>(buffer + 12) = ((word2 >> 13) & 0x1FFF) + 1;
    }
    ctx.r3.u64 = 0;
}
