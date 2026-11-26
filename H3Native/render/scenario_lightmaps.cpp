// Scenario lightmap ('sLdT') lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
    constexpr uint32_t kLightmapBspBytes = 0x1B4;
}

// scenario_lightmap_get_bsp_data(r3 lightmap datum, r4 bsp index) -> the
// guest address of the Lightmap BSP's entry for that structure bsp, or 0
// (also for NONE).
H3_REPLACE_INT(82141348, scenario_lightmap_get_bsp_data, {
    uint32_t lightmap = ctx.r3.u32;
    if (lightmap == kNone)
        return 0u;
    const auto* tag = h3::native::tags::TagBySlotOf<host::scenario_lightmap_tag>(lightmap & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& bsps = tag->scenario_lightmap.lightmap_bsp_s;
    if (bsps.storedCount <= 0)
        return 0u;
    if (bsps.storedCount > bsps.count)
        return std::nullopt;
    int32_t bsp = int32_t(ctx.r4.u32);
    for (int32_t i = 0; i < bsps.count; i++)
        if (int32_t(bsps[i].bsp_reference_index) == bsp)
            return bsps.address + uint32_t(i) * kLightmapBspBytes;
    return 0u;
})

#endif
