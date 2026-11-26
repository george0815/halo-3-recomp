// Render method lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// Every shader group ('rmsh', 'rmw ', ...) derives from 'rm  ', whose part
// (host::render_method_struct_definition) comes first.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kRenderMethodGroup = 0x726D2020;  // 'rm  '
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// render_method_is_blended(r3 render method datum) -> whether its first
// postprocess entry's blend mode isn't opaque (0); false for NONE. The
// original reads the first entry without checking the count.
H3_REPLACE_INT(82163490, render_method_is_blended, {
    uint32_t method = ctx.r3.u32;
    if (method == kNone)
        return 0u;
    const auto* part = h3::native::tags::TagPartBySlot<host::render_method_struct_definition>(method & 0xFFFF, kRenderMethodGroup);
    if (!part)
        return std::nullopt;
    const auto& postprocess = part->postprocess;
    if (postprocess.storedCount <= 0 || postprocess.count <= 0)
        return std::nullopt;
    return postprocess[0].blend_mode != 0 ? 1u : 0u;
})

#endif
