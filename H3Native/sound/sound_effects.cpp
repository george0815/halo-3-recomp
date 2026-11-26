// Sound effect collection ('sfx+') lookups, decompiled to read the host tag
// cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// sound_effect_collection_find(r3 collection datum, r4 name string id) ->
// the index of the sound effect with that name, or NONE.
H3_REPLACE_INT(82287A70, sound_effect_collection_find, {
    uint32_t collection = ctx.r3.u32;
    if (collection == kNone)
        return kNone;
    const auto* tag = h3::native::tags::TagBySlotOf<host::sound_effect_collection_tag>(collection & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& effects = tag->sound_effect_collection.sound_effects;
    if (effects.storedCount <= 0)
        return kNone;
    if (effects.storedCount > effects.count)
        return std::nullopt;
    for (int32_t i = 0; i < effects.count; i++)
        if (effects[i].name == ctx.r4.u32)
            return uint32_t(i);
    return kNone;
})

#endif
