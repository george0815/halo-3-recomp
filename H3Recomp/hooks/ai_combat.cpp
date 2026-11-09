// AI burst length at 60 ticks per second (one of MCC's 60 Hz content fixes,
// hooks::ContentFixes(); hooks/tick_rate.h).
//
// MCC's Halo 3 (ai/actor_combat.cpp in H3EK's halo3_tag_test.exe,
// FUN_140d91e20) picks an actor's burst length ("ai-burst-time": a random
// time between the firing pattern's burst duration bounds, in ticks) and
// then raises it to at least game_ticks_per_second / 30 (FUN_14036f2c0),
// i.e. one 30 Hz tick. Retail Halo 3 (sub_8251C8A0, the same function)
// stores the rounded ticks at actor + kActorBurstTicks with no minimum, so
// at 60 Hz a burst shorter than 1/60 s rounds to 0 ticks where at 30 it
// lasted one. The rest of the function divides by and scales the burst
// length, so the minimum has to be in place before it reads it again.
//
// Hooked at retail's first call after the store: every path from the store
// reaches sub_8251C8A0's call to sub_820C51D0 (return address
// kAfterBurstCall) with r31 = the actor, and nothing reads the burst length
// in between.

#include "hooks/tick_rate.h"
#include "config/env.h"
#include "memory.h"

#include <cstdio>

#include <ppc_recomp_shared.h>

namespace
{
    constexpr uint32_t kAfterBurstCall = 0x8251CB20;
    constexpr uint32_t kActorBurstTicks = 2316;  // s16 (MCC: actor + 0x910)
}

PPC_FUNC_IMPL(__imp__sub_820C51D0);
PPC_FUNC(sub_820C51D0)
{
    if (uint32_t(ctx.lr) == kAfterBurstCall && h3::hooks::ContentFixes())
    {
        auto& burst = h3::GuestRef<int16_t>(ctx.r31.u32 + kActorBurstTicks);
        int16_t minimum = int16_t(h3::hooks::TicksPerOriginalTick());
        // H3_LOG_AI_BURST=1 (testing): how many bursts the minimum raises.
        static const bool log = h3::EnvFlag("H3_LOG_AI_BURST");
        static uint32_t bursts = 0, raised = 0;
        bursts++;
        if (int16_t(burst) <= minimum)
        {
            if (int16_t(burst) < minimum)
                raised++;
            burst = minimum;
        }
        if (log && bursts % 20 == 0)
            fprintf(stderr, "[ai] bursts %u, raised to one 30 Hz tick %u\n", bursts, raised);
    }
    __imp__sub_820C51D0(ctx, base);
}

