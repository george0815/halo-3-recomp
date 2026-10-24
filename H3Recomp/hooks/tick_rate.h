// Frame rate unlock: the game tick rate in effect (hooks/tick_rate.cpp).

#pragma once

namespace h3::hooks
{
    // Game ticks per second the game runs at: settings.ini frame_rate, or
    // H3_TICK_RATE (30 = the original rate).
    int GameTickRate();

    // Game ticks per original (30 Hz) tick: 1 at 30, 2 at 60. Content tuned
    // per tick runs at most once per this many ticks to keep its 30 Hz pace.
    int TicksPerOriginalTick();

    // Whether the 60 Hz content fixes are on: the game runs above 30 ticks
    // per second and H3_NO_60HZ_FIXES is unset. They are MCC's own fixes for
    // content tuned per tick (weapon fire rates, AI bursts, camera shake;
    // H3Native/weapons/weapon_barrels.cpp, hooks/player_effects.cpp; MCC's
    // AI burst minimum is not done, docs/ROADMAP.md 5). Saved films replay a
    // run's inputs only, so a film recorded without them must be replayed
    // without them (tools/films/run_film.sh sets H3_NO_60HZ_FIXES).
    bool ContentFixes();
}
