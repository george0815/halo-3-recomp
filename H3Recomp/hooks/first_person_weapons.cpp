// First-person weapon animation timing at 60 ticks per second (two of MCC's
// 60 Hz content fixes, hooks::ContentFixes(); hooks/tick_rate.h).
//
// Symptom (private/BUGS.md, "weapon switch", 2026-11-02): at 60 Hz, switching
// weapons while moving drew the incoming weapon for one frame partway up,
// then hid it and played its raise from the start. Never at 30 Hz, never
// standing still.
//
// MCC's Halo 3 (interface/first_person_weapons.cpp in H3EK's
// halo3_tag_test.exe) scales two first-person animation timings by
// game_ticks_per_second / 30 (FUN_14036f2c0) that retail Halo 3, built for
// 30 ticks per second, doesn't:
//
//   FUN_140965720 / retail sub_82477D50 (user, ticks): starts the put-away
//   animation (39) on both weapon slots, playing it at
//   max(length, minimum) / ticks so it lasts the weapon's put-away time. MCC
//   multiplies the rate by the tick multiplier; retail passes it to
//   sub_8228EF98 (animation playback rate) unscaled, so at 60 Hz the
//   first-person put-away ran at half speed against the weapon's own timer.
//
//   FUN_14096a4a0 / retail sub_82474FF0, the per-tick first-person update:
//   when the player starts moving it starts the movement overlay and blends
//   into it (sub_82474C60: snapshots the node matrices, blend time in f1).
//   MCC divides that blend time by the tick multiplier on this path only.
//
// Both are matched to their call sites by return address.

#include "hooks/tick_rate.h"

#include <ppc_recomp_shared.h>

namespace
{
    // Return addresses: sub_82477D50's call to sub_8228EF98, and
    // sub_82474FF0's movement-branch call to sub_82474C60.
    constexpr uint32_t kPutAwayRateCall = 0x82477E34;
    constexpr uint32_t kMovementBlendCall = 0x82475684;
}

// Animation playback rate (r3 = animation channel, f1 = rate).
PPC_FUNC_IMPL(__imp__sub_8228EF98);
PPC_FUNC(sub_8228EF98)
{
    if (uint32_t(ctx.lr) == kPutAwayRateCall && h3::hooks::ContentFixes())
        ctx.f1.f64 = double(float(ctx.f1.f64 * double(h3::hooks::TicksPerOriginalTick())));
    __imp__sub_8228EF98(ctx, base);
}

// First-person weapon blend start (r3 = user, r4 = weapon slot, f1 = blend
// time).
PPC_FUNC_IMPL(__imp__sub_82474C60);
PPC_FUNC(sub_82474C60)
{
    if (uint32_t(ctx.lr) == kMovementBlendCall && h3::hooks::ContentFixes())
        ctx.f1.f64 = double(float(ctx.f1.f64 / double(h3::hooks::TicksPerOriginalTick())));
    __imp__sub_82474C60(ctx, base);
}
