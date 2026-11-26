// Object queries that combine the object's game state (guest memory) with
// its definition (host tag cache), decompiled (docs/ROADMAP.md 6.3
// milestone A; replace.h for the mechanism). Object data fields:
// game/game_state_layout.h and private notes.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

#include <cstdint>

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

// object_hold_timer_elapsed(r3 object) -> bool: the object's physics flags
// (+172) lack bit 9, its +1216 flags have bit 1, the s8 at +29 of its
// state (the s16 offset at +358 from the object data) is at least the
// game's ticks per second (game time globals +4) times a time (the
// constant at 0x82035D3C, times the definition's real at +1028 when that
// is above 0) rounded (+0.5, truncated), and its +1218 byte is 1 (name
// unconfirmed; the definition is read from guest memory).
H3_REPLACE_INT(82385420, object_hold_timer_elapsed, {
    constexpr uint32_t kRealTime = 0x82035D3C;  // lis -32253, +23868
    constexpr uint32_t kRealHalf = 0x82022DC0;  // lis -32254, +11712
    constexpr uint32_t kRealZero = 0x8202251C;  // +11712 -2212
    constexpr uint32_t kTlsGameTimeGlobals = 188;
    uint32_t object = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t definition = objects::RawTagAddress(objects::Load32(object));
    uint32_t state = object + uint32_t(int32_t(objects::LoadS16(object + 358)));
    double time = objects::LoadFloat(kRealTime);
    double scale = objects::LoadFloat(definition + 1028);
    if (scale > double(objects::LoadFloat(kRealZero)))
        time = double(float(scale * time));
    if ((objects::Load32(object + kObjectPhysicsFlags) >> 9) & 1)
        return 0u;
    if ((objects::Load16(object + 1216) & 2) == 0)
        return 0u;
    int32_t ticks = objects::LoadS16(objects::TlsSlot(ctx, kTlsGameTimeGlobals) + 4);
    double ticksReal = double(float(double(int64_t(ticks))));
    int32_t needed = objects::TruncateToInt32(double(float(ticksReal * time + double(objects::LoadFloat(kRealHalf)))));
    if (objects::LoadS8(state + 29) < needed)
        return 0u;
    return objects::Load8(object + 1218) == 1 ? 1u : 0u;
})

// object_get_timer_fraction_remaining(r3 object) -> real: 1 minus the
// game ticks since the object's +376 start time (game time globals +12)
// over the definition's duration (+428 seconds, as ticks rounded, at
// least 1), clamped to [0, 1]; 0 with no start time (NONE) or a duration
// not above the constant at 0x8203AA78 (name unconfirmed; the definition
// is read from guest memory).
H3_REPLACE_FLOAT(820EBA28, object_get_timer_fraction_remaining, {
    constexpr uint32_t kRealZero = 0x8202251C;      // lis -32254, +9500
    constexpr uint32_t kRealHalf = 0x82022DC0;      // +9500 +2212
    constexpr uint32_t kRealOne = 0x820221FC;       // +9500 -800
    constexpr uint32_t kRealMinimum = 0x8203AA78;   // lis -32252, -21896
    constexpr uint32_t kTlsGameTimeGlobals = 188;
    uint32_t object = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t start = objects::Load32(object + 376);
    uint32_t definition = objects::RawTagAddress(objects::Load32(object));
    double zero = objects::LoadFloat(kRealZero);
    if (start == kNone)
        return float(zero);
    double duration = objects::LoadFloat(definition + 428);
    if (duration <= double(objects::LoadFloat(kRealMinimum)))
        return float(zero);
    uint32_t time = objects::TlsSlot(ctx, kTlsGameTimeGlobals);
    double ticksPerSecond = double(float(double(int64_t(objects::LoadS16(time + 4)))));
    int32_t ticks = objects::TruncateToInt32(double(float(ticksPerSecond * duration + double(objects::LoadFloat(kRealHalf)))));
    if (ticks < 1)
        ticks = 1;
    int32_t elapsed = int32_t(objects::Load32(time + 12) - start) / ticks;
    double one = objects::LoadFloat(kRealOne);
    double fraction = double(float(one - double(float(double(int64_t(elapsed))))));
    if (!(fraction <= zero) && fraction >= one)
        return float(one);
    if (fraction <= zero)
        return float(zero);
    return float(fraction);
})

// object_update_due(r3 struct: +1368 s32 count, r4 object, r7 ticks
// multiplier, r8 divisor, f1 distance, f2 value) -> bool: false when the
// distance reaches the definition's kind's (+26: 0, 1, 2+) limit or the
// count isn't positive; else from the kind's three rates (the tables at
// 0x82039798 and on), the count and the game tick length (game time
// globals +8) a period in ticks, and true when (game time + object +
// period / r8 squared, at least 1, times r7) is a multiple of it (or the
// period is 0) (name unconfirmed; float operations as the recompiled
// code's; the definition is read from guest memory).
H3_REPLACE_INT(821BA530, object_update_due, {
    constexpr uint32_t kLimits = 0x82039798;        // lis -32252, -21896 -4832: real[3]
    constexpr uint32_t kScales = 0x820397BC;        // -4796
    constexpr uint32_t kNearRates = 0x820397B0;     // -4808
    constexpr uint32_t kFarRates = 0x820397A4;      // -4820
    constexpr uint32_t kRealMinimum = 0x8203AA78;   // lis -32252, -21896
    constexpr uint32_t kRealZero = 0x8202251C;      // lis -32254, +11712 -2212
    constexpr uint32_t kRealOne = 0x820221FC;       // +11712 -3012
    constexpr uint32_t kRealHalf = 0x82022DC0;      // +11712
    constexpr uint32_t kTlsGameTimeGlobals = 188;
    auto real = [](uint32_t address) { return double(objects::LoadFloat(address)); };
    auto single = [](double value) { return double(float(value)); };
    uint32_t owner = ctx.r3.u32;
    uint32_t objectIndex = ctx.r4.u32;
    uint32_t multiplier = ctx.r7.u32;
    int32_t divisor = int32_t(ctx.r8.u32);
    double distance = ctx.f1.f64, value = ctx.f2.f64;
    uint32_t object = objects::ObjectData(ctx, objectIndex);
    uint32_t kind = objects::Load8(objects::RawTagAddress(objects::Load32(object)) + 26);
    uint32_t index = kind == 0 ? 0 : kind >= 2 ? 2 : 1;
    if (distance >= real(kLimits + index * 4))
        return 0u;
    int32_t count = int32_t(objects::Load32(owner + 1368));
    if (count <= 0)
        return 0u;
    double zero = real(kRealZero), one = real(kRealOne);
    double t = single(value / real(kScales + index * 4));
    if (t > zero)
    {
        if (t >= one)
            t = one;
    }
    else
        t = zero;
    uint32_t time = objects::TlsSlot(ctx, kTlsGameTimeGlobals);
    double rest = single(one - t);
    double share = single(one / single(double(int64_t(count))));
    double near = single(real(kNearRates + index * 4) * share);
    double far = single(real(kFarRates + index * 4) * share);
    double rate = single(near * t);
    rate = single(far * rest + rate);
    rate = single(rate * real(time + 8));
    if (rate <= real(kRealMinimum))
        return 0u;
    double period = single(one / rate);
    double divisorReal = single(double(int64_t(divisor)));
    period = single(period / divisorReal);
    double step = single(period / divisorReal);
    double half = real(kRealHalf);
    int32_t ticks = objects::TruncateToInt32(single(period + half));
    int32_t every = objects::TruncateToInt32(single(step + half));
    if (every <= 1)
        every = 1;
    if (ticks == 0)
        return 1u;
    int32_t total = int32_t(uint32_t(every) * multiplier + objects::Load32(time + 12) + objectIndex);
    int32_t quotient = (total == INT32_MIN && ticks == -1) ? 0 : total / ticks;
    return int32_t(uint32_t(total) - uint32_t(quotient) * uint32_t(ticks)) == 0 ? 1u : 0u;
})

// object_definition_can_create_havok_component(r3 definition datum) ->
// bool: true when the definition isn't affected by phantom volumes
// (object_is_affected_by_phantom_volumes with no object, i.e. needs no
// physics), else whether the "havok components" data array (pointer at
// 0x828C425C: lis -32116 = 0x828C0000, + 16988) has fewer actual than
// maximum elements (s32 +60 < +32) (name unconfirmed).
H3_REPLACE_INT(821BBFC8, object_definition_can_create_havok_component, {
    constexpr uint32_t kHavokComponents = 0x828C425C;
    if ((h3::native::Call(ctx, base, sub_82112470, kNone, ctx.r3.u32) & 0xFF) == 0)
        return 1u;
    uint32_t array = objects::Load32(kHavokComponents);
    return int32_t(objects::Load32(array + 60)) < int32_t(objects::Load32(array + 32)) ? 1u : 0u;
})

PPC_EXTERN_FUNC(sub_820DE6A0);  // object_get_damage_info (native, object_definitions.cpp)

// object_damage_constraint_is_set(r3 object, r4 s16 constraint type, r5
// s16 constraint index) -> bool: the object's damage info has a Damage
// Constraint with that runtime type (+16) and index (+18), and the bit of
// its position in the object's u16 at +236 is set; false otherwise. The
// runtime fields are read from guest memory (name unconfirmed).
H3_REPLACE_INT(8233C5D0, object_damage_constraint_is_set, {
    constexpr uint32_t kConstraintBytes = 20;
    uint32_t object = ctx.r3.u32;
    int32_t type = int32_t(ctx.r4.u32);
    int32_t index = int32_t(ctx.r5.u32);
    uint32_t info = h3::native::Call(ctx, base, sub_820DE6A0, object);
    if (info == 0)
        return 0u;
    int32_t count = int32_t(objects::Load32(info + 244));
    uint32_t constraints = objects::Load32(info + 248);
    for (int32_t i = 0; i < count; i++)
    {
        uint32_t constraint = constraints + uint32_t(i) * kConstraintBytes;
        if (objects::LoadS16(constraint + 18) != index || objects::LoadS16(constraint + 16) != type)
            continue;
        uint32_t mask = objects::Load16(objects::ObjectData(ctx, object) + 236);
        return (mask & objects::ShiftBit(uint32_t(i))) != 0 ? 1u : 0u;
    }
    return 0u;
})

#endif
