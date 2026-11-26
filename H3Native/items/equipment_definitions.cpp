// Equipment definition lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    constexpr uint32_t kEquipmentGroup = 0x65716970;  // 'eqip'
}

// equipment_definition_get_type(r3 equipment definition datum, r4 n) -> the
// index of the n-th non-empty equipment type block (0 super shield .. 8 tree
// of life), or NONE if there are fewer. Equipment normally has one type, so
// n = 0 asks for "the" type.
H3_REPLACE_INT(820E5898, equipment_definition_get_type, {
    const host::equipment* equipment = h3::native::tags::TagPartBySlot<host::equipment>(ctx.r3.u32 & 0xFFFF, kEquipmentGroup);
    if (!equipment)
        return std::nullopt;
    const int32_t counts[] = {
        equipment->super_shield.storedCount,
        equipment->multiplayer_powerup.storedCount,
        equipment->spawner.storedCount,
        equipment->proximity_mine.storedCount,
        equipment->motion_tracker_noise.storedCount,
        equipment->showme.storedCount,
        equipment->invisibility_mode.storedCount,
        equipment->invincibility_mode.storedCount,
        equipment->tree_of_life.storedCount,
    };
    int32_t wanted = int32_t(ctx.r4.u32);  // compared as the full register, like the original
    int32_t found = 0;
    for (uint32_t type = 0; type < std::size(counts); type++)
    {
        if (counts[type] == 0)
            continue;
        if (found == wanted)
            return type;
        found++;
    }
    return 0xFFFFFFFFu;
})

// equipment_get_remaining_charges(r3 equipment object) -> its definition's
// Charges minus the charges used (object data +380, a byte), or NONE for
// unlimited charges (Charges -1).
H3_REPLACE_INT(820E5580, equipment_get_remaining_charges, {
    namespace objects = h3::native::objects;
    uint32_t data = objects::ObjectData(ctx, ctx.r3.u32);
    uint32_t definition = objects::Load32(data);
    const host::equipment* equipment = h3::native::tags::TagPartBySlot<host::equipment>(definition & 0xFFFF, kEquipmentGroup);
    if (!equipment)
        return std::nullopt;
    int32_t charges = equipment->charges;
    if (charges == -1)
        return 0xFFFFFFFFu;
    constexpr uint32_t kEquipmentChargesUsed = 380;
    return uint32_t(charges - int32_t(uint8_t(h3::GuestRef<uint8_t>(data + kEquipmentChargesUsed))));
})

#endif
