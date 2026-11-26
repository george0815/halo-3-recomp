// Vehicle definition lookups made by unit (seat) code, decompiled to read
// the host tag cache (docs/ROADMAP.md 6.3 milestone A; replace.h for the
// mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kVehicleGroup = 0x76656869;  // 'vehi'
}

// vehicle_get_size_class(r3 vehicle object) -> s16: 2 if its definition's
// Vehicle Size is Large (or above), else 1 (Small).
H3_REPLACE_INT(8251B568, vehicle_get_size_class, {
    uint32_t definition = h3::native::objects::ObjectDefinition(ctx, ctx.r3.u32);
    const host::vehicle* vehicle = h3::native::tags::TagPartBySlot<host::vehicle>(definition & 0xFFFF, kVehicleGroup);
    if (!vehicle)
        return std::nullopt;
    return vehicle->vehicle_size > 0 ? 2u : 1u;
})

#endif
