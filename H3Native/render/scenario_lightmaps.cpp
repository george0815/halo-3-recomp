// Scenario lightmap ('sLdT') lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
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

// scenario_structure_bsp_lightmap_valid(r3 scenario datum, r4 bsp index)
// -> bool: the scenario's Lightmaps tag has an entry for the structure
// bsp (scenario_lightmap_get_bsp_data) whose structure BSP import
// checksum and imported mesh count match the bsp's Import Info Checksum
// and mesh count, and which has both its lightprobe and dominant light
// intensity textures (name unconfirmed). No range check on the bsp index.
H3_REPLACE_INT(8215F880, scenario_structure_bsp_lightmap_valid, {
    uint32_t bspIndex = ctx.r4.u32;
    const auto* scenario = h3::native::tags::TagBySlotOf<host::scenario_tag>(ctx.r3.u32 & 0xFFFF);
    if (!scenario)
        return std::nullopt;
    uint32_t lightmap = scenario->scenario.lightmaps.datum;
    if (lightmap == kNone)
        return 0u;
    uint32_t entry = h3::native::Call(ctx, base, sub_82141348, lightmap, bspIndex);
    if (entry == 0)
        return 0u;
    const auto* lightmapTag = h3::native::tags::TagBySlotOf<host::scenario_lightmap_tag>(lightmap & 0xFFFF);
    if (!lightmapTag)
        return std::nullopt;
    const auto& entries = lightmapTag->scenario_lightmap.lightmap_bsp_s;
    uint32_t offset = entry - entries.address;
    if (offset % kLightmapBspBytes != 0 || int32_t(offset / kLightmapBspBytes) >= entries.count)
        return std::nullopt;
    const auto& data = entries[int32_t(offset / kLightmapBspBytes)];
    const auto& bsps = scenario->scenario.structure_bsps;
    int32_t index = int32_t(bspIndex);
    if (index < 0 || index >= bsps.count)
        return std::nullopt;
    const auto* bsp =
        h3::native::tags::TagBySlotOf<host::scenario_structure_bsp_tag>(bsps[index].structure_bsp.datum & 0xFFFF);
    if (!bsp)
        return std::nullopt;
    if (data.structure_bsp_import_checksum != bsp->scenario_structure_bsp.import_info_checksum)
        return 0u;
    if (data.imported_geometry.meshes.storedCount != bsp->scenario_structure_bsp.meshes_2.storedCount)
        return 0u;
    if (data.lightprobe_texture_reference.datum == kNone)
        return 0u;
    return data.dominant_light_intensity_texture_reference.datum != kNone ? 1u : 0u;
})

#endif
