// Qt key and mouse events to settings.ini binding names (key_names.h).

#include "key_names.h"

#include "config/settings.h"

#include <QKeyEvent>

namespace h3::launcher
{
    namespace
    {
        // Linux reports XKB keycodes as the native scan code: the evdev code
        // plus 8. Only used to tell left modifiers from right ones, which Qt
        // key codes don't.
        constexpr quint32 kXkbOffset = 8;
        constexpr quint32 kEvdevRightCtrl = 97;
        constexpr quint32 kEvdevRightShift = 54;
        constexpr quint32 kEvdevRightAlt = 100;
        constexpr quint32 kEvdevRightMeta = 126;

        // "Left X" or "Right X" for a modifier key.
        QString Side(const QKeyEvent& event, quint32 rightEvdevCode, const char* name)
        {
            bool right = event.nativeScanCode() == rightEvdevCode + kXkbOffset;
            return QStringLiteral("%1 %2").arg(right ? "Right" : "Left", name);
        }

        // Keys whose SDL name isn't simply their character.
        struct NamedKey
        {
            int key;
            const char* name;
        };
        constexpr NamedKey kNamedKeys[] = {
            { Qt::Key_Space, "Space" },
            { Qt::Key_Return, "Return" },
            { Qt::Key_Enter, "Return" },
            { Qt::Key_Escape, "Escape" },
            { Qt::Key_Backspace, "Backspace" },
            { Qt::Key_Tab, "Tab" },
            { Qt::Key_Backtab, "Tab" },
            { Qt::Key_CapsLock, "CapsLock" },
            { Qt::Key_Up, "Up" },
            { Qt::Key_Down, "Down" },
            { Qt::Key_Left, "Left" },
            { Qt::Key_Right, "Right" },
            { Qt::Key_Insert, "Insert" },
            { Qt::Key_Delete, "Delete" },
            { Qt::Key_Home, "Home" },
            { Qt::Key_End, "End" },
            { Qt::Key_PageUp, "PageUp" },
            { Qt::Key_PageDown, "PageDown" },
            { Qt::Key_Minus, "-" },
            { Qt::Key_Equal, "=" },
            { Qt::Key_BracketLeft, "[" },
            { Qt::Key_BracketRight, "]" },
            { Qt::Key_Backslash, "\\" },
            { Qt::Key_Semicolon, ";" },
            { Qt::Key_Apostrophe, "'" },
            { Qt::Key_QuoteLeft, "`" },
            { Qt::Key_Comma, "," },
            { Qt::Key_Period, "." },
            { Qt::Key_Slash, "/" },
        };

        // Numeric keypad keys (Qt marks them with KeypadModifier).
        constexpr NamedKey kKeypadKeys[] = {
            { Qt::Key_Slash, "Keypad /" },
            { Qt::Key_Asterisk, "Keypad *" },
            { Qt::Key_Minus, "Keypad -" },
            { Qt::Key_Plus, "Keypad +" },
            { Qt::Key_Enter, "Keypad Enter" },
            { Qt::Key_Period, "Keypad ." },
            { Qt::Key_Delete, "Keypad ." },
        };
    }

    QString BindingNameForKey(const QKeyEvent& event)
    {
        const int key = event.key();
        switch (key)
        {
        case Qt::Key_Control: return Side(event, kEvdevRightCtrl, "Ctrl");
        case Qt::Key_Shift: return Side(event, kEvdevRightShift, "Shift");
        case Qt::Key_Alt: return Side(event, kEvdevRightAlt, "Alt");
        case Qt::Key_AltGr: return QStringLiteral("Right Alt");
        case Qt::Key_Meta: return Side(event, kEvdevRightMeta, "GUI");
        default: break;
        }
        if (event.modifiers() & Qt::KeypadModifier)
        {
            if (key >= Qt::Key_0 && key <= Qt::Key_9)
                return QStringLiteral("Keypad %1").arg(key - Qt::Key_0);
            for (const NamedKey& entry : kKeypadKeys)
                if (entry.key == key)
                    return QString::fromLatin1(entry.name);
        }
        if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
            return QString(QChar(key));
        if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
            return QStringLiteral("F%1").arg(key - Qt::Key_F1 + 1);
        for (const NamedKey& entry : kNamedKeys)
            if (entry.key == key)
                return QString::fromLatin1(entry.name);
        return {};
    }

    QString BindingNameForMouse(int button)
    {
        switch (button)
        {
        case Qt::LeftButton: return QString::fromUtf8(config::kMouseLeft.data(), config::kMouseLeft.size());
        case Qt::RightButton: return QString::fromUtf8(config::kMouseRight.data(), config::kMouseRight.size());
        case Qt::MiddleButton: return QString::fromUtf8(config::kMouseMiddle.data(), config::kMouseMiddle.size());
        case Qt::BackButton: return QString::fromUtf8(config::kMouseX1.data(), config::kMouseX1.size());
        case Qt::ForwardButton: return QString::fromUtf8(config::kMouseX2.data(), config::kMouseX2.size());
        default: return {};
        }
    }
}
