// Native mouse look and what it learns about player 1 (mouse_look.cpp).

#pragma once

namespace h3::hooks
{
    // Any thread: whether player 1 is zoomed, as of the last game tick (the
    // unit's zoom level, mouse_look.cpp).
    bool Player1Zoomed();
}
