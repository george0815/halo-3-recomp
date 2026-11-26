// Structure BSP lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The scenario lists its structure BSPs (+0x14, 0x6C-byte elements whose
// first field references the 'sbsp' tag); which ones are loaded is a game
// state bit mask (kActiveBspMask, one bit per BSP index 0-15).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "guest_call.h"
#include "objects/object_access.h"
#include "replace.h"
#include "structures/structure_bsps.h"
#include "tags/tag_cache.h"

#include <cstring>

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

// Looks up a tag resource (r3 guest address of its resource handle) in the
// game state's resource table -> guest address of its entry (+4 the
// loaded data). Reads only.
PPC_EXTERN_FUNC(sub_82088C60);

// structure_bsp_get_resources(r3 bsp index) -> guest address of the BSP's
// loaded tag resources: through the resource table when the 'sbsp' has
// Use Resource Items (+0x384), else the raw resources pointer (+0x374).
// No range check, as in the original (name unconfirmed). Both fields are
// read from guest memory: the game may fill them at load.
H3_REPLACE_INT(82122D10, structure_bsp_get_resources, {
    constexpr uint32_t kRawResources = 0x374;      // guest address
    constexpr uint32_t kResourceHandle = 0x37C;
    constexpr uint32_t kUseResourceItems = 0x384;
    using h3::native::objects::Load32;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    int32_t index = int32_t(ctx.r3.u32);
    const auto& bsps = scenario->scenario.structure_bsps;
    if (index < 0 || index >= bsps.count)
        return std::nullopt;
    uint32_t datum = bsps[index].structure_bsp.datum;
    if (h3::native::tags::SlotGroupTag(datum & 0xFFFF) != host::scenario_structure_bsp_tag::kGroup)
        return std::nullopt;
    uint32_t bsp = h3::native::tags::TagSlotAddress(datum & 0xFFFF);
    if (!bsp)
        return std::nullopt;
    if (Load32(bsp + kUseResourceItems) != 0)
        return Load32(h3::native::Call(ctx, base, sub_82088C60, bsp + kResourceHandle) + 4);
    return Load32(Load32(bsp + kRawResources) + 4);
})

PPC_EXTERN_FUNC(sub_820AC748);

// structure_or_object_reference_valid(r3 s16 bsp index, r4 resource
// instance index, r5 instanced geometry instance index, r6 object) ->
// bool: for an object (r6 not NONE), that it exists (sub_820AC748, any
// type); otherwise that the BSP is in the scenario and loaded and either
// r5 is below its Instanced Geometry Instances count or, when r5 is NONE,
// r4 is below the count at +60 of its loaded resources
// (structure_bsp_get_resources, above) (name unconfirmed).
H3_REPLACE_INT(82345D50, structure_or_object_reference_valid, {
    constexpr uint32_t kResourcesInstanceCount = 60;  // s32 (instanced geometry definitions, probably)
    uint32_t object = ctx.r6.u32;
    if (object != 0xFFFFFFFF)
        return h3::native::Call(ctx, base, sub_820AC748, object, 0xFFFFFFFFu) != 0 ? 1u : 0u;
    int32_t index = int16_t(uint16_t(ctx.r3.u32));
    int32_t resourceInstance = int32_t(ctx.r4.u32);
    int32_t instance = int32_t(ctx.r5.u32);
    if (index < 0)
        return 0u;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    if (index >= scenario->scenario.structure_bsps.storedCount || !BspActive(index))
        return 0u;
    if (instance != -1)
    {
        if (instance < 0)
            return 0u;
        const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(index);
        if (!bsp)
            return std::nullopt;
        return instance < bsp->scenario_structure_bsp.instanced_geometry_instances.storedCount ? 1u : 0u;
    }
    if (resourceInstance < 0)
        return 0u;
    if (!ScenarioBsp(index))
        return std::nullopt;
    uint32_t resources = h3::native::Call(ctx, base, sub_82122D10, uint32_t(index));
    return resourceInstance < int32_t(h3::native::objects::Load32(resources + kResourcesInstanceCount)) ? 1u : 0u;
})

// Distance from a point (r5) to a cluster portal (r4, element of the
// 'sbsp' r3's Cluster Portals) -> f1; reads only (its callee fills its own
// stack buffers).
PPC_EXTERN_FUNC(sub_82127C50);

// cluster_get_nearest_portal_distance(r3 point, r4 cluster reference: s8
// BSP in bits 8-15, u8 cluster in bits 0-7) -> real: the smallest
// sub_82127C50 distance from the point to the cluster's Portals, or the
// constant at 0x82021694 when it has none. No range checks in the
// original (name unconfirmed).
H3_REPLACE_FLOAT(82165520, cluster_get_nearest_portal_distance, {
    constexpr uint32_t kRealFar = 0x82021694;  // lis -32254, +5780
    constexpr uint32_t kClusterPortalBytes = 40;
    uint32_t point = ctx.r3.u32;
    int32_t index = int8_t(uint8_t(ctx.r4.u32 >> 8));
    int32_t cluster = int32_t(ctx.r4.u32 & 0xFF);
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario || index < 0 || index >= scenario->scenario.structure_bsps.count)
        return std::nullopt;
    uint32_t datum = scenario->scenario.structure_bsps[index].structure_bsp.datum;
    const auto* bsp = h3::native::tags::TagBySlotOf<host::scenario_structure_bsp_tag>(datum & 0xFFFF);
    uint32_t address = h3::native::tags::TagSlotAddress(datum & 0xFFFF);
    if (!bsp || !address)
        return std::nullopt;
    const auto& clusters = bsp->scenario_structure_bsp.clusters;
    if (cluster >= clusters.count)
        return std::nullopt;
    const auto& portals = clusters[cluster].portals;
    if (portals.storedCount > 0 && portals.count != portals.storedCount)
        return std::nullopt;
    uint32_t clusterPortals = bsp->scenario_structure_bsp.cluster_portals.address;
    uint32_t farBits = h3::native::objects::Load32(kRealFar);
    float far;
    std::memcpy(&far, &farBits, sizeof(far));
    double nearest = far;
    for (int32_t i = 0; i < portals.storedCount; i++)
    {
        uint32_t portal = clusterPortals + uint32_t(int32_t(portals[i].portal_index)) * kClusterPortalBytes;
        h3::native::Call(ctx, base, sub_82127C50, address, portal, point);
        if (!(nearest < ctx.f1.f64))
            nearest = ctx.f1.f64;
    }
    return float(nearest);
})

#endif
