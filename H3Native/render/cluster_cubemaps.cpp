// Structure BSP cluster cubemap lookups, decompiled to read the host tag
// cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
}

// cluster_get_cubemap_bitmap_index(r3 cluster reference: s8 bsp index in
// bits 8-15, u8 cluster index in bits 0-7) -> the cubemap bitmap index of
// the cluster's first cubemap, or NONE (no BSP, or no cubemaps). The
// original doesn't range-check the BSP or the cluster.
H3_REPLACE_INT(82175F40, cluster_get_cubemap_bitmap_index, {
    int32_t bspIndex = int8_t(ctx.r3.u32 >> 8);
    uint32_t clusterIndex = ctx.r3.u32 & 0xFF;
    if (bspIndex == -1)
        return 0xFFFFFFFFu;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario || bspIndex < 0 || bspIndex >= scenario->scenario.structure_bsps.count)
        return std::nullopt;
    uint32_t datum = scenario->scenario.structure_bsps[bspIndex].structure_bsp.datum;
    const auto* bsp = h3::native::tags::TagBySlotOf<host::scenario_structure_bsp_tag>(datum & 0xFFFF);
    if (!bsp)
        return std::nullopt;
    const auto& clusters = bsp->scenario_structure_bsp.clusters;
    if (int32_t(clusterIndex) >= clusters.count)
        return std::nullopt;
    const auto& cubemaps = clusters[clusterIndex].cluster_cubemaps;
    if (cubemaps.storedCount <= 0)
        return 0xFFFFFFFFu;
    if (cubemaps.count <= 0)
        return std::nullopt;
    return uint32_t(int32_t(cubemaps[0].cubemap_bitmap_index));
})

#endif
