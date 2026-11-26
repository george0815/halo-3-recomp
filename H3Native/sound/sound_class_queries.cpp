// Sound class flag queries, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism). The
// classes: sound/sound_classes.cpp. Names are descriptive (not in the
// symbol database).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint16_t kDeterministic = 1u << 5;  // sound class flags (Assembly's sncl plugin)
    constexpr uint32_t kSoundClassIndex = 2;      // sound definition: s8 class
    constexpr uint32_t kPlaybackValue = 4;        // r3's s8 at +4
}

// sound_get_nondeterministic_value(r3 struct, r4 sound definition) -> the
// s8 at r3 + 4, or -1 if the sound's class is Deterministic. No range
// check on the class, as in the original.
H3_REPLACE_INT(820FA480, sound_get_nondeterministic_value, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals || globals->globals.sound_globals.storedCount == 0 || globals->globals.sound_globals.count <= 0)
        return std::nullopt;  // the original reads through a null element then
    uint32_t datum = globals->globals.sound_globals[0].sound_classes.datum;
    const auto* classes = h3::native::tags::TagBySlotOf<host::sound_classes_tag>(datum & 0xFFFF);
    if (!classes)
        return std::nullopt;
    int32_t index = int8_t(uint8_t(h3::GuestRef<uint8_t>(ctx.r4.u32 + kSoundClassIndex)));
    const auto& block = classes->sound_classes.sound_classes;
    if (index < 0 || index >= block.count)
        return std::nullopt;
    if (block[index].flags & kDeterministic)
        return 0xFFFFFFFFu;
    return uint32_t(int32_t(int8_t(uint8_t(h3::GuestRef<uint8_t>(ctx.r3.u32 + kPlaybackValue)))));
})

#endif
