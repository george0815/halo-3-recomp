// Weapon trigger queries combining the weapon's trigger states (guest
// memory) with its definition's New Triggers (host tag cache), decompiled
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "game/game_state_layout.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    namespace game = h3::native::game;
    constexpr uint32_t kWeaponGroup = 0x77656170;  // 'weap'
    constexpr uint32_t kRealZero = 0x8202251C;     // read-only reals the originals load
    constexpr uint32_t kRealOne = 0x820221FC;
    constexpr int16_t kBehaviorLatchAutofire = 2;
    constexpr int16_t kSecondaryActionCharge = 1;
    // Trigger states (weapon data +540, 12 bytes each): +0 s8 state, +4 s16
    // a tick counter.
    constexpr uint8_t kTriggerStateCharged = 2;
    constexpr int8_t kTriggerStateCharging = 1;

    // The weapon part of a weapon object's definition, or nullptr.
    const host::weapon* WeaponPart(uint32_t data)
    {
        return h3::native::tags::TagPartBySlot<host::weapon>(objects::Load32(data) & 0xFFFF, kWeaponGroup);
    }
}

// weapon_has_autofire_charge_trigger(r3 weapon object, r4 bool any state)
// -> bool: the definition's first trigger is Latch-Autofire with secondary
// action Charge (and, unless r4, the trigger's state is 2).
H3_REPLACE_INT(823511E8, weapon_has_autofire_charge_trigger, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    const host::weapon* weapon = WeaponPart(data);
    if (!weapon)
        return std::nullopt;
    const auto& triggers = weapon->new_triggers;
    if (triggers.storedCount <= 0)
        return 0u;
    if (triggers.count <= 0)
        return std::nullopt;
    if ((ctx.r4.u32 & 0xFF) == 0 && objects::Load8(data + game::weapon::kTriggerStates) != kTriggerStateCharged)
        return 0u;
    const host::weapon_triggers& trigger = triggers[0];
    return (trigger.behavior == kBehaviorLatchAutofire && trigger.autofire.secondary_action == kSecondaryActionCharge) ? 1u
                                                                                                                       : 0u;
})

// weapon_trigger_get_charge_fraction(r3 weapon object, r4 s16 trigger) ->
// real: 1 for a charged trigger (state 2); for a charging one (state 1)
// 1 - counter * seconds per tick / the trigger's Charging Time; else 0
// (also for a trigger past the definition's count).
H3_REPLACE_FLOAT(8234BC18, weapon_trigger_get_charge_fraction, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    const host::weapon* weapon = WeaponPart(data);
    if (!weapon)
        return std::nullopt;
    float zero = objects::LoadFloat(kRealZero);
    float one = objects::LoadFloat(kRealOne);
    int32_t index = int16_t(uint16_t(ctx.r4.u32));
    const auto& triggers = weapon->new_triggers;
    if (triggers.storedCount <= index)
        return zero;
    uint32_t state = data + game::weapon::kTriggerStates + uint32_t(index) * game::weapon::kTriggerStateBytes;
    int8_t value = objects::LoadS8(state);
    if (value == kTriggerStateCharging)
    {
        if (index < 0 || index >= triggers.count)
            return std::nullopt;
        float counter = float(double(int64_t(objects::LoadS16(state + 4))));  // fcfid, frsp
        uint32_t gameTime = objects::TlsSlot(ctx, game::tls::kGameTimeGlobals);
        float elapsed = objects::LoadFloat(gameTime + game::game_time::kSecondsPerTick) * counter;
        return one - elapsed / triggers[index].charging.charging_time;
    }
    if (value == int8_t(kTriggerStateCharged))
        return one;
    return zero;
})

#endif
