// Declaring decompiled replacements for recompiled functions (docs/
// ROADMAP.md 6.0, replacement registry).
//
//   H3_REPLACE_INT(820B36B8, structure_bsp_get_x, { ...; return value; })
//   H3_REPLACE_FLOAT(820DA2E8, some_real_getter, { ...; return value; })
//
// define the override of sub_<address>. The body is a lambda (it sees ctx
// for its arguments) returning std::optional: the result (r3, or f1 for
// FLOAT), or std::nullopt when it can't answer natively (no host tag
// cache, an unexpected tag), in which case the original runs. Honors
// H3_ORIGINAL=<name> and H3_CHECK_NATIVE=1 (native.h); with the check the
// game gets the original's result and differences are logged.
//
// Only for functions whose effect is their result: no stores to guest
// memory and no calls with side effects (tools/re/tag_readers.py --list
// --leaf-pure lists candidates). Every replacement is registered
// (Replacements()) for progress reports.

#pragma once

#include "native.h"

#include <ppc_recomp_shared.h>

#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace h3::native
{
    struct Replacement
    {
        uint32_t address;
        const char* name;
    };

    // Every replacement declared with H3_REPLACE_* (in registration order).
    inline std::vector<Replacement>& Replacements()
    {
        static std::vector<Replacement> list;
        return list;
    }

    struct Registration
    {
        Registration(uint32_t address, const char* name) { Replacements().push_back({ address, name }); }
    };

    template <typename Body>
    void ReplaceInt(PPCContext& ctx, uint8_t* base, PPCFunc* original, const char* name, NativeCheck& checker, Body&& body)
    {
        static_cast<void>(base);
        if (UseOriginal(name))
        {
            original(ctx, base);
            return;
        }
        PPCContext entry = ctx;
        std::optional<uint32_t> result = body(entry);
        if (!result)
        {
            original(ctx, base);
            return;
        }
        if (CheckNative())
        {
            original(ctx, base);
            checker.Compare(ctx.r3.u32 == *result, [&] {
                char text[128];
                snprintf(text, sizeof(text), "r3 %08X r4 %08X: native %08X, original %08X", entry.r3.u32, entry.r4.u32, *result,
                    ctx.r3.u32);
                return std::string(text);
            });
            return;  // the original's registers stand
        }
        ctx.r3.u64 = *result;
    }

    // Results compared as the float the original computes (f1 holds it as
    // a double, exactly representable).
    template <typename Body>
    void ReplaceFloat(PPCContext& ctx, uint8_t* base, PPCFunc* original, const char* name, NativeCheck& checker, Body&& body)
    {
        static_cast<void>(base);
        if (UseOriginal(name))
        {
            original(ctx, base);
            return;
        }
        PPCContext entry = ctx;
        std::optional<float> result = body(entry);
        if (!result)
        {
            original(ctx, base);
            return;
        }
        if (CheckNative())
        {
            original(ctx, base);
            float theirs = float(ctx.f1.f64);
            checker.Compare(memcmp(&theirs, &*result, sizeof(float)) == 0, [&] {
                char text[128];
                snprintf(text, sizeof(text), "r3 %08X f1 %g: native %.9g, original %.9g", entry.r3.u32, entry.f1.f64, *result,
                    theirs);
                return std::string(text);
            });
            return;
        }
        ctx.f1.f64 = double(*result);
    }
}

#define H3_REPLACE_IMPL(address, name, kind, ...)                                                                  \
    PPC_FUNC_IMPL(__imp__sub_##address);                                                                           \
    static ::h3::native::Registration h3_registration_##address(0x##address, #name);                               \
    PPC_FUNC(sub_##address)                                                                                        \
    {                                                                                                              \
        static ::h3::native::NativeCheck checker(#name);                                                           \
        ::h3::native::kind(ctx, base, __imp__sub_##address, #name, checker, [&](PPCContext & ctx) __VA_ARGS__);    \
    }

#define H3_REPLACE_INT(address, name, ...) H3_REPLACE_IMPL(address, name, ReplaceInt, -> std::optional<uint32_t> __VA_ARGS__)
#define H3_REPLACE_FLOAT(address, name, ...) H3_REPLACE_IMPL(address, name, ReplaceFloat, -> std::optional<float> __VA_ARGS__)
