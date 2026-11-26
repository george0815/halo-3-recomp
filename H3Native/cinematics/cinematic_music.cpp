// Cinematic tag lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// The scenario lists its cinematics ('cine'); each has scenes ('cisc'),
// each scene shots (0xA4 bytes), each shot music/foley cues (0x18 bytes).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// cinematic_get_shot_music(r3 cinematic index, r4 scene, r5 shot, r6 cue)
// -> the datum of that cue's music/foley sound, or NONE if an index is out
// of range or a reference is NONE. The original doesn't range-check the
// cinematic index.
H3_REPLACE_INT(82240F28, cinematic_get_shot_music, {
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    const auto& cinematics = scenario->scenario.cinematics;
    uint32_t cinematicIndex = ctx.r3.u32;
    if (cinematicIndex >= uint32_t(cinematics.count))
        return std::nullopt;
    uint32_t cinematicDatum = cinematics[cinematicIndex].name.datum;
    if (cinematicDatum == kNone)
        return kNone;
    const host::cinematic_tag* cinematic = h3::native::tags::TagBySlotOf<host::cinematic_tag>(cinematicDatum & 0xFFFF);
    if (!cinematic)
        return std::nullopt;

    int32_t sceneIndex = int32_t(ctx.r4.u32);
    const auto& scenes = cinematic->cinematic.scenes;
    if (sceneIndex < 0 || sceneIndex >= scenes.storedCount)
        return kNone;
    if (sceneIndex >= scenes.count)
        return std::nullopt;
    uint32_t sceneDatum = scenes[sceneIndex].scene.datum;
    if (sceneDatum == kNone)
        return kNone;
    const host::cinematic_scene_tag* scene = h3::native::tags::TagBySlotOf<host::cinematic_scene_tag>(sceneDatum & 0xFFFF);
    if (!scene)
        return std::nullopt;

    int32_t shotIndex = int32_t(ctx.r5.u32);
    const auto& shots = scene->cinematic_scene.shots;
    if (shotIndex < 0 || shotIndex >= shots.storedCount)
        return kNone;
    if (shotIndex >= shots.count)
        return std::nullopt;
    int32_t cueIndex = int32_t(ctx.r6.u32);
    const auto& music = shots[shotIndex].music;
    if (cueIndex < 0 || cueIndex >= music.storedCount)
        return kNone;
    if (cueIndex >= music.count)
        return std::nullopt;
    return music[cueIndex].music_foley.datum;
})

#endif

#ifdef H3_HAVE_TAG_LAYOUTS
// cinematic_get_scene(r3 cinematic index or cortana effect datum, r4 scene)
// -> guest address of the scene tag ('cisc', group unchecked), or 0: for a
// scenario cinematic in range, its scenes entry (0 for an out-of-range
// scene or a NONE reference); otherwise 0 for NONE and indexes 0-1000,
// and for any other value the 'crte' tag it names (else 0) gives its
// Cinematic Scene.
H3_REPLACE_INT(82301180, cinematic_get_scene, {
    constexpr uint32_t kCortanaEffectGroup = host::cortana_effect_definition_tag::kGroup;
    const host::scenario_tag* scenario = h3::native::tags::GlobalScenario();
    if (!scenario)
        return std::nullopt;
    int32_t index = int32_t(ctx.r3.u32);
    const auto& cinematics = scenario->scenario.cinematics;
    uint32_t scene = kNone;
    bool fromCinematic = false;
    if (index >= 0 && index < cinematics.storedCount)
    {
        if (index >= cinematics.count)
            return std::nullopt;
        uint32_t datum = cinematics[index].name.datum;
        if (datum != kNone)
        {
            if (!h3::native::tags::TagSlotAddress(datum & 0xFFFF))
                return std::nullopt;  // (0 would take the fallback; can't tell an empty slot from one past the table)
            const host::cinematic_tag* cinematic = h3::native::tags::TagBySlotOf<host::cinematic_tag>(datum & 0xFFFF);
            if (!cinematic)
                return std::nullopt;
            int32_t sceneIndex = int32_t(ctx.r4.u32);
            const auto& scenes = cinematic->cinematic.scenes;
            if (sceneIndex < 0 || sceneIndex >= scenes.storedCount)
                return 0u;
            if (sceneIndex >= scenes.count)
                return std::nullopt;
            scene = scenes[sceneIndex].scene.datum;
            fromCinematic = true;
        }
    }
    if (!fromCinematic)
    {
        if (index == -1 || (index >= 0 && index <= 1000))
            return 0u;
        if (ctx.r3.u32 & 0x8000)
            return std::nullopt;  // the original indexes the tag table with a negative short
        uint32_t slot = ctx.r3.u32 & 0xFFFF;
        uint32_t group = h3::native::tags::SlotGroupTag(slot);
        if (group == 0)
            return std::nullopt;  // empty slot: the original reads the group table at -1
        if (group != kCortanaEffectGroup)
            return 0u;
        const auto* effect = h3::native::tags::TagBySlotOf<host::cortana_effect_definition_tag>(slot);
        if (!effect)
            return std::nullopt;
        scene = effect->cortana_effect_definition.cinematic_scene.datum;
    }
    if (scene == kNone)
        return 0u;
    uint32_t address = h3::native::tags::TagSlotAddress(scene & 0xFFFF);
    return address ? std::optional<uint32_t>(address) : std::nullopt;
})
#endif
