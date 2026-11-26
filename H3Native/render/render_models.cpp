// Render model ('mode') lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// render_model_count_permutation_nodes(r3 render model datum, r5 guest u8
// per region: the mesh (per mesh node map index) drawn, 255 for none) ->
// 1 if the model has node maps, else its node count, plus the node map
// size of each region's mesh. The datum isn't checked.
H3_REPLACE_INT(8215AD58, render_model_count_permutation_nodes, {
    constexpr uint16_t kHasNodeMaps = 1u << 2;
    if (ctx.r3.u32 == kNone)
        return std::nullopt;
    const auto* tag = h3::native::tags::TagBySlotOf<host::render_model_tag>(ctx.r3.u32 & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& model = tag->render_model;
    uint32_t count = (model.flags & kHasNodeMaps) ? 1u : uint32_t(model.nodes.storedCount);
    const auto& nodeMaps = model.render_geometry.per_mesh_node_map;
    if (nodeMaps.storedCount <= 0)
        return count;
    int32_t regions = model.regions.storedCount;
    for (int32_t i = 0; i < regions; i++)
    {
        uint8_t mesh = h3::GuestRef<uint8_t>(ctx.r5.u32 + uint32_t(i));
        if (mesh == 255)
            continue;
        if (mesh >= nodeMaps.count)
            return std::nullopt;  // the original doesn't range-check it
        count += uint32_t(nodeMaps[mesh].node_map.storedCount);
    }
    return count;
})

#endif
