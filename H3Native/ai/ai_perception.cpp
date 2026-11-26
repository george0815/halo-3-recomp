// AI lookups of object and weapon definitions, decompiled to read the
// host tag cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the
// mechanism). Object state stays in guest memory (objects/object_access.h).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    constexpr uint32_t kObjectGroup = 0x6F626A65;  // 'obje'
    constexpr uint32_t kWeaponGroup = 0x77656170;  // 'weap'
    // Object header byte: the object's type (0 biped, 1 vehicle, ...).
    constexpr uint32_t kObjectHeaderType = 3;
    constexpr uint32_t kObjectTypeVehicle = 1;
    // Unit object data: the current weapon's slot in kUnitWeapons (s8,
    // NONE for none).
    constexpr uint32_t kUnitCurrentWeaponSlot = 602;
}

// object_is_vehicle_with_ai_type(r3 object or NONE, r4 string id) -> bool:
// the object is a vehicle whose definition's first AI Properties element
// has that AI Type Name.
H3_REPLACE_INT(82367120, object_is_vehicle_with_ai_type, {
    uint32_t object = ctx.r3.u32;
    if (object == objects::kNone)
        return 0u;
    uint32_t header = objects::DataArrayElement(ctx, objects::kTlsObjects, object, objects::kObjectHeaderBytes);
    uint8_t type = uint8_t(h3::GuestRef<uint8_t>(header + kObjectHeaderType));
    if (type != kObjectTypeVehicle)
        return 0u;
    uint32_t definition = objects::Load32(objects::Load32(header + objects::kObjectHeaderData));
    const host::object_struct_definition* definitionPart =
        h3::native::tags::TagPartBySlot<host::object_struct_definition>(definition & 0xFFFF, kObjectGroup);
    if (!definitionPart)
        return std::nullopt;
    const auto& properties = definitionPart->ai_properties;
    if (properties.storedCount <= 0)
        return 0u;
    if (properties.count <= 0)
        return std::nullopt;
    return properties[0].ai_type_name == ctx.r4.u32 ? 1u : 0u;
})

// unit_current_weapon_is_scary(r3 unit object) -> bool: the unit holds a
// weapon whose definition's AI Scariness is at least the AI globals' Scary
// Weapon Threshold.
H3_REPLACE_INT(82564120, unit_current_weapon_is_scary, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    int32_t slot = int8_t(uint8_t(h3::GuestRef<uint8_t>(data + kUnitCurrentWeaponSlot)));
    uint32_t weapon = objects::kNone;
    if (slot != -1)  // any other value is used as is, like the original
        weapon = objects::Load32(data + objects::kUnitWeapons + uint32_t(slot) * 4);
    if (weapon == objects::kNone)
        return 0u;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& aiGlobals = globals->globals.ai_globals;
    if (aiGlobals.storedCount <= 0)
        return 0u;
    if (aiGlobals.count <= 0)
        return std::nullopt;
    float threshold = aiGlobals[0].scary_weapon_threshold;
    uint32_t definition = objects::ObjectDefinition(ctx, weapon);
    const host::weapon* weaponPart = h3::native::tags::TagPartBySlot<host::weapon>(definition & 0xFFFF, kWeaponGroup);
    if (!weaponPart)
        return std::nullopt;
    return weaponPart->ai_scariness >= threshold ? 1u : 0u;
})

#endif
