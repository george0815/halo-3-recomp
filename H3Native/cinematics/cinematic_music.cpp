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
