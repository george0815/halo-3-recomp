// User settings: the options the launcher (H3Launcher) edits and the game
// reads at boot. Stored as a small INI file, settings.ini, next to
// default.xex (private/settings.ini by default):
//
//     [video]
//     width = 1280
//     fullscreen = false
//     render_scale = 2
//
//     [profile]
//     gamertag = Player
//
//     [controls]
//     fire = Left Ctrl, E, Mouse Left
//
// This file has no SDL or Qt dependency so both programs can share it (it is
// built as the H3Config library). Anything missing from the file keeps its
// default, and unknown keys are ignored, so old files keep working.

#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace h3::config
{
    // Every input the game sees that can be bound to keys or mouse buttons:
    // the XInput buttons, the triggers, and the four directions of each
    // stick. A gamepad always drives these directly; the bindings below are
    // for the keyboard and mouse.
    enum class Action : uint8_t
    {
        MoveForward,
        MoveBack,
        MoveLeft,
        MoveRight,
        LookUp,
        LookDown,
        LookLeft,
        LookRight,
        Fire,            // right trigger
        Grenade,         // left trigger
        Jump,            // A
        Melee,           // B
        Equipment,       // X
        SwitchWeapon,    // Y
        LeftBumper,      // LB
        RightBumper,     // RB
        Crouch,          // left stick click
        Zoom,            // right stick click
        Pause,           // Start
        Scoreboard,      // Back
        DpadUp,
        DpadDown,
        DpadLeft,
        DpadRight,
        Count,
    };
    constexpr size_t kActionCount = size_t(Action::Count);

    // Static description of one action.
    struct ActionInfo
    {
        const char* key;    // INI key in [controls], e.g. "fire"
        const char* label;  // shown in the launcher, e.g. "Fire"
        const char* pad;    // the controller input it stands for, e.g. "RT"
    };
    const ActionInfo& Describe(Action action);

    // Binding names. Keys use SDL's scancode names ("Space", "Left Shift",
    // "A"; SDL_GetScancodeFromName reads them back). The mouse has its own:
    constexpr std::string_view kMouseLeft = "Mouse Left";
    constexpr std::string_view kMouseRight = "Mouse Right";
    constexpr std::string_view kMouseMiddle = "Mouse Middle";
    constexpr std::string_view kMouseX1 = "Mouse X1";
    constexpr std::string_view kMouseX2 = "Mouse X2";
    constexpr std::string_view kMouseWheel = "Mouse Wheel";  // each notch is a short press

    // Most bindings an action can have (the launcher shows this many slots).
    constexpr size_t kMaxBindings = 3;

    // Limits the launcher enforces and the game clamps to.
    constexpr int kMinWindowSize = 320;
    // Render targets grow with the square of the scale (a 16-bit float
    // target is 25 MB at 1x, 225 MB at 3x) and Halo keeps a dozen or more.
    constexpr int kMaxRenderScale = 3;   // 3x = 3840x2160 for a 720p game
    constexpr size_t kMaxGamertag = 15;  // the 360's limit (XUSER_NAME_SIZE - 1)
    // Game tick rates (frame_rate): the original 30, or 60 (experimental;
    // hooks/tick_rate.cpp). Halo 3 renders one frame per game tick.
    constexpr int kOriginalTickRate = 30;
    constexpr int kUnlockedTickRate = 60;

    // Post-process anti-aliasing on the finished frame (Halo 3 renders
    // without any; render scale > 1 supersamples on top).
    enum class AntiAliasing
    {
        Off,
        Fxaa,
    };

    // [video]: the window, rendering and display options.
    struct VideoSettings
    {
        int width = 1280;   // window size in windowed mode (the game renders 1280x720)
        int height = 720;
        bool fullscreen = false;  // borderless desktop-size fullscreen
        bool vsync = true;
        // Internal resolution multiplier: the game's render targets are
        // rendered at renderScale x their native size, then scaled to the
        // window. 1 = native 720p.
        int renderScale = 1;
        // Recreate the shaders and pipelines this machine has seen before at
        // start, on background threads (gpu/renderer/pipelines.cpp).
        bool precompileShaders = true;
        // Compile new shaders and pipelines on background threads and skip
        // what needs them meanwhile: no stutter, brief pop-in.
        bool asyncShaders = false;
        // Game ticks (and so frames) per second: kOriginalTickRate or
        // kUnlockedTickRate. 60 runs the whole simulation at 60 Hz, which
        // the engine supports (it reads the rate everywhere) but Halo 3
        // shipped at 30, so some content may misbehave.
        int frameRate = kOriginalTickRate;
        AntiAliasing antiAliasing = AntiAliasing::Fxaa;
        // Skip the intro video at startup (kernel/vfs.cpp hides it).
        bool skipIntro = false;
        // The crosshair (and the aim) in the middle of the screen instead
        // of below it (hooks/crosshair.cpp).
        bool centeredCrosshair = false;
        // English text (hooks/pc_text.cpp): "PC" for "Xbox 360 console" and
        // the like, and the keys bound to buttons in prompts while playing
        // on the keyboard. Off: the 360's text and button icons.
        bool pcWording = true;
        bool pcKeyNames = true;
        // Horizontal field of view in degrees, 60-120 (hooks/fov.cpp); the
        // game's is 70.
        float fov = 70.0f;
    };

    // [profile]: gamertags, the gamer picture and the options Halo 3 copies
    // into a new profile.
    struct ProfileSettings
    {
        std::string gamertag = "Player";
        // The profiles signed in on controllers 2-4 (split screen).
        std::array<std::string, 3> otherGamertags = { "Player 2", "Player 3", "Player 4" };
        // Gamer picture (an image file path). Shown by the launcher; the game
        // never asks for it (Halo 3 shows emblems, which are set in game).
        std::string picture;
        // Standard 360 profile options. Halo 3 copies these into a new
        // player profile; after that its own settings menu wins.
        bool invertLook = false;
        bool vibration = true;
        bool autoAim = true;
        bool autoCenter = true;
        // Every armor piece selectable: the profile reports all of Halo 3's
        // achievements as earned (kernel/xam_content.cpp), which is what the
        // armor unlocks check.
        bool unlockAllArmor = false;
    };

    // [controls]: key bindings (one list per Action) and the mouse options.
    struct ControlSettings
    {
        std::array<std::vector<std::string>, kActionCount> bindings;
        float mouseSensitivity = 1.0f;  // scales mouse look speed
        float vehicleMouseSensitivity = 1.0f;  // native mouse look: times that in a seat (vehicles, turrets)
        bool invertMouse = false;       // mouse only; invertLook flips everything
        // Split screen: the keyboard and mouse are player 1 on their own and
        // the first gamepad is player 2 (otherwise the first gamepad is
        // player 1 too).
        bool keyboardOwnPlayer = false;
        // The mouse turns the view directly, as in MCC's PC version, instead
        // of driving the right stick (hooks/mouse_look.cpp).
        bool nativeMouseLook = false;
        // Keyboard and mouse: the crouch key toggles crouching (the game's is
        // held), and zoom lasts while the zoom key is held (the game's
        // toggles on each press).
        bool toggleCrouch = false;
        bool holdToZoom = false;
        // Menus: the mouse points and clicks (hooks/menu_cursor.cpp).
        bool menuCursor = true;
    };

    // [debug]: the log file, mission unlocks and H3_* switches.
    struct DebugSettings
    {
        // H3_* environment switches (README, "Debugging") the game sets for
        // itself at boot, name -> value (e.g. H3_LOG_FPS -> 1). A variable
        // already set in the real environment wins.
        std::map<std::string, std::string> environment;
        // Send stdout and stderr to h3recomp.log next to default.xex.
        bool logToFile = false;
        // Every campaign mission selectable in the campaign lobby
        // (hooks/campaign.cpp sets the profile's unlock mask).
        bool unlockAllLevels = false;
    };

    // System link (net/link.cpp): the game's LAN traffic carried over one
    // UDP port, to the LAN and to friends' addresses.
    constexpr uint16_t kDefaultLinkPort = 41001;
    struct NetworkSettings
    {
        bool systemLink = false;          // off: no network cable, as before
        uint16_t port = kDefaultLinkPort;  // the UDP port this game listens on
        // Friends to play with over the internet: "host" or "host:port"
        // (their port, if not the default).
        std::vector<std::string> peers;
    };

    // Everything in settings.ini, one struct per section.
    struct Settings
    {
        VideoSettings video;
        ProfileSettings profile;
        ControlSettings controls;
        NetworkSettings network;
        DebugSettings debug;
    };

    // Factory defaults, including the default key layout.
    Settings Defaults();

    // Reads path into settings (starting from Defaults()). Returns false if
    // the file can't be opened (settings are then the defaults).
    bool Load(const std::filesystem::path& path, Settings& settings);
    // Writes settings to path (replacing it atomically). Returns false on error.
    bool Save(const std::filesystem::path& path, const Settings& settings);

    // The file name, next to default.xex.
    constexpr const char* kFileName = "settings.ini";
    // The log file DebugSettings::logToFile writes, next to default.xex.
    constexpr const char* kLogFileName = "h3recomp.log";

    // The settings the game is running with: loaded once at boot by main.cpp
    // (SetActive), read from anywhere afterwards.
    const Settings& Active();
    void SetActive(const Settings& settings);
}
