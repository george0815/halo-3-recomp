// Projectile definition lookups, decompiled to read the host tag cache
// (docs/ROADMAP.md 6.3 milestone A; replace.h for the mechanism).

#ifdef H3_HAVE_TAG_LAYOUTS

#include "game/game_state_layout.h"
#include "guest_call.h"
#include "objects/object_reads.h"
#include "replace.h"
#include "tags/tag_cache.h"

PPC_EXTERN_FUNC(sub_820C0AA8);

namespace
{
    namespace host = h3::native::tags::host;
    namespace objects = h3::native::objects;
    // Read-only reals the original loads (0, 1 and the heroic blend, 0.5
    // presumably).
    constexpr uint32_t kRealZero = 0x8202251C;
    constexpr uint32_t kRealOne = 0x820221FC;
    constexpr uint32_t kRealHeroicBlend = 0x82022DC0;
    // Game globals (TLS 128): +16 s32 (1 in campaign), +332 s16 difficulty.
    constexpr uint32_t kGameGlobalsMode = 16;
    constexpr uint32_t kGameGlobalsDifficulty = 332;
}

// projectile_get_ai_velocity_scale(r3 projectile definition datum) -> real:
// the AI Velocity Scale for the campaign difficulty: Normal's at easy and
// normal, Legendary's at legendary, blended at heroic; a scale not above 0
// counts as 1 (and 1 if both are). Difficulty 1 outside the campaign.
H3_REPLACE_FLOAT(820C0AA8, projectile_get_ai_velocity_scale, {
    const host::projectile_tag* tag = h3::native::tags::TagBySlotOf<host::projectile_tag>(ctx.r3.u32 & 0xFFFF);
    if (!tag)
        return std::nullopt;
    float zero = objects::LoadFloat(kRealZero);
    float one = objects::LoadFloat(kRealOne);
    float normal = tag->projectile.ai_velocity_scale_normal;
    float legendary = tag->projectile.ai_velocity_scale_legendary;
    if (!(normal > zero) && !(legendary > zero))
        return one;
    if (!(normal > zero))
        normal = one;
    if (!(legendary > zero))
        legendary = one;
    uint32_t globals = objects::TlsSlot(ctx, h3::native::game::tls::kGameGlobals);
    int32_t difficulty = 1;
    if (int32_t(objects::Load32(globals + kGameGlobalsMode)) == 1)
        difficulty = objects::LoadS16(globals + kGameGlobalsDifficulty);
    if (difficulty <= 1)
        return normal;
    if (difficulty >= 3)
        return legendary;
    // fsubs, fmuls by (difficulty - 1) as a real, then fmadds as the
    // recompiled code computes it (in double, rounded once to float).
    float step = float(double(int64_t(difficulty - 1)));
    float range = (legendary - normal) * step;
    return float(double(range) * double(objects::LoadFloat(kRealHeroicBlend)) + double(normal));
})

// projectile_get_travel_time(r3 projectile definition datum, f1 distance)
// -> real: the distance over the projectile's Initial Velocity times its
// AI velocity scale (sub_820C0AA8, above), or 0 when that speed isn't
// above 0. No group check (name unconfirmed).
H3_REPLACE_FLOAT(82340AB8, projectile_get_travel_time, {
    const host::projectile_tag* tag = h3::native::tags::TagBySlotOf<host::projectile_tag>(ctx.r3.u32 & 0xFFFF);
    if (!tag)
        return std::nullopt;
    float distance = float(ctx.f1.f64);
    h3::native::Call(ctx, base, sub_820C0AA8, ctx.r3.u32);
    float speed = float(ctx.f1.f64) * tag->projectile.initial_velocity;
    float zero = objects::LoadFloat(kRealZero);
    if (!(speed > zero))
        return zero;
    return distance / speed;
})

#endif
