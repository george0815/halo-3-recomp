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

#endif
