// Unit definition lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).
//
// A unit object's definition ('bipd', 'vehi', 'gint': all derive from
// 'unit') has the unit part (host::unit_struct_definition) after the object
// part; tags::TagPartBySlot finds it for any of them.

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kUnitGroup = 0x756E6974;  // 'unit'

    // The unit part of an object's definition, or nullptr.
    const host::unit_struct_definition* UnitDefinition(const PPCContext& ctx, uint32_t object)
    {
        uint32_t definition = h3::native::objects::ObjectDefinition(ctx, object);
        return h3::native::tags::TagPartBySlot<host::unit_struct_definition>(definition & 0xFFFF, kUnitGroup);
    }
}

// unit_seat_test_flag(r3 unit object, r4 s16 seat, r5 flag bit) -> bool:
// whether the unit definition's seat has that flag (false for a seat index
// outside its Seats block).
H3_REPLACE_INT(82327B38, unit_seat_test_flag, {
    const host::unit_struct_definition* unit = UnitDefinition(ctx, ctx.r3.u32);
    if (!unit)
        return std::nullopt;
    int32_t seat = int16_t(ctx.r4.u32);
    if (seat < 0 || seat >= unit->seats.storedCount)
        return 0u;
    if (seat >= unit->seats.count)
        return std::nullopt;
    uint32_t shift = ctx.r5.u32 & 63;  // slw: shifts of 32-63 give 0
    uint32_t bit = shift < 32 ? 1u << shift : 0u;
    return (unit->seats[seat].flags & bit) ? 1u : 0u;
})

// unit_find_driver_seat(r3 unit object) -> s16: the index of the unit
// definition's first seat with the Driver flag, or NONE (-1).
H3_REPLACE_INT(82327AB0, unit_find_driver_seat, {
    const host::unit_struct_definition* unit = UnitDefinition(ctx, ctx.r3.u32);
    if (!unit)
        return std::nullopt;
    constexpr uint32_t kSeatDriver = 1u << 2;
    const auto& seats = unit->seats;
    if (seats.storedCount <= 0)
        return 0xFFFFFFFFu;
    if (seats.storedCount > seats.count)
        return std::nullopt;
    // The original counts with a 16-bit index: fine for any real block.
    for (int32_t i = 0; i < seats.count; i++)
        if (seats[i].flags & kSeatDriver)
            return uint32_t(i);
    return 0xFFFFFFFFu;
})

#endif
