// Object queries that combine the object's game state (guest memory) with
// its definition (host tag cache), decompiled (docs/ROADMAP.md 6.3
// milestone A; replace.h for the mechanism). Object data fields:
// game/game_state_layout.h and private notes.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    constexpr uint32_t kObjectGroup = 0x6F626A65;  // 'obje'
    constexpr uint32_t kNone = objects::kNone;

    // Object data fields the originals read.
    constexpr uint32_t kObjectFlags = 4;          // u32: bit 0 tested below (meaning unknown)
    constexpr uint32_t kObjectParent = 16;        // object datum
    constexpr uint32_t kObjectType = 154;         // s8
    constexpr uint32_t kObjectPhysicsFlags = 172; // u32: bits 4, 7, 20 tested below
    constexpr uint32_t kTlsEarlyMovers = 264;     // "object early movers" globals
    constexpr uint32_t kEarlyMoverObjects = 9728; // u32[...] object datums
    constexpr uint32_t kEarlyMoverCount = 9856;   // s32

    // Object definition Flags (obje +2; Assembly's names).
    constexpr uint16_t kExtensionOfParent = 1u << 4;
    constexpr uint16_t kEarlyMoverLocalizedPhysics = 1u << 7;

    // The obje part of the definition in this slot (any object group), or
    // nullptr.
    const host::object_struct_definition* ObjectPart(uint32_t definition)
    {
        return h3::native::tags::TagPartBySlot<host::object_struct_definition>(definition & 0xFFFF, kObjectGroup);
    }

    // Longer parent chains than this are a loop (the original would spin).
    constexpr int kMaxParents = 256;
}

// object_get_acceleration_scale(r3 object) -> real: the definition's
// Acceleration Scale of the object, or of the first parent up the chain
// with physics flag 7 or 4 set (the root if none has).
H3_REPLACE_FLOAT(82339F48, object_get_acceleration_scale, {
    uint32_t object = ctx.r3.u32;
    for (int depth = 0; depth < kMaxParents; depth++)
    {
        uint32_t data = objects::ObjectData(ctx, object);
        const host::object_struct_definition* part = ObjectPart(objects::Load32(data));
        if (!part)
            return std::nullopt;
        uint32_t flags = objects::Load32(data + kObjectPhysicsFlags);
        if (flags & ((1u << 7) | (1u << 4)))
            return part->acceleration_scale;
        object = objects::Load32(data + kObjectParent);
        if (object == kNone)
            return part->acceleration_scale;
    }
    return std::nullopt;
})

// object_get_physics_motion_class(r3 object) -> 15, or 10 for a vehicle,
// or for a biped 4 if its physics flags have Flying, else 0 (name a
// guess: callers are in the havok code).
H3_REPLACE_INT(8236D598, object_get_physics_motion_class, {
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t type = uint32_t(int32_t(objects::LoadS8(data + kObjectType)));  // cmplwi on the sign-extended type
    if (type == 1)
        return 10u;
    if (type > 1)
        return 15u;
    const host::biped_tag* biped = h3::native::tags::TagBySlotOf<host::biped_tag>(objects::Load32(data) & 0xFFFF);
    if (!biped)
        return std::nullopt;
    return (biped->biped.physics.flags >> 2) & 4;  // Flying (bit 4) moved to bit 2
})

// object_get_model_source(r3 object) -> the object, or its parent when the
// object's header flag 0x10 or data flag bit 0 is set or its definition
// has no model (the object itself if it has no parent).
H3_REPLACE_INT(8234BA40, object_get_model_source, {
    uint32_t object = ctx.r3.u32;
    uint32_t header = objects::ObjectHeader(ctx, object);
    uint32_t data = objects::Load32(header + objects::kObjectHeaderData);
    bool useParent = (objects::Load8(header + 2) & 0x10) != 0 || (objects::Load32(data + kObjectFlags) & 1) != 0;
    if (!useParent)
    {
        const host::object_struct_definition* part = ObjectPart(objects::Load32(data));
        if (!part)
            return std::nullopt;
        if (part->model.datum != kNone)
            return object;
    }
    uint32_t parent = objects::Load32(data + kObjectParent);
    return parent == kNone ? object : parent;
})

// object_is_early_mover(r3 object, r4 bool localized only) -> bool: the
// object is in the early movers list (and, if asked, its definition has
// Early Mover Localized Physics). False for NONE.
H3_REPLACE_INT(8232F3C0, object_is_early_mover, {
    uint32_t object = ctx.r3.u32;
    if (object == kNone)
        return 0u;
    uint32_t movers = objects::TlsSlot(ctx, kTlsEarlyMovers);
    int32_t count = int32_t(objects::Load32(movers + kEarlyMoverCount));
    bool found = false;
    for (int32_t i = 0; i < count && !found; i++)
        found = objects::Load32(movers + kEarlyMoverObjects + uint32_t(i) * 4) == object;
    if (!found)
        return 0u;
    if ((ctx.r4.u32 & 0xFF) == 0)
        return 1u;
    const host::object_struct_definition* part = ObjectPart(objects::ObjectDefinition(ctx, object));
    if (!part)
        return std::nullopt;
    return (part->flags & kEarlyMoverLocalizedPhysics) ? 1u : 0u;
})

// object_get_extension_owner(r3 object) -> object: walks up from the
// object through parents while the definition has Extension Of Parent;
// then, for a weapon or equipment with data flag bit 0 and item state 1,
// the item's +364 (its holder, probably), else that object.
H3_REPLACE_INT(8231ADE0, object_get_extension_owner, {
    uint32_t object = ctx.r3.u32;
    for (int depth = 0; depth < kMaxParents; depth++)
    {
        uint32_t data = objects::ObjectData(ctx, object);
        uint32_t parent = objects::Load32(data + kObjectParent);
        if (parent != kNone)
        {
            const host::object_struct_definition* part = ObjectPart(objects::Load32(data));
            if (!part)
                return std::nullopt;
            if (part->flags & kExtensionOfParent)
            {
                object = parent;
                continue;
            }
        }
        if ((objects::Load32(data + kObjectFlags) & 1) == 0)
            return object;
        uint32_t typeBit = objects::ShiftBit(uint32_t(int32_t(objects::LoadS8(data + kObjectType))));
        if ((typeBit & 0xC) == 0)  // weapon, equipment
            return object;
        if (objects::Load8(data + 353) != 1)
            return object;
        return objects::Load32(data + 364);
    }
    return std::nullopt;
})

// object_is_affected_by_phantom_volumes(r3 object or NONE, r4 definition
// datum) -> bool, by the definition's object type: true for bipeds,
// vehicles, weapons, equipment, terminals, machines, crates, creatures and
// giants; a projectile if it has Affected By Phantom Volumes; a scenery
// unless it's Not Physical (then: true for NONE, else the object's
// physics flag bit 20); false otherwise. (Name from the projectile flag.)
H3_REPLACE_INT(82112470, object_is_affected_by_phantom_volumes, {
    uint32_t object = ctx.r3.u32;
    uint32_t slot = ctx.r4.u32 & 0xFFFF;
    const host::object_struct_definition* part = ObjectPart(slot);
    if (!part)
        return std::nullopt;
    uint32_t typeBit = objects::ShiftBit(uint32_t(int32_t(part->runtime_object_type)));
    if (typeBit & (1u << 6))  // scenery
    {
        const host::scenery_tag* scenery = h3::native::tags::TagBySlotOf<host::scenery_tag>(slot);
        if (!scenery)
            return std::nullopt;
        if ((scenery->scenery.flags & 1) == 0 || object == kNone)  // Not Physical
            return 1u;
        return (objects::Load32(objects::ObjectData(ctx, object) + kObjectPhysicsFlags) >> 20) & 1;
    }
    if (typeBit & 0x1CDF)
        return 1u;
    if (typeBit & (1u << 5))  // projectile
    {
        const host::projectile_tag* projectile = h3::native::tags::TagBySlotOf<host::projectile_tag>(slot);
        if (!projectile)
            return std::nullopt;
        return (projectile->projectile.flags >> 10) & 1;
    }
    return 0u;
})

// object_definition_can_be_damaged(r3 object type, r4 definition datum)
// -> bool: true for every type but projectiles, scenery, sound scenery and
// effect scenery; a scenery if its model has New Damage Info.
H3_REPLACE_INT(82332308, object_definition_can_be_damaged, {
    uint32_t typeBit = objects::ShiftBit(ctx.r3.u32);
    if (typeBit & 0x1D9F)
        return 1u;
    if ((typeBit & (1u << 6)) == 0)
        return 0u;
    uint32_t definition = ctx.r4.u32;
    if (objects::RawTagAddress(definition) == 0)
        return 0u;
    const host::object_struct_definition* part = ObjectPart(definition);
    if (!part)
        return std::nullopt;
    uint32_t model = part->model.datum;
    if (model == kNone)
        return 0u;
    const host::model_tag* tag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
    if (!tag)
        return std::nullopt;
    return tag->model.new_damage_info.storedCount > 0 ? 1u : 0u;
})

// object_definition_get_shield_impact_override(r3 definition datum, r4
// bool first person) -> guest address of the model's (1st Person) Shield
// Impact Parameter Override tag, or 0 for none.
H3_REPLACE_INT(82468F70, object_definition_get_shield_impact_override, {
    const host::object_struct_definition* part = ObjectPart(ctx.r3.u32);
    if (!part)
        return std::nullopt;
    uint32_t model = part->model.datum;
    if (model == kNone)
        return 0u;
    const host::model_tag* tag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
    if (!tag)
        return std::nullopt;
    uint32_t parameters = (ctx.r4.u32 & 0xFF) ? tag->model._1st_person_shield_impact_parameter_override.datum
                                              : tag->model.shield_impact_parameter_override.datum;
    if (parameters == kNone)
        return 0u;
    return objects::RawTagAddress(parameters);
})

#endif
