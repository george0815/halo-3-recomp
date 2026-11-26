// Material effects ('foot') lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The tag's effects block has one entry per effect type (0x24 bytes: old
// materials, sounds, effects); a type with nothing of the asked kind falls
// back to a related type (the original's switch table), until none is left.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr int32_t kNone = -1;

    // The type a type falls back to, or NONE.
    int32_t FallbackType(int32_t type)
    {
        switch (type)
        {
            case 8: return 5;
            case 13: return 14;
            case 16:
            case 17: return 15;
            case 19: return 16;
            case 20: return 19;
            default: return kNone;
        }
    }
}

// material_effects_find_type(r3 material effects datum, r4 effects (else
// sounds), r5 type) -> the first type in the type's fallback chain that
// has sounds (or effects), or NONE. The original doesn't check the datum
// for NONE.
H3_REPLACE_INT(8225E3B0, material_effects_find_type, {
    int32_t type = int32_t(ctx.r5.u32);
    if (type == kNone)
        return uint32_t(kNone);
    uint32_t datum = ctx.r3.u32;
    if (datum == 0xFFFFFFFFu)
        return std::nullopt;
    const host::material_effects_tag* tag = h3::native::tags::TagBySlotOf<host::material_effects_tag>(datum & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& effects = tag->material_effects.effects;
    bool wantEffects = ctx.r4.u32 != 0;
    while (type != kNone)
    {
        if (type < effects.storedCount)
        {
            if (type < 0 || type >= effects.count)
                return std::nullopt;
            const auto& entry = effects[type];
            if ((wantEffects ? entry.effects.storedCount : entry.sounds.storedCount) > 0)
                return uint32_t(type);
        }
        type = FallbackType(type);
    }
    return uint32_t(kNone);
})

// material_effects_find_material(r3 material effects datum, r4 type, r5
// effects (else sounds), r6 s16 global material index) -> guest address
// of the sounds (or effects) element of the type (or its fallback, as
// material_effects_find_type) whose runtime material index is the
// material's, else its parent material's and so on up the game globals'
// Materials; 0 if there's no such type or element. Runtime material
// indexes ('!') come from guest memory.
H3_REPLACE_INT(820F6900, material_effects_find_material, {
    constexpr uint32_t kMaterialBytes = 40;
    constexpr uint32_t kMaterialRuntimeIndex = 36;  // s16
    constexpr uint32_t kGlobalMaterialBytes = 368;
    constexpr uint32_t kGlobalMaterialParentIndex = 8;  // s16
    using h3::GuestRef;
    uint32_t datum = ctx.r3.u32;
    bool wantEffects = ctx.r5.u32 != 0;
    int32_t requested = int32_t(ctx.r4.u32);
    int32_t material = int16_t(uint16_t(ctx.r6.u32));
    if (requested == kNone)
        return 0u;
    if (datum == 0xFFFFFFFFu)
        return std::nullopt;
    const host::material_effects_tag* tag = h3::native::tags::TagBySlotOf<host::material_effects_tag>(datum & 0xFFFF);
    if (!tag)
        return std::nullopt;
    const auto& effects = tag->material_effects.effects;
    // material_effects_find_type, as above.
    int32_t type = requested;
    while (type != kNone)
    {
        if (type < effects.storedCount)
        {
            if (type < 0 || type >= effects.count)
                return std::nullopt;
            const auto& entry = effects[type];
            if ((wantEffects ? entry.effects.storedCount : entry.sounds.storedCount) > 0)
                break;
        }
        type = FallbackType(type);
    }
    if (type == kNone)
        return 0u;
    const auto& list = wantEffects ? effects[type].effects : effects[type].sounds;
    const host::globals_tag* globals = h3::native::tags::GlobalGameGlobals();
    if (!globals)
        return std::nullopt;
    const auto& materials = globals->globals.materials;
    for (int depth = 0; depth < 4096; depth++)
    {
        if (material == kNone || material < 0 || material >= materials.storedCount)
            return 0u;
        uint32_t global = materials.address + uint32_t(material) * kGlobalMaterialBytes;
        if (global == 0)
            return 0u;
        for (int32_t i = 0; i < list.storedCount; i++)
        {
            uint32_t element = list.address + uint32_t(i) * kMaterialBytes;
            if (int16_t(uint16_t(GuestRef<uint16_t>(element + kMaterialRuntimeIndex))) == material)
                return element;
        }
        material = int16_t(uint16_t(GuestRef<uint16_t>(global + kGlobalMaterialParentIndex)));
    }
    return std::nullopt;
})

#endif
