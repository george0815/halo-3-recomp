// Object definition lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// Every object definition group derives from 'obje', whose part
// (host::object_struct_definition) comes first; tags::TagPartBySlot finds it.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kObjectGroup = 0x6F626A65;  // 'obje'
}

// object_get_damage_info(r3 object) -> guest address of the first element
// of its model's New Damage Info block; 0 for NONE, no model or an empty
// block.
H3_REPLACE_INT(820DE6A0, object_get_damage_info, {
    uint32_t object = ctx.r3.u32;
    if (object == h3::native::objects::kNone)
        return 0u;
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, object);
    const auto* part = h3::native::tags::TagPartBySlot<host::object_struct_definition>(definition & 0xFFFF, kObjectGroup);
    if (!part)
        return std::nullopt;
    uint32_t model = part->model.datum;
    if (model == host::kNone)
        return 0u;
    const host::model_tag* tag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& damage = tag->model.new_damage_info;
    return damage.storedCount > 0 ? damage.address : 0u;  // signed, like the original
})

// model_find_node(r3 model datum, r4 name string id) -> the index of the
// model's runtime node with that name, or NONE.
H3_REPLACE_INT(82263870, model_find_node, {
    uint32_t model = ctx.r3.u32;
    if (model == h3::native::objects::kNone)
        return 0xFFFFFFFFu;
    const host::model_tag* tag = h3::native::tags::TagBySlotOf<host::model_tag>(model & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& nodes = tag->model.runtime_nodes;
    if (nodes.storedCount <= 0)
        return 0xFFFFFFFFu;
    if (nodes.storedCount > nodes.count)
        return std::nullopt;
    for (int32_t i = 0; i < nodes.count; i++)
        if (nodes[i].name == ctx.r4.u32)
            return uint32_t(i);
    return 0xFFFFFFFFu;
})

// render_model_find_marker_group(r3 render model datum, r4 name string id)
// -> the index of the model's marker group with that name, or NONE (also
// for a NONE model or a name of NONE or 0).
H3_REPLACE_INT(8209CFF8, render_model_find_marker_group, {
    uint32_t model = ctx.r3.u32;
    uint32_t name = ctx.r4.u32;
    if (model == h3::native::objects::kNone || name == 0xFFFFFFFFu || name == 0)
        return 0xFFFFFFFFu;
    const host::render_model_tag* tag = h3::native::tags::TagBySlotOf<host::render_model_tag>(model & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& groups = tag->render_model.marker_groups;
    if (groups.storedCount <= 0)
        return 0xFFFFFFFFu;
    if (groups.storedCount > groups.count)
        return std::nullopt;
    for (int32_t i = 0; i < groups.storedCount; i++)
        if (groups[i].name == name)
            return uint32_t(i);
    return 0xFFFFFFFFu;
})

#endif
