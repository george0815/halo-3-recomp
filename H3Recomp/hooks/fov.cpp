// Field of view ([video] fov, degrees; H3_FOV=<degrees> overrides). The
// first-person view takes its FOV from the player's unit: every biped and
// vehicle tag has a camera field of view (unit +0x24, tag +0x11C; 70
// degrees on the multiplayer maps), found by poking candidates live
// (private/re/fov.md). MCC's slider overrides it in code, for one local
// player only (split screen views are narrower already). Here each map's
// unit FOVs are scaled by fov / 70 once it loads, and put back while more
// than one player plays on this PC. Zoom levels are relative to it.

#include "hooks/fov.h"
#include "config/env.h"
#include "config/settings.h"
#include "input/input.h"
#include "memory.h"

#include "tags/tag_cache.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace h3::hooks
{
    namespace
    {
        constexpr uint32_t kUnitCameraFov = 0x11C;  // object struct (0xF8) + unit camera_field_of_view (0x24)
        constexpr uint32_t kGroupBiped = 0x62697064, kGroupVehicle = 0x76656869, kGroupGiant = 0x67696E74;
        constexpr float kOriginalDegrees = 70.0f;

        uint32_t g_tags = 0;  // the loaded map's tags (LoadedTagsIdentity)
        std::vector<std::pair<uint32_t, float>> g_originals;  // field address, the tag's own FOV
        float g_applied = 0;  // the scale written
    }

    void UpdateFov()
    {
        static const float scale = [] {
            const char* text = EnvString("H3_FOV");
            float degrees = text ? float(atof(text)) : config::Active().video.fov;
            return std::clamp(degrees, 60.0f, 120.0f) / kOriginalDegrees;
        }();
        static const bool log = EnvFlag("H3_LOG_FOV");
        uint32_t tags = native::tags::LoadedTagsIdentity();
        if (tags == 0)
            return;
        if (tags != g_tags)
        {
            // A new map: note its units' FOVs before changing any.
            g_tags = tags;
            g_originals.clear();
            g_applied = 1;
            for (uint32_t slot = 0, count = native::tags::TagSlotCount(); slot < count; slot++)
            {
                uint32_t group = native::tags::SlotGroupTag(slot);
                if (group != kGroupBiped && group != kGroupVehicle && group != kGroupGiant)
                    continue;
                uint32_t address = native::tags::TagSlotAddress(slot);
                if (address == 0)
                    continue;
                float fov = GuestRef<float>(address + kUnitCameraFov);
                if (fov > 0.1f && fov < 3.0f)  // radians: a sane FOV
                    g_originals.push_back({ address + kUnitCameraFov, fov });
            }
        }
        // More than one player on this PC: the game's own FOV, as MCC.
        float wanted = std::popcount(input::JoinedMask()) > 1 ? 1.0f : scale;
        if (wanted == g_applied)
            return;
        for (const auto& [address, fov] : g_originals)
            GuestRef<float>(address) = fov * wanted;
        g_applied = wanted;
        if (log)
            fprintf(stderr, "[fov] %zu unit FOVs scaled by %.3f\n", g_originals.size(), wanted);
    }
}
