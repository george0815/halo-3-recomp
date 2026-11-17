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
#include "config/env.h"
#include "config/settings.h"
#include "input/input.h"
#include "memory.h"

#include "tags/tag_cache.h"

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
        // controller input each stands for. Ones not confirmed yet stay.
        struct Icon
        {
            char32_t character;
            uint16_t buttons[4];  // the controller inputs, joined with "/" (0: unused)
        };
        constexpr Icon kIcons[] = {
            { 0xE100, { input::kA } },
            { 0xE101, { input::kB } },
            { 0xE102, { input::kX } },
            { 0xE103, { input::kY } },
            { 0xE104, { input::kLeftShoulder } },
            { 0xE105, { input::kRightShoulder } },
            { 0xE106, { input::kLeftTriggerKey } },
            { 0xE107, { input::kRightTriggerKey } },
            { 0xE108, { input::kDpadUp, input::kDpadDown, input::kDpadLeft, input::kDpadRight } },
            { 0xE10C, { input::kStart } },  // "Settings" in lobbies
            { 0xE10D, { input::kBack } },   // "Prior Game Stats"
        };

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

        // "[Space]" for an icon, or empty to keep the icon (nothing bound).
        std::string KeyLabel(const Icon& icon)
        {
            std::string names;
            for (uint16_t button : icon.buttons)
            {
                if (!button)
                    break;
                std::string name = input::KeyNameFor(button);
                if (name.empty())
                    return {};
                names += (names.empty() ? "" : "/") + ShortKeyName(name);
            }
            return "[" + names + "]";
        }

        std::string Utf8(char32_t c)
        {
            return { char(0xE0 | (c >> 12)), char(0x80 | ((c >> 6) & 0x3F)), char(0x80 | (c & 0x3F)) };
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
            if (keys)
                for (const Icon& icon : kIcons)
                {
                    std::string glyph = Utf8(icon.character);
                    if (result.find(glyph) == std::string::npos)
                        continue;
                    std::string label = KeyLabel(icon);
                    if (label.empty())
                        continue;
                    for (size_t at = result.find(glyph); at != std::string::npos; at = result.find(glyph, at + label.size()))
                        result.replace(at, glyph.size(), label);
                    changed = true;
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
        if (data == g_copy)
        {
            if (keys != g_keys)
                Build(pack, keys, log);  // the player switched devices
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
        Build(pack, keys, log);
    }
}
