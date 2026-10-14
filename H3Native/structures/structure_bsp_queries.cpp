// Structure BSP lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The scenario lists its structure BSPs (+0x14, 0x6C-byte elements whose
// first field references the 'sbsp' tag); which ones are loaded is a game
// state bit mask (kActiveBspMask, one bit per BSP index 0-15).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    using h3::GuestRef;

    constexpr uint32_t kActiveBspMask = 0x828A1ECC;  // u32: bit i = structure BSP i is loaded
    constexpr uint32_t kMaxMaskedBsps = 16;

    bool BspActive(int32_t index)
    {
        uint32_t mask = GuestRef<uint32_t>(kActiveBspMask);
        return uint32_t(index) < kMaxMaskedBsps && (mask & (1u << index));
    }

    // The scenario's BSP at index (no checks beyond the block's bounds,
    // like the callers' own checks before it), or nullptr if the host copy
    // can't say.
    const host::scenario_structure_bsp_tag* ScenarioBsp(int32_t index)
    {
        const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
        if (!scenario || index < 0 || index >= scenario->scenario.structure_bsps.count)
            return nullptr;
        uint32_t datum = scenario->scenario.structure_bsps[index].structure_bsp.datum;
        return h3::native::tags::TagBySlotOf<host::scenario_structure_bsp_tag>(datum & 0xFFFF);
    }
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
