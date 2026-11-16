// Havok world step settings at 60 ticks per second (one of MCC's 60 Hz
// content fixes, hooks::ContentFixes(); hooks/tick_rate.h).
//
// sub_821BB300 (MCC FUN_14043ae60) creates the Havok world: it fills a
// world construction info on its stack (r1 + 144; solver setup 7, flags
// 0x9E35, an 800-byte allocation, as in MCC) and passes it to the world
// constructor sub_82642058 (its only call, return address kWorldCall). Two
// of the fields come from the physics constants (0x8275DD10, MCC
// 0x1415CCF18) and are tuned per tick: +80 = constants + 0 (0.0328, just
// under 1/30 s, the expected minimum step) and +172 = constants + 24
// (0.02). MCC multiplies both by 1 / (game ticks per 30 Hz tick). The
// constants themselves are read elsewhere too, so the info is changed, not
// them.

#include "hooks/tick_rate.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <cstdio>

namespace
{
    constexpr uint32_t kWorldCall = 0x821BB454;
    constexpr uint32_t kStepTimeFields[] = { 80, 172 };  // floats in the info (r4)
}

PPC_FUNC_IMPL(__imp__sub_82642058);
PPC_FUNC(sub_82642058)
{
    if (uint32_t(ctx.lr) == kWorldCall && h3::hooks::ContentFixes())
        for (uint32_t offset : kStepTimeFields)
        {
            auto& field = h3::GuestRef<float>(ctx.r4.u32 + offset);
            field = float(field) / float(h3::hooks::TicksPerOriginalTick());
            fprintf(stderr, "[havok] world info +%u scaled to %g\n", offset, double(float(field)));
        }
    __imp__sub_82642058(ctx, base);
}
