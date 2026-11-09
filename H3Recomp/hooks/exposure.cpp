// Auto exposure adaptation at 60 ticks per second (one of MCC's 60 Hz
// content fixes, hooks::ContentFixes(); hooks/tick_rate.h).
//
// sub_8216B4B8 (MCC FUN_1408bdae0) is the camera's auto exposure update: it
// records the scene's target exposure in a 60-entry history and moves the
// current exposure (state + 12) toward it by a fraction per update, the
// adaptation rate (parameters + 12, r28), capped by the distance left times
// a constant. MCC divides the rate by game ticks per 30 Hz tick
// (FUN_14036f2c0). The update runs once per rendered frame, not per tick
// (H3_LOG_EXPOSURE: ~600 updates per 650 ticks at 60 Hz below 60 fps), so
// here the rate is scaled by the game ticks since the previous update over
// ticks per 30 Hz tick (at most 1): MCC's factor when every frame is one
// tick, the original pace when frames take a 30 Hz tick or longer.
//
// Retail loads the rate right after its call to sub_8216BA68 (exposure
// rising, return address kRisingCall) or sub_8244DAB8 (falling,
// kFallingCall); every path to the blend passes one of them. There the rate
// in memory is divided for the rest of the update and put back when
// sub_8216B4B8 returns.

#include "hooks/tick_rate.h"
#include "config/env.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <algorithm>
#include <cstdio>

namespace
{
    constexpr uint32_t kRisingCall = 0x8216B674;
    constexpr uint32_t kFallingCall = 0x8216B690;
    constexpr uint32_t kParametersRate = 12;  // float, in the parameters (r28)

    // The rate patched during the current update (0: none).
    uint32_t g_patchedRate = 0;
    float g_originalRate = 0.0f;

    // This update's share of a 30 Hz tick (set by sub_8216B4B8's hook).
    float g_rateScale = 1.0f;

    constexpr uint32_t kTlsGameTime = 188;
    constexpr uint32_t kGameTimeTick = 12;

    uint32_t GameTick(const PPCContext& ctx)
    {
        uint32_t gameTime = h3::GuestRef<uint32_t>(uint32_t(h3::GuestRef<uint32_t>(ctx.r13.u32)) + kTlsGameTime);
        return gameTime ? uint32_t(h3::GuestRef<uint32_t>(gameTime + kGameTimeTick)) : 0u;
    }

    void SlowRate(const PPCContext& ctx)
    {
        if (g_patchedRate || g_rateScale >= 1.0f)
            return;
        g_patchedRate = ctx.r28.u32 + kParametersRate;
        auto& rate = h3::GuestRef<float>(g_patchedRate);
        g_originalRate = rate;
        rate = g_originalRate * g_rateScale;
    }
}

PPC_FUNC_IMPL(__imp__sub_8216B4B8);
PPC_FUNC(sub_8216B4B8)
{
    uint32_t tick = GameTick(ctx);
    g_rateScale = 1.0f;
    if (h3::hooks::ContentFixes())
    {
        static uint32_t lastTick = 0;
        uint32_t ticks = tick > lastTick ? tick - lastTick : 1u;
        lastTick = tick;
        int perTick = h3::hooks::TicksPerOriginalTick();
        g_rateScale = float(std::min<uint32_t>(ticks, uint32_t(perTick))) / float(perTick);
    }
    // H3_LOG_EXPOSURE=1 (testing): updates per game tick.
    static const bool log = h3::EnvFlag("H3_LOG_EXPOSURE");
    if (log)
    {
        static uint32_t calls = 0, firstTick = 0;
        if (calls++ == 0 || tick < firstTick)
            firstTick = tick, calls = 1;
        if (calls % 600 == 0)
            fprintf(stderr, "[exposure] %u updates over %u ticks\n", calls, tick - firstTick);
    }
    __imp__sub_8216B4B8(ctx, base);
    if (g_patchedRate)
    {
        h3::GuestRef<float>(g_patchedRate) = g_originalRate;
        g_patchedRate = 0;
    }
}

PPC_FUNC_IMPL(__imp__sub_8216BA68);
PPC_FUNC(sub_8216BA68)
{
    if (uint32_t(ctx.lr) == kRisingCall)
        SlowRate(ctx);
    __imp__sub_8216BA68(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_8244DAB8);
PPC_FUNC(sub_8244DAB8)
{
    if (uint32_t(ctx.lr) == kFallingCall)
        SlowRate(ctx);
    __imp__sub_8244DAB8(ctx, base);
}
