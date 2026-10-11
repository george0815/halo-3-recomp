// "Unlock all missions" (settings.ini [debug] unlock_all_levels): marks every
// campaign mission as unlocked in the game's in-memory player profiles, so
// the campaign lobby's mission list offers all of them.
//
// Halo 3 keeps one player profile per controller in an array at kProfiles,
// kProfileStride bytes each (the profile-write task in sub_824A4E78 indexes
// the same array). In it:
//
//   +0x48  u8[32]  solo progress: per mission, bit d = completed on
//                  difficulty d (0 Easy .. 3 Legendary); sub_82470F68
//   +0x68  u8[32]  the same for co-op
//   +0xA4  u32     unlocked missions, bit i = mission i (big-endian word)
//
// The mission list (built by sub_824F0498) shows mission i as playable when
// bit i of the unlock mask is set; missions are unlocked by reaching them,
// not by completing the one before. Found with H3_WATCH_READS on the profile
// while the lobby builds the list. Only the unlock mask is touched here, so
// completion records (and anything derived from them) stay real.
//
// The mask is set every frame (cheap, and covers profiles signed in later).
// It lives in the profile, so if the game saves the profile while this is
// on, the unlocks are saved too, as if the missions had been reached.

#include "config/settings.h"
#include "memory.h"

#include <cstdint>

namespace h3::hooks
{
    namespace
    {
        constexpr uint32_t kProfiles = 0x831401E8;  // controller 0's profile
        constexpr uint32_t kProfileStride = 2512;
        constexpr uint32_t kControllers = 4;
        constexpr uint32_t kUnlockedMissions = 0xA4;  // offset of the unlock mask
        constexpr uint32_t kAllMissions = 0xFFFFFFFF;  // the game only tests real missions' bits
    }

    // Called once per presented frame (video/window.cpp).
    void UpdateCampaignUnlock()
    {
        static const bool enabled = config::Active().debug.unlockAllLevels;
        if (!enabled)
            return;
        for (uint32_t controller = 0; controller < kControllers; controller++)
            GuestRef<uint32_t>(kProfiles + controller * kProfileStride + kUnlockedMissions) = kAllMissions;
    }
}
