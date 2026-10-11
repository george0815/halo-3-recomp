// The game's XMA library writes the decoder's kick and lock registers
// (0x7FEA1940 / 0x7FEA1A40, one bit per context) from these two functions.
// The registers are plain memory here, so the hooks run the originals and
// then pass the contexts on to the decoder (apu/xma_decoder.h).
//
// Both take Halo's voice: +0 number of contexts, +8 the contexts' 96-byte
// records, each with its hardware context index at +80.

#include <ppc_recomp_shared.h>

#include "../apu/xma_decoder.h"
#include "../memory.h"

using namespace h3;

namespace
{
    template <typename F>
    void ForEachContext(uint32_t voice, F&& f)
    {
        uint32_t count = GuestRef<uint32_t>(voice);
        uint32_t records = GuestRef<uint32_t>(voice + 8);
        for (uint32_t i = 0; i < count; i++)
            f(uint32_t(GuestRef<uint16_t>(records + i * 96 + 80)));
    }
}

// Copies each record's context data to its hardware context, then kicks it.
PPC_FUNC_IMPL(__imp__sub_826C8590);
PPC_FUNC(sub_826C8590)
{
    uint32_t voice = ctx.r3.u32;
    __imp__sub_826C8590(ctx, base);
    ForEachContext(voice, [](uint32_t id) { apu::EnableXmaContext(id); });
}

// Locks (stops) the voice's contexts unless they're already stopping.
PPC_FUNC_IMPL(__imp__sub_82138CA0);
PPC_FUNC(sub_82138CA0)
{
    uint32_t voice = ctx.r3.u32;
    bool locks = (uint32_t(GuestRef<uint32_t>(voice + 4)) & 0x30000) == 0;
    __imp__sub_82138CA0(ctx, base);
    if (locks)
        ForEachContext(voice, [](uint32_t id) { apu::DisableXmaContext(id); });
}
