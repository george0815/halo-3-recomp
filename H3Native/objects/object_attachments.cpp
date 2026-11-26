// Object definition attachment lookups, decompiled to read the host tag
// cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// Every object definition group derives from 'obje', whose part
// (host::object_struct_definition) comes first; tags::TagPartBySlot finds it.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kObjectGroup = 0x6F626A65;  // 'obje'
    constexpr uint32_t kNone = 0xFFFFFFFF;
}

// object_get_attachment_marker(r3 object, s16 r4 attachment index) -> the
// marker name (old string id) of that attachment of the object's
// definition, or NONE if the index is out of range. The original doesn't
// check the object for NONE.
H3_REPLACE_INT(8210FF20, object_get_attachment_marker, {
    uint32_t object = ctx.r3.u32;
    if (object == kNone)
        return std::nullopt;
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, object);
    const auto* part = h3::native::tags::TagPartBySlot<host::object_struct_definition>(definition & 0xFFFF, kObjectGroup);
    if (!part)
        return std::nullopt;
    int32_t index = int16_t(ctx.r4.u32);
    const auto& attachments = part->attachments;
    if (index < 0 || index >= attachments.storedCount)
        return kNone;
    if (index >= attachments.count)
        return std::nullopt;
    return attachments[index].marker;
})

#endif
