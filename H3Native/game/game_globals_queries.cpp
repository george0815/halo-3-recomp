// Game globals lookups (tags referenced from 'matg' blocks), decompiled to
// read the host tag cache (docs/ROADMAP.md 6.3 milestone A; replace.h for
// the mechanism). Names are descriptive (not in the symbol database).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;

    // A tag reference's tag data (guest address), 0 for NONE: the tag
    // table entry, as the originals read it (no salt check).
    uint32_t ReferencedTag(uint32_t datum)
    {
        return datum == host::kNone ? 0u : h3::native::tags::TagSlotAddress(datum & 0xFFFF);
    }
}

// ai_globals_get_global_dialogue(void) -> guest address of the AI Globals'
// Global Dialogue Tag, or 0 (no AI Globals element, or no reference).
H3_REPLACE_INT(820AD658, ai_globals_get_global_dialogue, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;  // the original returns 0 with no globals loaded
    const auto& ai = globals->globals.ai_globals;
    if (ai.storedCount <= 0)
        return 0u;
    if (ai.count <= 0)
        return std::nullopt;
    return ReferencedTag(ai[0].global_dialogue_tag.datum);
})

// user_interface_globals_get(void) -> guest address of the interface
// globals tag for the scenario's type: Single Player UI Globals (solo),
// Multiplayer UI Globals (multiplayer) or Main Menu UI Globals (main menu);
// 0 for another type, no Interface Tags element, or no reference.
H3_REPLACE_INT(820895B0, user_interface_globals_get, {
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!scenario || !globals)
        return std::nullopt;  // the original returns 0 with either missing
    const auto& interface = globals->globals.interface_tags;
    if (interface.storedCount == 0 || interface.address == 0)
        return 0u;
    if (interface.count <= 0)
        return std::nullopt;
    const host::interface_tag_references& tags = interface[0];
    switch (uint32_t(int32_t(scenario->scenario.type)))
    {
    case 0: return ReferencedTag(tags.single_player_ui_globals.datum);
    case 1: return ReferencedTag(tags.multiplayer_ui_globals.datum);
    case 2: return ReferencedTag(tags.main_menu_ui_globals.datum);
    default: return 0u;
    }
})

#endif
