// Vehicle definition lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

#include <cstring>

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kVehicleGroup = 0x76656869;  // 'vehi'
    // What the original returns when none of the physics type blocks has
    // an element (one past "no type": it starts its result at 11).
    constexpr uint32_t kNoPhysicsType = 11;
}

// vehicle_get_physics_type(r3 vehicle object) -> the index of the first
// non-empty block of the vehicle definition's physics types (0 human tank
// .. 9 guardian), or 11 if all are empty.
H3_REPLACE_INT(820A9548, vehicle_get_physics_type, {
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, ctx.r3.u32);
    const host::vehicle* vehicle = h3::native::tags::TagPartBySlot<host::vehicle>(definition & 0xFFFF, kVehicleGroup);
    if (!vehicle)
        return std::nullopt;
    const host::vehicle_physics_types_struct& types = vehicle->physics_types;
    const int32_t counts[] = {
        types.type_human_tank.storedCount,
        types.type_human_jeep.storedCount,
        types.type_human_plane.storedCount,
        types.type_alien_scout.storedCount,
        types.type_alien_fighter.storedCount,
        types.type_turret.storedCount,
        types.type_mantis.storedCount,
        types.type_vtol.storedCount,
        types.type_chopper.storedCount,
        types.type_guardian.storedCount,
    };
    for (uint32_t type = 0; type < std::size(counts); type++)
        if (counts[type] != 0)
            return type;
    return kNoPhysicsType;
})

// vehicle_is_huge_physics_group(r3 object) -> bool: the object is a
// vehicle (object type 1, in its header's byte 3) whose definition has Huge
// Vehicle Physics Group; false for NONE and other types.
H3_REPLACE_INT(820DA370, vehicle_is_huge_physics_group, {
    uint32_t object = ctx.r3.u32;
    if (object == h3::native::objects::kNone)
        return 0u;
    uint32_t header = h3::native::objects::DataArrayElement(ctx, h3::native::objects::kTlsObjects, object,
                                                            h3::native::objects::kObjectHeaderBytes);
    uint32_t type = uint8_t(h3::GuestRef<uint8_t>(header + 3)) & 63;  // slw: shifts of 32-63 give 0
    if (type != 1)
        return 0u;
    uint32_t definition = h3::native::objects::Load32(h3::native::objects::Load32(header + h3::native::objects::kObjectHeaderData));
    const host::vehicle* vehicle = h3::native::tags::TagPartBySlot<host::vehicle>(definition & 0xFFFF, kVehicleGroup);
    if (!vehicle)
        return std::nullopt;
    return (vehicle->flags >> 11) & 1;  // Huge Vehicle Physics Group (Assembly's vehi plugin)
})

// vehicle_get_trick_duration(r3 trick state, r4 vehicle object) -> real:
// for the state's trick type (s8 at +24) 1 or 2 the alien fighter
// physics' Roll Trick Duration, 3 its Loop Trick Duration, else a default
// (read-only real at kRealZero, 0 presumably).
H3_REPLACE_FLOAT(82373CD8, vehicle_get_trick_duration, {
    constexpr uint32_t kRealZero = 0x8202251C;
    int32_t trick = int8_t(uint8_t(h3::GuestRef<uint8_t>(ctx.r3.u32 + 24)));
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, ctx.r4.u32);
    const host::vehicle* vehicle = h3::native::tags::TagPartBySlot<host::vehicle>(definition & 0xFFFF, kVehicleGroup);
    if (!vehicle)
        return std::nullopt;
    if (trick <= 0 || trick > 3)
    {
        uint32_t bits = h3::native::objects::Load32(kRealZero);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    // Element 0, read without checking the count.
    const auto& fighter = vehicle->physics_types.type_alien_fighter;
    if (fighter.count <= 0)
        return std::nullopt;
    return trick == 3 ? fighter[0].loop_trick_duration : fighter[0].roll_trick_duration;
})

// player_control_is_driving_flying_vehicle(r3 player control index) ->
// bool: the player control's unit sits in a seat of a vehicle whose
// physics type is human plane, alien fighter or VTOL, and that seat has
// the Driver flag (name unconfirmed).
H3_REPLACE_INT(82216868, player_control_is_driving_flying_vehicle, {
    namespace objects = h3::native::objects;
    constexpr uint32_t kTlsPlayerControlGlobals = 120;
    constexpr uint32_t kPlayerControlBytes = 236;
    constexpr uint32_t kPlayerControlUnit = 16;
    constexpr uint32_t kObjectParent = 16;
    constexpr uint32_t kObjectType = 154;      // s8
    constexpr uint32_t kUnitParentSeat = 582;  // s16
    constexpr uint32_t kUnitGroup = 0x756E6974;  // 'unit'
    constexpr uint32_t kFlyingTypes = (1u << 2) | (1u << 4) | (1u << 7);  // human plane, alien fighter, vtol
    constexpr uint32_t kSeatDriver = 1u << 2;
    uint32_t globals = objects::Load32(objects::Load32(ctx.r13.u32) + kTlsPlayerControlGlobals);
    uint32_t unit = objects::Load32(globals + ctx.r3.u32 * kPlayerControlBytes + kPlayerControlUnit);
    if (unit == objects::kNone)
        return 0u;
    uint32_t unitData = objects::ObjectData(ctx, unit);
    uint32_t vehicle = objects::Load32(unitData + kObjectParent);
    if (vehicle == objects::kNone)
        return 0u;
    int32_t seat = int16_t(objects::Load16(unitData + kUnitParentSeat));
    if (seat == -1)
        return 0u;
    uint32_t vehicleData = objects::ObjectData(ctx, vehicle);
    if (!(objects::ShiftBit(uint32_t(int32_t(objects::LoadS8(vehicleData + kObjectType)))) & 2))
        return 0u;
    uint32_t type = h3::native::Call(ctx, base, sub_820A9548, vehicle);
    if (!(objects::ShiftBit(type) & kFlyingTypes))
        return 0u;
    const host::unit_struct_definition* part =
        h3::native::tags::TagPartBySlot<host::unit_struct_definition>(objects::Load32(vehicleData) & 0xFFFF, kUnitGroup);
    if (!part)
        return std::nullopt;
    const auto& seats = part->seats;
    if (seat < 0 || seat >= seats.count)
        return std::nullopt;  // no range check in the original
    return (seats[seat].flags & kSeatDriver) ? 1u : 0u;
})

PPC_EXTERN_FUNC(sub_820A9548);  // vehicle_get_physics_type (native, above)
PPC_EXTERN_FUNC(sub_82373CD8);  // vehicle_get_trick_duration (native, above)

namespace
{
    constexpr uint32_t kPhysicsHumanJeep = 1;
    constexpr uint32_t kPhysicsAlienFighter = 4;
    constexpr uint32_t kPhysicsTurret = 5;
    constexpr uint32_t kPhysicsGuardian = 9;
}

// vehicle_get_turret_physics_state(r3 vehicle physics state, r4 vehicle
// object) -> r3 for a turret, r3 + 8 for a guardian, else 0 (name
// unconfirmed).
H3_REPLACE_INT(82353AC8, vehicle_get_turret_physics_state, {
    uint32_t state = ctx.r3.u32;
    uint32_t type = h3::native::Call(ctx, base, sub_820A9548, ctx.r4.u32);
    if (type == kPhysicsTurret)
        return state;
    if (type == kPhysicsGuardian)
        return state + 8;
    return 0u;
})

// vehicle_trick_in_progress(r3 trick state, r4 vehicle object) -> bool:
// a trick is active (u8 +24) and its elapsed time (u8 ticks at +25 times
// the seconds per tick) is below its duration (vehicle_get_trick_duration)
// (name unconfirmed).
H3_REPLACE_INT(82373E80, vehicle_trick_in_progress, {
    namespace objects = h3::native::objects;
    constexpr uint32_t kTlsGameTimeGlobals = 188;
    constexpr uint32_t kSecondsPerTick = 8;
    uint32_t state = ctx.r3.u32;
    if (objects::Load8(state + 24) == 0)
        return 0u;
    float ticks = float(double(objects::Load8(state + 25)));
    float elapsed = float(double(ticks) * double(objects::LoadFloat(objects::TlsSlot(ctx, kTlsGameTimeGlobals) + kSecondsPerTick)));
    h3::native::Call(ctx, base, sub_82373CD8, state, ctx.r4.u32);
    float duration = float(ctx.f1.f64);
    return elapsed < duration ? 1u : 0u;
})

// vehicle_is_doing_trick(r3 vehicle object) -> bool: an alien fighter
// whose trick state (object data +1112) is in progress
// (vehicle_trick_in_progress); false for other physics types (name
// unconfirmed).
H3_REPLACE_INT(820D80B8, vehicle_is_doing_trick, {
    uint32_t vehicle = ctx.r3.u32;
    if (h3::native::Call(ctx, base, sub_820A9548, vehicle) != kPhysicsAlienFighter)
        return 0u;
    return h3::native::Call(ctx, base, sub_82373E80, h3::native::objects::ObjectData(ctx, vehicle) + 1112, vehicle);
})

// vehicle_jeep_flag_11(r3 unused, r4 vehicle object) -> bool: a human
// jeep whose object data word +392 has bit 11 set (name unconfirmed).
H3_REPLACE_INT(820F12A0, vehicle_jeep_flag_11, {
    uint32_t vehicle = ctx.r4.u32;
    if (h3::native::Call(ctx, base, sub_820A9548, vehicle) != kPhysicsHumanJeep)
        return 0u;
    return (h3::native::objects::Load32(h3::native::objects::ObjectData(ctx, vehicle) + 392) >> 11) & 1;
})

PPC_EXTERN_FUNC(sub_821E0818);  // player is local (name unconfirmed; recompiled)
PPC_EXTERN_FUNC(sub_820F1820);  // object controlled by a local player (name unconfirmed; recompiled)

// vehicle_get_friction_point_update_period(r3 vehicle object) -> real: the
// definition's Havok Vehicle Physics Maximum Update Period when a local
// player controls it, else the Maximum Remote Update Period. "Local": in
// game modes (s8 at game globals +20) 4 and 5 a biped, vehicle or giant
// whose player (+380, else its parent's (+664)) is local (sub_821E0818);
// otherwise sub_820F1820. A parent (+664) whose +364 isn't NONE also
// gives the remote period, as does having no parent (name unconfirmed).
H3_REPLACE_FLOAT(82577FE8, vehicle_get_friction_point_update_period, {
    namespace objects = h3::native::objects;
    constexpr uint32_t kTlsGameGlobals = 128;
    constexpr uint32_t kObjectPlayer = 380;
    constexpr uint32_t kObjectParentLink = 664;
    uint32_t object = ctx.r3.u32;
    uint32_t data = objects::ObjectData(ctx, object);
    const host::vehicle* vehicle = h3::native::tags::TagPartBySlot<host::vehicle>(objects::Load32(data) & 0xFFFF, kVehicleGroup);
    if (!vehicle)
        return std::nullopt;
    int32_t mode = objects::LoadS8(objects::TlsSlot(ctx, kTlsGameGlobals) + 20);
    uint32_t local = 0;
    if (uint32_t(mode - 4) <= 1)
    {
        if (objects::ShiftBit(uint32_t(int32_t(objects::LoadS8(data + 154)))) & 0x1003)
        {
            uint32_t player = objects::Load32(data + kObjectPlayer);
            if (player == objects::kNone)
            {
                uint32_t parent = objects::Load32(data + kObjectParentLink);
                if (parent != objects::kNone)
                    player = objects::Load32(objects::ObjectData(ctx, parent) + kObjectPlayer);
            }
            if (player != objects::kNone)
                local = h3::native::Call(ctx, base, sub_821E0818, player);
        }
    }
    else
        local = h3::native::Call(ctx, base, sub_820F1820, object);
    bool remote = (local & 0xFF) == 0;
    uint32_t parent = objects::Load32(data + kObjectParentLink);
    if (parent == objects::kNone || objects::Load32(objects::ObjectData(ctx, parent) + 364) != objects::kNone)
        remote = true;
    const auto& physics = vehicle->havok_vehicle_physics;
    return remote ? physics.maximum_remote_update_period : physics.maximum_update_period;
})

// vehicle_trick_get_progress_curve(r3 trick state, r4 vehicle object) ->
// real: 0 once the trick's elapsed time (u8 ticks at +25 times the
// seconds per tick) reaches its duration (vehicle_get_trick_duration);
// else for t = elapsed / duration: 1 - (1 - t)^2 for trick types (s8
// +24) 1 and 2, t^2 otherwise (constants: lis -32254 + 8700 =
// 0x820221FC (1), + 800 = 0x8202251C (0)) (name unconfirmed).
H3_REPLACE_FLOAT(82373D50, vehicle_trick_get_progress_curve, {
    namespace objects = h3::native::objects;
    constexpr uint32_t kTlsGameTimeGlobals = 188;
    constexpr uint32_t kSecondsPerTick = 8;
    constexpr uint32_t kRealOne = 0x820221FC;
    constexpr uint32_t kRealZero = 0x8202251C;
    uint32_t state = ctx.r3.u32;
    float ticks = float(double(objects::Load8(state + 25)));
    float elapsed = float(double(ticks) * double(objects::LoadFloat(objects::TlsSlot(ctx, kTlsGameTimeGlobals) + kSecondsPerTick)));
    h3::native::Call(ctx, base, sub_82373CD8, state, ctx.r4.u32);
    double duration = ctx.f1.f64;
    if (double(elapsed) >= duration)
        return objects::LoadFloat(kRealZero);
    int32_t type = objects::LoadS8(state + 24);
    float t = float(double(elapsed) / duration);
    if (type == 1 || type == 2)
    {
        double one = double(objects::LoadFloat(kRealOne));
        float rest = float(one - double(t));
        return float(-(double(rest) * double(rest) - one));
    }
    return float(double(t) * double(t));
})

#endif
