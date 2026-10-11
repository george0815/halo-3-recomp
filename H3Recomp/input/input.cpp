// XInput emulation (input.h). The keyboard/mouse layout is listed in the
// README ("Controls"). The keyboard, the mouse and a gamepad are merged:
// buttons OR together, sticks take the larger deflection.

#include "input/input.h"
#include "config/env.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace h3::input
{
    namespace
    {
        // Keyboard layout (README "Controls"): WASD left stick, IJKL right
        // stick (look), arrow keys the D-pad (menus), the rest buttons and
        // triggers. The mouse is below.
        struct KeyButton
        {
            SDL_Scancode key;
            uint16_t button;
        };
        constexpr KeyButton kKeyButtons[] = {
            { SDL_SCANCODE_SPACE, kA },
            { SDL_SCANCODE_RETURN, kA },
            { SDL_SCANCODE_BACKSPACE, kB },
            { SDL_SCANCODE_V, kB },
            { SDL_SCANCODE_F, kX },
            { SDL_SCANCODE_Q, kY },
            { SDL_SCANCODE_G, kLeftShoulder },
            { SDL_SCANCODE_R, kRightShoulder },
            { SDL_SCANCODE_ESCAPE, kStart },
            { SDL_SCANCODE_TAB, kBack },
            { SDL_SCANCODE_C, kLeftThumb },
            { SDL_SCANCODE_Z, kRightThumb },
            { SDL_SCANCODE_UP, kDpadUp },
            { SDL_SCANCODE_DOWN, kDpadDown },
            { SDL_SCANCODE_LEFT, kDpadLeft },
            { SDL_SCANCODE_RIGHT, kDpadRight },
        };

        // H3_INPUT_SCRIPT="20-20.2:down,f900-905:a": buttons held between two
        // times, in seconds since start or (with f) presented frames, for
        // testing without a keyboard.
        struct ScriptedPress
        {
            double start, end;
            uint16_t button;
            bool frames;
        };
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
                ScriptedPress press{ atof(start.c_str()), atof(end.c_str()), ButtonNamed(item.substr(colon + 1)), frames };
                if (press.button)
                    g_script.push_back(press);
                else
                    fprintf(stderr, "[input] unknown button in H3_INPUT_SCRIPT: %s\n", item.c_str());
            }
        }

        // Mouse: click the window to capture it (F1 or losing focus lets go).
        // Movement drives the right stick; the stick deflection follows the
        // mouse's speed over a short window, since the game samples the pad
        // only once per frame. H3_MOUSE_SENSITIVITY scales it (default 1).
        struct MouseMotion
        {
            uint64_t ticksNs;
            float dx, dy;
        };
        constexpr uint64_t kMouseWindowNs = 50'000'000;
        std::vector<MouseMotion> g_mouseMotion;  // window thread only
        float g_mouseSensitivity = 1.0f;
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
        for (const KeyButton& mapping : kKeyButtons)
            if (keys[mapping.key])
                state.buttons |= mapping.button;
        double seconds = double(SDL_GetTicks() - g_startTicks) / 1000.0;
        for (const ScriptedPress& press : g_script)
            if ((press.frames ? double(frame) : seconds) >= press.start && (press.frames ? double(frame) : seconds) < press.end)
                state.buttons |= press.button;
        state.leftTrigger = keys[SDL_SCANCODE_LSHIFT] ? kTriggerMax : 0;  // throw grenade
        state.rightTrigger = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_E] ? kTriggerMax : 0;  // fire
        state.thumbLX = Axis(keys[SDL_SCANCODE_A], keys[SDL_SCANCODE_D]);
        state.thumbLY = Axis(keys[SDL_SCANCODE_S], keys[SDL_SCANCODE_W]);
        state.thumbRX = Axis(keys[SDL_SCANCODE_J], keys[SDL_SCANCODE_L]);
        state.thumbRY = Axis(keys[SDL_SCANCODE_K], keys[SDL_SCANCODE_I]);

        // Mouse (captured): look, left button fires, right button zooms
        // (right stick click), middle button melees (B), the wheel switches
        // weapons (Y).
        uint64_t now = SDL_GetTicksNS();
        std::erase_if(g_mouseMotion, [&](const MouseMotion& m) { return now - m.ticksNs > kMouseWindowNs; });
        float dx = 0, dy = 0;
        for (const MouseMotion& m : g_mouseMotion)
            dx += m.dx, dy += m.dy;
        constexpr float kWindowSeconds = float(kMouseWindowNs) / 1e9f;
        state.thumbRX = Merge(state.thumbRX, MouseAxis(dx / kWindowSeconds));
        state.thumbRY = Merge(state.thumbRY, MouseAxis(-dy / kWindowSeconds));
        SDL_Window* focus = SDL_GetMouseFocus();
        if (focus && SDL_GetWindowRelativeMouseMode(focus))
        {
            SDL_MouseButtonFlags mouse = SDL_GetMouseState(nullptr, nullptr);
            if (mouse & SDL_BUTTON_LMASK)
                state.rightTrigger = kTriggerMax;
            if (mouse & SDL_BUTTON_RMASK)
                state.buttons |= kRightThumb;
            if (mouse & SDL_BUTTON_MMASK)
                state.buttons |= kB;
        }
        // Each wheel notch is a ~100 ms press of Y (the game needs to see it
        // held for a frame or two), with a 50 ms release between notches.
        constexpr uint64_t kWheelPressNs = 100'000'000, kWheelGapNs = 50'000'000;
        if (g_mouseWheel > 0 && now >= g_wheelPressUntilNs + kWheelGapNs)
        {
            g_mouseWheel--;
            g_wheelPressUntilNs = now + kWheelPressNs;
        }
        if (now < g_wheelPressUntilNs)
            state.buttons |= kY;

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
