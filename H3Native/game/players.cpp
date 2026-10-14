// Player lookups, decompiled to read the host tag cache where they read
// tags (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    constexpr uint32_t kWeaponGroup = 0x77656170;  // 'weap'
}

// player_find_weapon_of_type(r3 player, r4 s16 multiplayer weapon type) ->
// the first of the player's unit's weapons (object index) whose definition
// has that Multiplayer Weapon Type, or NONE.
H3_REPLACE_INT(82250188, player_find_weapon_of_type, {
    uint32_t unit = objects::Load32(objects::PlayerAddress(ctx, ctx.r3.u32) + objects::kPlayerUnit);
    if (unit == objects::kNone)
        return objects::kNone;
    uint32_t weapons = objects::ObjectData(ctx, unit) + objects::kUnitWeapons;
    int32_t type = int32_t(ctx.r4.u32);  // compared as the full register, like the original
    for (uint32_t slot = 0; slot < objects::kUnitWeaponSlots; slot++)
    {
        uint32_t weapon = objects::Load32(weapons + slot * 4);
        if (weapon == objects::kNone)
            continue;
        uint32_t definition = objects::ObjectDefinition(ctx, weapon);
        const auto* part = h3::native::tags::TagPartBySlot<host::weapon>(definition & 0xFFFF, kWeaponGroup);
        if (!part)
            return std::nullopt;
        if (int32_t(part->multiplayer_weapon_type) == type)
            return weapon;
    }
    return objects::kNone;
})

#endif
