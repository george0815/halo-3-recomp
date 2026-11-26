// Looping sound ('lsnd') lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
    // Sound classes (the 'snd!' Sound Class enum).
    constexpr int32_t kSoundClassMusic = 32;
    constexpr int32_t kSoundClassCinematicMusic = 38;
}

// looping_sound_has_music(r3 looping sound datum) -> whether any track's
// loop sound is of class music or cinematic music. The original doesn't
// check the datum for NONE.
H3_REPLACE_INT(82200850, looping_sound_has_music, {
    uint32_t looping = ctx.r3.u32;
    if (looping == kNone)
        return std::nullopt;
    const auto* tag = h3::native::tags::TagBySlotOf<host::sound_looping_tag>(looping & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& tracks = tag->sound_looping.tracks;
    if (tracks.storedCount <= 0)
        return 0u;
    if (tracks.storedCount > tracks.count || tracks.storedCount > 0x7FFF)
        return std::nullopt;  // (the original's index is a short)
    for (int32_t i = 0; i < tracks.count; i++)
    {
        uint32_t sound = tracks[i].loop.datum;
        if (sound == kNone)
            continue;
        const host::sound_tag* soundTag = h3::native::tags::TagBySlotOf<host::sound_tag>(sound & 0xFFFF);
        if (!soundTag)
            return std::nullopt;
        int32_t soundClass = soundTag->sound.sound_class;
        if (soundClass == kSoundClassMusic || soundClass == kSoundClassCinematicMusic)
            return 1u;
    }
    return 0u;
})

#endif
