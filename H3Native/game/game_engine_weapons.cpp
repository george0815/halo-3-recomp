// Game engine checks on weapon definitions (multiplayer objectives),
// decompiled to read the host tag cache (docs/ROADMAP.md 6.3 milestone A;
// replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "memory.h"
#include "objects/object_access.h"
#include "replace.h"
#include "tags/tag_cache.h"

namespace
{
    namespace host = h3::native::tags::host;
    using h3::native::objects::Load32;
    constexpr uint32_t kNone = 0xFFFFFFFF;
    constexpr uint32_t kWeaponGroup = host::weapon_tag::kGroup;

    // Weapon definition Multiplayer Weapon Type values.
    constexpr int32_t kMultiplayerWeaponCtfFlag = 1;
    constexpr int32_t kMultiplayerWeaponOddballBall = 2;

    // The object's definition's Multiplayer Weapon Type, or nullopt if the
    // definition isn't a weapon (the originals read the field of any tag).
    std::optional<int32_t> MultiplayerWeaponType(const PPCContext& ctx, uint32_t object)
    {
        uint32_t definition = h3::native::objects::ObjectDefinition(ctx, object);
        const auto* weapon = h3::native::tags::TagPartBySlot<host::weapon>(definition & 0xFFFF, kWeaponGroup);
        if (!weapon)
            return std::nullopt;
        return int32_t(weapon->multiplayer_weapon_type);
    }
}

// game_engine_object_is_assault_bomb(r3 object) -> bool: a multiplayer
// game (game globals +16 == 2) whose game engine (+568) is 9 (assault),
// and the object's definition is a CTF flag weapon. Outside multiplayer
// the original reads the engine from guest address 0; that's kept.
H3_REPLACE_INT(8220C620, game_engine_object_is_assault_bomb, {
    constexpr uint32_t kTlsGameGlobals = 128;
    constexpr uint32_t kGameMode = 16;       // s32
    constexpr uint32_t kGameEngine = 568;    // s32 (game options +552)
    constexpr int32_t kModeMultiplayer = 2;
    constexpr int32_t kEngineAssault = 9;
    uint32_t globals = Load32(Load32(ctx.r13.u32) + kTlsGameGlobals);
    uint32_t engine = int32_t(Load32(globals + kGameMode)) == kModeMultiplayer ? globals + kGameEngine : 0;
    if (int32_t(Load32(engine)) != kEngineAssault)
        return 0u;
    if (ctx.r3.u32 == kNone)
        return 0u;
    std::optional<int32_t> type = MultiplayerWeaponType(ctx, ctx.r3.u32);
    if (!type)
        return std::nullopt;
    return uint16_t(*type) == kMultiplayerWeaponCtfFlag ? 1u : 0u;
})

// game_engine_oddball_object_is_ball(r3, r4 unused, r5 object) -> bool:
// the oddball globals (pointer at 0x828C43C4) have bit 0 of +432 set and
// the object's definition is an oddball ball weapon (a virtual method: no
// direct callers).
H3_REPLACE_INT(822E7AD0, game_engine_oddball_object_is_ball, {
    constexpr uint32_t kOddballGlobals = 0x828C43C4;
    constexpr uint32_t kFlags = 432;
    if (!(Load32(Load32(kOddballGlobals) + kFlags) & 1))
        return 0u;
    if (ctx.r5.u32 == kNone)
        return 0u;
    std::optional<int32_t> type = MultiplayerWeaponType(ctx, ctx.r5.u32);
    if (!type)
        return std::nullopt;
    return *type == kMultiplayerWeaponOddballBall ? 1u : 0u;
})

#endif
