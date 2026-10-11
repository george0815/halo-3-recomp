// Names for keys and mouse buttons as settings.ini stores them: SDL's
// scancode names ("Space", "Left Shift"), which the game turns back into
// keys with SDL_GetScancodeFromName, and config::kMouse* for the mouse.

#pragma once

#include <QString>

class QKeyEvent;

namespace h3::launcher
{
    // The binding name for a key press, or an empty string for keys that
    // can't be bound (or that SDL names differently).
    QString BindingNameForKey(const QKeyEvent& event);

    // The binding name for a mouse button (Qt::MouseButton), or empty.
    QString BindingNameForMouse(int button);
}
