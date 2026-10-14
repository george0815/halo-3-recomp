// Multiplayer globals lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The game globals reference the multiplayer globals ('mulg'); its
// Universal block (one element) has the weapon selections (+0x68, 0x18
// bytes each, a string id name first).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;

    // Names of the weapon selections that aren't entries (string ids).
    constexpr int32_t kSelectionDefault = -1, kSelectionNone = -2, kSelectionRandom = -3;
    constexpr uint32_t kDefaultName = 1;
    constexpr uint32_t kNoneName = 0x00010387;
    constexpr uint32_t kRandomName = 0x00040003;
}

// multiplayer_weapon_selection_get_name(s16 index) -> string id, NONE if
// the index isn't an entry.
H3_REPLACE_INT(8224FDD8, multiplayer_weapon_selection_get_name, {
    int32_t index = int16_t(ctx.r3.u32);
    if (index == kSelectionRandom)
        return kRandomName;
    if (index == kSelectionDefault)
        return kDefaultName;
    if (index == kSelectionNone)
        return kNoneName;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return kNone;
    uint32_t datum = globals->globals.multiplayer_globals.datum;
    if (datum == kNone)
        return kNone;
    const auto* multiplayer = h3::native::tags::TagBySlotOf<host::multiplayer_globals_tag>(datum & 0xFFFF);
    if (!multiplayer)
        return std::nullopt;
    const auto& universal = multiplayer->multiplayer_globals.universal;
    if (universal.storedCount <= 0)
        return kNone;
    if (universal.count <= 0)
        return std::nullopt;
    const auto& selections = universal[0].weapon_selections;
    if (index < 0 || index >= selections.storedCount)
        return kNone;
    if (index >= selections.count)
        return std::nullopt;
    return selections[index].name;
})

// multiplayer_globals_get_constants() -> guest address of the multiplayer
// globals' Multiplayer Constants (runtime block, first element), or 0.
H3_REPLACE_INT(8227DD60, multiplayer_globals_get_constants, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return 0u;
    uint32_t datum = globals->globals.multiplayer_globals.datum;
    if (datum == kNone)
        return 0u;
    const auto* multiplayer = h3::native::tags::TagBySlotOf<host::multiplayer_globals_tag>(datum & 0xFFFF);
    if (!multiplayer)
        return std::nullopt;
    const auto& runtime = multiplayer->multiplayer_globals.runtime;
    if (runtime.storedCount <= 0 || !runtime.address)
        return 0u;
    if (runtime.count <= 0)
        return std::nullopt;
    return runtime[0].multiplayer_constants.address;  // no count check, as in the original
})

#endif
