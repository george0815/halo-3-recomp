// Frame rate unlock: the game tick rate in effect (hooks/tick_rate.cpp).

#pragma once

namespace h3::hooks
{
    // Game ticks per second the game runs at: settings.ini frame_rate, or
    // H3_TICK_RATE (30 = the original rate).
    int GameTickRate();
}
