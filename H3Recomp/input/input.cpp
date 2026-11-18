// XInput emulation (input.h). Keyboard and mouse bindings come from the
// [controls] section of settings.ini (config/settings.h, edited with the
// launcher; the defaults are listed in the README, "Controls"). Controller
// 0 merges the keyboard, the mouse and (unless keyboard_own_player) the
// first gamepad: buttons OR together, sticks take the larger deflection.

#include "input/input.h"
#include "config/env.h"
#include "config/settings.h"
#include "hooks/menu_cursor.h"
#include "hooks/mouse_look.h"
#include "video/window.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace h3::input
{
    namespace
    {
        using config::Action;

        // One parsed binding from settings.ini: a key, a mouse button, or
        // the mouse wheel.
        struct Binding
        {
            enum class Kind : uint8_t { Key, MouseButton, Wheel } kind;
            SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
            SDL_MouseButtonFlags mouseMask = 0;
        };
        // g_bindings[action]: everything bound to it (window thread only).
        std::array<std::vector<Binding>, config::kActionCount> g_bindings;

        // The XInput button each button action presses (0 for the stick
        // directions and triggers, which are handled separately).
        constexpr uint16_t ButtonFor(Action action)
        {
            switch (action)
            {
            case Action::Jump: return kA;
            case Action::Melee: return kB;
            case Action::Equipment: return kX;
            case Action::SwitchWeapon: return kY;
            case Action::LeftBumper: return kLeftShoulder;
            case Action::RightBumper: return kRightShoulder;
            case Action::Crouch: return kLeftThumb;
            case Action::Zoom: return kRightThumb;
            case Action::Pause: return kStart;
            case Action::Scoreboard: return kBack;
            case Action::DpadUp: return kDpadUp;
            case Action::DpadDown: return kDpadDown;
            case Action::DpadLeft: return kDpadLeft;
            case Action::DpadRight: return kDpadRight;
            default: return 0;
            }
        }

        // Turns the binding names from the settings into Binding entries.
        // Unknown names are reported and skipped.
        void ParseBindings(const config::ControlSettings& controls)
        {
            static const struct
            {
                std::string_view name;
                SDL_MouseButtonFlags mask;
            } kMouseButtons[] = {
                { config::kMouseLeft, SDL_BUTTON_LMASK },
                { config::kMouseRight, SDL_BUTTON_RMASK },
                { config::kMouseMiddle, SDL_BUTTON_MMASK },
                { config::kMouseX1, SDL_BUTTON_X1MASK },
                { config::kMouseX2, SDL_BUTTON_X2MASK },
            };
            for (size_t action = 0; action < config::kActionCount; action++)
            {
                g_bindings[action].clear();
                for (const std::string& name : controls.bindings[action])
                {
                    Binding binding{ Binding::Kind::Key };
                    if (name == config::kMouseWheel)
                        binding.kind = Binding::Kind::Wheel;
                    else if (auto mouse = std::find_if(std::begin(kMouseButtons), std::end(kMouseButtons),
                                 [&](const auto& entry) { return entry.name == name; });
                             mouse != std::end(kMouseButtons))
                    {
                        binding.kind = Binding::Kind::MouseButton;
                        binding.mouseMask = mouse->mask;
                    }
                    else
                    {
                        binding.key = SDL_GetScancodeFromName(name.c_str());
                        if (binding.key == SDL_SCANCODE_UNKNOWN)
                        {
                            fprintf(stderr, "[input] unknown key \"%s\" bound to %s\n", name.c_str(),
                                config::Describe(Action(action)).key);
                            continue;
                        }
                    }
                    g_bindings[action].push_back(binding);
                }
            }
        }

        // H3_INPUT_SCRIPT="20-20.2:down,f900-905:a": buttons (or stick
        // directions and triggers) held between two times, in seconds since
        // start or (with f) presented frames, for testing without a keyboard.
        // A "p2." prefix (p1-p4) presses it on another controller
        // ("f900-905:p2.start"); H3_VIRTUAL_PADS=<n> connects controllers
        // 2 to n+1 with no gamepad behind them, for split screen tests, and
        // "unplug" disconnects one while it's held ("f5000-6000:p2.unplug").
        // Analog controls a script can hold (full deflection or pull), by
        // name: left/right stick directions and the triggers.
        enum class ScriptedAnalog : uint8_t
        {
            None, LeftUp, LeftDown, LeftLeft, LeftRight, RightUp, RightDown, RightLeft, RightRight, LeftTrigger, RightTrigger,
            MouseUp, MouseDown, MouseLeft, MouseRight,  // native mouse look: kScriptedMouseCounts a frame
        };
        constexpr float kScriptedMouseCounts = 10;
        struct ScriptedPress
        {
            double start, end;
            uint16_t button;  // 0 for an analog control
            bool frames;
            ScriptedAnalog analog = ScriptedAnalog::None;
            uint32_t player = 0;
            bool unplug = false;
        };

        // The analog control for a name in H3_INPUT_SCRIPT ("rup", "lt"...).
        ScriptedAnalog AnalogNamed(const std::string& name)
        {
            static const struct
            {
                const char* name;
                ScriptedAnalog analog;
            } kNames[] = {
                { "lup", ScriptedAnalog::LeftUp }, { "ldown", ScriptedAnalog::LeftDown }, { "lleft", ScriptedAnalog::LeftLeft },
                { "lright", ScriptedAnalog::LeftRight }, { "rup", ScriptedAnalog::RightUp }, { "rdown", ScriptedAnalog::RightDown },
                { "rleft", ScriptedAnalog::RightLeft }, { "rright", ScriptedAnalog::RightRight }, { "lt", ScriptedAnalog::LeftTrigger },
                { "rt", ScriptedAnalog::RightTrigger }, { "mup", ScriptedAnalog::MouseUp }, { "mdown", ScriptedAnalog::MouseDown },
                { "mleft", ScriptedAnalog::MouseLeft }, { "mright", ScriptedAnalog::MouseRight },
            };
            for (const auto& entry : kNames)
                if (name == entry.name)
                    return entry.analog;
            return ScriptedAnalog::None;
        }
        std::vector<ScriptedPress> g_script;
        uint64_t g_startTicks = 0;

        // The button bit for a name in H3_INPUT_SCRIPT ("a", "start", "up"...),
        // or 0.
        uint16_t ButtonNamed(const std::string& name)
        {
            static const struct
            {
                const char* name;
                uint16_t button;
            } kNames[] = {
                { "a", kA }, { "b", kB }, { "x", kX }, { "y", kY }, { "start", kStart }, { "back", kBack },
                { "up", kDpadUp }, { "down", kDpadDown }, { "left", kDpadLeft }, { "right", kDpadRight },
                { "lb", kLeftShoulder }, { "rb", kRightShoulder }, { "ls", kLeftThumb }, { "rs", kRightThumb },
            };
            for (const auto& entry : kNames)
                if (name == entry.name)
                    return entry.button;
            return 0;
        }

        // Parses H3_INPUT_SCRIPT into g_script.
        void ParseScript(const char* text)
        {
            std::string script = text;
            size_t position = 0;
            while (position < script.size())
            {
                size_t comma = script.find(',', position);
                std::string item = script.substr(position, comma == std::string::npos ? std::string::npos : comma - position);
                position = comma == std::string::npos ? script.size() : comma + 1;
                size_t dash = item.find('-'), colon = item.find(':');
                if (dash == std::string::npos || colon == std::string::npos || colon < dash)
                    continue;
                bool frames = !item.empty() && item[0] == 'f';
                std::string start = item.substr(frames ? 1 : 0, dash - (frames ? 1 : 0));
                std::string end = item.substr(dash + 1, colon - dash - 1);
                if (!end.empty() && end[0] == 'f')
                    end = end.substr(1);
                std::string name = item.substr(colon + 1);
                uint32_t player = 0;
                if (name.size() > 3 && name[0] == 'p' && name[1] >= '1' && name[1] <= '4' && name[2] == '.')
                {
                    player = uint32_t(name[1] - '1');
                    name = name.substr(3);
                }
                ScriptedPress press{ atof(start.c_str()), atof(end.c_str()), ButtonNamed(name), frames, AnalogNamed(name), player,
                    name == "unplug" };
                if (press.button || press.analog != ScriptedAnalog::None || press.unplug)
                    g_script.push_back(press);
                else
                    fprintf(stderr, "[input] unknown button in H3_INPUT_SCRIPT: %s\n", item.c_str());
            }
        }

        // Mouse: click the window to capture it (F1 or losing focus lets go).
        // Movement drives the right stick; the stick deflection follows the
        // mouse's speed over a short window, since the game samples the pad
        // only once per frame. The mouse_sensitivity setting scales it
        // (H3_MOUSE_SENSITIVITY overrides it).
        struct MouseMotion
        {
            uint64_t ticksNs;
            float dx, dy;
        };
        constexpr uint64_t kMouseWindowNs = 50'000'000;
        std::vector<MouseMotion> g_mouseMotion;  // window thread only
        // Native mouse look: counts since the game last took them.
        bool g_nativeMouse = false;
        // Toggle crouch / hold to zoom (settings.h): window thread only.
        bool g_toggleCrouch = false, g_holdToZoom = false;
        bool g_crouchHeld = false, g_crouched = false;  // the key last frame; the toggled state
        bool g_zoomHeld = false;
        uint64_t g_zoomClickUntilNs = 0;  // the zoom click being sent
        constexpr uint64_t kClickNs = 50'000'000;  // long enough for the game to see it (a tick or more)
        // Unzooming after the key comes up: once the game has taken the
        // last click (kSettleNs), asked again while it's still zoomed, up to
        // kMaxUnzoomClicks times.
        constexpr uint64_t kSettleNs = 100'000'000;
        constexpr int kMaxUnzoomClicks = 3;
        int g_zoomKeys = 0;  // zoom keys held last frame
        bool g_unzoomPending = false;
        uint64_t g_unzoomCheckNs = 0;
        int g_unzoomClicks = 0;
        std::mutex g_lookMutex;
        float g_lookDx = 0, g_lookDy = 0;
        // The common PC shooter scale: 0.022 degrees per count at
        // sensitivity 1.
        constexpr float kRadiansPerCount = 0.022f * 3.14159265f / 180.0f;
        float g_mouseSensitivity = 1.0f;
        float g_mouseInvertY = 1.0f;    // -1 to invert vertical mouse look
        int g_mouseWheel = 0;           // whole notches since the last Update
        uint64_t g_wheelPressUntilNs = 0;

        // Menu cursor (hooks/menu_cursor.cpp): while a menu is open the mouse
        // is released and points; a left click on an item is A, a right
        // click B, a wheel notch D-pad up or down. g_menuReleased: we
        // released a captured mouse for the menu (and take it back after).
        bool g_menuCursor = true;
        bool g_menuMode = false, g_menuReleased = false;
        bool g_menuCursorPlaced = false;  // the cursor was given to the menu since it opened
        uint64_t g_menuAUntilNs = 0, g_menuBUntilNs = 0, g_menuDpadUntilNs = 0;
        uint64_t g_menuClickAtNs = 0;  // a left click waiting for its item to be focused (0: none)
        uint16_t g_menuDpadButton = 0;
        int g_menuWheelSteps = 0;       // + up, - down, not yet sent

        // The window an event belongs to (null for global events).
        SDL_Window* EventWindow(const SDL_Event& event)
        {
            return SDL_GetWindowFromEvent(&event);
        }

        // Mouse velocity (pixels per second) to a stick axis: past the dead
        // zone as soon as the mouse moves, full deflection when fast.
        int16_t MouseAxis(float pixelsPerSecond)
        {
            constexpr float kDeadZone = 8000.0f, kScale = 40.0f;
            if (std::abs(pixelsPerSecond) < 1.0f)
                return 0;
            float magnitude = std::min(kDeadZone + std::abs(pixelsPerSecond) * kScale * g_mouseSensitivity, float(kStickMax));
            return int16_t(pixelsPerSecond < 0 ? -magnitude : magnitude);
        }

        std::mutex g_mutex;
        std::array<GamepadState, kMaxPlayers> g_states;  // guarded by g_mutex
        std::atomic<uint32_t> g_connected{ 1 };
        std::atomic<uint32_t> g_signedIn{ 1 };
        std::atomic<bool> g_keyboardLast{ true };
        // Controllers 2-4 join (their player signs in) by pressing START;
        // until then the game doesn't see them, so a gamepad nobody plays
        // with (a spare one, Steam's virtual copy of a real one) adds no
        // player. The joining press is swallowed. Window thread only.
        uint32_t g_joined = 1;
        uint32_t g_swallow = 0;  // controllers whose buttons are held back until released
        // Holding BACK and START together for kLeaveHoldNs signs a player
        // out (the 360's guide menu did that). The two buttons are kept
        // from the game while held together, so the hold doesn't pause.
        constexpr uint64_t kLeaveHoldNs = 2'000'000'000;
        std::array<uint64_t, kMaxPlayers> g_leaveHeldSinceNs{};
        // The gamepad on each controller (window thread only).
        std::array<SDL_Gamepad*, kMaxPlayers> g_pads{};
        uint32_t g_virtualPads = 0;      // H3_VIRTUAL_PADS: bits of controllers with no gamepad
        bool g_keyboardOwnPlayer = false;  // the first gamepad is controller 1, not merged into 0
        bool g_log = false;

        // A stick axis from two keys (full deflection either way).
        int16_t Axis(bool negative, bool positive)
        {
            return int16_t(positive ? kStickMax : 0) - int16_t(negative ? kStickMax : 0);
        }

        // The larger deflection of keyboard and gamepad.
        int16_t Merge(int16_t a, int16_t b) { return std::abs(int32_t(a)) >= std::abs(int32_t(b)) ? a : b; }

        // Gives every connected gamepad that has no controller yet the
        // lowest free one. A controller keeps its gamepad until it's
        // unplugged, so players don't swap when someone else's drops out.
        void AssignGamepads()
        {
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            for (int i = 0; ids && i < count; i++)
            {
                bool assigned = std::any_of(g_pads.begin(), g_pads.end(),
                    [&](SDL_Gamepad* pad) { return pad && SDL_GetGamepadID(pad) == ids[i]; });
                if (assigned)
                    continue;
                for (uint32_t player = g_keyboardOwnPlayer ? 1 : 0; player < kMaxPlayers; player++)
                {
                    if (g_pads[player] || (g_virtualPads & (1u << player)))
                        continue;
                    if ((g_pads[player] = SDL_OpenGamepad(ids[i])))
                        fprintf(stderr, "[input] controller %u: %s\n", player + 1, SDL_GetGamepadName(g_pads[player]));
                    break;
                }
            }
            SDL_free(ids);
        }

        // Adds a gamepad's buttons, triggers and sticks to a state.
        void AddGamepad(GamepadState& state, SDL_Gamepad* pad)
        {
            struct PadButton
            {
                SDL_GamepadButton button;
                uint16_t bit;
            };
            static constexpr PadButton kPadButtons[] = {
                { SDL_GAMEPAD_BUTTON_SOUTH, kA },
                { SDL_GAMEPAD_BUTTON_EAST, kB },
                { SDL_GAMEPAD_BUTTON_WEST, kX },
                { SDL_GAMEPAD_BUTTON_NORTH, kY },
                { SDL_GAMEPAD_BUTTON_BACK, kBack },
                { SDL_GAMEPAD_BUTTON_START, kStart },
                { SDL_GAMEPAD_BUTTON_LEFT_STICK, kLeftThumb },
                { SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRightThumb },
                { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLeftShoulder },
                { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRightShoulder },
                { SDL_GAMEPAD_BUTTON_DPAD_UP, kDpadUp },
                { SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDpadDown },
                { SDL_GAMEPAD_BUTTON_DPAD_LEFT, kDpadLeft },
                { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kDpadRight },
            };
            for (const PadButton& mapping : kPadButtons)
                if (SDL_GetGamepadButton(pad, mapping.button))
                    state.buttons |= mapping.bit;
            // SDL triggers are 0..32767; sticks are Y-down, XInput's Y-up.
            static constexpr int32_t kSdlAxisMax = 32767;
            auto trigger = [](int16_t value) { return uint8_t(std::clamp(int32_t(value), 0, kSdlAxisMax) * kTriggerMax / kSdlAxisMax); };
            auto invert = [](int16_t value) { return int16_t(std::clamp(-int32_t(value), -32768, 32767)); };
            state.leftTrigger = std::max(state.leftTrigger, trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)));
            state.rightTrigger = std::max(state.rightTrigger, trigger(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)));
            state.thumbLX = Merge(state.thumbLX, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX));
            state.thumbLY = Merge(state.thumbLY, invert(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)));
            state.thumbRX = Merge(state.thumbRX, SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX));
            state.thumbRY = Merge(state.thumbRY, invert(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY)));
        }
    }

    void Initialize()
    {
        g_log = EnvFlag("H3_LOG_INPUT");
        g_startTicks = SDL_GetTicks();
        const config::ControlSettings& controls = config::Active().controls;
        ParseBindings(controls);
        g_mouseSensitivity = controls.mouseSensitivity;
        g_mouseInvertY = controls.invertMouse ? -1.0f : 1.0f;
        if (const char* sensitivity = EnvString("H3_MOUSE_SENSITIVITY"))
            g_mouseSensitivity = std::max(float(atof(sensitivity)), 0.01f);
        if (const char* script = EnvString("H3_INPUT_SCRIPT"))
            ParseScript(script);
        g_keyboardOwnPlayer = controls.keyboardOwnPlayer;
        g_nativeMouse = controls.nativeMouseLook || EnvFlag("H3_NATIVE_MOUSE");
        g_toggleCrouch = controls.toggleCrouch;
        g_holdToZoom = controls.holdToZoom;
        g_menuCursor = controls.menuCursor && !EnvFlag("H3_NO_MENU_CURSOR");
        if (const char* pads = EnvString("H3_VIRTUAL_PADS"))
            for (uint32_t player = 1; player <= std::min<uint32_t>(uint32_t(atoi(pads)), kMaxPlayers - 1); player++)
                g_virtualPads |= 1u << player;
        AssignGamepads();
    }

    void HandleEvent(const SDL_Event& event)
    {
        // Which device player 1 used last (gamepads on other controllers
        // don't count, nor does mouse motion that isn't looking).
        switch (event.type)
        {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            g_keyboardLast.store(true);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (!g_keyboardOwnPlayer && g_pads[0] && SDL_GetGamepadID(g_pads[0]) == event.gbutton.which)
                g_keyboardLast.store(false);
            break;
        default:
            break;
        }

        switch (event.type)
        {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (g_menuMode)
            {
                // A click in a menu: A on the item under the cursor, B for
                // the right button; never a capture.
                uint64_t now = SDL_GetTicksNS();
                if (event.button.button == SDL_BUTTON_LEFT)
                {
                    hooks::RequestMenuClick();
                    g_menuClickAtNs = now;
                }
                else if (event.button.button == SDL_BUTTON_RIGHT)
                    g_menuBUntilNs = now + kClickNs;
                return;
            }
            if (SDL_Window* window = EventWindow(event); window && !SDL_GetWindowRelativeMouseMode(window))
            {
                SDL_SetWindowRelativeMouseMode(window, true);
                return;  // the capturing click isn't a press
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            // H3_LOG_INPUT: the raw key events too (repeats included), to
            // tell a doubled press from the keyboard apart from the game's.
            if (g_log)
                fprintf(stderr, "[input] key down %s%s\n", SDL_GetScancodeName(event.key.scancode), event.key.repeat ? " (repeat)" : "");
            if (event.key.scancode == SDL_SCANCODE_F1)
                if (SDL_Window* window = EventWindow(event))
                    SDL_SetWindowRelativeMouseMode(window, false);
            break;
        case SDL_EVENT_KEY_UP:
            if (g_log)
                fprintf(stderr, "[input] key up %s\n", SDL_GetScancodeName(event.key.scancode));
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (SDL_Window* window = EventWindow(event); window && SDL_GetWindowRelativeMouseMode(window))
            {
                if (g_nativeMouse)
                {
                    std::lock_guard lock(g_lookMutex);
                    g_lookDx += event.motion.xrel;
                    g_lookDy += event.motion.yrel;
                }
                else
                    g_mouseMotion.push_back({ SDL_GetTicksNS(), event.motion.xrel, event.motion.yrel });
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            // Whole notches only: a high-resolution wheel (or smooth
            // scrolling) sends several fractional events per notch, and
            // counting each as a notch switched weapons twice or more.
            // SDL accumulates the fractions into integer_y.
            if (g_menuMode)
                g_menuWheelSteps = std::clamp(g_menuWheelSteps + int(event.wheel.integer_y), -8, 8);
            else if (SDL_Window* window = EventWindow(event); window && SDL_GetWindowRelativeMouseMode(window))
                g_mouseWheel += std::abs(int(event.wheel.integer_y));
            break;
        default:
            break;
        }

        if (event.type == SDL_EVENT_GAMEPAD_ADDED)
            AssignGamepads();
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED)
        {
            for (uint32_t player = 0; player < kMaxPlayers; player++)
                if (g_pads[player] && SDL_GetGamepadID(g_pads[player]) == event.gdevice.which)
                {
                    fprintf(stderr, "[input] controller %u disconnected\n", player + 1);
                    SDL_CloseGamepad(g_pads[player]);
                    g_pads[player] = nullptr;
                }
            AssignGamepads();  // a gamepad left over (a fifth) takes the free controller
        }
    }

    namespace
    {
        // Menu mode follows hooks::MenuOpen(): the mouse is released (the
        // system cursor shows) and its position goes to the menu cursor;
        // when the menu closes, a mouse we released is captured again.
        void UpdateMenuCursor(uint64_t now, uint32_t frame)
        {
            SDL_Window* window = SDL_GetKeyboardFocus();
            bool menu = g_menuCursor && hooks::MenuOpen();
            if (menu != g_menuMode)
            {
                if (g_log || EnvFlag("H3_LOG_MENU"))
                    fprintf(stderr, "[input] frame %u: %s\n", frame, menu ? "menu open: the mouse points" : "menu closed: the mouse looks");
                g_menuMode = menu;
                g_menuWheelSteps = 0;
                g_menuCursorPlaced = false;
                if (menu && window && SDL_GetWindowRelativeMouseMode(window))
                {
                    SDL_SetWindowRelativeMouseMode(window, false);
                    int width = 0, height = 0;
                    SDL_GetWindowSize(window, &width, &height);
                    SDL_WarpMouseInWindow(window, width / 2.0f, height / 2.0f);
                    g_menuReleased = true;
                }
                else if (!menu)
                {
                    // Back to the game: capture the mouse again (also when
                        // the game started in the menus and it never was).
                    if (window && (g_menuReleased || SDL_GetMouseFocus() == window))
                        SDL_SetWindowRelativeMouseMode(window, true);
                    g_menuReleased = false;
                    hooks::SetMenuCursor(false, 0, 0);
                }
            }
            if (!menu)
                return;
            // Keep the pointer free and shown while the menu lasts (the
            // switch alone left it hidden, coming back from a game).
            if (window && SDL_GetWindowRelativeMouseMode(window))
            {
                SDL_SetWindowRelativeMouseMode(window, false);
                g_menuReleased = true;
            }
            if (!SDL_CursorVisible())
                SDL_ShowCursor();

            float windowX = 0, windowY = 0, frameX = 0, frameY = 0;
            SDL_GetMouseState(&windowX, &windowY);
            bool inside = window && SDL_GetMouseFocus() == window && video::WindowToFrame(windowX, windowY, frameX, frameY);
            // H3_TEST_MENU_CURSOR=<frame>:<x>,<y>;... (testing): the cursor
            // at those frame fractions from each frame on, the real mouse
            // ignored.
            if (const char* test = EnvString("H3_TEST_MENU_CURSOR"))
            {
                inside = false;
                for (const char* at = test; at && *at; at = strchr(at, ';') ? strchr(at, ';') + 1 : nullptr)
                {
                    unsigned from = 0;
                    float x = 0, y = 0;
                    if (sscanf(at, "%u:%f,%f", &from, &x, &y) == 3 && frame >= from)
                        inside = true, frameX = x, frameY = y;
                }
            }
            hooks::SetMenuCursor(inside, frameX, frameY, g_menuCursorPlaced);
            g_menuCursorPlaced = true;

            // H3_TEST_MENU_CLICKS=<frame>,... (testing): left clicks then
            // (in order; each on the first update from its frame on).
            static const char* testClick = EnvString("H3_TEST_MENU_CLICKS");
            if (testClick && *testClick && frame >= uint32_t(atoi(testClick)))
            {
                hooks::RequestMenuClick();
                g_menuClickAtNs = now;
                testClick = strchr(testClick, ',') ? strchr(testClick, ',') + 1 : nullptr;
            }

            // A left click becomes A once the item under the cursor has the
            // focus (a click off the items, unanswered, is dropped).
            if (g_menuClickAtNs)
            {
                if (hooks::TakeMenuClick())
                {
                    g_menuAUntilNs = now + kClickNs;
                    g_menuClickAtNs = 0;
                }
                else if (now - g_menuClickAtNs > 250'000'000)
                {
                    hooks::CancelMenuClick();
                    g_menuClickAtNs = 0;
                }
            }

            // One D-pad press per wheel notch (wheel up = up; over a slider,
            // right), with a gap between presses so the game sees each.
            constexpr uint64_t kGapNs = 40'000'000;
            if (g_menuWheelSteps != 0 && now >= g_menuDpadUntilNs + kGapNs)
            {
                // Over a slider, the wheel moves it (up = right, more).
                if (hooks::MenuCursorOverSlider())
                    g_menuDpadButton = g_menuWheelSteps > 0 ? kDpadRight : kDpadLeft;
                else
                    g_menuDpadButton = g_menuWheelSteps > 0 ? kDpadUp : kDpadDown;
                g_menuWheelSteps += g_menuWheelSteps > 0 ? -1 : 1;
                g_menuDpadUntilNs = now + kClickNs;
            }
        }
    }

    void Update(uint32_t frame)
    {
        std::array<GamepadState, kMaxPlayers> states;
        GamepadState& state = states[0];  // the keyboard and mouse are controller 1
        const bool* keys = SDL_GetKeyboardState(nullptr);
        uint64_t now = SDL_GetTicksNS();

        UpdateMenuCursor(now, frame);

        // Mouse buttons and the wheel only count while the mouse is captured
        // (the click that captures it isn't a press).
        SDL_Window* focus = SDL_GetMouseFocus();
        bool captured = focus && SDL_GetWindowRelativeMouseMode(focus);
        SDL_MouseButtonFlags mouse = captured ? SDL_GetMouseState(nullptr, nullptr) : 0;

        // A scroll is a ~100 ms press of whatever "Mouse Wheel" is bound to
        // (the game needs to see it held for a frame or two), then a 50 ms
        // release. Notches arriving during that press or release belong to
        // the same scroll and are dropped: queued, a two-notch flick became
        // two weapon switches 150 ms apart, the weapon dropping and coming
        // straight back up (the "switch animation plays twice" bug).
        constexpr uint64_t kWheelPressNs = 100'000'000, kWheelGapNs = 50'000'000;
        if (now < g_wheelPressUntilNs + kWheelGapNs)
            g_mouseWheel = 0;
        else if (g_mouseWheel > 0)
        {
            g_mouseWheel = 0;
            g_wheelPressUntilNs = now + kWheelPressNs;
        }
        bool wheel = now < g_wheelPressUntilNs;

        // Whether any binding of an action is held.
        auto held = [&](Action action)
        {
            for (const Binding& binding : g_bindings[size_t(action)])
            {
                switch (binding.kind)
                {
                case Binding::Kind::Key: if (keys[binding.key]) return true; break;
                case Binding::Kind::MouseButton: if (mouse & binding.mouseMask) return true; break;
                case Binding::Kind::Wheel: if (wheel) return true; break;
                }
            }
            return false;
        };

        for (size_t action = 0; action < config::kActionCount; action++)
            if (uint16_t button = ButtonFor(Action(action)); button && held(Action(action)))
                state.buttons |= button;

        // The menu cursor's clicks and wheel.
        if (now < g_menuAUntilNs)
            state.buttons |= kA;
        if (now < g_menuBUntilNs)
            state.buttons |= kB;
        if (now < g_menuDpadUntilNs)
            state.buttons |= g_menuDpadButton;

        // Toggle crouch: each press of the crouch key flips it.
        if (g_toggleCrouch)
        {
            bool down = held(Action::Crouch);
            if (down && !g_crouchHeld)
                g_crouched = !g_crouched;
            g_crouchHeld = down;
            state.buttons = uint16_t(g_crouched ? state.buttons | kLeftThumb : state.buttons & ~kLeftThumb);
        }
        // Hold to zoom. The key going down clicks zoom (the weapon's first
        // level); another zoom key pressed while one is held clicks again
        // (a sniper rifle's second level). The key coming up asks the game
        // to unzoom outright (hooks::RequestPlayer1Unzoom: the requested
        // zoom level to none, no stepping through levels), once the game
        // has taken the last click, and again while it's still zoomed.
        if (g_holdToZoom)
        {
            int zoomKeys = 0;
            for (const Binding& binding : g_bindings[size_t(Action::Zoom)])
                if ((binding.kind == Binding::Kind::Key && keys[binding.key]) ||
                    (binding.kind == Binding::Kind::MouseButton && (mouse & binding.mouseMask)))
                    zoomKeys++;
            bool down = zoomKeys > 0;
            if (down && !g_zoomHeld)
            {
                g_zoomClickUntilNs = now + kClickNs;
                g_unzoomPending = false;
            }
            else if (down && zoomKeys > g_zoomKeys && now >= g_zoomClickUntilNs)
                g_zoomClickUntilNs = now + kClickNs;  // the next zoom level
            else if (!down && g_zoomHeld)
            {
                g_unzoomPending = true;
                g_unzoomClicks = 0;
                g_unzoomCheckNs = std::max(now, g_zoomClickUntilNs) + kSettleNs;
            }
            g_zoomKeys = zoomKeys;
            if (g_unzoomPending && now >= g_unzoomCheckNs)
            {
                if (hooks::Player1Zoomed() && g_unzoomClicks < kMaxUnzoomClicks)
                {
                    hooks::RequestPlayer1Unzoom();
                    g_unzoomClicks++;
                    g_unzoomCheckNs = now + kSettleNs;
                }
                else
                    g_unzoomPending = false;
            }
            g_zoomHeld = down;
            state.buttons = uint16_t(now < g_zoomClickUntilNs ? state.buttons | kRightThumb : state.buttons & ~kRightThumb);
        }
        state.leftTrigger = held(Action::Grenade) ? kTriggerMax : 0;
        state.rightTrigger = held(Action::Fire) ? kTriggerMax : 0;
        state.thumbLX = Axis(held(Action::MoveLeft), held(Action::MoveRight));
        state.thumbLY = Axis(held(Action::MoveBack), held(Action::MoveForward));
        state.thumbRX = Axis(held(Action::LookLeft), held(Action::LookRight));
        state.thumbRY = Axis(held(Action::LookDown), held(Action::LookUp));

        // Mouse movement (captured) adds to the look stick.
        std::erase_if(g_mouseMotion, [&](const MouseMotion& m) { return now - m.ticksNs > kMouseWindowNs; });
        float dx = 0, dy = 0;
        for (const MouseMotion& m : g_mouseMotion)
            dx += m.dx, dy += m.dy;
        constexpr float kWindowSeconds = float(kMouseWindowNs) / 1e9f;
        state.thumbRX = Merge(state.thumbRX, MouseAxis(dx / kWindowSeconds));
        state.thumbRY = Merge(state.thumbRY, MouseAxis(-dy * g_mouseInvertY / kWindowSeconds));

        // Scripted presses, on whichever controller they name.
        double seconds = double(SDL_GetTicks() - g_startTicks) / 1000.0;
        uint32_t unplugged = 0;
        for (const ScriptedPress& press : g_script)
        {
            double at = press.frames ? double(frame) : seconds;
            if (at < press.start || at >= press.end)
                continue;
            if (press.unplug)
                unplugged |= 1u << press.player;
            GamepadState& target = states[press.player];
            target.buttons |= press.button;
            switch (press.analog)
            {
            case ScriptedAnalog::LeftTrigger: target.leftTrigger = kTriggerMax; break;
            case ScriptedAnalog::RightTrigger: target.rightTrigger = kTriggerMax; break;
            case ScriptedAnalog::LeftLeft: target.thumbLX = -kStickMax; break;
            case ScriptedAnalog::LeftRight: target.thumbLX = kStickMax; break;
            case ScriptedAnalog::LeftDown: target.thumbLY = -kStickMax; break;
            case ScriptedAnalog::LeftUp: target.thumbLY = kStickMax; break;
            case ScriptedAnalog::RightLeft: target.thumbRX = -kStickMax; break;
            case ScriptedAnalog::RightRight: target.thumbRX = kStickMax; break;
            case ScriptedAnalog::RightDown: target.thumbRY = -kStickMax; break;
            case ScriptedAnalog::RightUp: target.thumbRY = kStickMax; break;
            case ScriptedAnalog::MouseUp:
            case ScriptedAnalog::MouseDown:
            case ScriptedAnalog::MouseLeft:
            case ScriptedAnalog::MouseRight:
                if (g_nativeMouse)
                {
                    std::lock_guard lock(g_lookMutex);
                    g_lookDx += press.analog == ScriptedAnalog::MouseLeft ? -kScriptedMouseCounts
                        : press.analog == ScriptedAnalog::MouseRight      ? kScriptedMouseCounts
                                                                          : 0;
                    g_lookDy += press.analog == ScriptedAnalog::MouseUp ? -kScriptedMouseCounts
                        : press.analog == ScriptedAnalog::MouseDown     ? kScriptedMouseCounts
                                                                        : 0;
                }
                break;
            case ScriptedAnalog::None: break;
            }
        }

        uint32_t present = 1 | g_virtualPads;
        for (uint32_t player = 0; player < kMaxPlayers; player++)
            if (g_pads[player])
            {
                AddGamepad(states[player], g_pads[player]);
                present |= 1u << player;
            }
        present &= ~unplugged | 1u;
        for (uint32_t player = 1; player < kMaxPlayers; player++)
        {
            uint32_t bit = 1u << player;
            if (!(present & bit))
            {
                states[player] = {};
                continue;
            }
            // What the controller holds, before any of it is kept from the game.
            const GamepadState held = states[player];
            constexpr uint16_t kLeaveButtons = kBack | kStart;
            if ((g_joined & bit) && (held.buttons & kLeaveButtons) == kLeaveButtons)
            {
                states[player].buttons &= uint16_t(~kLeaveButtons);
                if (!g_leaveHeldSinceNs[player])
                    g_leaveHeldSinceNs[player] = now;
                else if (now - g_leaveHeldSinceNs[player] >= kLeaveHoldNs)
                {
                    g_joined &= ~bit;
                    g_swallow |= bit;
                    g_leaveHeldSinceNs[player] = 0;
                    fprintf(stderr, "[input] controller %u left\n", player + 1);
                }
            }
            else
                g_leaveHeldSinceNs[player] = 0;
            if (!(g_joined & bit) && !(g_swallow & bit) && (held.buttons & kStart))
            {
                g_joined |= bit;
                g_swallow |= bit;
                fprintf(stderr, "[input] controller %u joined\n", player + 1);
            }
            if (g_swallow & bit)
            {
                bool idle = held.buttons == 0 && !held.leftTrigger && !held.rightTrigger;
                if (idle)
                    g_swallow &= ~bit;
                else
                    states[player] = {};
            }
        }
        // A player whose controller is unplugged stays signed in, as on the
        // 360: the game pauses with its own "reconnect your controller"
        // message and goes on when it's back.
        g_connected.store(g_joined & present);
        g_signedIn.store(g_joined);

        std::lock_guard lock(g_mutex);
        for (uint32_t player = 0; player < kMaxPlayers; player++)
        {
            GamepadState& next = states[player];
            GamepadState& last = g_states[player];
            bool changed = next.buttons != last.buttons || next.leftTrigger != last.leftTrigger ||
                next.rightTrigger != last.rightTrigger || next.thumbLX != last.thumbLX || next.thumbLY != last.thumbLY ||
                next.thumbRX != last.thumbRX || next.thumbRY != last.thumbRY;
            if (!changed)
                continue;
            next.packetNumber = last.packetNumber + 1;
            last = next;
            if (g_log)
                fprintf(stderr, "[input] frame %u controller %u buttons %04X triggers %u %u left %d %d right %d %d\n", frame,
                    player + 1, next.buttons, next.leftTrigger, next.rightTrigger, next.thumbLX, next.thumbLY, next.thumbRX,
                    next.thumbRY);
        }
    }

    GamepadState GetState(uint32_t player)
    {
        if (player >= kMaxPlayers)
            return {};
        std::lock_guard lock(g_mutex);
        return g_states[player];
    }

    uint32_t ConnectedMask()
    {
        return g_connected.load();
    }

    uint32_t JoinedMask()
    {
        return g_signedIn.load();
    }

    bool KeyboardLastUsed()
    {
        return g_keyboardLast.load();
    }

    std::string KeyNameFor(uint16_t button)
    {
        Action action;
        if (button == kLeftTriggerKey)
            action = Action::Grenade;
        else if (button == kRightTriggerKey)
            action = Action::Fire;
        else
        {
            size_t found = config::kActionCount;
            for (size_t a = 0; a < config::kActionCount && found == config::kActionCount; a++)
                if (ButtonFor(Action(a)) == button)
                    found = a;
            if (found == config::kActionCount)
                return {};
            action = Action(found);
        }
        const std::vector<std::string>& names = config::Active().controls.bindings[size_t(action)];
        return names.empty() ? std::string() : names.front();
    }

    bool NativeMouseLook()
    {
        return g_nativeMouse;
    }

    void TakeMouseLook(float& yaw, float& pitch)
    {
        std::lock_guard lock(g_lookMutex);
        yaw = -g_lookDx * kRadiansPerCount * g_mouseSensitivity;  // the game's yaw grows to the left
        pitch = -g_lookDy * kRadiansPerCount * g_mouseSensitivity * g_mouseInvertY;
        g_lookDx = g_lookDy = 0;
    }
}
