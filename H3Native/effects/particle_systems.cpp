// Particle system lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// A particle system in the game state (guest memory) names its definition
// by effect, event and index: +12 the effect ('effe') datum, +16 u8 the
// event, +10 u16 the event's particle system.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "replace.h"
#include "tags/tag_cache.h"

#include <cstring>

namespace
{
    namespace host = h3::native::tags::host;
    using h3::GuestRef;
    constexpr uint32_t kNone = 0xFFFFFFFF;

    constexpr uint32_t kSystemIndex = 10;  // u16
    constexpr uint32_t kSystemEffect = 12; // effect datum
    constexpr uint32_t kSystemEvent = 16;  // u8

    // The definition (the effect event's particle systems entry) of the
    // particle system at this guest address, or nullptr if the host copy
    // can't say (the originals check none of the indexes).
    const host::particle_system_definition_block_new* SystemDefinition(uint32_t system)
    {
        uint32_t effectDatum = GuestRef<uint32_t>(system + kSystemEffect);
        if (effectDatum == kNone)
            return nullptr;
        const auto* effect = h3::native::tags::TagBySlotOf<host::effect_tag>(effectDatum & 0xFFFF);
        if (!effect)
            return nullptr;
        int32_t eventIndex = uint8_t(GuestRef<uint8_t>(system + kSystemEvent));
        const auto& events = effect->effect.events;
        if (eventIndex >= events.count)
            return nullptr;
        int32_t systemIndex = uint16_t(GuestRef<uint16_t>(system + kSystemIndex));
        const auto& systems = events[eventIndex].particle_systems;
        if (systemIndex >= systems.count)
            return nullptr;
        return &systems[systemIndex];
    }

    // A float in read-only (or game) data.
    float LoadFloat(uint32_t address)
    {
        uint32_t bits = GuestRef<uint32_t>(address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
}

// particle_system_get_particle(r3 particle system) -> guest address of
// its definition's particle tag ('prt3', group unchecked), or 0 for NONE.
H3_REPLACE_INT(82145528, particle_system_get_particle, {
    const host::particle_system_definition_block_new* definition = SystemDefinition(ctx.r3.u32);
    if (!definition)
        return std::nullopt;
    uint32_t particle = definition->particle.datum;
    if (particle == kNone)
        return 0u;
    uint32_t address = h3::native::tags::TagSlotAddress(particle & 0xFFFF);
    return address ? std::optional<uint32_t>(address) : std::nullopt;
})

// particle_system_get_lod_fade(r3 particle system, f1 distance) -> real:
// how much the system shows at that distance (its definition's LOD in/out
// distances and feathers): 1 for "lod always 1.0"; 0 before the LOD in
// distance and from the LOD out distance on; ramps in over the feather
// in delta and out over the feather out delta (by the inverse feathers).
// The distance is scaled by the split-screen LOD scale (game data) unless
// "lod same in splitscreen". Float steps as the original (single-precision
// results, double compares; NaN compares take the original's branches).
H3_REPLACE_FLOAT(822B63D0, particle_system_get_lod_fade, {
    constexpr uint32_t kOne = 0x820221FC;            // read-only real: 1.0
    constexpr uint32_t kZero = 0x8202251C;           // read-only real: 0.0
    constexpr uint32_t kSplitscreenLodScale = 0x82744484;  // real
    constexpr uint16_t kLodAlwaysOne = 1u << 2;
    constexpr uint16_t kLodSameInSplitscreen = 1u << 3;
    const host::particle_system_definition_block_new* definition = SystemDefinition(ctx.r3.u32);
    if (!definition)
        return std::nullopt;
    double one = LoadFloat(kOne);
    double zero = LoadFloat(kZero);
    uint16_t flags = definition->flags;
    if (flags & kLodAlwaysOne)
        return float(one);
    double distance = ctx.f1.f64;
    if (!(flags & kLodSameInSplitscreen))
        distance = double(float(double(LoadFloat(kSplitscreenLodScale)) * distance));
    double in = definition->lod_in_distance;
    if (!(distance > in))
        return float(zero);
    double out = definition->lod_out_distance;
    if (!(distance < out))
        return float(zero);
    double fadedIn = double(float(double(definition->lod_feather_in_delta) + in));
    if (distance < fadedIn)
        return float(double(float(distance - in)) * double(definition->inverse_lod_feather_in));
    double fadingOut = double(float(out - double(definition->lod_feather_out_delta)));
    if (!(distance > fadingOut))
        return float(one);
    return float(double(float(out - distance)) * double(definition->inverse_lod_feather_out));
})

#endif
