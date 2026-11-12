// Camera shake at 60 ticks per second (one of the 60 Hz content fixes,
// hooks::ContentFixes(); hooks/tick_rate.h).
//
// sub_8212BF98 is the player effects update: the scripted camera shake
// (player_effect_set_max_translation / _rotation / _rumble and
// player_effect_start: the globals in TLS slot kTlsPlayerEffects) applied
// to a player's view. Each call draws six random numbers from the local
// random seed (hooks/random_seed.cpp) for the shake's translation and
// rotation, and the rumble draws more (sub_8223EB20), so the view jumps to
// a new random offset every tick. At 60 ticks per second it jumps twice as
// often: in The Halo's cutscenes, where the collapsing ring shakes the
// camera the whole time, Chief and Johnson visibly shook.
//
// MCC's Halo 3 (player_effects.cpp in H3EK's halo3_tag_test.exe) keeps
// each player's random offsets and draws new ones only on ticks that are a
// multiple of ticks per 30 Hz tick, and not again within a tick. Here the
// original runs as is on those ticks, with the seed it started from saved;
// on the others it runs again from that saved seed, so it draws the same
// values (scaled by the shake's current intensity), and the seed is put
// back afterwards so the rest of the game's cosmetic randomness goes on as
// if the call had drawn nothing.

#include "hooks/tick_rate.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <cstdint>
#include <unordered_map>

namespace
{
    // The local (cosmetic) random seed (hooks/random_seed.cpp).
    constexpr uint32_t kLocalRandomSeed = 0x827E3648;

    // Game time globals (TLS slot kTlsGameTime): the current tick (u32).
    constexpr uint32_t kTlsGameTime = 188;
    constexpr uint32_t kGameTimeTick = 12;

    constexpr uint32_t kNone = 0xFFFFFFFF;

    // One player's shake draws: the tick last seen, and the seed the last
    // fresh draw started from.
    struct ShakeDraw
    {
        uint32_t tick = kNone;
        uint32_t seed = 0;
        bool drawn = false;
    };
}

// player_effects_update(r3 player index, ...): the scripted shake and rumble
// for one player's view.
PPC_FUNC_IMPL(__imp__sub_8212BF98);
PPC_FUNC(sub_8212BF98)
{
    if (!h3::hooks::ContentFixes() || ctx.r3.u32 == kNone)
    {
        __imp__sub_8212BF98(ctx, base);
        return;
    }
    // Only the main thread runs it (the player's view): no locking needed.
    static std::unordered_map<uint32_t, ShakeDraw> draws;
    ShakeDraw& draw = draws[ctx.r3.u32];
    uint32_t gameTime = h3::GuestRef<uint32_t>(uint32_t(h3::GuestRef<uint32_t>(ctx.r13.u32)) + kTlsGameTime);
    uint32_t tick = h3::GuestRef<uint32_t>(gameTime + kGameTimeTick);
    bool fresh = false;
    if (tick != draw.tick)
    {
        draw.tick = tick;
        fresh = tick % uint32_t(h3::hooks::TicksPerOriginalTick()) == 0;
    }
    auto& seed = h3::GuestRef<uint32_t>(kLocalRandomSeed);
    if (fresh || !draw.drawn)
    {
        draw.seed = seed;
        draw.drawn = true;
        __imp__sub_8212BF98(ctx, base);
        return;
    }
    uint32_t current = seed;
    seed = draw.seed;
    __imp__sub_8212BF98(ctx, base);
    seed = current;
}

// Camera impulse length at 60 ticks per second (another of MCC's fixes).
//
// sub_8223EE80 (MCC FUN_1405b8f60, player_effects.cpp) starts a damage
// response's camera impulse on a player's view (r4 = the player's effect
// state, r5 = the impulse definition, duration in seconds first): it copies
// the definition into the state and converts the duration to ticks (state +
// kImpulseTicksFloat, and s16 at kImpulseTicks). MCC then keeps impulses of
// exactly one 30 Hz tick (trunc(duration * 30) == 1) at one game tick: it
// divides both by game ticks per 30 Hz tick, so the kick isn't spread over
// two ticks at 60. Applied at the first call after the stores (cosine of the
// impulse angle, return address kAfterImpulseTicks), r31 = state, r27 =
// definition.
namespace
{
    constexpr uint32_t kAfterImpulseTicks = 0x8223F030;
    constexpr uint32_t kImpulseTicksFloat = 76;
    constexpr uint32_t kImpulseTicks = 132;
}

namespace h3::hooks
{
    bool CheckTrigEnabled();
    void CheckTrig(bool cosine, double x, double got);
}

PPC_FUNC_IMPL(__imp__sub_82081428);
PPC_FUNC(sub_82081428)
{
    double trigInput = ctx.f1.f64;
    if (uint32_t(ctx.lr) == kAfterImpulseTicks && h3::hooks::ContentFixes())
    {
        float duration = h3::GuestRef<float>(ctx.r27.u32);
        if (int16_t(int32_t(duration * 30.0f)) == 1)
        {
            int perTick = h3::hooks::TicksPerOriginalTick();
            auto& ticksFloat = h3::GuestRef<float>(ctx.r31.u32 + kImpulseTicksFloat);
            auto& ticks = h3::GuestRef<int16_t>(ctx.r31.u32 + kImpulseTicks);
            ticksFloat = float(ticksFloat) / float(perTick);
            ticks = int16_t(int32_t(int16_t(ticks)) / perTick);
        }
    }
    __imp__sub_82081428(ctx, base);
    if (h3::hooks::CheckTrigEnabled())
        h3::hooks::CheckTrig(true, trigInput, ctx.f1.f64);
}
