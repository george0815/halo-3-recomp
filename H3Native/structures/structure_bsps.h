// The scenario's structure BSPs, for readers on the host tag cache.
//
// The scenario lists its structure BSPs (+0x14, 0x6C-byte elements whose
// first field references the 'sbsp' tag); which ones are loaded is a game
// state bit mask (kActiveBspMask, one bit per BSP index 0-15).

#pragma once

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "tags/tag_cache.h"

namespace h3::native::structures
{
    inline constexpr uint32_t kActiveBspMask = 0x828A1ECC;  // u32: bit i = structure BSP i is loaded
    inline constexpr uint32_t kMaxMaskedBsps = 16;

    // Whether structure BSP index is loaded (bits past the mask's 16 read
    // as not loaded).
    inline bool BspActive(int32_t index)
    {
        uint32_t mask = h3::GuestRef<uint32_t>(kActiveBspMask);
        return uint32_t(index) < kMaxMaskedBsps && (mask & (1u << index));
    }

    // The scenario's BSP at index (no checks beyond the block's bounds,
    // like the callers' own checks before it), or nullptr if the host copy
    // can't say.
    inline const tags::host::scenario_structure_bsp_tag* ScenarioBsp(int32_t index)
    {
        const tags::host::scenario_tag* scenario = tags::GlobalScenario();
        if (!scenario || index < 0 || index >= scenario->scenario.structure_bsps.count)
            return nullptr;
        uint32_t datum = scenario->scenario.structure_bsps[index].structure_bsp.datum;
        return tags::TagBySlotOf<tags::host::scenario_structure_bsp_tag>(datum & 0xFFFF);
    }
}

#endif
