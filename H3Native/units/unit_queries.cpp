// Unit, biped and giant queries combining the unit's game state (guest
// memory) with its definition (host tag cache), decompiled
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    constexpr uint32_t kUnitGroup = 0x756E6974;  // 'unit'
    constexpr uint32_t kNone = objects::kNone;

    constexpr uint32_t kObjectParent = 16;       // object datum
    constexpr uint32_t kObjectType = 154;        // s8
    constexpr uint32_t kUnitParentSeat = 582;    // s16
    constexpr uint32_t kUnitSeatBytes = 228;     // unit_seat_block
    constexpr uint32_t kSeatDriver = 1u << 2;    // unit seat Flags
    constexpr uint32_t kBipedGravityScaleOverride = 1704;  // real
    constexpr uint32_t kGiantBuckleSettingsIndex = 13808;  // s16
    constexpr uint32_t kGiantBuckleBytes = 0x5C;
    constexpr uint32_t kRealZero = 0x8202251C;   // read-only reals the originals load
    constexpr uint32_t kRealDefaultGravityScale = 0x820221FC;  // 0x8202251C - 800
    constexpr uint32_t kDefaultBuckleAnimation = 845;  // string id
}

// biped_get_gravity_scale(r3 biped object) -> real: the definition's
// ground physics Gravity Scale (a default if not above 0), times the
// biped's override (+1704) when that is above 0.
H3_REPLACE_FLOAT(823463C0, biped_get_gravity_scale, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    const host::biped_tag* biped = h3::native::tags::TagBySlotOf<host::biped_tag>(objects::Load32(data) & 0xFFFF);
    if (!biped)
        return std::nullopt;
    float zero = objects::LoadFloat(kRealZero);
    float scale = objects::LoadFloat(kRealDefaultGravityScale);
    float definition = biped->biped.physics.ground_physics.gravity_scale;
    if (definition > zero)
        scale = definition;
    float override = objects::LoadFloat(data + kBipedGravityScaleOverride);
    if (override > zero)
        scale = override * scale;
    return scale;
})

// giant_get_buckle_animation(r3 giant object) -> string id: the Buckle
// Animation of the definition's buckle-settings element the giant uses
// (+13808), or 845 if it is NONE.
H3_REPLACE_INT(82360C08, giant_get_buckle_animation, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    const host::giant_tag* giant = h3::native::tags::TagBySlotOf<host::giant_tag>(objects::Load32(data) & 0xFFFF);
    if (!giant)
        return std::nullopt;
    int32_t index = objects::LoadS16(data + kGiantBuckleSettingsIndex);
    const auto& buckles = giant->giant.buckle_settings;
    // The original tests the element's address against 0, nothing else.
    if (buckles.address + uint32_t(index) * kGiantBuckleBytes == 0)
        return kDefaultBuckleAnimation;
    if (index < 0 || index >= buckles.count)
        return std::nullopt;
    uint32_t animation = buckles[index].buckle_animation;
    return animation == kNone ? kDefaultBuckleAnimation : animation;
})

// unit_get_parent_seat_definition(r3 object) -> guest address of the
// object's seat (+582) in its parent's unit definition Seats block, or 0
// if it has no parent or seat or the parent isn't a biped, vehicle or
// giant. Not bounds-checked, like the original (it only forms the address).
H3_REPLACE_INT(82324F70, unit_get_parent_seat_definition, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t parent = objects::Load32(data + kObjectParent);
    if (parent == kNone)
        return 0u;
    int32_t seat = objects::LoadS16(data + kUnitParentSeat);
    if (seat == -1)
        return 0u;
    uint32_t parentData = objects::ObjectData(ctx, parent);
    uint32_t typeBit = objects::ShiftBit(uint32_t(int32_t(objects::LoadS8(parentData + kObjectType))));
    if ((typeBit & 0x1003) == 0)  // biped, vehicle, giant
        return 0u;
    const host::unit_struct_definition* unit =
        h3::native::tags::TagPartBySlot<host::unit_struct_definition>(objects::Load32(parentData) & 0xFFFF, kUnitGroup);
    if (!unit)
        return std::nullopt;
    return unit->seats.address + uint32_t(seat) * kUnitSeatBytes;
})

// unit_is_driving_player_training_vehicle(r3 unit, r4 training vehicle
// type, r5 bool any type) -> bool: the unit sits in a Driver seat of a
// vehicle whose Player Training Vehicle Type is r4 (or, with r5, any type
// but None).
H3_REPLACE_INT(82225EB8, unit_is_driving_player_training_vehicle, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t parent = objects::Load32(data + kObjectParent);
    if (parent == kNone)
        return 0u;
    int32_t seat = objects::LoadS16(data + kUnitParentSeat);
    if (seat == -1)
        return 0u;
    uint32_t header = objects::ObjectHeader(ctx, parent);
    if ((objects::ShiftBit(objects::Load8(header + 3)) & 2) == 0)  // vehicle
        return 0u;
    uint32_t definition = objects::Load32(objects::Load32(header + objects::kObjectHeaderData));
    const host::vehicle_tag* vehicle = h3::native::tags::TagBySlotOf<host::vehicle_tag>(definition & 0xFFFF);
    if (!vehicle)
        return std::nullopt;
    const auto& seats = vehicle->unit.seats;
    if (seat < 0 || seat >= seats.count)
        return std::nullopt;  // the original reads outside the block
    if ((seats[seat].flags & kSeatDriver) == 0)
        return 0u;
    int32_t type = vehicle->vehicle.player_training_vehicle_type;
    if (type == int32_t(ctx.r4.u32))
        return 1u;
    if ((ctx.r5.u32 & 0xFF) == 0)
        return 0u;
    return type != 0 ? 1u : 0u;
})

// Recompiled callees (reads only).
PPC_EXTERN_FUNC(sub_820DC770);  // object animation state (r3 data + its s16 at +336) has a usable animation
PPC_EXTERN_FUNC(sub_820A02A0);  // unit_find_weapon_that_must_be_readied
PPC_EXTERN_FUNC(sub_82130598);  // unit_get_first_other_weapon (below)

// unit_animation_test_8215C0A8(r3 object) -> bool: the definition doesn't
// have unit flag bit 11 (or, for another object, bit 11 of the word after
// its obje part), its animation state
// (sub_820DC770) answers true, it has +392 bit 15 or +372 bit 23 set, and
// one of the bytes at +577/+578 isn't 0xFF (name unconfirmed).
H3_REPLACE_INT(8215C0A8, unit_animation_test_8215C0A8, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    // The word after the definition's obje part: a unit's Flags. Other
    // objects get here too (the original doesn't check): for them, the
    // word from guest memory, as the original reads it.
    uint32_t definition = objects::Load32(data);
    const host::unit_struct_definition* unit =
        h3::native::tags::TagPartBySlot<host::unit_struct_definition>(definition & 0xFFFF, kUnitGroup);
    uint32_t flags;
    if (unit)
        flags = unit->flags;
    else
    {
        uint32_t tag = objects::RawTagAddress(definition);
        if (tag == 0)
            return std::nullopt;
        flags = objects::Load32(tag + 248);
    }
    if (flags & (1u << 11))
        return 0u;
    uint32_t state = data + uint32_t(int32_t(objects::LoadS16(data + 336)));
    if ((h3::native::Call(ctx, base, sub_820DC770, state) & 0xFF) == 0)
        return 0u;
    if (!(objects::Load32(data + 392) & 0x8000) && !(objects::Load32(data + 372) & (1u << 23)))
        return 0u;
    return (objects::Load8(data + 577) != 0xFF || objects::Load8(data + 578) != 0xFF) ? 1u : 0u;
})

// unit_get_first_other_weapon(r3 unit object) -> the unit's first weapon
// (slots 0-3, +608) that isn't in its current (+602) or next (+603) slot,
// or NONE. Guest memory only.
H3_REPLACE_INT(82130598, unit_get_first_other_weapon, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    int32_t current = objects::LoadS8(data + 602);
    int32_t next = objects::LoadS8(data + 603);
    for (int32_t slot = 0; slot < 4; slot++)
    {
        if (slot == current || slot == next)
            continue;
        uint32_t weapon = objects::Load32(data + objects::kUnitWeapons + uint32_t(slot) * 4);
        if (weapon != kNone)
            return weapon;
    }
    return kNone;
})

// unit_get_weapon_to_switch_to(r3 unit object) -> the weapon to switch to:
// skipping the slot of the weapon that must be readied (or the current
// slot, when the current weapon is a Support Weapon), the weapon with the
// highest s32 at +624 + 4 * slot (above -1); with no such slot to skip,
// unit_get_first_other_weapon (name unconfirmed).
H3_REPLACE_INT(82130438, unit_get_weapon_to_switch_to, {
    constexpr uint32_t kWeaponGroup = 0x77656170;  // 'weap'
    constexpr uint32_t kSupportWeapon = 1u << 13;
    constexpr uint32_t kUnitWeaponPriority = 624;  // s32 per slot (name unconfirmed)
    uint32_t unit = ctx.r3.u32;
    uint32_t data = objects::ObjectData(ctx, unit);
    int32_t skip = int16_t(uint16_t(h3::native::Call(ctx, base, sub_820A02A0, unit, data)));
    uint32_t fallback = h3::native::Call(ctx, base, sub_82130598, unit);
    int32_t current = objects::LoadS8(data + 602);
    if (current != -1)
    {
        uint32_t weapon = objects::Load32(data + objects::kUnitWeapons + uint32_t(current) * 4);
        if (weapon != kNone)
        {
            const host::weapon* part = h3::native::tags::TagPartBySlot<host::weapon>(
                objects::ObjectDefinition(ctx, weapon) & 0xFFFF, kWeaponGroup);
            if (!part)
                return std::nullopt;
            if (part->flags & kSupportWeapon)
                skip = current;
        }
    }
    if (skip == -1)
        return fallback;
    int32_t best = -1;
    uint32_t result = kNone;
    for (int32_t slot = 0; slot < 4; slot++)
    {
        if (slot == skip)
            continue;
        uint32_t weapon = objects::Load32(data + objects::kUnitWeapons + uint32_t(slot) * 4);
        if (weapon == kNone)
            continue;
        int32_t priority = int32_t(objects::Load32(data + kUnitWeaponPriority + uint32_t(slot) * 4));
        if (priority > best)
        {
            best = priority;
            result = weapon;
        }
    }
    return result;
})

// object_try_and_get_and_verify_type(r3 object, r4 type mask) -> guest
// address of the object's data if it's live (salt matches) and its type
// is in the mask, else 0. Reads only.
PPC_EXTERN_FUNC(sub_820AC748);

namespace
{
    constexpr uint32_t kUnitTypes = 0x1003;  // biped, vehicle, giant
    constexpr uint32_t kUnitControllingUnit = 668;  // object datum (name unconfirmed)

    // The unit part of the definition in this slot, or nullptr.
    const host::unit_struct_definition* UnitPart(uint32_t definition)
    {
        return h3::native::tags::TagPartBySlot<host::unit_struct_definition>(definition & 0xFFFF, kUnitGroup);
    }
}

// unit_get_controlling_unit(r3 object) -> NONE without a parent unit;
// else the parent unit's controller (+668) or, if that's NONE, the parent
// itself; when the parent's definition has unit flag bit 26 and its own
// parent is a unit too, that grandparent's controller (or the grandparent)
// instead (name unconfirmed).
H3_REPLACE_INT(8234BB00, unit_get_controlling_unit, {
    uint32_t parent = objects::Load32(objects::ObjectData(ctx, ctx.r3.u32) + kObjectParent);
    if (parent == kNone)
        return kNone;
    uint32_t data = h3::native::Call(ctx, base, sub_820AC748, parent, kUnitTypes);
    if (data == 0)
        return kNone;
    const host::unit_struct_definition* unit = UnitPart(objects::Load32(data));
    if (!unit)
        return std::nullopt;
    if (unit->flags & (1u << 26))
    {
        uint32_t grandparent = objects::Load32(data + kObjectParent);
        if (grandparent != kNone &&
            (objects::ShiftBit(objects::Load8(objects::ObjectHeader(ctx, grandparent) + 3)) & kUnitTypes))
        {
            uint32_t controller = objects::Load32(objects::ObjectData(ctx, grandparent) + kUnitControllingUnit);
            return controller != kNone ? controller : grandparent;
        }
    }
    uint32_t controller = objects::Load32(data + kUnitControllingUnit);
    return controller != kNone ? controller : parent;
})

#endif
