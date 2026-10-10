#pragma once

#include <cstdint>

union SDL_Event;

// Controller 0 for the game's XInput calls, from the keyboard, the mouse and
// the first SDL gamepad (merged). Sampled on the window thread, read from any thread.
namespace h3::input
{
    // XINPUT_GAMEPAD button bits.
    enum : uint16_t
    {
        kDpadUp = 0x0001,
        kDpadDown = 0x0002,
        kDpadLeft = 0x0004,
        kDpadRight = 0x0008,
        kStart = 0x0010,
        kBack = 0x0020,
        kLeftThumb = 0x0040,
        kRightThumb = 0x0080,
        kLeftShoulder = 0x0100,
        kRightShoulder = 0x0200,
        kA = 0x1000,
        kB = 0x2000,
        kX = 0x4000,
        kY = 0x8000,
    };

    struct GamepadState
    {
        uint32_t packetNumber = 0;  // changes whenever the rest changes
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
    };

    void Initialize();
    void HandleEvent(const SDL_Event& event);  // gamepads coming and going, mouse capture and motion
    void Update(uint32_t frame);               // once per window loop iteration (frames presented so far)

    GamepadState GetState();
}
