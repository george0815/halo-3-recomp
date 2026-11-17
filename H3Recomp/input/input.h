// XInput emulation: controllers 0-3 for the game (split screen). Controller
// 0 is the keyboard and mouse, merged with the first SDL gamepad unless
// [controls] keyboard_own_player is set; the other gamepads are controllers
// 1-3 in the order they connect, each keeping its controller until it's
// unplugged. Controllers 1-3 join by pressing START.

#pragma once

#include <cstdint>
#include <string>

union SDL_Event;

// Sampled on the window thread, read from any thread.
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

    // Full deflection of a stick axis / a trigger in XINPUT_GAMEPAD.
    constexpr int16_t kStickMax = 32767;
    constexpr uint8_t kTriggerMax = 255;

    // One XINPUT_GAMEPAD snapshot.
    struct GamepadState
    {
        uint32_t packetNumber = 0;  // changes whenever the rest changes
        uint16_t buttons = 0;
        uint8_t leftTrigger = 0, rightTrigger = 0;
        int16_t thumbLX = 0, thumbLY = 0, thumbRX = 0, thumbRY = 0;
    };

    // Reads the H3_* input options and opens a gamepad if one is connected.
    // Window thread, after SDL is initialized.
    void Initialize();
    // Window thread: gamepads coming and going, mouse capture and motion.
    void HandleEvent(const SDL_Event& event);
    // Window thread, once per event loop iteration (frame: frames presented
    // so far, for scripted input): rebuilds the merged state.
    void Update(uint32_t frame);

    constexpr uint32_t kMaxPlayers = 4;

    // Any thread (the game's XInputGetState): controller `player`'s latest
    // state (0-3; empty for one that isn't connected).
    GamepadState GetState(uint32_t player);
    // Any thread: one bit per controller the game can read (bit 0, the
    // keyboard, always set): joined and plugged in.
    uint32_t ConnectedMask();
    // Any thread: one bit per player who has joined (pressed START on
    // their controller; bit 0 always set). Their profile is signed in.
    uint32_t JoinedMask();

    // Native mouse look ([controls] native_mouse_look, or H3_NATIVE_MOUSE=1;
    // hooks/mouse_look.cpp): the mouse turns player 1 directly instead of
    // through the right stick.
    bool NativeMouseLook();
    // Game thread, once per tick: the mouse movement since the last call,
    // as yaw and pitch in radians (yaw right and pitch up positive, invert
    // mouse applied). Zero when native mouse look is off.
    void TakeMouseLook(float& yaw, float& pitch);

    // Any thread: whether player 1 last used the keyboard or mouse (true)
    // or a gamepad, for showing keys instead of buttons (hooks/pc_text.cpp).
    bool KeyboardLastUsed();
    // The name of the first key or mouse button bound to an XINPUT button
    // bit (kA...) or, for kLeftTriggerKey / kRightTriggerKey, a trigger;
    // empty if nothing is bound. Window thread or after Initialize.
    constexpr uint16_t kLeftTriggerKey = 0xFFFE, kRightTriggerKey = 0xFFFF;
    std::string KeyNameFor(uint16_t button);
}
