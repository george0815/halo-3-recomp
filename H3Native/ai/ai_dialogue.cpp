// AI dialogue globals lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The game globals' AI Globals (element 0) reference the global dialogue
// tag ('adlg').

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
}

// ai_dialogue_get_involuntary_vocalization(r3 s16 index) -> the global
// dialogue's Involuntary Data[index] vocalization index (u16), or NONE (-1)
// when there is no dialogue tag or the index is outside the block.
H3_REPLACE_INT(8251E658, ai_dialogue_get_involuntary_vocalization, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& aiGlobals = globals->globals.ai_globals;
    if (aiGlobals.storedCount <= 0)
        return 0xFFFFFFFFu;
    if (aiGlobals.count <= 0)
        return std::nullopt;
    uint32_t dialogue = aiGlobals[0].global_dialogue_tag.datum;
    if (dialogue == 0xFFFFFFFFu)
        return 0xFFFFFFFFu;
    if (!h3::native::tags::TagSlotAddress(dialogue & 0xFFFF))
        return 0xFFFFFFFFu;
    const host::ai_dialogue_globals_tag* tag =
        h3::native::tags::TagBySlotOf<host::ai_dialogue_globals_tag>(dialogue & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& involuntary = tag->ai_dialogue_globals.involuntary_data;
    int32_t index = int16_t(ctx.r3.u32);
    if (index < 0 || index >= involuntary.storedCount)
        return 0xFFFFFFFFu;
    if (index >= involuntary.count)
        return std::nullopt;
    return uint32_t(uint16_t(involuntary[index].involuntary_vocalization_index));
})

#endif
