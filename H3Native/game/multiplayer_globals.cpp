// Multiplayer globals lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The game globals reference the multiplayer globals ('mulg'); its
// Universal block (one element) has the weapon selections (+0x68, 0x18
// bytes each, a string id name first).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
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

// multiplayer_globals_get_universal() -> guest address of the multiplayer
// globals' Universal block (first element), or 0 if there are no game
// globals or the block is empty. The original doesn't check the reference
// for NONE (it would read slot 0xFFFF); that case runs the original.
H3_REPLACE_INT(822E08B8, multiplayer_globals_get_universal, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return 0u;
    uint32_t datum = globals->globals.multiplayer_globals.datum;
    if (datum == kNone)
        return std::nullopt;
    const auto* multiplayer = h3::native::tags::TagBySlotOf<host::multiplayer_globals_tag>(datum & 0xFFFF);
    if (!multiplayer)
        return std::nullopt;
    const auto& universal = multiplayer->multiplayer_globals.universal;
    return universal.storedCount > 0 ? universal.address : 0u;  // signed, like the original
})

// multiplayer_globals_get_game_engine_settings() -> guest address of the
// tag the Universal block's Game Engine Settings references (unchecked
// group, like the original), or 0 unless Universal has exactly one element
// and the reference is set.
H3_REPLACE_INT(822BC0E8, multiplayer_globals_get_game_engine_settings, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return 0u;
    uint32_t datum = globals->globals.multiplayer_globals.datum;
    if (datum == kNone)
        return 0u;
    const auto* multiplayer = h3::native::tags::TagBySlotOf<host::multiplayer_globals_tag>(datum & 0xFFFF);
    if (!multiplayer)
        return std::nullopt;
    const auto& universal = multiplayer->multiplayer_globals.universal;
    if (universal.storedCount != 1)
        return 0u;
    if (universal.count != 1)
        return std::nullopt;
    uint32_t settings = universal[0].game_engine_settings.datum;
    if (settings == kNone)
        return 0u;
    uint32_t address = h3::native::tags::TagSlotAddress(settings & 0xFFFF);
    return address ? std::optional<uint32_t>(address) : std::nullopt;
})

// multiplayer_universal_get_customized_character(r3 species: 0 Spartan,
// otherwise Elite) -> guest address of the Universal block's Customized
// Characters entry for it (by character name), or 0.
H3_REPLACE_INT(82354D48, multiplayer_universal_get_customized_character, {
    constexpr uint32_t kSpartanName = 197, kEliteName = 198;  // string ids
    constexpr uint32_t kCustomizedCharacterBytes = 16;
    uint32_t name = ctx.r3.u32 >= 1 ? kEliteName : kSpartanName;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return 0u;
    uint32_t datum = globals->globals.multiplayer_globals.datum;
    if (datum == kNone)
        return 0u;
    const auto* multiplayer = h3::native::tags::TagBySlotOf<host::multiplayer_globals_tag>(datum & 0xFFFF);
    if (!multiplayer)
        return std::nullopt;
    const auto& universal = multiplayer->multiplayer_globals.universal;
    if (universal.storedCount <= 0)
        return 0u;
    if (universal.count <= 0)
        return std::nullopt;
    const auto& characters = universal[0].customized_characters;
    if (characters.storedCount <= 0)
        return 0u;
    if (characters.storedCount > characters.count)
        return std::nullopt;
    for (int32_t i = 0; i < characters.count; i++)
        if (characters[i].character_name == name)
            return characters.address + uint32_t(i) * kCustomizedCharacterBytes;
    return 0u;
})

// multiplayer_globals_get_sandbox_object_properties() -> guest address of
// the tag the Universal block's Sandbox Object Properties Values references
// (unchecked group), or 0 when there's no multiplayer globals or Universal.
H3_REPLACE_INT(824F41F8, multiplayer_globals_get_sandbox_object_properties, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return 0u;
    uint32_t datum = globals->globals.multiplayer_globals.datum;
    if (datum == kNone)
        return 0u;
    const auto* multiplayer = h3::native::tags::TagBySlotOf<host::multiplayer_globals_tag>(datum & 0xFFFF);
    if (!multiplayer)
        return std::nullopt;
    const auto& universal = multiplayer->multiplayer_globals.universal;
    if (universal.storedCount <= 0 || !universal.address)
        return 0u;
    if (universal.count <= 0)
        return std::nullopt;
    uint32_t properties = universal[0].sandbox_object_properties_values.datum;
    uint32_t address = properties == kNone ? 0 : h3::native::tags::TagSlotAddress(properties & 0xFFFF);
    return address ? std::optional<uint32_t>(address) : std::nullopt;  // NONE: the original reads slot 0xFFFF
})

namespace
{
    // The Universal block's set at index (16-byte elements: weapon sets,
    // vehicle sets): its guest address, 0 when there's no game globals, no
    // Universal or the index is out of range, nullopt when the host copy
    // can't say (or the original would read slot 0xFFFF).
    template <typename Sets>
    std::optional<uint32_t> UniversalSet(int32_t index, Sets host::multiplayer_universal_block::*sets)
    {
        constexpr uint32_t kSetBytes = 16;
        const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
        if (!globals)
            return 0u;
        uint32_t datum = globals->globals.multiplayer_globals.datum;
        if (datum == kNone)
            return std::nullopt;
        const auto* multiplayer = h3::native::tags::TagBySlotOf<host::multiplayer_globals_tag>(datum & 0xFFFF);
        if (!multiplayer)
            return std::nullopt;
        const auto& universal = multiplayer->multiplayer_globals.universal;
        if (universal.storedCount <= 0 || !universal.address)
            return 0u;
        if (universal.count <= 0)
            return std::nullopt;
        const auto& block = universal[0].*sets;
        if (index < 0 || index >= block.storedCount)
            return 0u;
        return block.address + uint32_t(index) * kSetBytes;
    }
}

// multiplayer_universal_get_weapon_set(s16 index) -> guest address of the
// Universal block's Weapon Sets entry, or 0.
H3_REPLACE_INT(822E0CA0, multiplayer_universal_get_weapon_set, {
    return UniversalSet(int16_t(ctx.r3.u32), &host::multiplayer_universal_block::weapon_sets);
})

// multiplayer_universal_get_vehicle_set(s16 index) -> guest address of the
// Universal block's Vehicle Sets entry, or 0.
H3_REPLACE_INT(822E0DA0, multiplayer_universal_get_vehicle_set, {
    return UniversalSet(int16_t(ctx.r3.u32), &host::multiplayer_universal_block::vehicle_sets);
})

// multiplayer_weapon_selection_find(r3 name string id) -> the index of the
// weapon selection with that name and a weapon tag (the inverse of
// multiplayer_weapon_selection_get_name): -2 for the "none" name, -3 for
// "random", NONE (-1) for 0, NONE or a name that isn't an entry.
H3_REPLACE_INT(8224FCE0, multiplayer_weapon_selection_find, {
    int32_t name = int32_t(ctx.r3.u32);
    if (name == int32_t(kNoneName))
        return uint32_t(kSelectionNone);
    if (name == int32_t(kRandomName))
        return uint32_t(kSelectionRandom);
    if (name == -1 || name == 0)
        return kNone;
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
    if (selections.storedCount <= 0)
        return kNone;
    if (selections.storedCount > selections.count || selections.storedCount > 0x7FFF)
        return std::nullopt;  // (the original's index is a short)
    for (int32_t i = 0; i < selections.count; i++)
        if (int32_t(selections[i].name) == name && selections[i].weapon_tag.datum != kNone)
            return uint32_t(i);
    return kNone;
})

// multiplayer_get_weapon_spawn_influence(r3 object) -> guest address of the
// weapon spawn influencer for the object's definition: the scenario's own
// first, then the multiplayer constants' Weapon Influencers; 0 if neither
// has one. The original doesn't check the object for NONE.
H3_REPLACE_INT(8227DDC0, multiplayer_get_weapon_spawn_influence, {
    constexpr uint32_t kInfluenceBytes = 0x20;
    uint32_t object = ctx.r3.u32;
    if (object == kNone)
        return std::nullopt;
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, object);
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& own = scenario->scenario.weapon_spawn_influencers;
    if (own.storedCount > own.count)
        return std::nullopt;
    for (int32_t i = 0; i < own.count; i++)
        if (own[i].weapon.datum == definition)
            return own.address + uint32_t(i) * kInfluenceBytes;

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
    const auto& constants = runtime[0].multiplayer_constants;
    if (!constants.address)
        return 0u;
    if (constants.count <= 0)
        return std::nullopt;  // the original reads the first element regardless
    const auto& influencers = constants[0].weapon_influencers;
    if (influencers.storedCount > influencers.count)
        return std::nullopt;
    for (int32_t i = 0; i < influencers.count; i++)
        if (influencers[i].weapon.datum == definition)
            return influencers.address + uint32_t(i) * kInfluenceBytes;
    return 0u;
})

#endif
