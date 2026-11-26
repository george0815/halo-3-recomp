// Scenario zone set lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
    // Globals: the loaded scenario's datum (g_scenario_datum) and the
    // active zone set's index next to it.
    constexpr uint32_t kScenarioDatum = 0x8274B794;
    constexpr uint32_t kZoneSetIndex = 0x8274B79C;
}

// scenario_get_current_zone_set_name() -> the name (string id) of the
// active zone set, or 0 with no scenario or an index out of range.
H3_REPLACE_INT(82198F78, scenario_get_current_zone_set_name, {
    uint32_t scenario = h3::native::objects::Load32(kScenarioDatum);
    if (scenario == kNone)
        return 0u;
    const host::scenario_tag* tag = h3::native::tags::TagBySlotOf<host::scenario_tag>(scenario & 0xFFFF);
    if (!tag)
        return std::nullopt;
    int32_t index = int32_t(h3::native::objects::Load32(kZoneSetIndex));
    const auto& zoneSets = tag->scenario.zone_sets;
    if (index < 0 || index >= zoneSets.storedCount)
        return 0u;
    if (index >= zoneSets.count)
        return std::nullopt;
    return zoneSets[index].name;
})

#endif
