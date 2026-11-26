// Game globals lookups (tags referenced from 'matg' blocks), decompiled to
// read the host tag cache (docs/ROADMAP.md 6.3 milestone A; replace.h for
// the mechanism). Names are descriptive (not in the symbol database).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "game/game_state_layout.h"
#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

PPC_EXTERN_FUNC(sub_820F6900);  // material_effects_find_material (native)
PPC_EXTERN_FUNC(sub_82081428);  // cosine (f1 -> f1, pure)
PPC_EXTERN_FUNC(sub_820A1B48);  // object_get_orientation (writes r4 / r5 vectors if not null)

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

namespace
{
    // The datum of the interface globals tag user_interface_globals_get
    // returns (NONE when it returns 0), or nullopt when the host copy can't
    // say.
    std::optional<uint32_t> InterfaceGlobalsDatum()
    {
        const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
        const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
        if (!scenario || !globals)
            return std::nullopt;  // the original returns 0 with either missing
        const auto& interface = globals->globals.interface_tags;
        if (interface.storedCount == 0 || interface.address == 0)
            return host::kNone;
        if (interface.count <= 0)
            return std::nullopt;
        const host::interface_tag_references& tags = interface[0];
        switch (uint32_t(int32_t(scenario->scenario.type)))
        {
        case 0: return tags.single_player_ui_globals.datum;
        case 1: return tags.multiplayer_ui_globals.datum;
        case 2: return tags.main_menu_ui_globals.datum;
        default: return host::kNone;
        }
    }

    // The host copy of the interface globals tag ('wgtz'), or nullptr: none
    // when user_interface_globals_get returns 0, unknown when the host copy
    // can't say.
    const host::user_interface_globals_definition_tag* InterfaceGlobals(bool& none, bool& unknown)
    {
        none = unknown = false;
        std::optional<uint32_t> datum = InterfaceGlobalsDatum();
        if (!datum)
        {
            unknown = true;
            return nullptr;
        }
        if (ReferencedTag(*datum) == 0)
        {
            none = true;
            return nullptr;
        }
        const auto* tag = h3::native::tags::TagBySlotOf<host::user_interface_globals_definition_tag>(*datum & 0xFFFF);
        unknown = tag == nullptr;
        return tag;
    }
}

// user_interface_globals_get(void) -> guest address of the interface
// globals tag for the scenario's type: Single Player UI Globals (solo),
// Multiplayer UI Globals (multiplayer) or Main Menu UI Globals (main menu);
// 0 for another type, no Interface Tags element, or no reference.
H3_REPLACE_INT(820895B0, user_interface_globals_get, {
    std::optional<uint32_t> datum = InterfaceGlobalsDatum();
    if (!datum)
        return std::nullopt;
    return ReferencedTag(*datum);
})

// user_interface_shared_globals_get(void) -> guest address of the
// interface globals' Shared Globals tag ('wigl'), or 0 (no interface
// globals, or no reference).
H3_REPLACE_INT(82089550, user_interface_shared_globals_get, {
    bool none, unknown;
    const host::user_interface_globals_definition_tag* tag = InterfaceGlobals(none, unknown);
    if (none)
        return 0u;
    if (!tag)
        return std::nullopt;
    return ReferencedTag(tag->user_interface_globals_definition.shared_globals.datum);
})

// user_interface_find_screen(r3 string id) -> the datum of the first of
// the interface globals' Halox Screen Widgets whose screen ('scn3') has
// that Name, or NONE (no interface globals or no such screen).
H3_REPLACE_INT(8247F640, user_interface_find_screen, {
    bool none, unknown;
    const host::user_interface_globals_definition_tag* tag = InterfaceGlobals(none, unknown);
    if (none)
        return host::kNone;
    if (!tag)
        return std::nullopt;
    const auto& widgets = tag->user_interface_globals_definition.halox_screen_widgets;
    if (widgets.storedCount <= 0)
        return host::kNone;
    if (widgets.storedCount > widgets.count)
        return std::nullopt;
    for (const auto& widget : widgets)
    {
        uint32_t datum = widget.widget_tag.datum;
        if (datum == host::kNone)
            continue;
        const auto* screen = h3::native::tags::TagBySlotOf<host::gui_screen_widget_definition_tag>(datum & 0xFFFF);
        if (!screen)
            return std::nullopt;  // the original reads the word at +4 of whatever is there
        if (screen->gui_screen_widget_definition.name == ctx.r3.u32)
            return datum;
    }
    return host::kNone;
})

namespace
{
    namespace objects = h3::native::objects;
    constexpr uint32_t kRealOne = 0x820221FC;  // read-only real the originals load
    static_assert(sizeof(host::difficulty_block) == 0x284, "difficulty_block: 161 four-byte fields");
}

// game_difficulty_get_real_value(r3 s16 value, r4 s16 difficulty) -> real:
// the game globals' Difficulty element's real for the value at the
// difficulty clamped to 0-3 (four reals per value); 1 with no globals or
// no Difficulty element.
H3_REPLACE_FLOAT(8221A048, game_difficulty_get_real_value, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& difficulty = globals->globals.difficulty;
    if (difficulty.storedCount == 0 || difficulty.address == 0)
        return objects::LoadFloat(kRealOne);
    int32_t level = int16_t(uint16_t(ctx.r4.u32));
    level = level <= 0 ? 0 : level >= 3 ? 3 : level;
    int32_t index = int32_t(int16_t(uint16_t(ctx.r3.u32))) * 4 + level;
    constexpr int32_t kReals = int32_t(sizeof(host::difficulty_block) / 4);
    if (difficulty.count <= 0 || index < 0 || index >= kReals)
        return std::nullopt;
    float value;
    std::memcpy(&value, reinterpret_cast<const uint8_t*>(&difficulty[0]) + index * 4, sizeof(value));
    return value;
})

// globals_get_grenade_type_from_equipment(r3 equipment datum) -> the index
// of the first game globals Grenades element whose Equipment is it, or
// NONE (name unconfirmed).
H3_REPLACE_INT(8221A0C0, globals_get_grenade_type_from_equipment, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& grenades = globals->globals.grenades;
    if (grenades.storedCount <= 0)
        return 0xFFFFFFFFu;
    if (grenades.count != grenades.storedCount)
        return std::nullopt;
    for (int32_t i = 0; i < grenades.count; i++)
        if (grenades[i].equipment.datum == ctx.r3.u32)
            return uint32_t(i);
    return 0xFFFFFFFFu;
})

// campaign_metagame_get_difficulty_multiplier() -> real: the Campaign
// Metagame Globals' (exactly one element) difficulty multiplier for the
// campaign difficulty (game globals +332; 1 outside the campaign), or 1
// when there's no such element or the difficulty is out of its range
// (name unconfirmed).
H3_REPLACE_FLOAT(82218B28, campaign_metagame_get_difficulty_multiplier, {
    constexpr uint32_t kGameGlobalsMode = 16;
    constexpr uint32_t kGameGlobalsDifficulty = 332;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    float one = objects::LoadFloat(kRealOne);
    const auto& metagame = globals->globals.campaign_metagame_globals;
    if (metagame.storedCount != 1)
        return one;
    if (metagame.count != 1)
        return std::nullopt;
    uint32_t game = objects::TlsSlot(ctx, h3::native::game::tls::kGameGlobals);
    int32_t difficulty = 1;
    if (int32_t(objects::Load32(game + kGameGlobalsMode)) == 1)
        difficulty = objects::LoadS16(game + kGameGlobalsDifficulty);
    const auto& scales = metagame[0].difficulty;
    if (difficulty < 0 || difficulty >= scales.storedCount)
        return one;
    if (difficulty >= scales.count)
        return std::nullopt;
    return scales[difficulty].difficulty_multiplier;
})

// object_definition_find_global_material(r3 definition tag data: +624
// block of 64-byte entries with an s16 global material index at +8, r4
// s16 global material index) -> guest address of the entry for the
// material, else for its parent material and so on up the game globals'
// Materials (runtime indexes '!' from guest memory), else the default at
// 0x828C4960 (name unconfirmed; the tag's group is unknown, so its block
// is read from guest memory).
H3_REPLACE_INT(82344468, object_definition_find_global_material, {
    constexpr uint32_t kEntries = 624;
    constexpr uint32_t kEntryBytes = 64;
    constexpr uint32_t kEntryMaterial = 8;           // s16
    constexpr uint32_t kGlobalMaterialBytes = 368;
    constexpr uint32_t kGlobalMaterialParent = 8;    // s16 runtime index of the parent
    constexpr uint32_t kDefaultEntry = 0x828C4960;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& materials = globals->globals.materials;
    int32_t count = int32_t(objects::Load32(ctx.r3.u32 + kEntries));
    uint32_t entries = objects::Load32(ctx.r3.u32 + kEntries + 4);
    int32_t material = int16_t(uint16_t(ctx.r4.u32));
    for (int depth = 0; depth < 65536; depth++)
    {
        if (material < 0 || material >= materials.storedCount || materials.address == 0)
            return kDefaultEntry;
        for (int32_t i = 0; i < count; i++)
            if (objects::LoadS16(entries + uint32_t(i) * kEntryBytes + kEntryMaterial) == material)
                return entries + uint32_t(i) * kEntryBytes;
        material = objects::LoadS16(materials.address + uint32_t(material) * kGlobalMaterialBytes + kGlobalMaterialParent);
    }
    return std::nullopt;  // a parent loop: the original never returns
})

namespace
{
    constexpr uint32_t kGlobalMaterialBytes = 368;
    constexpr uint32_t kGlobalMaterialParent = 8;  // s16 runtime index of the parent ('!': guest memory)
}

// material_effects_type_find_material(r3 material effects type entry: +12
// sounds, +24 effects (40-byte elements, +36 s16 runtime material index),
// r4 effects (else sounds), r5 s16 global material index) -> guest address
// of the first element for the material, else for its parent material
// and so on up the game globals' Materials, or 0 (name unconfirmed).
H3_REPLACE_INT(8225E2D8, material_effects_type_find_material, {
    constexpr uint32_t kElementBytes = 40;
    constexpr uint32_t kElementMaterial = 36;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    uint32_t type = ctx.r3.u32;
    uint32_t block = type + (ctx.r4.u32 == 0 ? 12 : 24);
    int32_t count = int32_t(objects::Load32(block));
    uint32_t elements = objects::Load32(block + 4);
    if (count > 0 && elements == 0)
        return std::nullopt;
    const auto& materials = globals->globals.materials;
    int32_t material = int16_t(uint16_t(ctx.r5.u32));
    for (int depth = 0; depth < 65536; depth++)
    {
        if (material < 0 || material >= materials.storedCount)
            return 0u;
        uint32_t entry = materials.address + uint32_t(material) * kGlobalMaterialBytes;
        if (entry == 0)
            return 0u;
        for (int32_t i = 0; i < count; i++)
            if (objects::LoadS16(elements + uint32_t(i) * kElementBytes + kElementMaterial) == material)
                return elements + uint32_t(i) * kElementBytes;
        material = objects::LoadS16(entry + kGlobalMaterialParent);
    }
    return std::nullopt;  // a parent loop: the original never returns
})

// global_material_find_effect(r3 s16 global material index, r4 type, r5
// effects, r6) -> material_effects_find_material on the material's
// Material Effects tag (skipped when NONE), else on its parent's and so
// on up the game globals' Materials; 0 when none answers (name
// unconfirmed).
H3_REPLACE_INT(8225E4C0, global_material_find_effect, {
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& materials = globals->globals.materials;
    uint32_t type = ctx.r4.u32, kind = ctx.r5.u32, extra = ctx.r6.u32;
    int32_t material = int16_t(uint16_t(ctx.r3.u32));
    for (int depth = 0; depth < 65536; depth++)
    {
        if (material < 0 || material >= materials.storedCount)
            return 0u;
        uint32_t entry = materials.address + uint32_t(material) * kGlobalMaterialBytes;
        if (entry == 0)
            return 0u;
        if (material >= materials.count)
            return std::nullopt;
        uint32_t effects = materials[material].material_effects.datum;
        if (effects != host::kNone)
        {
            uint32_t found = h3::native::Call(ctx, base, sub_820F6900, effects, type, kind, extra);
            if (found != 0)
                return found;
        }
        material = objects::LoadS16(entry + kGlobalMaterialParent);
    }
    return std::nullopt;
})

// campaign_metagame_get_skull_bonus() -> real: 0 plus the Campaign
// Metagame Globals' (exactly one element) Primary Skulls multipliers of
// the active primary skulls (game globals +340 bits 0-8, campaign only)
// and Secondary Skulls multipliers of the active secondary skulls (+344
// bits 0-4) that have elements (name unconfirmed).
H3_REPLACE_FLOAT(82218BB0, campaign_metagame_get_skull_bonus, {
    constexpr uint32_t kRealZero = 0x8202251C;  // lis -32254, +9500
    constexpr uint32_t kGameGlobalsMode = 16;
    constexpr uint32_t kGameGlobalsPrimarySkulls = 340;
    constexpr uint32_t kGameGlobalsSecondarySkulls = 344;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& metagame = globals->globals.campaign_metagame_globals;
    const host::campaign_metagame_globals_block* element = nullptr;
    if (metagame.storedCount == 1)
    {
        if (metagame.count != 1)
            return std::nullopt;
        element = &metagame[0];
    }
    uint32_t game = objects::TlsSlot(ctx, h3::native::game::tls::kGameGlobals);
    bool campaign = int32_t(objects::Load32(game + kGameGlobalsMode)) == 1;
    uint32_t primary = campaign ? objects::Load32(game + kGameGlobalsPrimarySkulls) : 0;
    uint32_t secondary = campaign ? objects::Load32(game + kGameGlobalsSecondarySkulls) : 0;
    float sum = objects::LoadFloat(kRealZero);
    for (int32_t i = 0; i < 9; i++)
    {
        if ((primary & (1u << i)) == 0)
            continue;
        if (!element)
            return std::nullopt;  // the original reads through null
        const auto& skulls = element->primary_skulls;
        if (i >= skulls.storedCount)
            continue;
        if (i >= skulls.count)
            return std::nullopt;
        sum = float(double(skulls[i].difficulty_multiplier) + double(sum));
    }
    for (int32_t i = 0; i < 5; i++)
    {
        if ((secondary & (1u << i)) == 0)
            continue;
        if (!element)
            return std::nullopt;
        const auto& skulls = element->secondary_skulls;
        if (i >= skulls.storedCount)
            continue;
        if (i >= skulls.count)
            return std::nullopt;
        sum = float(double(skulls[i].difficulty_multiplier) + double(sum));
    }
    return sum;
})

namespace
{
    // The index of the sound effect collection's (datum, 'sfx+') first
    // sound effect with that name, or NONE (also for a NONE datum);
    // nullopt when the host copy can't say (sub_82287A70, no group check).
    std::optional<uint32_t> SoundEffectIndex(uint32_t datum, uint32_t name)
    {
        if (datum == host::kNone)
            return host::kNone;
        const auto* tag = h3::native::tags::TagBySlotOf<host::sound_effect_collection_tag>(datum & 0xFFFF);
        if (!tag)
            return std::nullopt;
        const auto& effects = tag->sound_effect_collection.sound_effects;
        if (effects.storedCount <= 0)
            return host::kNone;
        if (effects.count != effects.storedCount)
            return std::nullopt;
        for (int32_t i = 0; i < effects.count; i++)
            if (effects[i].name == name)
                return uint32_t(i);
        return host::kNone;
    }
}

// sound_effect_find(r3 name string id) -> handle of the named sound
// effect: 0xC0000000 for name 243, the scenario's Sound Effect Collection
// index | 0x80000000, else the game globals' Sound Globals' Sound Effects
// collection index | 0x40000000, else NONE (also for 0 / NONE) (name
// unconfirmed).
H3_REPLACE_INT(822879A8, sound_effect_find, {
    uint32_t name = ctx.r3.u32;
    if (name == 0 || name == host::kNone)
        return host::kNone;
    if (name == 243)
        return 0xC0000000u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!scenario || !globals)
        return std::nullopt;
    std::optional<uint32_t> index = SoundEffectIndex(scenario->scenario.sound_effect_collection.datum, name);
    if (!index)
        return std::nullopt;
    if (*index != host::kNone)
        return (*index & 0x3FFFFFFF) | 0x80000000;
    const auto& sound = globals->globals.sound_globals;
    if (sound.storedCount == 0 || sound.address == 0)
        return host::kNone;
    if (sound.count <= 0)
        return std::nullopt;
    index = SoundEffectIndex(sound[0].sound_effects.datum, name);
    if (!index)
        return std::nullopt;
    if (*index == host::kNone)
        return host::kNone;
    return (*index & 0x3FFFFFFF) | 0x40000000;
})

// object_is_upended(r3 object) -> bool: the z of the object's up vector
// (sub_820A1B48's second output) is below the cosine of (the constant at
// 0x82000A90, presumably pi/2, minus the game globals' Player Control
// Minimum Angle For Vehicle Flipping) (name unconfirmed).
H3_REPLACE_INT(820A1AD0, object_is_upended, {
    constexpr uint32_t kRealRightAngle = 0x82000A90;  // lis -32256, +2704
    constexpr uint32_t kMinimumFlipAngle = 112;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    uint32_t control = globals->globals.player_control.address;
    if (control == 0)
        return std::nullopt;  // the original reads through null
    uint32_t object = ctx.r3.u32;
    float angle = objects::LoadFloat(control + kMinimumFlipAngle);
    ctx.f1.f64 = double(float(double(objects::LoadFloat(kRealRightAngle)) - double(angle)));
    h3::native::Call(ctx, base, sub_82081428);
    float limit = float(ctx.f1.f64);
    h3::native::GuestFrame frame(ctx, base, 16);
    h3::native::Call(ctx, base, sub_820A1B48, object, 0u, frame.Address(0));
    return objects::LoadFloat(frame.Address(8)) < limit ? 1u : 0u;
})

// user_interface_navigation_delay_elapsed(r4 u8 skip, r5 u16 elapsed ms)
// -> bool: false with no shared globals ('wigl'); true when skip is 1 or
// the elapsed time reaches the shared globals' Navigation Tab Delay ms
// (name unconfirmed).
H3_REPLACE_INT(820894E8, user_interface_navigation_delay_elapsed, {
    bool none, unknown;
    const host::user_interface_globals_definition_tag* tag = InterfaceGlobals(none, unknown);
    if (none)
        return 0u;
    if (!tag)
        return std::nullopt;
    uint32_t datum = tag->user_interface_globals_definition.shared_globals.datum;
    if (ReferencedTag(datum) == 0)
        return 0u;
    const auto* shared = h3::native::tags::TagBySlotOf<host::user_interface_shared_globals_definition_tag>(datum & 0xFFFF);
    if (!shared)
        return std::nullopt;
    if ((ctx.r4.u32 & 0xFF) == 1)
        return 1u;
    int32_t elapsed = int32_t(ctx.r5.u32 & 0xFFFF);
    return elapsed >= shared->user_interface_shared_globals_definition.navigation_tab_delay_ms ? 1u : 0u;
})

#endif
