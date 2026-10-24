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
