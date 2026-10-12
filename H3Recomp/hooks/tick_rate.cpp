// Frame rate unlock (settings.ini [video] frame_rate = 60, or H3_TICK_RATE):
// runs Halo 3's game simulation at another tick rate.
//
// Halo 3 renders one frame per game tick, and its tick rate isn't baked in:
// game_time_globals (TLS slot 188; layout as ElDorito's GameTimeGlobals)
// holds it, and the engine reads ticks per second (+4, u16) and seconds per
// tick (+8, float) from there in hundreds of places. The rate comes from
// game options (TLS slot 128, +22: the game tick rate, 30), copied in by
//
//   sub_821D6F30  game_time_initialize: clears the globals, sets the rate,
//                 seconds per tick = 1 / rate, speed (+16) = 1
//   sub_821D7098  game time speed change: rate and seconds per tick scaled
//                 by the new speed
//
// Both hooks below substitute the configured rate for the game option while
// the original runs, then put the option back, so nothing else (saved
// films, network options) sees a changed value. Setting the rate mid-run
// showed the menu and gameplay at 60 fps; Halo 3 shipped at 30, so content
// tuned per tick rather than per second may misbehave at 60.

#include <ppc_recomp_shared.h>

#include "hooks/tick_rate.h"
#include "config/env.h"
#include "config/settings.h"
#include "memory.h"

#include <algorithm>
#include <cstdio>

using namespace h3;

namespace
{
    // Game options: the pointer in TLS slot kGameOptionsSlot (a byte offset
    // into the thread's TLS array, which r13 + 0 points to), its game tick
    // rate (s16) at kTickRateOffset.
    constexpr uint32_t kGameOptionsSlot = 128;
    constexpr uint32_t kTickRateOffset = 22;

    // The tick rate to run at: H3_TICK_RATE, else settings.ini; 0 = the
    // game's own (nothing to change).
    int ConfiguredTickRate()
    {
        static const int rate = [] {
            int value = int(EnvNumber("H3_TICK_RATE"));
            if (!value)
                value = config::Active().video.frameRate;
            if (value == config::kOriginalTickRate || value <= 0)
                return 0;
            fprintf(stderr, "[hooks] game tick rate %d\n", value);
            return value;
        }();
        return rate;
    }

    // TLS slot of game_time_globals, and the globals last initialized (for
    // H3_LOG_TICK_RATE).
    constexpr uint32_t kGameTimeSlot = 188;
    uint32_t g_gameTime = 0;
}

namespace h3::debug
{
    void ProvideWatchAddress(uint32_t address);  // debug/write_watch.cpp
}

namespace
{
    // Runs original(ctx, base) with the game options' tick rate replaced by
    // the configured one.
    template <typename F>
    void WithTickRate(PPCContext& ctx, uint8_t* base, F&& original)
    {
        // H3_TICK_RATE_SKIP=<n> (testing): leave the first n calls at the
        // game's rate, e.g. 1 = menus at 30 and levels at the new rate (so
        // frame-timed H3_INPUT_SCRIPT menu navigation still works).
        static const uint32_t skip = EnvNumber("H3_TICK_RATE_SKIP");
        static uint32_t calls = 0;
        int rate = calls++ < skip ? 0 : ConfiguredTickRate();
        uint32_t options = 0;
        if (rate)
        {
            uint32_t tls = GuestRef<uint32_t>(ctx.r13.u32);
            options = GuestRef<uint32_t>(tls + kGameOptionsSlot);
        }
        g_gameTime = GuestRef<uint32_t>(uint32_t(GuestRef<uint32_t>(ctx.r13.u32)) + kGameTimeSlot);
        // H3_WATCH_WRITES with address 0: watch this game time (debugging).
        debug::ProvideWatchAddress(g_gameTime);
        if (!options)
        {
            original(ctx, base);
            return;
        }
        uint32_t field = options + kTickRateOffset;
        int16_t saved = int16_t(GuestRef<int16_t>(field));
        GuestRef<int16_t>(field) = int16_t(rate);
        original(ctx, base);
        GuestRef<int16_t>(field) = saved;
    }
}

// game_time_initialize.
PPC_FUNC_IMPL(__imp__sub_821D6F30);
PPC_FUNC(sub_821D6F30)
{
    WithTickRate(ctx, base, __imp__sub_821D6F30);
}

// Game time speed change (f1 = speed).
PPC_FUNC_IMPL(__imp__sub_821D7098);
PPC_FUNC(sub_821D7098)
{
    WithTickRate(ctx, base, __imp__sub_821D7098);
}

namespace h3::hooks
{
    int GameTickRate()
    {
        int rate = ConfiguredTickRate();
        return rate ? rate : config::kOriginalTickRate;
    }

    // H3_LOG_TICK_RATE=1: log game_time_globals every few seconds (called
    // once per presented frame).
    void LogTickRate()
    {
        static const bool enabled = EnvFlag("H3_LOG_TICK_RATE");
        static uint32_t frames = 0;
        constexpr uint32_t kInterval = 300;
        if (!enabled || !g_gameTime || ++frames % kInterval != 0)
            return;
        float secondsPerTick = GuestRef<float>(g_gameTime + 8);
        fprintf(stderr, "[hooks] game time %08X: %u ticks/s, %g s/tick, tick %u, speed %g\n", g_gameTime,
            uint32_t(GuestRef<uint16_t>(g_gameTime + 4)), secondsPerTick, uint32_t(GuestRef<uint32_t>(g_gameTime + 12)),
            float(GuestRef<float>(g_gameTime + 16)));
    }
}


// HaloScript game_reverted (sub_822D7BB0; r4 = the script thread): true
// while the game time tick equals the tick the last game state load
// restored (kRevertTickAddress). Cinematic skips depend on it: the
// level script saves the game when a cutscene starts, and the end (or a
// skip) reverts to that save; the script, back where it saved, asks
// game_reverted and moves past the cutscene. Its check polls once per
// script period, which seems to scale with the tick rate: at 60 ticks it
// first ran a tick after the revert tick, got false and replayed the
// cutscene forever (The Ark's intro). So the revert counts for the ticks
// one original 30 Hz tick spans: exactly the original at 30.
namespace
{
    constexpr uint32_t kRevertTickAddress = 0x828C4A50 + 0x1F35C;  // main game globals + 0x1F35C
    // HaloScript returns a boolean as the high byte of the value word.
    constexpr uint32_t kScriptTrue = 0x01000000;
}
PPC_FUNC_IMPL(__imp__sub_820AB8A0);  // script function return (r3 thread, r4 value)
PPC_FUNC_IMPL(__imp__sub_822D7BB0);
PPC_FUNC(sub_822D7BB0)
{
    uint32_t gameTime = GuestRef<uint32_t>(uint32_t(GuestRef<uint32_t>(ctx.r13.u32)) + kGameTimeSlot);
    uint32_t tick = GuestRef<uint32_t>(gameTime + 12);
    uint32_t revertTick = GuestRef<uint32_t>(kRevertTickAddress);
    uint32_t window = uint32_t(std::max(1, h3::hooks::GameTickRate() / config::kOriginalTickRate));
    bool reverted = tick - revertTick < window;  // unsigned: ticks before the revert don't count
    static const bool log = EnvFlag("H3_LOG_TICK_RATE");
    if (log)
        fprintf(stderr, "[hooks] game_reverted: tick %u, revert tick %u -> %d\n", tick, revertTick, reverted);
    ctx.r3.u64 = ctx.r4.u64;
    ctx.r4.u64 = reverted ? kScriptTrue : 0;
    sub_820AB8A0(ctx, base);
}
