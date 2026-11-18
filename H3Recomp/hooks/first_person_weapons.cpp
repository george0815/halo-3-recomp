// First-person weapon animation timing at 60 ticks per second (two of MCC's
// 60 Hz content fixes, hooks::ContentFixes(); hooks/tick_rate.h).
//
// A third fix of ours, not MCC's (2026-11-17, the Ark BR -> sniper pop
// while moving): see the movement blend below.
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
#include "config/env.h"

#include <ppc_recomp_shared.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "debug/state_hash.h"

namespace
{
    // First-person animation 12 is a weapon's ready (raise); bit n: a
    // ready started on slot n during the current first-person tick
    // (cleared when sub_82474FF0's tick update ends). Player 1 only, which
    // is what the pop was seen on; other players' slots share the bits.
    constexpr uint32_t kReadyAnimation = 12;
    thread_local uint32_t t_readyStarted = 0;

    // Return addresses: sub_82477D50's call to sub_8228EF98, and
    // sub_82474FF0's movement-branch call to sub_82474C60.
    constexpr uint32_t kPutAwayRateCall = 0x82477E34;
    constexpr uint32_t kMovementBlendCall = 0x82475684;
}

// Animation playback rate (r3 = animation channel, f1 = rate).
PPC_FUNC_IMPL(__imp__sub_8228EF98);
PPC_FUNC(sub_8228EF98)
{
    // H3_LOG_FP_WEAPONS=<first>-<last level time> (research): every call.
    static const char* range = h3::EnvString("H3_LOG_FP_WEAPONS");
    if (range)
    {
        static const uint32_t first = uint32_t(atoi(range)), last = uint32_t(atoi(strchr(range, '-') ? strchr(range, '-') + 1 : range));
        uint32_t frame = h3::debug::CurrentLevelTime();
        if (frame >= first && frame <= last)
            fprintf(stderr, "[fp] lt %u rate lr=%08X r3=%08X f1=%.4f\n", frame, uint32_t(ctx.lr), ctx.r3.u32, ctx.f1.f64);
    }
    if (uint32_t(ctx.lr) == kPutAwayRateCall && h3::hooks::ContentFixes())
        ctx.f1.f64 = double(float(ctx.f1.f64 * double(h3::hooks::TicksPerOriginalTick())));
    __imp__sub_8228EF98(ctx, base);
}

// First-person weapon blend start (r3 = user, r4 = weapon slot, f1 = blend
// time).
PPC_FUNC_IMPL(__imp__sub_82474C60);
PPC_FUNC(sub_82474C60)
{
    static const char* range = h3::EnvString("H3_LOG_FP_WEAPONS");
    if (range)
    {
        static const uint32_t first = uint32_t(atoi(range)), last = uint32_t(atoi(strchr(range, '-') ? strchr(range, '-') + 1 : range));
        uint32_t frame = h3::debug::CurrentLevelTime();
        if (frame >= first && frame <= last)
            fprintf(stderr, "[fp] lt %u blend lr=%08X user %u slot %u f1=%.4f\n", frame, uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32,
                ctx.f1.f64);
    }
    // The movement branch's blend in the same tick as a weapon's ready
    // started: the blend would start from the incoming weapon's
    // never-animated bones (its held pose), drawing it fully raised for a
    // frame before the raise (the Ark BR -> sniper pop while moving). No
    // blend then: the ready animation is the pose. H3_FP_READY_BLEND=1 keeps it.
    static const bool keepReadyBlend = h3::EnvFlag("H3_FP_READY_BLEND");
    if (uint32_t(ctx.lr) == kMovementBlendCall && ctx.r4.u32 < 32 && (t_readyStarted & (1u << ctx.r4.u32)) &&
        !keepReadyBlend && h3::hooks::ContentFixes())
    {
        ctx.f1.f64 = 0.0;
        __imp__sub_82474C60(ctx, base);
        return;
    }
    if (uint32_t(ctx.lr) == kMovementBlendCall && h3::hooks::ContentFixes())
        ctx.f1.f64 = double(float(ctx.f1.f64 / double(h3::hooks::TicksPerOriginalTick())));
    __imp__sub_82474C60(ctx, base);
}

// Research (H3_LOG_FP_WEAPONS=<first>-<last level time>): the per-tick
// first-person update and every first-person animation start.
namespace
{
    bool LogFrame(uint32_t& frame)
    {
        static const char* range = h3::EnvString("H3_LOG_FP_WEAPONS");
        if (!range)
            return false;
        static const uint32_t first = uint32_t(atoi(range)), last = uint32_t(atoi(strchr(range, '-') ? strchr(range, '-') + 1 : range));
        frame = h3::debug::CurrentLevelTime();
        return frame >= first && frame <= last;
    }
}

PPC_FUNC_IMPL(__imp__sub_82474FF0);
PPC_FUNC(sub_82474FF0)
{
    uint32_t frame;
    if (LogFrame(frame))
        fprintf(stderr, "[fp] lt %u tick update r3=%08X\n", frame, ctx.r3.u32);
    __imp__sub_82474FF0(ctx, base);
    t_readyStarted = 0;
}

PPC_FUNC_IMPL(__imp__sub_82474178);
PPC_FUNC(sub_82474178)
{
    uint32_t frame;
    if (LogFrame(frame))
        fprintf(stderr, "[fp] lt %u start anim lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n", frame, uint32_t(ctx.lr),
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
    if (ctx.r5.u32 == kReadyAnimation && ctx.r4.u32 < 32)
        t_readyStarted |= 1u << ctx.r4.u32;
    __imp__sub_82474178(ctx, base);
}
