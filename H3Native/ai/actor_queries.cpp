// AI queries: actors, props and characters, combining the AI game state
// (guest memory) with definitions (host tag cache), decompiled
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "game/game_state_layout.h"
#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

#include <cmath>

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    namespace tls = h3::native::game::tls;
    constexpr uint32_t kObjectGroup = 0x6F626A65;  // 'obje'
    constexpr uint32_t kUnitGroup = 0x756E6974;    // 'unit'
    constexpr uint32_t kNone = objects::kNone;
    constexpr uint32_t kRealZero = 0x8202251C;     // read-only real the originals load

    // Data array elements.
    constexpr uint32_t kActorBytes = 2700;
    constexpr uint32_t kActorUnit = 20;            // object datum
    constexpr uint32_t kActorSquad = 48;           // squad datum
    constexpr uint32_t kActorCharacter = 92;       // 'char' datum
    constexpr uint32_t kActorLastGuardianSurge = 760;  // s32 game time, -1 never
    constexpr uint32_t kActorCover = 1196;         // must not be NONE (82542338)
    constexpr uint32_t kActorCoverObject = 1208;   // object datum
    constexpr uint32_t kActorCommandScript = 2688; // command script datum
    constexpr uint32_t kCommandScriptBytes = 392;
    constexpr uint32_t kCommandScriptHasStyle = 337;  // u8
    constexpr uint32_t kCommandScriptStyle = 340;     // 'styl' datum
    constexpr uint32_t kSquadBytes = 176;
    constexpr uint32_t kSquadStyle = 164;          // 'styl' datum
    constexpr uint32_t kTlsPropReferences = 104;
    constexpr uint32_t kPropReferenceBytes = 60;
    constexpr uint32_t kPropBytes = 196;
    constexpr uint32_t kPropObject = 8;            // object datum

    // Object data fields.
    constexpr uint32_t kObjectNextSibling = 8;
    constexpr uint32_t kObjectFirstChild = 12;
    constexpr uint32_t kObjectParent = 16;
    constexpr uint32_t kObjectType = 154;          // s8
    constexpr uint32_t kUnitParentSeat = 582;      // s16
    constexpr uint32_t kSeatBoarding = 1u << 11;   // unit seat Flags: Boarding Seat
    constexpr uint32_t kAiTypeName143 = 143;       // string id (name unknown)

    // The obje part of the definition in this slot, or nullptr.
    const host::object_struct_definition* ObjectPart(uint32_t definition)
    {
        return h3::native::tags::TagPartBySlot<host::object_struct_definition>(definition & 0xFFFF, kObjectGroup);
    }

    constexpr int kMaxChain = 256;  // longer object/character chains are loops
}

// actor_get_cover_destroyable_state(r3 actor) -> 3 if the actor has cover
// (+1196 not NONE) whose object (+1208) has AI properties with flag bit 0
// (destroyable cover, probably) and a real at +248 above 0 (its vitality,
// probably); else 0. (3 is presumably an enum value; name a guess.)
H3_REPLACE_INT(82542338, actor_get_cover_destroyable_state, {
    uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, ctx.r3.u32, kActorBytes);
    if (objects::Load32(actor + kActorCover) == kNone)
        return 0u;
    uint32_t object = objects::Load32(actor + kActorCoverObject);
    if (object == kNone)
        return 0u;
    uint32_t data = objects::ObjectData(ctx, object);
    const host::object_struct_definition* part = ObjectPart(objects::Load32(data));
    if (!part)
        return std::nullopt;
    const auto& properties = part->ai_properties;
    if (properties.storedCount <= 0)
        return 0u;
    if (properties.count <= 0)
        return std::nullopt;
    if ((properties[0].ai_flags & 1) == 0)
        return 0u;
    return objects::LoadFloat(data + 248) > objects::LoadFloat(kRealZero) ? 3u : 0u;
})

// actor_unit_has_equipment_with_ai_type(r3 actor) -> bool: one of the
// actor's unit's children is equipment whose first AI properties element
// has AI Type Name string id 143.
H3_REPLACE_INT(820C21D8, actor_unit_has_equipment_with_ai_type, {
    uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, ctx.r3.u32, kActorBytes);
    uint32_t unit = objects::ObjectData(ctx, objects::Load32(actor + kActorUnit));
    uint32_t child = objects::Load32(unit + kObjectFirstChild);
    for (int i = 0; i < 65536 && child != kNone; i++)
    {
        uint32_t data = objects::ObjectData(ctx, child);
        if (objects::Load8(data + kObjectType) == 3)  // equipment (lbz, unsigned)
        {
            const host::object_struct_definition* part = ObjectPart(objects::Load32(data));
            if (!part)
                return std::nullopt;
            const auto& properties = part->ai_properties;
            if (properties.storedCount > 0)
            {
                if (properties.count <= 0)
                    return std::nullopt;
                if (properties[0].ai_type_name == kAiTypeName143)
                    return 1u;
            }
        }
        child = objects::Load32(data + kObjectNextSibling);
    }
    return child == kNone ? std::optional<uint32_t>(0u) : std::nullopt;
})

namespace
{
    // The datum of the style tag actor_get_style returns: its command
    // script's style if it sets one whose tag table entry isn't empty, else
    // its squad's; NONE when actor_get_style returns 0.
    uint32_t ActorStyleDatum(const PPCContext& ctx, uint32_t actorIndex)
    {
        uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, actorIndex, kActorBytes);
        uint32_t script = objects::Load32(actor + kActorCommandScript);
        if (script != kNone)
        {
            uint32_t element = objects::DataArrayElement(ctx, tls::kCommandScripts, script, kCommandScriptBytes);
            if (objects::Load8(element + kCommandScriptHasStyle) != 0)
            {
                uint32_t style = objects::Load32(element + kCommandScriptStyle);
                if (objects::RawTagAddress(style) != 0)
                    return style;
            }
        }
        uint32_t squad = objects::Load32(actor + kActorSquad);
        if (squad == kNone)
            return kNone;
        uint32_t style = objects::Load32(objects::DataArrayElement(ctx, tls::kSquads, squad, kSquadBytes) + kSquadStyle);
        if (style == kNone || objects::RawTagAddress(style) == 0)
            return kNone;
        return style;
    }
}

// actor_get_style(r3 actor) -> guest address of the actor's style tag
// ('styl'): its command script's style if it sets one, else its squad's;
// 0 if neither. Guest memory only (the tag table by slot, as the original).
H3_REPLACE_INT(820B4710, actor_get_style, {
    uint32_t style = ActorStyleDatum(ctx, ctx.r3.u32);
    return style == kNone ? 0u : objects::RawTagAddress(style);
})

// actor_can_guardian_surge(r3 actor) -> bool: true unless the actor's
// character (or the first parent character with guardian properties)
// has Guardian Properties and the actor surged (+760) no longer than
// Surge Delay Time ago. True with no guardian properties or no surge yet.
H3_REPLACE_INT(82571390, actor_can_guardian_surge, {
    uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, ctx.r3.u32, kActorBytes);
    uint32_t character = objects::Load32(actor + kActorCharacter);
    const host::character_guardian_block* guardian = nullptr;
    uint32_t guardianAddress = 0;
    for (int depth = 0; depth < kMaxChain && character != kNone; depth++)
    {
        const host::character_tag* tag = h3::native::tags::TagBySlotOf<host::character_tag>(character & 0xFFFF);
        if (!tag)
            return std::nullopt;
        const auto& properties = tag->character.guardian_properties;
        if (properties.storedCount > 0)
        {
            if (properties.count <= 0)
                return std::nullopt;
            guardian = &properties[0];
            guardianAddress = properties.address;
            break;
        }
        character = tag->character.parent_character.datum;
    }
    if (!guardian && character != kNone)
        return std::nullopt;  // a loop
    if (guardianAddress == 0)
        return 1u;
    int32_t lastSurge = int32_t(objects::Load32(actor + kActorLastGuardianSurge));
    if (lastSurge == -1)
        return 1u;
    uint32_t gameTime = objects::TlsSlot(ctx, tls::kGameTimeGlobals);
    int32_t ticks = int32_t(objects::Load32(gameTime + h3::native::game::game_time::kElapsedTicks) - uint32_t(lastSurge));
    float seconds = float(double(int64_t(ticks))) * objects::LoadFloat(gameTime + h3::native::game::game_time::kSecondsPerTick);
    return seconds > guardian->surge_delay_time ? 1u : 0u;
})

// character_get_grenade_properties(r3 character datum, r4 s16 grenade
// type) -> guest address of the first Grenades Properties element for
// that grenade type, looking through parent characters; 0 if none.
H3_REPLACE_INT(820BF010, character_get_grenade_properties, {
    constexpr uint32_t kGrenadeBytes = 0x3C;
    uint32_t character = ctx.r3.u32;
    int16_t type = int16_t(uint16_t(ctx.r4.u32));
    for (int depth = 0; depth < kMaxChain && character != kNone; depth++)
    {
        const host::character_tag* tag = h3::native::tags::TagBySlotOf<host::character_tag>(character & 0xFFFF);
        if (!tag)
            return std::nullopt;
        const auto& grenades = tag->character.grenades_properties;
        if (grenades.storedCount > grenades.count)
            return std::nullopt;
        // The original counts with a 16-bit index: fine for any real block.
        for (int32_t i = 0; i < grenades.count; i++)
            if (grenades[i].grenade_type == type)
                return grenades.address + uint32_t(i) * kGrenadeBytes;
        character = tag->character.parent_character.datum;
    }
    return character == kNone ? std::optional<uint32_t>(0u) : std::nullopt;
})

// prop_get_object(r3 prop) -> the prop's object (+8) if it's a biped in a
// Boarding Seat of its parent unit, else the root of its parent chain
// (NONE for NONE). Name a guess.
H3_REPLACE_INT(820C00C0, prop_get_object, {
    uint32_t prop = objects::DataArrayElement(ctx, tls::kProps, ctx.r3.u32, kPropBytes);
    uint32_t object = objects::Load32(prop + kPropObject);
    uint32_t header = objects::ObjectHeader(ctx, object);
    uint32_t data = objects::Load32(header + objects::kObjectHeaderData);
    if (objects::Load8(data + kObjectType) == 0 && (objects::ShiftBit(objects::Load8(header + 3)) & 0x1003) != 0)
    {
        uint32_t parent = objects::Load32(data + kObjectParent);
        int32_t seat = objects::LoadS16(data + kUnitParentSeat);
        if (parent != kNone && seat != -1)
        {
            uint32_t definition = objects::ObjectDefinition(ctx, parent);
            const host::unit_struct_definition* unit =
                h3::native::tags::TagPartBySlot<host::unit_struct_definition>(definition & 0xFFFF, kUnitGroup);
            if (!unit)
                return std::nullopt;
            if (seat < 0 || seat >= unit->seats.count)
                return std::nullopt;  // the original reads outside the block
            if (unit->seats[seat].flags & kSeatBoarding)
                return object;
        }
    }
    if (object == kNone)
        return kNone;
    for (int depth = 0; depth < kMaxChain; depth++)
    {
        uint32_t parent = objects::Load32(objects::ObjectData(ctx, object) + kObjectParent);
        if (parent == kNone)
            return object;
        object = parent;
    }
    return std::nullopt;
})

// prop_reference_can_be_reanimated(r3 prop reference) -> bool: a prop
// reference in state 1 or 2 (+28) of kind 6 or 7 (+2) whose object is a
// biped with object flag +276 bit 2 (dead, probably), whose definition has
// a Reanimation Character, and whose +21 byte doesn't have both bits 0
// and 1. Name a guess (the flood reanimating bodies).
H3_REPLACE_INT(82570BE0, prop_reference_can_be_reanimated, {
    uint32_t reference = objects::DataArrayElement(ctx, kTlsPropReferences, ctx.r3.u32, kPropReferenceBytes);
    int32_t state = objects::LoadS8(reference + 28);
    if (state < 1 || state > 2)
        return 0u;
    int32_t kind = objects::LoadS8(reference + 2);
    if (kind != 7 && kind != 6)
        return 0u;
    uint32_t data = objects::ObjectData(ctx, objects::Load32(reference + 16));
    if (((objects::Load32(data + 276) >> 2) & 1) == 0)
        return 0u;
    if (objects::Load8(data + kObjectType) != 0)  // biped (lbz, unsigned)
        return 0u;
    const host::biped_tag* biped = h3::native::tags::TagBySlotOf<host::biped_tag>(objects::Load32(data) & 0xFFFF);
    if (!biped)
        return std::nullopt;
    if (biped->biped.reanimation_character.datum == kNone)
        return 0u;
    uint8_t flags = objects::Load8(data + 21);
    return ((flags & 1) && (flags & 2)) ? 0u : 1u;
})

// Actor property lookups: a character properties block (one element per
// mode or attitude) on the actor's character, or the first parent
// character that has elements.
PPC_EXTERN_FUNC(sub_820AE510);  // actor_get_weapon (guest memory only)

namespace
{
    // Guest address of element index of the first character up the actor's
    // character's parent chain whose block (member) has elements: that
    // block's first element when index is past its end (the index is
    // compared signed, as the originals do: a negative one gives an address
    // before the elements); 0 if no character in the chain has any.
    template <typename Element>
    std::optional<uint32_t> CharacterPropertiesElement(const PPCContext& ctx, uint32_t actorIndex,
        host::Block<Element> host::character_struct_definition::*member, uint32_t elementBytes, int32_t index)
    {
        uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, actorIndex, kActorBytes);
        uint32_t character = objects::Load32(actor + kActorCharacter);
        for (int depth = 0; depth < kMaxChain && character != kNone; depth++)
        {
            const host::character_tag* tag = h3::native::tags::TagBySlotOf<host::character_tag>(character & 0xFFFF);
            if (!tag)
                return std::nullopt;
            const host::Block<Element>& block = tag->character.*member;
            if (block.storedCount > 0)
            {
                if (index < block.storedCount)
                    return block.address + uint32_t(index * int32_t(elementBytes));
                return block.address;
            }
            character = tag->character.parent_character.datum;
        }
        return character == kNone ? std::optional<uint32_t>(0u) : std::nullopt;
    }

    // The attitude of the actor's style (actor_get_style; 0 without one),
    // the index of the attitude-dependent character properties.
    std::optional<int32_t> ActorStyleAttitude(const PPCContext& ctx, uint32_t actorIndex)
    {
        uint32_t style = ActorStyleDatum(ctx, actorIndex);
        if (style == kNone)
            return 0;
        const host::style_tag* tag = h3::native::tags::TagBySlotOf<host::style_tag>(style & 0xFFFF);
        if (!tag)
            return std::nullopt;
        return int32_t(tag->style.attitude);
    }

    // The character properties element for the actor's style attitude.
    template <typename Element>
    std::optional<uint32_t> ActorAttitudeProperties(const PPCContext& ctx,
        host::Block<Element> host::character_struct_definition::*member, uint32_t elementBytes)
    {
        std::optional<int32_t> attitude = ActorStyleAttitude(ctx, ctx.r3.u32);
        if (!attitude)
            return std::nullopt;
        return CharacterPropertiesElement(ctx, ctx.r3.u32, member, elementBytes, *attitude);
    }
}

// actor_get_<kind>_properties(r3 actor) -> guest address of the character
// properties element for the actor's style attitude (the first element if
// the block has fewer), from its character or the first parent character
// with any; 0 if none has.
H3_REPLACE_INT(82518F20, actor_get_swarm_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::swarm_properties, 56);
})
H3_REPLACE_INT(820B4630, actor_get_charge_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::charge_properties, 124);
})
H3_REPLACE_INT(820C3490, actor_get_evasion_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::evasion_properties, 20);
})
H3_REPLACE_INT(820C3870, actor_get_cover_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::cover_properties, 84);
})
H3_REPLACE_INT(820C8F30, actor_get_retreat_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::retreat_properties, 88);
})
H3_REPLACE_INT(820D2588, actor_get_search_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::search_properties, 32);
})
H3_REPLACE_INT(820CB3C0, actor_get_presearch_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::pre_search_properties, 40);
})
H3_REPLACE_INT(825190E0, actor_get_combatform_properties, {
    return ActorAttitudeProperties(ctx, &host::character_struct_definition::combatform_properties, 8);
})

// actor_get_firing_pattern_properties(r3 actor) -> guest address of the
// Firing Pattern Properties element for the actor's weapon's definition,
// from its character or the first parent character that has one; 0 if
// none has or the actor has no weapon.
H3_REPLACE_INT(820C1CD0, actor_get_firing_pattern_properties, {
    constexpr uint32_t kFiringPatternBytes = 28;
    uint32_t actorIndex = ctx.r3.u32;
    uint32_t weapon = h3::native::Call(ctx, base, sub_820AE510, actorIndex);
    if (weapon == kNone)
        return 0u;
    uint32_t weaponDefinition = objects::ObjectDefinition(ctx, weapon);
    uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, actorIndex, kActorBytes);
    uint32_t character = objects::Load32(actor + kActorCharacter);
    for (int depth = 0; depth < kMaxChain && character != kNone; depth++)
    {
        const host::character_tag* tag = h3::native::tags::TagBySlotOf<host::character_tag>(character & 0xFFFF);
        if (!tag)
            return std::nullopt;
        const auto& patterns = tag->character.firing_pattern_properties;
        if (patterns.storedCount > patterns.count || patterns.storedCount > 0x7FFF)
            return std::nullopt;
        for (int32_t i = 0; i < patterns.count; i++)
            if (patterns[i].weapon.datum == weaponDefinition)
                return patterns.address + uint32_t(i) * kFiringPatternBytes;
        character = tag->character.parent_character.datum;
    }
    return character == kNone ? std::optional<uint32_t>(0u) : std::nullopt;
})

// actor_weapon_lacks_flag_24(r3 actor) -> bool: the actor has a weapon
// whose definition doesn't have weapon flag bit 24, and its unit (if any)
// doesn't have object flag +21 bit 1 (name unconfirmed).
H3_REPLACE_INT(820B4560, actor_weapon_lacks_flag_24, {
    constexpr uint32_t kWeaponGroup = 0x77656170;  // 'weap'
    uint32_t actorIndex = ctx.r3.u32;
    uint32_t weapon = h3::native::Call(ctx, base, sub_820AE510, actorIndex);
    if (weapon == kNone)
        return 0u;
    uint32_t definition = objects::ObjectDefinition(ctx, weapon);
    const host::weapon* part = h3::native::tags::TagPartBySlot<host::weapon>(definition & 0xFFFF, kWeaponGroup);
    if (!part)
        return std::nullopt;
    if (part->flags & (1u << 24))
        return 0u;
    uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, actorIndex, kActorBytes);
    uint32_t unit = objects::Load32(actor + kActorUnit);
    if (unit != kNone && (objects::Load8(objects::ObjectData(ctx, unit) + 21) & 2))
        return 0u;
    return 1u;
})

PPC_EXTERN_FUNC(sub_820ADCF0);  // character_get_general_properties
PPC_EXTERN_FUNC(sub_820AE150);  // character_get_movement_properties

namespace
{
    // The game globals' AI Globals element (guest address), or 0 when the
    // block is empty; nullopt without host globals.
    std::optional<uint32_t> AiGlobalsAddress()
    {
        const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
        if (!globals)
            return std::nullopt;
        const auto& ai = globals->globals.ai_globals;
        return ai.storedCount > 0 ? ai.address : 0u;
    }
    constexpr uint32_t kAiGlobalsScaryTargetThreshold = 368;  // real
    constexpr uint32_t kAiGlobalsJumpDownHeights = 172;       // 6 reals: down .. tower
    constexpr uint32_t kGeneralPropertiesScariness = 20;      // real
    constexpr uint32_t kMovementPropertiesJumpHeight = 26;    // s16 enum
}

// actor_is_scary(r3 actor) -> bool: the actor's character has General
// Properties and their scariness (+20) is at least the AI Globals' Scary
// Target Threshold (false with no AI Globals) (name unconfirmed).
H3_REPLACE_INT(82564098, actor_is_scary, {
    std::optional<uint32_t> ai = AiGlobalsAddress();
    if (!ai)
        return std::nullopt;
    uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, ctx.r3.u32, kActorBytes);
    uint32_t properties = h3::native::Call(ctx, base, sub_820ADCF0, objects::Load32(actor + kActorCharacter));
    if (properties == 0 || *ai == 0)
        return 0u;
    float scariness = objects::LoadFloat(properties + kGeneralPropertiesScariness);
    return scariness >= objects::LoadFloat(*ai + kAiGlobalsScaryTargetThreshold) ? 1u : 0u;
})

// actor_get_jump_down_speed(r3 actor) -> real: for the character's
// Movement Properties jump height (+26; 7 gives the constant at
// 0x82024078, above 6 counts as 6) the AI Globals' Maximum Jump Down
// Height of that index - 1 (0 out of range or with no AI Globals) times
// two constants, square-rooted (name unconfirmed).
H3_REPLACE_FLOAT(825015F0, actor_get_jump_down_speed, {
    constexpr uint32_t kRealSeven = 0x82024078;    // lis -32254, +17112 -608
    constexpr uint32_t kRealScale1 = 0x820242D8;   // lis -32254, +17112
    constexpr uint32_t kRealScale2 = 0x82058750;   // lis -32250, -30896
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    uint32_t actor = objects::DataArrayElement(ctx, tls::kActors, ctx.r3.u32, kActorBytes);
    uint32_t properties = h3::native::Call(ctx, base, sub_820AE150, objects::Load32(actor + kActorCharacter));
    if (properties == 0)
        return std::nullopt;  // the original reads through null
    int32_t height = objects::LoadS16(properties + kMovementPropertiesJumpHeight);
    if (height == 7)
        return objects::LoadFloat(kRealSeven);
    if (height >= 6)
        height = 6;
    int32_t index = int16_t(uint16_t(uint32_t(height - 1)));
    float value = objects::LoadFloat(kRealZero);
    const auto& ai = globals->globals.ai_globals;
    if (index >= 0 && index < 6 && ai.storedCount > 0)
        value = objects::LoadFloat(ai.address + kAiGlobalsJumpDownHeights + uint32_t(index) * 4);
    value = float(double(value) * double(objects::LoadFloat(kRealScale1)));
    value = float(double(value) * double(objects::LoadFloat(kRealScale2)));
    return float(std::sqrt(double(value)));
})

PPC_EXTERN_FUNC(sub_820AE5E0);  // actor_find_?(r3 actor, r4 s16 value) -> s16 slot or NONE; reads only
PPC_EXTERN_FUNC(sub_820EBA28);  // object_get_? (r3 object) -> f1 real in [0, 1]; reads only

// object_get_scariness(r3 object) -> real: 0 for NONE, a flagged object
// (+276 bit 2) or one that isn't a biped, vehicle or giant; else the
// actor's character scariness (General Properties +20) or, without an
// actor, the AI Globals' player scariness (+376, for a player object:
// +380 not NONE), plus the parent vehicle seat's (+676 block of 228-byte
// entries, +60) and both weapons' (+900 of their definitions) scariness
// and, from the AI Globals, the berserking (+380, actor +834) and actor
// state 18 (+384) bonuses and, when the player's +640 object is past the
// definition's thresholds (+428, +448, +536), +388 (name unconfirmed;
// object definitions are read from guest memory).
H3_REPLACE_FLOAT(820AE250, object_get_scariness, {
    constexpr uint32_t kRealOne = 0x820221FC;   // lis -32254, +8700
    constexpr uint32_t kRealZero2 = 0x8202251C; // +8700 +800
    uint32_t objectIndex = ctx.r3.u32;
    auto sum = [](double a, double b) { return double(float(a + b)); };
    double zero = objects::LoadFloat(kRealZero2);
    double scariness = zero;
    if (objectIndex == kNone)
        return float(scariness);
    std::optional<uint32_t> aiAddress = AiGlobalsAddress();
    if (!aiAddress)
        return std::nullopt;
    uint32_t ai = *aiAddress;
    uint32_t object = objects::ObjectData(ctx, objectIndex);
    if ((objects::Load32(object + 276) >> 2) & 1)
        return float(zero);
    if ((objects::ShiftBit(uint32_t(int32_t(objects::LoadS8(object + 154)))) & 4099) == 0)
        return float(zero);
    uint32_t actorIndex = objects::Load32(object + 364);
    uint32_t actor = 0;
    if (actorIndex != kNone)
        actor = objects::DataArrayElement(ctx, tls::kActors, actorIndex, kActorBytes);
    uint32_t weapon;
    if (actor != 0)
    {
        uint32_t properties = h3::native::Call(ctx, base, sub_820ADCF0, objects::Load32(actor + kActorCharacter));
        if (properties == 0)
            return std::nullopt;  // the original reads through null
        scariness = objects::LoadFloat(properties + kGeneralPropertiesScariness);
        weapon = h3::native::Call(ctx, base, sub_820AE510, actorIndex);
    }
    else
    {
        int32_t slot = objects::LoadS8(object + 602);
        weapon = slot == -1 ? kNone : objects::Load32(object + uint32_t(slot + 152) * 4);
        if (objects::Load32(object + 380) != kNone && ai != 0)
            scariness = objects::LoadFloat(ai + 376);
    }
    int32_t slot2 = objects::LoadS8(object + 603);
    uint32_t weapon2 = slot2 == -1 ? kNone : objects::Load32(object + uint32_t(slot2 + 152) * 4);
    uint32_t parent = objects::Load32(object + 16);
    if (parent != kNone)
    {
        uint32_t vehicle = objects::ObjectData(ctx, parent);
        int32_t seat = objects::LoadS16(object + 582);
        if (objects::Load8(vehicle + 154) == 1 && seat != -1)
        {
            uint32_t definition = objects::RawTagAddress(objects::Load32(vehicle));
            uint32_t entry = objects::Load32(definition + 676) + uint32_t(seat) * 228;
            scariness = sum(objects::LoadFloat(entry + 60), scariness);
        }
    }
    if (weapon != kNone)
        scariness = sum(objects::LoadFloat(objects::RawTagAddress(objects::ObjectDefinition(ctx, weapon)) + 900), scariness);
    if (weapon2 != kNone)
        scariness = sum(objects::LoadFloat(objects::RawTagAddress(objects::ObjectDefinition(ctx, weapon2)) + 900), scariness);
    if (actor != 0)
    {
        if (ai == 0)
            return float(scariness);
        if (objects::Load8(actor + 834) != 0)
            scariness = sum(objects::LoadFloat(ai + 380), scariness);
        if (int16_t(uint16_t(h3::native::Call(ctx, base, sub_820AE5E0, actorIndex, 18u))) != -1)
            scariness = sum(objects::LoadFloat(ai + 384), scariness);
    }
    if (ai == 0 || objects::Load32(object + 380) == kNone)
        return float(scariness);
    uint32_t target = objects::Load32(object + 640);
    if (target == kNone)
        return float(scariness);
    h3::native::Call(ctx, base, sub_820EBA28, target);
    double value = ctx.f1.f64;
    uint32_t definition = objects::RawTagAddress(objects::ObjectDefinition(ctx, target));
    if (value <= zero)
        return float(scariness);
    double scaled = double(float(double(objects::LoadFloat(kRealOne)) - value));
    scaled = double(float(scaled * double(objects::LoadFloat(definition + 428))));
    if (scaled <= double(objects::LoadFloat(definition + 448)))
        return float(scariness);
    if (objects::Load32(definition + 536) == 0)
        return float(scariness);
    return float(sum(objects::LoadFloat(ai + 388), scariness));
})

#endif
