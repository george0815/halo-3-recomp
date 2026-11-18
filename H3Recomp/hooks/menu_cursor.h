// Clickable menu cursor (hooks/menu_cursor.cpp): pointing at a menu item
// focuses it, as the D-pad would; input/input.cpp turns clicks into A and B.

#pragma once

namespace h3::hooks
{
    // Main thread: where the mouse is, as a fraction of the game frame
    // (0..1 both ways); inside = false when it's off the frame (or released).
    // moved = false: where it is as a menu opens (not a move: it doesn't
    // take the focus from the game's choice).
    void SetMenuCursor(bool inside, float x, float y, bool moved = true);

    // Any thread: a menu list is on screen (one was updated in the last
    // 150 ms): the mouse is a pointer, not the look stick.
    bool MenuOpen();

    // Any thread: the cursor is over an item with a slider (the wheel then
    // moves the slider: D-pad left/right).
    bool MenuCursorOverSlider();

    // A click: RequestMenuClick asks the game thread to focus the item under
    // the cursor; TakeMenuClick is true once it has (then press A). False
    // until then; CancelMenuClick drops a request nothing answered.
    void RequestMenuClick();
    bool TakeMenuClick();
    void CancelMenuClick();
}
