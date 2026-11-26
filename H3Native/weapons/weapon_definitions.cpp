// Weapon definition lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

#include <cstring>

// The time a projectile takes to cover a distance (r3 projectile
// definition datum, f1 distance): still recompiled (it scales by game
// state through sub_820C0AA8); reads only, so calling it from a reader is
// safe.
PPC_EXTERN_FUNC(sub_82340AB8);

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kWeaponGroup = 0x77656170;  // 'weap'
    // Weapon Flags bits (Assembly's weap plugin names).
    constexpr uint32_t kCanBeDualWielded = 1u << 22;
    constexpr uint32_t kCanOnlyBeDualWielded = 1u << 23;
    constexpr uint32_t kDoesntCountTowardMaximum = 1u << 4;
    constexpr uint32_t kSupportWeapon = 1u << 13;
    constexpr int16_t kWeaponTypeEnergyBlade = 6;
    constexpr int16_t kTriggerBehaviorSwordCharge = 7;

    // The weapon part of a weapon object's definition, or nullptr.
    const host::weapon* WeaponOfObject(const PPCContext& ctx, uint32_t object)
    {
        uint32_t definition = h3::native::objects::ObjectDefinition(ctx, object);
        return h3::native::tags::TagPartBySlot<host::weapon>(definition & 0xFFFF, kWeaponGroup);
    }
}

// weapon_can_be_dual_wielded(r3 weapon object) -> bool: its definition has
// Can Be Dual Wielded or Can Only Be Dual Wielded.
H3_REPLACE_INT(8234B158, weapon_can_be_dual_wielded, {
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, ctx.r3.u32);
    const host::weapon* weapon = h3::native::tags::TagPartBySlot<host::weapon>(definition & 0xFFFF, kWeaponGroup);
    if (!weapon)
        return std::nullopt;
    return (weapon->flags & (kCanBeDualWielded | kCanOnlyBeDualWielded)) ? 1u : 0u;
})

// weapon_definition_can_run_out(r3 weapon definition datum) -> bool: it
// has NO magazines (the original's beq on the Magazines count), or it has
// magazines and a barrel that ages the weapon per round (Age or CAMPAIGN
// Age Generated Per Round above 0). The name predates reading it right:
// the meaning of "no magazines -> true" is unconfirmed.
H3_REPLACE_INT(8234B8C0, weapon_definition_can_run_out, {
    const host::weapon* weapon = h3::native::tags::TagPartBySlot<host::weapon>(ctx.r3.u32 & 0xFFFF, kWeaponGroup);
    if (!weapon)
        return std::nullopt;
    if (weapon->magazines.storedCount == 0)
        return 1u;
    const auto& barrels = weapon->barrels;
    if (barrels.storedCount <= 0)
        return 0u;
    if (barrels.storedCount > barrels.count)
        return std::nullopt;
    for (int32_t i = 0; i < barrels.count; i++)
        if (barrels[i].age_generated_per_round > 0.0f || barrels[i].campaign_age_generated_per_round > 0.0f)
            return 1u;
    return 0u;
})

// weapon_has_magnification(r3 weapon object) -> bool: its definition's
// Magnification Levels is above 0 (a zoomable weapon).
H3_REPLACE_INT(8234AD70, weapon_has_magnification, {
    const host::weapon* weapon = WeaponOfObject(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    return weapon->magnification_levels > 0 ? 1u : 0u;
})

// weapon_is_energy_blade(r3 weapon object or NONE) -> bool: its
// definition's Weapon Type is Energy Blade (false for NONE).
H3_REPLACE_INT(82347F68, weapon_is_energy_blade, {
    if (ctx.r3.u32 == h3::native::objects::kNone)
        return 0u;
    const host::weapon* weapon = WeaponOfObject(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    return weapon->weapon_type == kWeaponTypeEnergyBlade ? 1u : 0u;
})

// weapon_counts_toward_maximum(r3 weapon object) -> bool: its definition
// has neither Doesn't Count Toward Maximum nor Support Weapon.
H3_REPLACE_INT(8234ADC8, weapon_counts_toward_maximum, {
    const host::weapon* weapon = WeaponOfObject(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    return (weapon->flags & (kDoesntCountTowardMaximum | kSupportWeapon)) ? 0u : 1u;
})

// weapon_has_sword_charge_trigger(r3 weapon object or NONE) -> bool: its
// definition's first trigger has the Sword-Charge behavior.
H3_REPLACE_INT(8234AE28, weapon_has_sword_charge_trigger, {
    if (ctx.r3.u32 == h3::native::objects::kNone)
        return 0u;
    const host::weapon* weapon = WeaponOfObject(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    const auto& triggers = weapon->new_triggers;
    if (triggers.storedCount <= 0)
        return 0u;
    if (triggers.count <= 0)
        return std::nullopt;
    return triggers[0].behavior == kTriggerBehaviorSwordCharge ? 1u : 0u;
})

// weapon_get_barrel_marker_name(r3 weapon object, r4 barrel 0 or 1) ->
// string id: the barrel's Optional Barrel Marker Name, or the default
// marker (string id 202 for barrel 0, 203 for barrel 1) if it has none or
// the weapon has no such barrel; 0 for a barrel above 1.
H3_REPLACE_INT(8234B628, weapon_get_barrel_marker_name, {
    uint32_t barrel = ctx.r4.u32;
    if (barrel > 1)
        return 0u;
    uint32_t defaultMarker = 202 + barrel;
    const host::weapon* weapon = WeaponOfObject(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    const auto& barrels = weapon->barrels;
    if (int32_t(barrel) >= barrels.storedCount)
        return defaultMarker;
    if (int32_t(barrel) >= barrels.count)
        return std::nullopt;
    uint32_t marker = barrels[barrel].optional_barrel_marker_name;
    return (marker == 0 || marker == 0xFFFFFFFFu) ? defaultMarker : marker;
})

// weapon_barrel_get_projectile_travel_time(r3 weapon object, r4 s16
// barrel, f1 distance) -> real: the time the barrel's projectile takes to
// cover the distance (sub_82340AB8 on the barrel's Projectile), or the
// default (read-only data at kDefaultTravelTime) for a negative or missing
// barrel or one without a projectile.
H3_REPLACE_FLOAT(820C88E0, weapon_barrel_get_projectile_travel_time, {
    constexpr uint32_t kDefaultTravelTime = 0x8202251C;
    uint32_t bits = h3::native::objects::Load32(kDefaultTravelTime);
    float fallback;
    std::memcpy(&fallback, &bits, sizeof(fallback));
    const host::weapon* weapon = WeaponOfObject(ctx, ctx.r3.u32);
    if (!weapon)
        return std::nullopt;
    int32_t barrel = int16_t(uint16_t(ctx.r4.u32));
    const auto& barrels = weapon->barrels;
    if (barrel < 0 || barrel >= barrels.storedCount)
        return fallback;
    if (barrel >= barrels.count)
        return std::nullopt;
    uint32_t projectile = barrels[barrel].projectile.datum;
    if (projectile == h3::native::objects::kNone)
        return fallback;
    // f1 (the distance) is still the entry's.
    h3::native::Call(ctx, base, sub_82340AB8, projectile);
    return float(ctx.f1.f64);
})

#endif
