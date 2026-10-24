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
// --leaf-pure lists candidates). Functions that store to guest memory (but
// call nothing with side effects) use H3_REPLACE_STORES, which records the
// stores (GuestStores) so the check can compare them too. Every
// replacement is registered (Replacements()) for progress reports.

#pragma once

#include "memory.h"
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

namespace h3::native
{
    // The guest stores a replacement with side effects makes (H3_REPLACE_STORES):
    // recorded rather than written, so the H3_CHECK_NATIVE run can compare
    // them with what the original stored. The body reads guest memory as it
    // was on entry: it must keep values it stores in locals rather than
    // read them back.
    class GuestStores
    {
    public:
        void Store8(uint32_t address, uint8_t value) { m_stores.push_back({ address, 1, value }); }
        void Store16(uint32_t address, uint16_t value) { m_stores.push_back({ address, 2, value }); }
        void Store32(uint32_t address, uint32_t value) { m_stores.push_back({ address, 4, value }); }
        void Store64(uint32_t address, uint64_t value)
        {
            Store32(address, uint32_t(value >> 32));
            Store32(address + 4, uint32_t(value));
        }
        void StoreFloat(uint32_t address, float value)
        {
            uint32_t bits;
            memcpy(&bits, &value, sizeof(bits));
            Store32(address, bits);
        }

        // H3_CHECK_NATIVE: also compare these bytes whole (the memory the
        // original may store to), so a store the native misses shows up too.
        // Copies them now: call before anything is stored.
        void Watch(uint32_t address, uint32_t size)
        {
            const uint8_t* bytes = GuestPtr<uint8_t>(address);
            m_watched.push_back({ address, std::vector<uint8_t>(bytes, bytes + size) });
        }

        // Writes the recorded stores in order (big-endian, as the game does).
        void Apply() const
        {
            for (const Store& s : m_stores)
                Write(s);
        }

        // Whether guest memory now holds every recorded value (the last
        // store to an address wins), and every watched range what it held
        // plus the recorded stores; describes the first mismatch in what.
        bool Match(std::string& what) const
        {
            for (const Watched& w : m_watched)
            {
                std::vector<uint8_t> expected = w.bytes;
                for (const Store& s : m_stores)
                    for (uint32_t i = 0; i < s.size; i++)
                        if (s.address + i >= w.address && s.address + i < w.address + expected.size())
                            expected[s.address + i - w.address] = uint8_t(s.value >> (8 * (s.size - 1 - i)));
                const uint8_t* now = GuestPtr<uint8_t>(w.address);
                for (uint32_t i = 0; i < expected.size(); i++)
                {
                    if (now[i] == expected[i])
                        continue;
                    char text[96];
                    snprintf(text, sizeof(text), "watched %08X+%u: native %02X, original %02X", w.address, i, expected[i], now[i]);
                    what = text;
                    return false;
                }
            }
            for (size_t i = 0; i < m_stores.size(); i++)
            {
                const Store& s = m_stores[i];
                bool overwritten = false;
                for (size_t j = i + 1; j < m_stores.size(); j++)
                    overwritten |= m_stores[j].address == s.address && m_stores[j].size == s.size;
                uint32_t now = Read(s);
                if (overwritten || now == s.value)
                    continue;
                char text[96];
                snprintf(text, sizeof(text), "%08X (%u bytes): native %08X, original %08X", s.address, s.size, s.value, now);
                what = text;
                return false;
            }
            return true;
        }

    private:
        struct Store
        {
            uint32_t address, size, value;
        };
        static void Write(const Store& s)
        {
            if (s.size == 1)
                GuestRef<uint8_t>(s.address) = uint8_t(s.value);
            else if (s.size == 2)
                GuestRef<uint16_t>(s.address) = uint16_t(s.value);
            else
                GuestRef<uint32_t>(s.address) = s.value;
        }
        static uint32_t Read(const Store& s)
        {
            if (s.size == 1)
                return GuestRef<uint8_t>(s.address);
            if (s.size == 2)
                return GuestRef<uint16_t>(s.address);
            return GuestRef<uint32_t>(s.address);
        }
        struct Watched
        {
            uint32_t address;
            std::vector<uint8_t> bytes;
        };
        std::vector<Store> m_stores;
        std::vector<Watched> m_watched;
    };

    // How a replacement with side effects is being run: for the game, or
    // as H3_CHECK_NATIVE's comparison with the original (where it must
    // compute exactly what the original does: behavior changes, such as
    // the 60 Hz content fixes, stay off).
    enum class Run
    {
        Game,
        Check,
    };

    // A replacement that stores to guest memory: body(ctx, stores, run)
    // returns its r3 (std::nullopt: run the original instead) and records
    // its stores. With H3_CHECK_NATIVE the original runs and stands, and
    // both its r3 and its memory are compared with the native's.
    template <typename Body>
    void ReplaceStores(PPCContext& ctx, uint8_t* base, PPCFunc* original, const char* name, NativeCheck& checker, Body&& body)
    {
        if (UseOriginal(name))
        {
            original(ctx, base);
            return;
        }
        PPCContext entry = ctx;
        GuestStores stores;
        bool check = CheckNative();
        std::optional<uint32_t> result = body(entry, stores, check ? Run::Check : Run::Game);
        if (!result)
        {
            original(ctx, base);
            return;
        }
        if (check)
        {
            original(ctx, base);
            std::string what;
            bool same = ctx.r3.u32 == *result && stores.Match(what);
            checker.Compare(same, [&] {
                char text[192];
                snprintf(text, sizeof(text), "r3 %08X r4 %08X: r3 native %08X, original %08X; %s", entry.r3.u32, entry.r4.u32,
                    *result, ctx.r3.u32, what.empty() ? "stores match" : what.c_str());
                return std::string(text);
            });
            return;
        }
        // H3_NATIVE_SHADOW=1 (diagnosis): run the original first for every
        // effect it has besides the native's (its stack frame below the
        // caller's, memory outside the stores), then the native's stores, with
        // the registers as on entry but r3: what differs from H3_CHECK_NATIVE
        // and from plain native runs tells where an effect is missing.
        static const bool shadow = EnvFlag("H3_NATIVE_SHADOW");
        if (shadow)
        {
            original(ctx, base);
            auto fpscr = ctx.fpscr;  // the host's float mode is the original's now
            ctx = entry;
            ctx.fpscr = fpscr;
        }
        stores.Apply();
        ctx.r3.u64 = *result;
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

// A replacement that stores to guest memory (ReplaceStores): the body sees
// ctx, stores (GuestStores) and run (Run), and returns its r3 (void
// functions return their entry r3: the game ignores it).
#define H3_REPLACE_STORES(address, name, ...)                                                                      \
    PPC_FUNC_IMPL(__imp__sub_##address);                                                                           \
    static ::h3::native::Registration h3_registration_##address(0x##address, #name);                               \
    PPC_FUNC(sub_##address)                                                                                        \
    {                                                                                                              \
        static ::h3::native::NativeCheck checker(#name);                                                           \
        ::h3::native::ReplaceStores(ctx, base, __imp__sub_##address, #name, checker,                               \
            [&](PPCContext & ctx, ::h3::native::GuestStores & stores, ::h3::native::Run run)                       \
                -> std::optional<uint32_t> __VA_ARGS__);                                                           \
    }
