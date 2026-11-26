// Structure BSP lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The scenario lists its structure BSPs (+0x14, 0x6C-byte elements whose
// first field references the 'sbsp' tag); which ones are loaded is a game
// state bit mask (kActiveBspMask, one bit per BSP index 0-15).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "replace.h"
#include "structures/structure_bsps.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    using h3::GuestRef;

    using h3::native::structures::BspActive;
    using h3::native::structures::ScenarioBsp;
}

// structure_bsp_get_pathfinding_data(s16 bsp index): the active BSP's
// pathfinding data (guest address of its first element), or 0.
H3_REPLACE_INT(8209E680, structure_bsp_get_pathfinding_data, {
    int32_t index = int16_t(ctx.r3.u32);
    if (!BspActive(index))
        return 0u;
    const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(index);
    if (!bsp)
        return std::nullopt;
    const auto& pathfinding = bsp->scenario_structure_bsp.pathfinding_data;
    return pathfinding.storedCount > 0 ? pathfinding.address : 0u;
})

// pathfinding_sector_reference_valid(r3 = {s16 bsp index, u16 sector}):
// whether the BSP is in the scenario, active, has pathfinding data, and
// the sector index is below its sector count.
H3_REPLACE_INT(820B36B8, pathfinding_sector_reference_valid, {
    uint32_t reference = ctx.r3.u32;
    int32_t index = int16_t(uint16_t(GuestRef<uint16_t>(reference)));
    if (index < 0)  // includes NONE (-1)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    if (index >= scenario->scenario.structure_bsps.storedCount || !BspActive(index))
        return 0u;
    const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(index);
    if (!bsp)
        return std::nullopt;
    const auto& pathfinding = bsp->scenario_structure_bsp.pathfinding_data;
    if (pathfinding.storedCount <= 0 || !pathfinding.address)
        return 0u;
    if (pathfinding.count <= 0)
        return std::nullopt;
    int32_t sectors = pathfinding[0].sectors.storedCount;
    int32_t sector = uint16_t(GuestRef<uint16_t>(reference + 2));
    return sector < sectors ? 1u : 0u;
})

#endif

#ifdef H3_HAVE_TAG_LAYOUTS
// structure_bsp_leaf_get_cluster_reference(r3 bsp index, r4 leaf index) ->
// the cluster reference {u8 bsp index, s8 cluster} as a halfword (bsp in
// the high byte), or NONE (-1) for leaf NONE. No bounds checks, like the
// original (callers pass valid leaves).
H3_REPLACE_INT(8209E788, structure_bsp_leaf_get_cluster_reference, {
    int32_t leaf = int32_t(ctx.r4.u32);
    if (leaf == -1)
        return 0xFFFFFFFFu;
    const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(int32_t(ctx.r3.u32));
    if (!bsp)
        return std::nullopt;
    const auto& leaves = bsp->scenario_structure_bsp.leaves;
    if (leaf < 0 || leaf >= leaves.count)
        return std::nullopt;
    uint8_t cluster = uint8_t(leaves[leaf].cluster);
    return ((ctx.r3.u32 & 0xFF) << 8) | cluster;
})
// structure_clusters_have_pvs_bound_object(r3 cluster bits, r4 cluster
// bits, r5 {s16 object index, u16 scenario object index}) -> bool: some
// cluster set in both bit vectors (8 words per BSP, a bit per cluster
// index modulo 256) of a loaded BSP lists that object in its PVS Bound
// Object References. Walks every bit of the loaded-BSP mask (all 32, not
// just BspActive's 16), lowest first.
H3_REPLACE_INT(823185E8, structure_clusters_have_pvs_bound_object, {
    constexpr uint32_t kWordsPerBsp = 8;
    uint32_t mask = GuestRef<uint32_t>(h3::native::structures::kActiveBspMask);
    int32_t objectIndex = int16_t(uint16_t(GuestRef<uint16_t>(ctx.r5.u32)));
    uint16_t scenarioObjectIndex = GuestRef<uint16_t>(ctx.r5.u32 + 2);
    for (uint32_t index = 0; index < 32; index++)
    {
        if (!(mask & (1u << index)))
            continue;
        const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(int32_t(index));
        if (!bsp)
            return std::nullopt;
        const auto& clusters = bsp->scenario_structure_bsp.clusters;
        if (clusters.storedCount > clusters.count)
            return std::nullopt;
        for (int32_t cluster = 0; cluster < clusters.storedCount; cluster++)
        {
            uint32_t bit = uint32_t(cluster) & 0xFF;
            uint32_t word = (index * kWordsPerBsp + (bit >> 5)) * 4;
            uint32_t flag = 1u << (bit & 31);
            if (!(uint32_t(GuestRef<uint32_t>(ctx.r4.u32 + word)) & flag) ||
                !(uint32_t(GuestRef<uint32_t>(ctx.r3.u32 + word)) & flag))
                continue;
            const auto& references = clusters[cluster].pvs_bound_object_references;
            if (references.storedCount > references.count)
                return std::nullopt;
            for (int32_t i = 0; i < references.storedCount; i++)
                if (references[i].object_index == objectIndex &&
                    uint16_t(references[i].scenario_object_index) == scenarioObjectIndex)
                    return 1u;
        }
    }
    return 0u;
})
#endif
