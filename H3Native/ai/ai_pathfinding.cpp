// Pathfinding lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// Structure BSPs: the scenario's list (0x6C-byte elements referencing the
// 'sbsp' tag); which are loaded is a game state bit mask (kActiveBspMask,
// one bit per BSP index 0-15); helpers in
// structures/structure_bsps.h.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "objects/object_access.h"
#include "replace.h"
#include "structures/structure_bsps.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    using h3::GuestRef;

    using h3::native::structures::BspActive;
    using h3::native::structures::ScenarioBsp;

    constexpr uint32_t kObjectHeaderType = 3;      // object header byte: object type
    constexpr uint32_t kObjectTypeMachine = 7;
    constexpr uint32_t kObjectTypeCrate = 10;
    constexpr uint32_t kObjectTypeGiant = 12;
    constexpr uint16_t kMachineIsNotPathfindingMobile = 1u << 4;  // machine Flags
    constexpr int16_t kMachineTypeDoor = 0;
}

// pathfinding_hint_reference_valid(r3 = {s16 bsp index, s16 hint}): whether
// the BSP is in the scenario, loaded, has pathfinding data, and the hint
// index is within its Pathfinding Hints.
H3_REPLACE_INT(8256A038, pathfinding_hint_reference_valid, {
    uint32_t reference = ctx.r3.u32;
    int32_t index = int16_t(uint16_t(GuestRef<uint16_t>(reference)));
    if (index < 0)
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
    int32_t hint = int16_t(uint16_t(GuestRef<uint16_t>(reference + 2)));
    return (hint >= 0 && hint < pathfinding[0].pathfinding_hints.storedCount) ? 1u : 0u;
})

// object_is_pathfinding_mobile(r3 object) -> bool: crates and giants are;
// machines are unless their definition is a door or has Is NOT
// Pathfinding Mobile; other objects aren't.
H3_REPLACE_INT(8250C9E0, object_is_pathfinding_mobile, {
    uint32_t header = objects::DataArrayElement(ctx, objects::kTlsObjects, ctx.r3.u32, objects::kObjectHeaderBytes);
    uint8_t type = uint8_t(GuestRef<uint8_t>(header + kObjectHeaderType));
    if (type == kObjectTypeCrate || type == kObjectTypeGiant)
        return 1u;
    if (type != kObjectTypeMachine)
        return 0u;
    uint32_t definition = objects::Load32(objects::Load32(header + objects::kObjectHeaderData));
    const host::device_machine_tag* machine =
        h3::native::tags::TagBySlotOf<host::device_machine_tag>(definition & 0xFFFF);
    if (!machine)
        return std::nullopt;
    if (machine->device_machine.flags & kMachineIsNotPathfindingMobile)
        return 0u;
    return machine->device_machine.type != kMachineTypeDoor ? 1u : 0u;
})

#endif
