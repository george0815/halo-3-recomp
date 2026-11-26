// Weapon definition flag and trigger queries, decompiled to read the host
// tag cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The originals read the object's definition at the weapon part's offsets
// without checking its group; these read the 'weap' part and leave
// anything that isn't a weapon to the original (nullopt). Names are
// descriptive (the functions aren't named in the symbol database).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    constexpr uint32_t kWeaponGroup = 0x77656170;  // 'weap'
    // Weapon Flags bits (Assembly's weap plugin names).
    constexpr uint32_t kMustBeReadied = 1u << 3;
    constexpr uint32_t kSupportWeapon = 1u << 13;
    constexpr uint32_t kMeleeOnly = 1u << 24;
    // Weapon object data: one 12-byte state per trigger, its first byte the
    // trigger's state.
    constexpr uint32_t kWeaponTriggerStates = 540;
    constexpr uint32_t kWeaponTriggerStateBytes = 12;

    const host::weapon* ObjectWeapon(const PPCContext& ctx, uint32_t object)
    {
        uint32_t definition = objects::ObjectDefinition(ctx, object);
        return h3::native::tags::TagPartBySlot<host::weapon>(definition & 0xFFFF, kWeaponGroup);
    }
}

// weapon_is_melee_only(r3 weapon object) -> bool: Melee Only; false for
// NONE.
H3_REPLACE_INT(8209A030, weapon_is_melee_only, {
    if (ctx.r3.u32 == objects::kNone)
        return 0u;
    const host::weapon* weapon = ObjectWeapon(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    return (weapon->flags & kMeleeOnly) ? 1u : 0u;
})

// weapon_is_support_or_must_be_readied(r3 weapon object) -> bool: Support
// Weapon or Must Be Readied.
H3_REPLACE_INT(820EFCB8, weapon_is_support_or_must_be_readied, {
    const host::weapon* weapon = ObjectWeapon(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    return (weapon->flags & (kSupportWeapon | kMustBeReadied)) ? 1u : 0u;
})

// unit_find_weapon_that_must_be_readied(r3 unit object) -> the first of the
// unit's 4 weapon slots holding a Must Be Readied weapon, or NONE.
H3_REPLACE_INT(820A02A0, unit_find_weapon_that_must_be_readied, {
    uint32_t unit = objects::ObjectData(ctx, ctx.r3.u32);
    for (uint32_t slot = 0; slot < objects::kUnitWeaponSlots; slot++)
    {
        uint32_t weaponObject = objects::Load32(unit + objects::kUnitWeapons + slot * 4);
        if (weaponObject == objects::kNone)
            continue;
        const host::weapon* weapon = ObjectWeapon(ctx, weaponObject);
        if (!weapon)
            return std::nullopt;
        if (weapon->flags & kMustBeReadied)
            return slot;
    }
    return 0xFFFFFFFFu;
})

// weapon_any_trigger_busy(r3 weapon object) -> bool: one of its triggers
// (as many as the definition's New Triggers) is in state 3, 5, 6 or 7, or
// in state 2 with a Tracking Type set.
H3_REPLACE_INT(8209A228, weapon_any_trigger_busy, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    const host::weapon* weapon = h3::native::tags::TagPartBySlot<host::weapon>(objects::Load32(data) & 0xFFFF, kWeaponGroup);
    if (!weapon)
        return std::nullopt;
    int32_t triggers = weapon->new_triggers.storedCount;
    for (int32_t i = 0; i < triggers; i++)
    {
        int8_t state = int8_t(uint8_t(h3::GuestRef<uint8_t>(data + kWeaponTriggerStates + uint32_t(i) * kWeaponTriggerStateBytes)));
        if (state == 3 || state == 5 || state == 6 || state == 7)
            return 1u;
        if (state == 2 && weapon->tracking_type != 0)
            return 1u;
    }
    return 0u;
})

#endif
