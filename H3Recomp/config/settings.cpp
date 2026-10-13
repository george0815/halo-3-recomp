// User settings (settings.h): defaults, the INI reader/writer, and the
// active copy the game reads.

#include "config/settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace h3::config
{
    namespace
    {
        // In Action order. The INI keys are part of the file format: rename
        // one and existing settings files lose that binding.
        constexpr ActionInfo kActions[kActionCount] = {
            { "move_forward", "Move forward", "Left stick up" },
            { "move_back", "Move back", "Left stick down" },
            { "move_left", "Strafe left", "Left stick left" },
            { "move_right", "Strafe right", "Left stick right" },
            { "look_up", "Look up", "Right stick up" },
            { "look_down", "Look down", "Right stick down" },
            { "look_left", "Look left", "Right stick left" },
            { "look_right", "Look right", "Right stick right" },
            { "fire", "Fire", "RT" },
            { "grenade", "Throw grenade", "LT" },
            { "jump", "Jump / accept", "A" },
            { "melee", "Melee / back", "B" },
            { "equipment", "Use equipment", "X" },
            { "switch_weapon", "Switch weapon", "Y" },
            { "left_bumper", "Switch grenades / left hand", "LB" },
            { "right_bumper", "Reload / pick up", "RB" },
            { "crouch", "Crouch", "LS click" },
            { "zoom", "Zoom", "RS click" },
            { "pause", "Pause menu", "Start" },
            { "scoreboard", "Scoreboard", "Back" },
            { "dpad_up", "D-pad up", "D-pad up" },
            { "dpad_down", "D-pad down", "D-pad down" },
            { "dpad_left", "D-pad left", "D-pad left" },
            { "dpad_right", "D-pad right", "D-pad right" },
        };

        Settings g_active = Defaults();

        // Text with spaces and tabs removed from both ends.
        std::string Trim(std::string_view text)
        {
            size_t first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            size_t last = text.find_last_not_of(" \t\r\n");
            return std::string(text.substr(first, last - first + 1));
        }

        // "true"/"yes"/"on"/"1" (any case) are true; anything else false.
        bool ParseBool(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            return value == "true" || value == "yes" || value == "on" || value == "1";
        }

        // "Space, Left Ctrl" -> {"Space", "Left Ctrl"}. Empty items dropped,
        // at most kMaxBindings kept.
        std::vector<std::string> ParseBindings(std::string_view value)
        {
            std::vector<std::string> bindings;
            while (!value.empty() && bindings.size() < kMaxBindings)
            {
                size_t comma = value.find(',');
                std::string item = Trim(value.substr(0, comma));
                if (!item.empty())
                    bindings.push_back(std::move(item));
                if (comma == std::string_view::npos)
                    break;
                value.remove_prefix(comma + 1);
            }
            return bindings;
        }

        // Applies one "key = value" line from section to settings. Unknown
        // keys are ignored.
        void Apply(Settings& settings, const std::string& section, const std::string& key, const std::string& value)
        {
            if (section == "video")
            {
                VideoSettings& video = settings.video;
                if (key == "width")
                    video.width = std::max(atoi(value.c_str()), kMinWindowSize);
                else if (key == "height")
                    video.height = std::max(atoi(value.c_str()), kMinWindowSize);
                else if (key == "fullscreen")
                    video.fullscreen = ParseBool(value);
                else if (key == "vsync")
                    video.vsync = ParseBool(value);
                else if (key == "render_scale")
                    video.renderScale = std::clamp(atoi(value.c_str()), 1, kMaxRenderScale);
                else if (key == "precompile_shaders")
                    video.precompileShaders = ParseBool(value);
                else if (key == "async_shaders")
                    video.asyncShaders = ParseBool(value);
                else if (key == "frame_rate")
                    video.frameRate = atoi(value.c_str()) >= kUnlockedTickRate ? kUnlockedTickRate : kOriginalTickRate;
                else if (key == "anti_aliasing")
                    video.antiAliasing = value == "off" ? AntiAliasing::Off : AntiAliasing::Fxaa;
            }
            else if (section == "profile")
            {
                ProfileSettings& profile = settings.profile;
                if (key == "gamertag")
                    profile.gamertag = value.substr(0, kMaxGamertag);
                else if (key == "picture")
                    profile.picture = value;
                else if (key == "invert_look")
                    profile.invertLook = ParseBool(value);
                else if (key == "vibration")
                    profile.vibration = ParseBool(value);
                else if (key == "auto_aim")
                    profile.autoAim = ParseBool(value);
                else if (key == "auto_center")
                    profile.autoCenter = ParseBool(value);
            }
            else if (section == "debug")
            {
                DebugSettings& debug = settings.debug;
                if (key.rfind("H3_", 0) == 0)
                {
                    if (!value.empty())
                        debug.environment[key] = value;
                }
                else if (key == "log_to_file")
                    debug.logToFile = ParseBool(value);
                else if (key == "unlock_all_levels")
                    debug.unlockAllLevels = ParseBool(value);
            }
            else if (section == "controls")
            {
                ControlSettings& controls = settings.controls;
                if (key == "mouse_sensitivity")
                    controls.mouseSensitivity = std::clamp(float(atof(value.c_str())), 0.05f, 20.0f);
                else if (key == "invert_mouse")
                    controls.invertMouse = ParseBool(value);
                else
                    for (size_t i = 0; i < kActionCount; i++)
                        if (key == kActions[i].key)
                            controls.bindings[i] = ParseBindings(value);
            }
        }

        const char* BoolText(bool value) { return value ? "true" : "false"; }
    }

    const ActionInfo& Describe(Action action)
    {
        return kActions[size_t(action)];
    }

    Settings Defaults()
    {
        Settings settings;
        // The default keyboard and mouse layout (also in the README,
        // "Controls"): WASD to move, mouse or IJKL to look, arrow keys for
        // the D-pad (menus).
        auto bind = [&](Action action, std::vector<std::string> keys) { settings.controls.bindings[size_t(action)] = std::move(keys); };
        bind(Action::MoveForward, { "W" });
        bind(Action::MoveBack, { "S" });
        bind(Action::MoveLeft, { "A" });
        bind(Action::MoveRight, { "D" });
        bind(Action::LookUp, { "I" });
        bind(Action::LookDown, { "K" });
        bind(Action::LookLeft, { "J" });
        bind(Action::LookRight, { "L" });
        bind(Action::Fire, { std::string(kMouseLeft), "Left Ctrl", "E" });
        bind(Action::Grenade, { "Left Shift" });
        bind(Action::Jump, { "Space", "Return" });
        bind(Action::Melee, { std::string(kMouseMiddle), "V", "Backspace" });
        bind(Action::Equipment, { "F" });
        bind(Action::SwitchWeapon, { std::string(kMouseWheel), "Q" });
        bind(Action::LeftBumper, { "G" });
        bind(Action::RightBumper, { "R" });
        bind(Action::Crouch, { "C" });
        bind(Action::Zoom, { std::string(kMouseRight), "Z" });
        bind(Action::Pause, { "Escape" });
        bind(Action::Scoreboard, { "Tab" });
        bind(Action::DpadUp, { "Up" });
        bind(Action::DpadDown, { "Down" });
        bind(Action::DpadLeft, { "Left" });
        bind(Action::DpadRight, { "Right" });
        return settings;
    }

    bool Load(const std::filesystem::path& path, Settings& settings)
    {
        settings = Defaults();
        std::ifstream file(path);
        if (!file)
            return false;
        std::string line, section;
        while (std::getline(file, line))
        {
            std::string text = Trim(line);
            if (text.empty() || text[0] == '#' || text[0] == ';')
                continue;
            if (text.front() == '[' && text.back() == ']')
            {
                section = Trim(std::string_view(text).substr(1, text.size() - 2));
                continue;
            }
            size_t equals = text.find('=');
            if (equals == std::string::npos)
                continue;
            Apply(settings, section, Trim(std::string_view(text).substr(0, equals)), Trim(std::string_view(text).substr(equals + 1)));
        }
        return true;
    }

    bool Save(const std::filesystem::path& path, const Settings& settings)
    {
        std::ostringstream out;
        out << "# Halo 3 Recomp settings. Written by H3Launcher; safe to edit by hand.\n"
               "# Missing keys use their defaults.\n\n";

        const VideoSettings& video = settings.video;
        out << "[video]\n"
            << "width = " << video.width << "\n"
            << "height = " << video.height << "\n"
            << "fullscreen = " << BoolText(video.fullscreen) << "\n"
            << "vsync = " << BoolText(video.vsync) << "\n"
            << "render_scale = " << video.renderScale << "\n"
            << "precompile_shaders = " << BoolText(video.precompileShaders) << "\n"
            << "async_shaders = " << BoolText(video.asyncShaders) << "\n"
            << "frame_rate = " << video.frameRate << "\n"
            << "anti_aliasing = " << (video.antiAliasing == AntiAliasing::Off ? "off" : "fxaa") << "\n\n";

        const ProfileSettings& profile = settings.profile;
        out << "[profile]\n"
            << "gamertag = " << profile.gamertag << "\n"
            << "picture = " << profile.picture << "\n"
            << "invert_look = " << BoolText(profile.invertLook) << "\n"
            << "vibration = " << BoolText(profile.vibration) << "\n"
            << "auto_aim = " << BoolText(profile.autoAim) << "\n"
            << "auto_center = " << BoolText(profile.autoCenter) << "\n\n";

        const ControlSettings& controls = settings.controls;
        out << "[controls]\n"
            << "# Keys use SDL scancode names; mouse: Mouse Left/Right/Middle/X1/X2, Mouse Wheel.\n"
            << "mouse_sensitivity = " << controls.mouseSensitivity << "\n"
            << "invert_mouse = " << BoolText(controls.invertMouse) << "\n";
        for (size_t i = 0; i < kActionCount; i++)
        {
            out << kActions[i].key << " =";
            const std::vector<std::string>& bindings = controls.bindings[i];
            for (size_t b = 0; b < bindings.size(); b++)
                out << (b ? ", " : " ") << bindings[b];
            out << "\n";
        }

        const DebugSettings& debug = settings.debug;
        out << "\n[debug]\n"
            << "# H3_* lines are environment switches the game sets for itself (README, \"Debugging\").\n"
            << "log_to_file = " << BoolText(debug.logToFile) << "\n"
            << "unlock_all_levels = " << BoolText(debug.unlockAllLevels) << "\n";
        for (const auto& [name, value] : debug.environment)
            out << name << " = " << value << "\n";

        // Write a temporary file and rename it over the old one, so a crash
        // mid-write never leaves a truncated settings file.
        std::filesystem::path temporary = path;
        temporary += ".tmp";
        {
            std::ofstream file(temporary, std::ios::trunc);
            if (!file)
                return false;
            file << out.str();
            if (!file.flush())
                return false;
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        return !error;
    }

    const Settings& Active()
    {
        return g_active;
    }

    void SetActive(const Settings& settings)
    {
        g_active = settings;
    }
}
