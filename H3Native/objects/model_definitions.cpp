// Model ('hlmt') lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// model_find_node (the runtime nodes) is in object_definitions.cpp; these
// search the runtime regions (0x14 bytes each: name, collision and physics
// region indexes, permutations).

#ifdef H3_HAVE_TAG_LAYOUTS

#include <cstring>

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// model_find_region(r3 model datum, r4 name string id) -> the index of the
// model's runtime region with that name, or NONE (also for a NONE model or
// a 0 name).
H3_REPLACE_INT(82262F60, model_find_region, {
    uint32_t model = ctx.r3.u32;
    if (model == kNone || ctx.r4.u32 == 0)
        return kNone;
    const host::model_tag* tag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& regions = tag->model.runtime_regions;
    if (regions.storedCount <= 0)
        return kNone;
    if (regions.storedCount > regions.count)
        return std::nullopt;
    for (int32_t i = 0; i < regions.count; i++)
        if (regions[i].name == ctx.r4.u32)
            return uint32_t(i);
    return kNone;
})

// model_find_region_by_physics_region(r3 model datum, r4 physics region
// index) -> the index of the first runtime region with that physics region
// index, or NONE. The original doesn't check the model for NONE.
H3_REPLACE_INT(82263810, model_find_region_by_physics_region, {
    uint32_t model = ctx.r3.u32;
    if (model == kNone)
        return std::nullopt;
    const host::model_tag* tag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& regions = tag->model.runtime_regions;
    if (regions.storedCount <= 0)
        return kNone;
    if (regions.storedCount > regions.count)
        return std::nullopt;
    int32_t physicsRegion = int32_t(ctx.r4.u32);
    for (int32_t i = 0; i < regions.count; i++)
        if (int32_t(regions[i].physics_region_index) == physicsRegion)
            return uint32_t(i);
    return kNone;
})

namespace
{
    constexpr uint32_t kObjectGroup = 0x6F626A65;  // 'obje'
    // Read-only data: the smallest bounding radius (compared against, and
    // the value used below it).
    constexpr uint32_t kMinimumRadiusThreshold = 0x8202490C;
    constexpr uint32_t kMinimumRadius = 0x820221FC;

    float GuestFloat(uint32_t address)
    {
        uint32_t bits = h3::native::objects::Load32(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
}

// object_has_volume_samples(r3 object) -> whether the render model of the
// object's model has volume samples. The original doesn't check the object
// for NONE.
H3_REPLACE_INT(822B7B28, object_has_volume_samples, {
    uint32_t object = ctx.r3.u32;
    if (object == kNone)
        return std::nullopt;
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, object);
    const auto* part = h3::native::tags::TagPartBySlot<host::object_struct_definition>(definition & 0xFFFF, kObjectGroup);
    if (!part)
        return std::nullopt;
    uint32_t model = part->model.datum;
    if (model == kNone)
        return 0u;
    const host::model_tag* modelTag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
    if (!modelTag)
        return std::nullopt;
    uint32_t renderModel = modelTag->model.render_model.datum;
    if (renderModel == kNone)
        return 0u;
    const auto* render = h3::native::tags::TagBySlotOf<host::render_model_tag>(renderModel & 0xFFFF);
    if (!render)
        return std::nullopt;
    return render->render_model.volume_samples.storedCount > 0 ? 1u : 0u;
})

// object_definition_get_bounding_radius(r3 object definition datum) -> the
// larger of its bounding radius and its model's first Model Object Data
// radius, at least a minimum radius.
H3_REPLACE_FLOAT(822721B8, object_definition_get_bounding_radius, {
    const auto* part = h3::native::tags::TagPartBySlot<host::object_struct_definition>(ctx.r3.u32 & 0xFFFF, kObjectGroup);
    if (!part)
        return std::nullopt;
    float radius = part->bounding_radius;
    uint32_t model = part->model.datum;
    if (model != kNone)
    {
        const host::model_tag* modelTag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
        if (!modelTag)
            return std::nullopt;
        const auto& data = modelTag->model.model_object_data;
        if (data.storedCount > 0 && data.address != 0)
        {
            if (data.count <= 0)
                return std::nullopt;
            float dataRadius = data[0].radius;
            if (!(radius > dataRadius))
                radius = dataRadius;
        }
    }
    if (!(radius < GuestFloat(kMinimumRadiusThreshold)))
        return radius;
    return GuestFloat(kMinimumRadius);
})

#endif
