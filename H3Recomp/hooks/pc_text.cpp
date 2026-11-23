// PC wording for the game's English text: "this Xbox 360 console" becomes
// "this PC", the controller isn't an "Xbox 360 Controller", and the tips
// that send players to the Xbox Guide say how it's done here. While player
// 1 plays on the keyboard and mouse, the button icons in the text (A, B,
// triggers...) become the keys bound to them: "[Space] Select". The
// research is in private/re/console_wording.md. H3_NO_PC_TEXT=1 keeps the
// original text.
//
// The text lives in the active language pack, part of the game globals
// tag ('matg'), loaded with each map: an index (string id and byte offset
// per string) into one block of UTF-8 strings. Once a pack is loaded, its
// block is copied into a larger one with the rewritten strings appended,
// the pack is pointed at the copy, and the rewritten strings' offsets are
// moved to their new text; when the input device changes, a new copy is
// made from the original strings the same way. Every copy starts with the
// original bytes, so every offset stays valid at every step: a game thread
// reading text meanwhile gets either version of a string, never a mix.
// Screens already open keep the text they took.

#include "hooks/pc_text.h"

#include <ppc_recomp_shared.h>
#include "config/env.h"
#include "config/settings.h"
#include "input/input.h"
#include "memory.h"

#include "tags/tag_cache.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace h3::hooks
{
    namespace
    {
        // Language packs inside the globals tag: 12 of 0x44 bytes (the
        // tag layout's language_pack_definition with 32-bit pointers).
        constexpr uint32_t kGlobalsLanguage = 0xAC;
        constexpr uint32_t kGlobalsLanguagePacks = 0x1BC;
        constexpr uint32_t kLanguagePackBytes = 0x44;
        constexpr uint32_t kPackIndex = 0x00;  // per string: u32 string id, u32 byte offset into the data
        constexpr uint32_t kIndexEntryBytes = 8;
        constexpr uint32_t kIndexEntryOffset = 4;
        constexpr uint32_t kPackData = 0x04;
        constexpr uint32_t kPackStringCount = 0x08;
        constexpr uint32_t kPackDataSize = 0x0C;
        constexpr uint32_t kPackLoaded = 0x40;
        constexpr uint32_t kEnglish = 0;

        // Whole strings rewritten first (exact matches), then phrases,
        // in order, within every English string.
        const std::pair<const char*, const char*> kWholeStrings[] = {
            { "If you would like to sign out, press the Xbox Guide button and sign out from the guide.",
                "To sign out, hold BACK and START on your controller for two seconds." },
            { "Leaving will take everyone on this Xbox 360 console with you. Tip: Sign out from the Xbox Guide to leave alone.",
                "Leaving will take everyone on this PC with you. Tip: Hold BACK and START on your controller for two seconds "
                "to leave alone." },
            { "Connect multiple Xbox 360 consoles to play over your local area network.",
                "Play with other PCs over your local network, or with friends over the internet." },
        };
        const std::pair<const char*, const char*> kPhrases[] = {
            { "Please don't turn off your Xbox 360 console.", "Please don't close the game." },
            { "don't turn off your Xbox 360 console", "don't close the game" },
            { "An Xbox 360 Hard Drive", "A hard drive" },
            { "an Xbox 360 Hard Drive", "a hard drive" },
            { "Xbox 360 Hard Drive", "hard drive" },
            { "this Xbox 360 console", "this PC" },
            { "this Xbox 360 Console", "this PC" },
            { "the same Xbox 360 console", "the same PC" },
            { "a different Xbox 360 console", "a different PC" },
            { "multiple Xbox 360 consoles", "multiple PCs" },
            { "Xbox 360 consoles", "PCs" },
            { "Xbox 360 console", "PC" },
            { "your Xbox 360 Controller", "your controller" },
            { "Your Xbox 360 Controller", "Your controller" },
            { "Xbox 360 Controller", "controller" },
        };

        // The button icons in the text: private-use characters the font
        // draws as the 360's buttons (U+E100.., UTF-8 EE 84 80..), and the
        // controller input each stands for (the game's own button -> icon
        // table, in sub_82494060; the stick clicks are animated, three
        // icons each).
        struct Icon
        {
            char32_t character;
            uint16_t button;
        };
        constexpr Icon kIcons[] = {
            { 0xE100, input::kA },
            { 0xE101, input::kB },
            { 0xE102, input::kX },
            { 0xE103, input::kY },
            { 0xE104, input::kLeftShoulder },
            { 0xE105, input::kRightShoulder },
            { 0xE106, input::kLeftTriggerKey },
            { 0xE107, input::kRightTriggerKey },
            { 0xE108, input::kDpadUp },
            { 0xE109, input::kDpadDown },
            { 0xE10A, input::kDpadLeft },
            { 0xE10B, input::kDpadRight },
            { 0xE10C, input::kStart },
            { 0xE10D, input::kBack },
            { 0xE133, input::kLeftThumb },
            { 0xE134, input::kLeftThumb },
            { 0xE135, input::kLeftThumb },
            { 0xE136, input::kRightThumb },
            { 0xE137, input::kRightThumb },
            { 0xE138, input::kRightThumb },
        };
        // The sticks themselves (theater: "Fly Camera", "Camera Look").
        constexpr char32_t kLeftStickIcon = 0xE110, kRightStickIcon = 0xE111;

        // Gameplay text names actions instead of buttons (U+E443..E460:
        // "Press <melee> to melee"), and the game draws the button the
        // player's controller layout gives that action (sub_82494060). The
        // token's action, per token from kFirstActionToken (-1: the trigger
        // that doesn't fire the right weapon, the left weapon's; kNoAction:
        // not a button). Move and look (E453, E454) are the sticks.
        constexpr char32_t kFirstActionToken = 0xE443, kMoveToken = 0xE453, kLookToken = 0xE454;
        constexpr int8_t kNoAction = -2, kOtherTrigger = -1;
        constexpr int8_t kTokenActions[] = {
            0, 1, 3, 2, 5, 17, 7, 8, 11, 12, 13, 14, 15, 16, 18, 19,                          // E443-E452
            kNoAction, kNoAction, kNoAction, kNoAction, kNoAction, kNoAction, kNoAction, kNoAction,  // E453-E45A
            kOtherTrigger, 3, 3, 4, 26, 6,                                                     // E45B-E460
        };
        constexpr int8_t kFireRightAction = 8;
        // The players' controller settings (52 bytes each, the game's copy
        // of the profile's): from +8 the button (below) of each action.
        constexpr uint32_t kControllerSettings = 0x829E74A0;
        constexpr uint32_t kActionButtons = 8;
        constexpr uint32_t kActionButtonCount = 28;  // up to the stick layout (+36)
        // The game's buttons (the actions' values) as controller inputs.
        constexpr uint16_t kGameButtons[] = {
            input::kLeftTriggerKey, input::kRightTriggerKey, input::kDpadUp, input::kDpadDown, input::kDpadLeft,
            input::kDpadRight, input::kStart, input::kBack, input::kLeftThumb, input::kRightThumb, input::kA, input::kB,
            input::kX, input::kY, input::kLeftShoulder, input::kRightShoulder,
        };

        // Player 1's action -> button bytes, as the rewrite last used them.
        std::array<uint8_t, kActionButtonCount> g_layout{};

        std::array<uint8_t, kActionButtonCount> CurrentLayout()
        {
            std::array<uint8_t, kActionButtonCount> layout{};
            for (uint32_t i = 0; i < kActionButtonCount; i++)
                layout[i] = GuestRef<uint8_t>(kControllerSettings + kActionButtons + i);
            return layout;
        }

        // The short name a key goes by in game text: the full SDL names
        // ("Mouse Middle") ran past menu footers built for button icons.
        std::string ShortKeyName(const std::string& name)
        {
            static const std::pair<const char*, const char*> kShort[] = {
                { "Mouse Left", "LMB" }, { "Mouse Right", "RMB" }, { "Mouse Middle", "MMB" }, { "Mouse X1", "M4" },
                { "Mouse X2", "M5" }, { "Mouse Wheel", "Wheel" }, { "Escape", "Esc" }, { "Return", "Enter" },
                { "Backspace", "Bksp" }, { "Left Shift", "LShift" }, { "Right Shift", "RShift" }, { "Left Ctrl", "LCtrl" },
                { "Right Ctrl", "RCtrl" }, { "Left Alt", "LAlt" }, { "Right Alt", "RAlt" }, { "CapsLock", "Caps" },
                { "PageUp", "PgUp" }, { "PageDown", "PgDn" }, { "Delete", "Del" }, { "Insert", "Ins" },
            };
            for (const auto& [full, abbreviated] : kShort)
                if (name == full)
                    return abbreviated;
            return name;
        }

        // "[Space]" for a controller input, or empty to keep the icon
        // (nothing bound).
        std::string ButtonLabel(uint16_t button)
        {
            std::string name = input::KeyNameFor(button);
            return name.empty() ? std::string() : "[" + ShortKeyName(name) + "]";
        }

        // "[W/A/S/D]": the movement keys, or empty if one isn't bound.
        std::string MoveLabel()
        {
            const auto& bindings = config::Active().controls.bindings;
            std::string names;
            for (config::Action action : { config::Action::MoveForward, config::Action::MoveLeft, config::Action::MoveBack,
                                           config::Action::MoveRight })
            {
                const auto& keys = bindings[size_t(action)];
                if (keys.empty())
                    return {};
                names += (names.empty() ? "" : "/") + ShortKeyName(keys.front());
            }
            return "[" + names + "]";
        }

        // The key text for a private-use character (a button icon or an
        // action token), or empty to leave it.
        std::string Label(char32_t c, const std::array<uint8_t, kActionButtonCount>& layout)
        {
            for (const Icon& icon : kIcons)
                if (icon.character == c)
                    return ButtonLabel(icon.button);
            if (c == kLeftStickIcon || c == kMoveToken)
                return MoveLabel();
            if (c == kRightStickIcon || c == kLookToken)
                return "[Mouse]";
            if (c < kFirstActionToken || c >= kFirstActionToken + std::size(kTokenActions))
                return {};
            int8_t action = kTokenActions[c - kFirstActionToken];
            if (action == kNoAction)
                return {};
            uint8_t button;
            if (action == kOtherTrigger)
                button = layout[kFireRightAction] == 1 ? 0 : 1;
            else
                button = layout[size_t(action)];
            return button < std::size(kGameButtons) ? ButtonLabel(kGameButtons[button]) : std::string();
        }

        // The rewrite of a string, or empty if it stays. [video] pc_wording
        // and pc_key_names turn each part off.
        std::string Rewrite(const char* text, bool keys)
        {
            const bool wording = config::Active().video.pcWording;
            keys = keys && config::Active().video.pcKeyNames;
            std::string result = text;
            bool changed = false;
            bool whole = false;
            if (wording)
                for (const auto& [from, to] : kWholeStrings)
                    if (result == from)
                    {
                        result = to;
                        changed = whole = true;
                        break;
                    }
            if (wording && !whole)
                for (const auto& [from, to] : kPhrases)
                    for (size_t at = result.find(from); at != std::string::npos; at = result.find(from, at + strlen(to)))
                    {
                        result.replace(at, strlen(from), to);
                        changed = true;
                    }
            if (keys && result.find('\xEE') != std::string::npos)
            {
                std::string out;
                for (size_t at = 0; at < result.size(); at++)
                {
                    if (at + 2 < result.size() && uint8_t(result[at]) == 0xEE)
                    {
                        char32_t c = 0xE000 | (char32_t(uint8_t(result[at + 1]) & 0x3F) << 6) | (uint8_t(result[at + 2]) & 0x3F);
                        std::string label = Label(c, g_layout);
                        if (!label.empty())
                        {
                            out += label;
                            at += 2;
                            changed = true;
                            continue;
                        }
                    }
                    out += result[at];
                }
                result = std::move(out);
            }
            return changed ? result : std::string();
        }

        // The pack being worked on.
        uint32_t g_index = 0, g_count = 0, g_size = 0;
        std::vector<uint32_t> g_originalOffsets;
        std::vector<char> g_original;  // the game's string block, as loaded
        uint32_t g_copy = 0, g_retired = 0;  // our current copy, and the one before it
        bool g_keys = false;                 // the current copy shows keys

        // Points the pack at a new copy of the original strings with the
        // rewrites for the keyboard (keys) or a gamepad.
        void Build(uint32_t pack, bool keys, bool log)
        {
            std::vector<std::pair<uint32_t, uint32_t>> moved;  // index entry, new offset
            std::string appended;
            for (uint32_t i = 0; i < g_count; i++)
            {
                uint32_t offset = g_originalOffsets[i];
                if (offset >= g_size)
                    continue;
                std::string text = Rewrite(g_original.data() + offset, keys);
                if (text.empty())
                    continue;
                moved.push_back({ i, g_size + uint32_t(appended.size()) });
                appended += text;
                appended += '\0';
            }
            uint32_t copy = SystemAlloc(g_size + uint32_t(appended.size()) + 1);
            if (copy == 0)
                return;
            memcpy(GuestPtr<uint8_t>(copy), g_original.data(), g_size);
            memcpy(GuestPtr<uint8_t>(copy + g_size), appended.data(), appended.size());
            GuestRef<uint32_t>(pack + kPackData) = copy;
            // Every string back at its original text, then the rewritten
            // ones at theirs.
            for (uint32_t i = 0; i < g_count; i++)
                if (GuestRef<uint32_t>(g_index + i * kIndexEntryBytes + kIndexEntryOffset) != g_originalOffsets[i])
                    GuestRef<uint32_t>(g_index + i * kIndexEntryBytes + kIndexEntryOffset) = g_originalOffsets[i];
            for (const auto& [entry, offset] : moved)
                GuestRef<uint32_t>(g_index + entry * kIndexEntryBytes + kIndexEntryOffset) = offset;
            if (g_retired != 0)
                SystemHeap().Free(g_retired);  // two copies back: no thread can still be reading it
            g_retired = g_copy;
            g_copy = copy;
            g_keys = keys;
            if (log)
                fprintf(stderr, "[text] %zu of %u English strings reworded for PC (%s)\n", moved.size(), g_count,
                    keys ? "keys" : "buttons");
        }
    }

    void UpdatePcText()
    {
        static const bool off = EnvFlag("H3_NO_PC_TEXT");
        static const bool log = EnvFlag("H3_LOG_PC_TEXT");
        if (off || (!config::Active().video.pcWording && !config::Active().video.pcKeyNames))
            return;  // the game's own text
        uint32_t globals = native::tags::GlobalGameGlobalsAddress();
        if (globals == 0 || GuestRef<uint32_t>(globals + kGlobalsLanguage) != kEnglish)
            return;
        uint32_t pack = globals + kGlobalsLanguagePacks + kEnglish * kLanguagePackBytes;
        uint32_t data = GuestRef<uint32_t>(pack + kPackData);
        if (GuestRef<uint32_t>(pack + kPackLoaded) == 0 || data == 0)
            return;
        bool keys = input::KeyboardLastUsed();
        // Action tokens follow player 1's controller layout (a profile
        // setting, loaded with the profile or changed in the settings).
        std::array<uint8_t, kActionButtonCount> layout = CurrentLayout();
        bool layoutChanged = keys && layout != g_layout;
        g_layout = layout;
        if (layoutChanged && log)
        {
            fprintf(stderr, "[text] controller layout:");
            for (uint8_t button : layout)
                fprintf(stderr, " %u", button);
            fprintf(stderr, "\n");
        }
        if (data == g_copy)
        {
            if (keys != g_keys || layoutChanged)
                Build(pack, keys, log);  // the player switched devices or layouts
            return;
        }

        // A pack the game just loaded (a new map): take its strings.
        uint32_t index = GuestRef<uint32_t>(pack + kPackIndex);
        uint32_t count = GuestRef<uint32_t>(pack + kPackStringCount);
        uint32_t size = GuestRef<uint32_t>(pack + kPackDataSize);
        constexpr uint32_t kMaxStrings = 1u << 20, kMaxData = 64u << 20;
        if (index == 0 || count == 0 || count > kMaxStrings || size == 0 || size > kMaxData)
        {
            static uint32_t reported = 0;
            if (reported != data)
                fprintf(stderr, "[text] unexpected language pack (count %u, size %u): leaving the text alone\n", count, size);
            reported = data;
            return;
        }
        g_index = index;
        g_count = count;
        g_size = size;
        g_originalOffsets.resize(count);
        for (uint32_t i = 0; i < count; i++)
            g_originalOffsets[i] = GuestRef<uint32_t>(index + i * kIndexEntryBytes + kIndexEntryOffset);
        g_original.assign(GuestPtr<char>(data), GuestPtr<char>(data) + size);
        g_original.push_back('\0');  // a last string without its terminator stays inside
        // H3_LOG_PC_TEXT: every string with a private-use character (button
        // icons U+E1xx, action tokens U+E4xx), shown as {Exxx}.
        if (log)
            for (uint32_t i = 0; i < count; i++)
            {
                uint32_t offset = g_originalOffsets[i];
                if (offset >= size)
                    continue;
                std::string text = g_original.data() + offset;
                if (text.find('\xEE') == std::string::npos)
                    continue;
                std::string shown;
                for (size_t at = 0; at < text.size(); at++)
                    if (at + 2 < text.size() && uint8_t(text[at]) == 0xEE)
                    {
                        char code[12];
                        snprintf(code, sizeof(code), "{%04X}",
                            0xE000u | (uint32_t(uint8_t(text[at + 1]) & 0x3F) << 6) | (uint8_t(text[at + 2]) & 0x3F));
                        shown += code;
                        at += 2;
                    }
                    else if (text[at] != '\n')
                        shown += text[at];
                    else
                        shown += "\\n";
                fprintf(stderr, "[text icons] %u: %s\n", i, shown.c_str());
            }
        Build(pack, keys, log);
    }
}

// The user interface's per-frame update (sub_82088FA0, the game thread;
// it updates and draws the menus): the text is reworded here, before any
// menu uses a language pack the game just loaded. (Checked after each
// presented frame instead, a menu built in the frames between showed the
// 360's button icons for a moment.)
PPC_FUNC_IMPL(__imp__sub_82088FA0);
PPC_FUNC(sub_82088FA0)
{
    h3::hooks::UpdatePcText();
    __imp__sub_82088FA0(ctx, base);
}
