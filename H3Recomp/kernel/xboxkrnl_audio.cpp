// xboxkrnl XAudio* exports: the render driver the game's audio engine mixes
// into (apu/audio_system.h). Reference: Xenia's xboxkrnl_audio.cc.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "../apu/audio_system.h"
#include "../apu/xma_decoder.h"
#include "../memory.h"

#include <bit>

using namespace h3;
using namespace h3::kernel;

namespace
{
    // Driver handles carry this tag above the client index, as on the console.
    constexpr uint32_t kDriverTag = 0x41550000;
}

// DWORD XAudioRegisterRenderDriverClient(LPDWORD callbackAndArg, LPDWORD driver)
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    uint32_t callback = GuestRef<uint32_t>(ctx.r3.u32);
    uint32_t callbackArg = GuestRef<uint32_t>(ctx.r3.u32 + 4);
    int index = apu::RegisterClient(callback, callbackArg);
    if (index < 0)
    {
        ctx.r3.u64 = 0x80070057;  // E_INVALIDARG; Xenia returns X_E_INVALIDARG when full
        return;
    }
    GuestRef<uint32_t>(ctx.r4.u32) = kDriverTag | uint32_t(index);
    ctx.r3.u64 = 0;
}

// DWORD XAudioUnregisterRenderDriverClient(DWORD driver)
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    apu::UnregisterClient(ctx.r3.u32 & 0xFFFF);
    ctx.r3.u64 = 0;
}

// DWORD XAudioSubmitRenderDriverFrame(DWORD driver, const float* samples)
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    apu::SubmitFrame(ctx.r3.u32 & 0xFFFF, ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// DWORD XAudioGetSpeakerConfig(LPDWORD config): 5.1, as Xenia reports.
PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    GuestRef<uint32_t>(ctx.r3.u32) = 0x00010001;
    ctx.r3.u64 = 0;
}

// DWORD XAudioGetVoiceCategoryVolumeChangeMask(PVOID driver, LPDWORD mask)
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    GuestRef<uint32_t>(ctx.r4.u32) = 0;
    ctx.r3.u64 = 0;
}

// DWORD XAudioGetVoiceCategoryVolume(DWORD category, float* volume)
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    GuestRef<uint32_t>(ctx.r4.u32) = std::bit_cast<uint32_t>(1.0f);
    ctx.r3.u64 = 0;
}

// DWORD XMACreateContext(PVOID* context)
PPC_FUNC(__imp__XMACreateContext)
{
    uint32_t context = apu::AllocateXmaContext();
    GuestRef<uint32_t>(ctx.r3.u32) = context;
    ctx.r3.u64 = context ? 0 : 0xC0000017;  // STATUS_NO_MEMORY
}

// DWORD XMAReleaseContext(PVOID context)
PPC_FUNC(__imp__XMAReleaseContext)
{
    apu::ReleaseXmaContext(ctx.r3.u32);
    ctx.r3.u64 = 0;
}
