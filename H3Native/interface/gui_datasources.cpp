// GUI datasource ('dsrc') lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// gui_tag_datasource_get_element_count(r3 datasource object, game state;
// +268 its 'dsrc' datum) -> the tag's element count, 0 for NONE (a virtual
// method: no direct callers). The original doesn't check the group.
H3_REPLACE_INT(824A1198, gui_tag_datasource_get_element_count, {
    constexpr uint32_t kDatasourceDatum = 268;
    uint32_t datum = h3::GuestRef<uint32_t>(ctx.r3.u32 + kDatasourceDatum);
    if (datum == kNone)
        return 0u;
    if (!h3::native::tags::TagSlotAddress(datum & 0xFFFF))
        return std::nullopt;  // the original returns 0 for an empty slot; past the table it reads garbage
    const auto* tag = h3::native::tags::TagBySlotOf<host::gui_datasource_definition_tag>(datum & 0xFFFF);
    if (!tag)
        return std::nullopt;
    return uint32_t(tag->gui_datasource_definition.elements.storedCount);
})

#endif
