// Structure BSP collision lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// A BSP's collision geometry lives in its loaded resources, not in the
// tag: per game state copy (the word in TLS slot 116), the game keeps a
// table of each BSP's collision BSP (kBspCollision) and of each BSP's
// instanced geometry definitions (kBspInstanceDefinitions; 0xB8 bytes
// each: +20 collision info, +116 render BSP count, +140 breakable surface
// set count). Those stay in guest memory; the tag gives the instances.
// Collision BSP: +60 surface count, +64 surfaces (12 bytes: +4 s16
// material, +6 s16 breakable surface set, +8 s16 breakable surface, +10
// u8 flags).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "guest_call.h"
#include "memory.h"
#include "replace.h"
#include "structures/structure_bsps.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    using h3::GuestRef;
    using h3::native::structures::ScenarioBsp;

    constexpr uint32_t kTlsGameStateCopy = 116;          // u32 index
    constexpr uint32_t kBspCollision = 0x827CF608;       // u32[copy * 64 + bsp]: collision BSP
    constexpr uint32_t kBspInstanceDefinitions = 0x827CF64C;  // u32 at copy * 256 + bsp * 12
    constexpr uint32_t kInstanceDefinitionBytes = 184;
    constexpr uint32_t kInstanceCollisionInfo = 20;
    constexpr uint32_t kInstanceRenderBspCount = 116;
    constexpr uint32_t kInstanceBreakableSurfaceSetCount = 140;
    constexpr uint32_t kCollisionSurfaceCount = 60;
    constexpr uint32_t kCollisionSurfaces = 64;
    constexpr uint32_t kSurfaceBytes = 12;
    constexpr uint8_t kSurfaceBreakable = 1u << 3;

    uint32_t Load32(uint32_t address) { return GuestRef<uint32_t>(address); }

    uint32_t GameStateCopy(const PPCContext& ctx) { return Load32(Load32(ctx.r13.u32) + kTlsGameStateCopy); }

    // The BSP's own collision BSP (guest address, 0 if not loaded).
    uint32_t BspCollision(const PPCContext& ctx, uint32_t bsp)
    {
        uint32_t copy = GameStateCopy(ctx);
        return Load32(kBspCollision + (((copy << 6) + bsp) << 2));
    }

    // The guest address of the BSP's instanced geometry definition at index
    // (the copy's table, unchecked like the originals).
    uint32_t InstanceDefinition(const PPCContext& ctx, uint32_t bsp, int32_t definition)
    {
        uint32_t copy = GameStateCopy(ctx);
        uint32_t definitions = Load32(kBspInstanceDefinitions + (copy << 8) + bsp * 12);
        return definitions + uint32_t(definition) * kInstanceDefinitionBytes;
    }
}

// structure_bsp_get_breakable_surface_set_count(r3 bsp index, r4 instance)
// -> the breakable surface set count of the BSP (instance NONE) or of that
// instanced geometry instance's definition; 0 for an instance out of
// range. The BSP index isn't checked, as in the original.
H3_REPLACE_INT(8209E520, structure_bsp_get_breakable_surface_set_count, {
    const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(int32_t(ctx.r3.u32));
    if (!bsp)
        return std::nullopt;
    int32_t instance = int32_t(ctx.r4.u32);
    const auto& structure = bsp->scenario_structure_bsp;
    if (instance == -1)
        return uint32_t(structure.breakable_surface_sets.storedCount);
    const auto& instances = structure.instanced_geometry_instances;
    if (instance < 0 || instance >= instances.storedCount)
        return 0u;
    if (instance >= instances.count)
        return std::nullopt;
    uint32_t definition = InstanceDefinition(ctx, ctx.r3.u32, instances[instance].instance_definition);
    return Load32(definition + kInstanceBreakableSurfaceSetCount);
})

// structure_collision_surface_is_breakable_surface(r3 bsp index, r4
// instance or NONE, r5 breakable surface, r6 breakable surface set, r7
// surface) -> bool: the surface of the BSP's (or the instance's) collision
// geometry is in range, breakable, and is that breakable surface of that
// set. False for an instance out of range or collision not loaded.
H3_REPLACE_INT(8222B628, structure_collision_surface_is_breakable_surface, {
    int32_t instance = int32_t(ctx.r4.u32);
    uint32_t collision;
    if (instance == -1)
        collision = BspCollision(ctx, ctx.r3.u32);
    else
    {
        const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(int32_t(ctx.r3.u32));
        if (!bsp)
            return std::nullopt;
        const auto& instances = bsp->scenario_structure_bsp.instanced_geometry_instances;
        if (instance < 0 || instance >= instances.storedCount)
            return 0u;
        if (instance >= instances.count)
            return std::nullopt;
        collision = InstanceDefinition(ctx, ctx.r3.u32, instances[instance].instance_definition) + kInstanceCollisionInfo;
    }
    if (collision == 0)
        return 0u;
    int32_t surface = int32_t(ctx.r7.u32);
    if (surface < 0 || surface >= int32_t(Load32(collision + kCollisionSurfaceCount)))
        return 0u;
    uint32_t entry = Load32(collision + kCollisionSurfaces) + uint32_t(surface) * kSurfaceBytes;
    if (!(uint8_t(GuestRef<uint8_t>(entry + 10)) & kSurfaceBreakable))
        return 0u;
    if (int32_t(int16_t(uint16_t(GuestRef<uint16_t>(entry + 8)))) != int32_t(ctx.r5.u32))
        return 0u;
    return int32_t(int16_t(uint16_t(GuestRef<uint16_t>(entry + 6)))) == int32_t(ctx.r6.u32) ? 1u : 0u;
})

// collision_result_surface_is_plain(r3 collision result: +0 type, +56
// instance or NONE, +72 bsp index, +80 surface) -> bool: not a type 4
// result; for an instance, its definition has no render BSP; the
// collision geometry is loaded and the surface (unchecked index) is none
// of two sided, invisible, breakable, invalid, conveyor, and has a
// material.
H3_REPLACE_INT(8222D498, collision_result_surface_is_plain, {
    constexpr uint32_t kType = 0, kInstance = 56, kBsp = 72, kSurface = 80;
    constexpr uint8_t kExcludedFlags = 0x3B;  // two sided, invisible, breakable, invalid, conveyor
    uint32_t result = ctx.r3.u32;
    if (int32_t(Load32(result + kType)) == 4)
        return 0u;
    int32_t instance = int32_t(Load32(result + kInstance));
    uint32_t bspIndex = Load32(result + kBsp);
    uint32_t collision;
    if (instance == -1)
        collision = BspCollision(ctx, bspIndex);
    else
    {
        const host::scenario_structure_bsp_tag* bsp = ScenarioBsp(int32_t(bspIndex));
        if (!bsp)
            return std::nullopt;
        const auto& instances = bsp->scenario_structure_bsp.instanced_geometry_instances;
        if (instance < 0 || instance >= instances.count)
            return std::nullopt;  // the original doesn't range-check it
        uint32_t definition = InstanceDefinition(ctx, bspIndex, instances[instance].instance_definition);
        if (int32_t(Load32(definition + kInstanceRenderBspCount)) != 0)
            return 0u;
        collision = definition + kInstanceCollisionInfo;
    }
    if (collision == 0)
        return 0u;
    uint32_t surface = Load32(result + kSurface);
    uint32_t entry = Load32(collision + kCollisionSurfaces) + surface * kSurfaceBytes;
    if (uint8_t(GuestRef<uint8_t>(entry + 10)) & kExcludedFlags)
        return 0u;
    return uint16_t(GuestRef<uint16_t>(entry + 4)) != 0xFFFF ? 1u : 0u;
})

// breakable_surface_set_get(r3 bsp index, r4 instance or NONE) -> guest
// address of the breakable surface set's 56-byte state in the breakable
// surface globals (TLS 172), or 0 when the BSP's bit isn't set in their
// +8 mask, it has no breakable surface sets
// (structure_bsp_get_breakable_surface_set_count <= 0) or its s16 index
// is NONE. The index table: u16 at globals + 2 * (bsp + 59132) for the
// BSP, at globals + 2 * ((bsp << 12) + instance + 59148) for an instance
// (addis 1, addi -6404 / -6388) (name unconfirmed).
H3_REPLACE_INT(8209E468, breakable_surface_set_get, {
    constexpr uint32_t kTlsBreakableSurfaceGlobals = 172;
    constexpr uint32_t kBspMask = 8;
    constexpr uint32_t kSetStates = 3576;
    constexpr uint32_t kSetStateBytes = 56;
    uint32_t bsp = ctx.r3.u32;
    uint32_t instance = ctx.r4.u32;
    uint32_t globals = Load32(Load32(ctx.r13.u32) + kTlsBreakableSurfaceGlobals);
    uint32_t shift = bsp & 63;  // slw
    uint32_t bit = shift < 32 ? 1u << shift : 0u;
    if ((Load32(globals + kBspMask) & bit) == 0)
        return 0u;
    if (int32_t(h3::native::Call(ctx, base, sub_8209E520, bsp, instance)) <= 0)
        return 0u;
    uint32_t slot = instance == 0xFFFFFFFFu ? bsp + 0x10000 - 6404 : (bsp << 12) + instance + 0x10000 - 6388;
    int32_t index = int16_t(uint16_t(GuestRef<uint16_t>(globals + (slot << 1))));
    if (index == -1)
        return 0u;
    return globals + uint32_t(index) * kSetStateBytes + kSetStates;
})

#endif
