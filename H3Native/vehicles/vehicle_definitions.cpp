// Vehicle definition lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

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

#endif
