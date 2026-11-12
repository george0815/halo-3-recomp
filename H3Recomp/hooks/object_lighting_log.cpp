// H3_LOG_OBJLIGHT=<first tick>[-<last tick>] (debugging): logs every call of
// the object lighting "geometry sampler" (sub_8215D3A0; MCC tag_test
// FUN_140680f80 in render_objects_static_lighting.cpp) while the level's game
// tick is in that range: its return code, arguments and the lighting it
// produced (SH coefficients per channel, sky light, dominant light). The
// sampler is what fills p_lighting_constant_0-9 (ps c1-c10) and the dominant
// light (c11, c13) for objects without cinematic lighting.
#include "config/env.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace
{
    constexpr uint32_t kTlsGameTime = 188;
    constexpr uint32_t kGameTimeTick = 12;

    uint32_t GameTick(const PPCContext& ctx)
    {
        uint32_t gameTime = h3::GuestRef<uint32_t>(uint32_t(h3::GuestRef<uint32_t>(ctx.r13.u32)) + kTlsGameTime);
        return gameTime ? uint32_t(h3::GuestRef<uint32_t>(gameTime + kGameTimeTick)) : 0u;
    }

    float GuestFloat(uint32_t address)
    {
        uint32_t bits = h3::GuestRef<uint32_t>(address);
        float value;
        memcpy(&value, &bits, 4);
        return value;
    }

    struct Range
    {
        uint32_t first = 0, last = 0;
        bool enabled = false;
    };

    const Range& LogRange()
    {
        static const Range range = [] {
            Range r;
            if (const char* spec = h3::EnvString("H3_LOG_OBJLIGHT"))
            {
                char* end;
                r.first = uint32_t(strtoul(spec, &end, 10));
                r.last = *end == '-' ? uint32_t(strtoul(end + 1, nullptr, 10)) : r.first + 120;
                r.enabled = true;
            }
            return r;
        }();
        return range;
    }
}

PPC_FUNC_IMPL(__imp__sub_8215D3A0);
PPC_FUNC(sub_8215D3A0)
{
    const Range& range = LogRange();
    uint32_t args[8] = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };
    uint32_t caller = uint32_t(ctx.lr);
    __imp__sub_8215D3A0(ctx, base);
    if (!range.enabled)
        return;
    uint32_t tick = GameTick(ctx);
    if (tick < range.first || tick > range.last)
        return;
    static uint32_t lines = 0;
    if (lines++ > 3000)
        return;
    fprintf(stderr, "[objlight] tick %u lr %08X ret %u args", tick, caller, ctx.r3.u32);
    for (uint32_t a : args)
        fprintf(stderr, " %08X", a);
    fprintf(stderr, "\n");
    // Guest pointers among the arguments: their first floats (a position,
    // the lighting output).
    for (uint32_t i = 0; i < 8; i++)
    {
        uint32_t a = args[i];
        if (a < 0x40000000 || a >= 0xC0000000 || (a & 3))
            continue;
        fprintf(stderr, "[objlight]   r%u:", 3 + i);
        uint32_t words = i == 6 ? 0x1F8 / 4 : 8;
        for (uint32_t w = 0; w < words; w++)
        {
            float v = GuestFloat(a + w * 4);
            if (v != 0.0f || w < 4)
                fprintf(stderr, " [%X]%g", w * 4, v);
        }
        fprintf(stderr, "\n");
    }
}

// sub_8215CA08: samples an object's lighting into its render state (MCC
// FUN_140b073b0). Logged with the state memory around the output pointer,
// to see where the sample lands and what the draws read.
PPC_FUNC_IMPL(__imp__sub_8215CA08);
PPC_FUNC(sub_8215CA08)
{
    const Range& range = LogRange();
    uint32_t args[6] = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32 };
    __imp__sub_8215CA08(ctx, base);
    if (!range.enabled)
        return;
    uint32_t tick = GameTick(ctx);
    if (tick < range.first || tick > range.last)
        return;
    static uint32_t lines = 0;
    if (lines++ > 600)
        return;
    fprintf(stderr, "[objstate] tick %u ret %u args %08X %08X %08X %08X %08X %08X\n", tick, ctx.r3.u32, args[0], args[1], args[2],
        args[3], args[4], args[5]);
    for (uint32_t i = 0; i < 6; i++)
    {
        uint32_t a = args[i];
        if (a < 0x40000000 || a >= 0xC0000000 || (a & 3))
            continue;
        fprintf(stderr, "[objstate]   r%u:", 3 + i);
        for (int32_t w = -16; w < 0x500 / 4; w++)
        {
            float v = GuestFloat(uint32_t(int32_t(a) + w * 4));
            if (v != 0.0f && v == v && v > -1e6f && v < 1e6f)
                fprintf(stderr, " [%X]%g", uint32_t(w * 4) & 0xFFFF, v);
        }
        fprintf(stderr, "\n");
    }
}

// sub_822B91E0: cinematic_light_object (object, marker, palette entry, camera
// point). Logged to see whether the cutscene lights objects this way.
PPC_FUNC_IMPL(__imp__sub_822B91E0);
PPC_FUNC(sub_822B91E0)
{
    uint32_t args[4] = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32 };
    __imp__sub_822B91E0(ctx, base);
    if (!LogRange().enabled)
        return;
    static uint32_t lines = 0;
    if (lines++ < 200)
        fprintf(stderr, "[cinlight] tick %u object %08X marker %08X palette %d camera %d\n", GameTick(ctx), args[0], args[1],
            int32_t(args[2]), int32_t(args[3]));
}

// sub_82300828: cinematic lighting SH state computation (MCC FUN_14071d240).
PPC_FUNC_IMPL(__imp__sub_82300828);
PPC_FUNC(sub_82300828)
{
    uint32_t state = ctx.r3.u32;
    __imp__sub_82300828(ctx, base);
    if (!LogRange().enabled)
        return;
    static uint32_t lines = 0;
    if (lines++ < 200)
    {
        fprintf(stderr, "[cinstate] tick %u state %08X:", GameTick(ctx), state);
        for (uint32_t w = 0; w < 0x2C4 / 4; w++)
        {
            float v = GuestFloat(state + w * 4);
            if (v != 0.0f && v == v && v > -1e6f && v < 1e6f)
                fprintf(stderr, " [%X]%g", w * 4, v);
        }
        fprintf(stderr, "\n");
    }
}

// sub_822B93B0: cinematic_lights_initialize_for_shot; sub_822D5D48: its
// script handler.
PPC_FUNC_IMPL(__imp__sub_822B93B0);
PPC_FUNC(sub_822B93B0)
{
    uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32;
    if (LogRange().enabled)
    {
        timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        fprintf(stderr, "[cinshot] tick %u init_for_shot r3 %08X r4 %08X lr %08X wall %.3f\n", GameTick(ctx), r3, r4, uint32_t(ctx.lr),
            double(now.tv_sec) + now.tv_nsec * 1e-9);
    }
    __imp__sub_822B93B0(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_822D5D48);
PPC_FUNC(sub_822D5D48)
{
    if (LogRange().enabled)
        fprintf(stderr, "[cinshot] tick %u hs handler r3 %08X r4 %08X r5 %08X\n", GameTick(ctx), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
    __imp__sub_822D5D48(ctx, base);
}

// H3_CHECK_TRIG=1 (debugging): compares the game's sin (sub_82081358) and
// cos (sub_82081428, checked from hooks/player_effects.cpp's hook) with the
// host's on every call; logs each new worst error.
namespace h3::hooks
{
    bool CheckTrigEnabled()
    {
        static const bool check = h3::EnvFlag("H3_CHECK_TRIG");
        return check;
    }

    void CheckTrig(bool cosine, double x, double got)
    {
        static double worst[2] = {};
        double want = cosine ? std::cos(x) : std::sin(x);
        double err = std::fabs(got - want);
        if (err > worst[cosine] && err > 1e-5)
        {
            worst[cosine] = err;
            fprintf(stderr, "[trig] %s(%.9g) = %.9g, host %.9g (error %.3g)\n", cosine ? "cos" : "sin", x, got, want, err);
        }
    }
}

PPC_FUNC_IMPL(__imp__sub_82081358);
PPC_FUNC(sub_82081358)
{
    double x = ctx.f1.f64;
    __imp__sub_82081358(ctx, base);
    if (h3::hooks::CheckTrigEnabled())
        h3::hooks::CheckTrig(false, x, ctx.f1.f64);
}

// sub_82300E60 (MCC FUN_14053f300): places one cinematic dynamic light
// (r3 = the lighting tag's dynamic light entry: flags, yaw, pitch, distance,
// light tag at +28; r4 = the object's position; r5 = the shot's camera
// frame matrix; r6 = object; r7 = marker). sub_823336A0: creates the light
// (r6 = position, r7 = forward, r8 = up). Both logged with H3_LOG_OBJLIGHT.
namespace
{
    void LogFloats(const char* label, uint32_t address, uint32_t count)
    {
        fprintf(stderr, " %s", label);
        for (uint32_t i = 0; i < count; i++)
            fprintf(stderr, " %g", GuestFloat(address + i * 4));
    }
}

PPC_FUNC_IMPL(__imp__sub_82300E60);
PPC_FUNC(sub_82300E60)
{
    if (LogRange().enabled)
    {
        uint32_t entry = ctx.r3.u32;
        fprintf(stderr, "[cinplace] tick %u entry %08X flags %08X object %08X marker %08X", GameTick(ctx), entry,
            uint32_t(h3::GuestRef<uint32_t>(entry)), ctx.r6.u32, ctx.r7.u32);
        LogFloats("angles/dist", entry + 4, 3);
        fprintf(stderr, " light %08X", uint32_t(h3::GuestRef<uint32_t>(entry + 28)));
        LogFloats("objpos", ctx.r4.u32, 3);
        LogFloats("matrix", ctx.r5.u32, 13);
        fprintf(stderr, "\n");
    }
    __imp__sub_82300E60(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_823336A0);
PPC_FUNC(sub_823336A0)
{
    if (LogRange().enabled)
    {
        fprintf(stderr, "[lightnew] tick %u tag %08X object %08X marker %08X lr %08X", GameTick(ctx), ctx.r3.u32, ctx.r4.u32,
            ctx.r5.u32, uint32_t(ctx.lr));
        LogFloats("pos", ctx.r6.u32, 3);
        LogFloats("fwd", ctx.r7.u32, 3);
        LogFloats("up", ctx.r8.u32, 3);
        fprintf(stderr, "\n");
    }
    __imp__sub_823336A0(ctx, base);
}

// H3_LOG_ATOC=<calls> (debugging): logs the first <calls> calls of
// c_rasterizer::set_alpha_to_coverage (sub_8216FD28; r3 = enable) and of
// set_alpha_blend_mode (sub_82168258; r3 = mode) with the return addresses
// of three stack frames, to see which render path draws alpha-tested
// geometry with opaque blending.
namespace
{
    uint32_t AtocLogLimit()
    {
        static const uint32_t limit = [] {
            const char* text = h3::EnvString("H3_LOG_ATOC");
            return text ? uint32_t(strtoul(text, nullptr, 10)) : 0u;
        }();
        return limit;
    }

    void LogCallChain(const char* label, const PPCContext& ctx)
    {
        static uint32_t lines = 0;
        if (lines++ >= AtocLogLimit())
            return;
        fprintf(stderr, "[atoc] %s r3 %u r4 %08X lr %08X", label, ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
        uint32_t frame = ctx.r1.u32;
        for (int i = 0; i < 3 && frame >= 0x40000000 && frame < 0xC0000000; i++)
        {
            frame = h3::GuestRef<uint32_t>(frame);  // back chain: the caller's frame
            if (frame < 0x40000000 || frame >= 0xC0000000)
                break;
            fprintf(stderr, " %08X", uint32_t(h3::GuestRef<uint32_t>(frame - 8)));
        }
        uint32_t states = h3::GuestRef<uint32_t>(uint32_t(h3::GuestRef<uint32_t>(ctx.r13.u32)) + 284);
        fprintf(stderr, " gate %u state0 %u\n", uint32_t(uint8_t(h3::GuestRef<uint8_t>(0x8272CB02))),
            states ? uint32_t(uint8_t(h3::GuestRef<uint8_t>(states))) : 255u);
    }
}

PPC_FUNC_IMPL(__imp__sub_8216FD28);
PPC_FUNC(sub_8216FD28)
{
    // Only the calls that turn blending off: enable, "keep blending" flag
    // 828C482D clear and the alpha blend cache 8281DAB8 set.
    if (AtocLogLimit() && (ctx.r3.u32 & 0xFF) && !uint8_t(h3::GuestRef<uint8_t>(0x828C482D)) &&
        uint32_t(h3::GuestRef<uint32_t>(0x8281DAB8)))
        LogCallChain("set_atoc_noblend", ctx);
    __imp__sub_8216FD28(ctx, base);
}

PPC_FUNC_IMPL(__imp__sub_82168258);
PPC_FUNC(sub_82168258)
{
    __imp__sub_82168258(ctx, base);
}

// sub_8211FC98: unit_set_emotion (unit r3, emotion r4 0-8, weight f1,
// transition seconds f2). H3_LOG_EMOTION=1 logs the calls;
// H3_NO_EMOTION=1 drops them (to see a face without its emotion overlay).
PPC_FUNC_IMPL(__imp__sub_8211FC98);
PPC_FUNC(sub_8211FC98)
{
    static const bool log = h3::EnvFlag("H3_LOG_EMOTION");
    static const bool drop = h3::EnvFlag("H3_NO_EMOTION");
    if (log)
        fprintf(stderr, "[emotion] tick %u unit %08X emotion %u weight %g seconds %g lr %08X\n", GameTick(ctx), ctx.r3.u32, ctx.r4.u32,
            ctx.f1.f64, ctx.f2.f64, uint32_t(ctx.lr));
    if (drop)
        return;
    __imp__sub_8211FC98(ctx, base);
}
