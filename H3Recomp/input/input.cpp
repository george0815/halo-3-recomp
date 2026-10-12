// XInput emulation (input.h). Keyboard and mouse bindings come from the
// [controls] section of settings.ini (config/settings.h, edited with the
// launcher; the defaults are listed in the README, "Controls"). The
// keyboard, the mouse and a gamepad are merged: buttons OR together, sticks
// take the larger deflection.

#include "input/input.h"
#include "config/env.h"
#include "config/settings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
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
        // Analog controls a script can hold (full deflection or pull), by
        // name: left/right stick directions and the triggers.
        enum class ScriptedAnalog : uint8_t
        {
            None, LeftUp, LeftDown, LeftLeft, LeftRight, RightUp, RightDown, RightLeft, RightRight, LeftTrigger, RightTrigger,
        };
        struct ScriptedPress
        {
            double start, end;
            uint16_t button;  // 0 for an analog control
            bool frames;
            ScriptedAnalog analog = ScriptedAnalog::None;
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
                { "rt", ScriptedAnalog::RightTrigger },
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
                ScriptedPress press{ atof(start.c_str()), atof(end.c_str()), ButtonNamed(name), frames, AnalogNamed(name) };
                if (press.button || press.analog != ScriptedAnalog::None)
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
        float g_mouseSensitivity = 1.0f;
        float g_mouseInvertY = 1.0f;    // -1 to invert vertical mouse look
        int g_mouseWheel = 0;           // notches not yet turned into presses
        uint64_t g_wheelPressUntilNs = 0;

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
        GamepadState g_state;
        SDL_Gamepad* g_gamepad = nullptr;
        bool g_log = false;

        // A stick axis from two keys (full deflection either way).
        int16_t Axis(bool negative, bool positive)
        {
            return int16_t(positive ? kStickMax : 0) - int16_t(negative ? kStickMax : 0);
        }

        // The larger deflection of keyboard and gamepad.
        int16_t Merge(int16_t a, int16_t b) { return std::abs(int32_t(a)) >= std::abs(int32_t(b)) ? a : b; }

        // Opens the first connected gamepad if none is open.
        void OpenFirstGamepad()
        {
            if (g_gamepad)
                return;
            int count = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&count);
            if (ids && count > 0)
            {
                g_gamepad = SDL_OpenGamepad(ids[0]);
                if (g_gamepad)
                    fprintf(stderr, "[input] gamepad: %s\n", SDL_GetGamepadName(g_gamepad));
            }
            SDL_free(ids);
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
        OpenFirstGamepad();
    }

    void HandleEvent(const SDL_Event& event)
    {
        switch (event.type)
        {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (SDL_Window* window = EventWindow(event); window && !SDL_GetWindowRelativeMouseMode(window))
            {
                SDL_SetWindowRelativeMouseMode(window, true);
                return;  // the capturing click isn't a press
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.scancode == SDL_SCANCODE_F1)
                if (SDL_Window* window = EventWindow(event))
                    SDL_SetWindowRelativeMouseMode(window, false);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (SDL_Window* window = EventWindow(event); window && SDL_GetWindowRelativeMouseMode(window))
                g_mouseMotion.push_back({ SDL_GetTicksNS(), event.motion.xrel, event.motion.yrel });
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (SDL_Window* window = EventWindow(event); window && SDL_GetWindowRelativeMouseMode(window))
                g_mouseWheel += event.wheel.y != 0 ? 1 : 0;
            break;
        default:
            break;
        }

        if (event.type == SDL_EVENT_GAMEPAD_ADDED)
            OpenFirstGamepad();
        else if (event.type == SDL_EVENT_GAMEPAD_REMOVED && g_gamepad &&
            SDL_GetGamepadID(g_gamepad) == event.gdevice.which)
        {
            SDL_CloseGamepad(g_gamepad);
            g_gamepad = nullptr;
            OpenFirstGamepad();
        }
    }

    void Update(uint32_t frame)
    {
        GamepadState state;
        const bool* keys = SDL_GetKeyboardState(nullptr);
        uint64_t now = SDL_GetTicksNS();

        // Mouse buttons and the wheel only count while the mouse is captured
        // (the click that captures it isn't a press).
        SDL_Window* focus = SDL_GetMouseFocus();
        bool captured = focus && SDL_GetWindowRelativeMouseMode(focus);
        SDL_MouseButtonFlags mouse = captured ? SDL_GetMouseState(nullptr, nullptr) : 0;

        // Each wheel notch is a ~100 ms press of whatever "Mouse Wheel" is
        // bound to (the game needs to see it held for a frame or two), with
        // a 50 ms release between notches.
        constexpr uint64_t kWheelPressNs = 100'000'000, kWheelGapNs = 50'000'000;
        if (g_mouseWheel > 0 && now >= g_wheelPressUntilNs + kWheelGapNs)
        {
            g_mouseWheel--;
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
        double seconds = double(SDL_GetTicks() - g_startTicks) / 1000.0;
        bool scripted[size_t(ScriptedAnalog::RightTrigger) + 1] = {};
        for (const ScriptedPress& press : g_script)
            if ((press.frames ? double(frame) : seconds) >= press.start && (press.frames ? double(frame) : seconds) < press.end)
            {
                state.buttons |= press.button;
                scripted[size_t(press.analog)] = true;
            }
        auto analog = [&](ScriptedAnalog control) { return scripted[size_t(control)]; };
        state.leftTrigger = held(Action::Grenade) || analog(ScriptedAnalog::LeftTrigger) ? kTriggerMax : 0;
        state.rightTrigger = held(Action::Fire) || analog(ScriptedAnalog::RightTrigger) ? kTriggerMax : 0;
        state.thumbLX = Axis(held(Action::MoveLeft) || analog(ScriptedAnalog::LeftLeft), held(Action::MoveRight) || analog(ScriptedAnalog::LeftRight));
        state.thumbLY = Axis(held(Action::MoveBack) || analog(ScriptedAnalog::LeftDown), held(Action::MoveForward) || analog(ScriptedAnalog::LeftUp));
        state.thumbRX = Axis(held(Action::LookLeft) || analog(ScriptedAnalog::RightLeft), held(Action::LookRight) || analog(ScriptedAnalog::RightRight));
        state.thumbRY = Axis(held(Action::LookDown) || analog(ScriptedAnalog::RightDown), held(Action::LookUp) || analog(ScriptedAnalog::RightUp));

        // Mouse movement (captured) adds to the look stick.
        std::erase_if(g_mouseMotion, [&](const MouseMotion& m) { return now - m.ticksNs > kMouseWindowNs; });
        float dx = 0, dy = 0;
        for (const MouseMotion& m : g_mouseMotion)
            dx += m.dx, dy += m.dy;
        constexpr float kWindowSeconds = float(kMouseWindowNs) / 1e9f;
        state.thumbRX = Merge(state.thumbRX, MouseAxis(dx / kWindowSeconds));
        state.thumbRY = Merge(state.thumbRY, MouseAxis(-dy * g_mouseInvertY / kWindowSeconds));

        if (g_gamepad)
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
                if (SDL_GetGamepadButton(g_gamepad, mapping.button))
                    state.buttons |= mapping.bit;
            // SDL triggers are 0..32767; sticks are Y-down, XInput's Y-up.
            static constexpr int32_t kSdlAxisMax = 32767;
            auto trigger = [](int16_t value) { return uint8_t(std::clamp(int32_t(value), 0, kSdlAxisMax) * kTriggerMax / kSdlAxisMax); };
            auto invert = [](int16_t value) { return int16_t(std::clamp(-int32_t(value), -32768, 32767)); };
            state.leftTrigger = std::max(state.leftTrigger, trigger(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)));
            state.rightTrigger = std::max(state.rightTrigger, trigger(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)));
            state.thumbLX = Merge(state.thumbLX, SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX));
            state.thumbLY = Merge(state.thumbLY, invert(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY)));
            state.thumbRX = Merge(state.thumbRX, SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
            state.thumbRY = Merge(state.thumbRY, invert(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY)));
        }

        std::lock_guard lock(g_mutex);
        bool changed = state.buttons != g_state.buttons || state.leftTrigger != g_state.leftTrigger ||
            state.rightTrigger != g_state.rightTrigger || state.thumbLX != g_state.thumbLX || state.thumbLY != g_state.thumbLY ||
            state.thumbRX != g_state.thumbRX || state.thumbRY != g_state.thumbRY;
        if (changed)
        {
            state.packetNumber = g_state.packetNumber + 1;
            g_state = state;
            if (g_log)
                fprintf(stderr, "[input] buttons %04X triggers %u %u left %d %d right %d %d\n", state.buttons, state.leftTrigger,
                    state.rightTrigger, state.thumbLX, state.thumbLY, state.thumbRX, state.thumbRY);
        }
    }

    GamepadState GetState()
    {
        std::lock_guard lock(g_mutex);
        return g_state;
    }
}
