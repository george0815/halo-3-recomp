// Ground push at 60 ticks per second (one of MCC's 60 Hz content fixes,
// hooks::ContentFixes(); hooks/tick_rate.h).
//
// sub_820E7A38 (MCC FUN_1406647d0) moves a biped along the ground. While it
// stands on the ground it pushes the velocity into the surface each tick by
// clamp(speed * k * maximum, minimum, maximum) (scaled by the physics
// state's own factor), with the minimum and maximum from two constant pairs
// in .rdata (kPushLimits: 0.234 / 0.12 for one mode, 0.468 / 0.24 for the
// other). A push per tick, so at 60 Hz twice the push per second. MCC
// multiplies the push by 1 / (game ticks per 30 Hz tick).
//
// Scaling both limits scales the clamped push by the same factor, and
// nothing else reads them (the only loads are sub_820E7A38's, from a base
// of 0x820328E4), so they are divided once, before the first update.

#include "hooks/tick_rate.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

namespace
{
    constexpr uint32_t kPushLimits = 0x820328DC;  // 4 floats
    constexpr uint32_t kPushLimitCount = 4;
}

PPC_FUNC_IMPL(__imp__sub_820E7A38);
PPC_FUNC(sub_820E7A38)
{
    static bool scaled = false;
    if (!scaled)
    {
        scaled = true;
        if (h3::hooks::ContentFixes())
            for (uint32_t i = 0; i < kPushLimitCount; i++)
            {
                auto& limit = h3::GuestRef<float>(kPushLimits + i * 4);
                limit = float(limit) / float(h3::hooks::TicksPerOriginalTick());
            }
    }
    __imp__sub_820E7A38(ctx, base);
}
