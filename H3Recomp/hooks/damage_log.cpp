// H3_LOG_DAMAGE=1 (testing damage at 60 ticks per second): logs every call
// of object_cause_damage with the tick, the damage effect tag and the
// object damaged, so the hits per second of one source (Guilty Spark's
// laser on The Halo felt like double damage at 60 Hz) can be counted and
// compared with its rounds per second (H3_LOG_BARRELS).
//
// sub_820F43C8 is object_cause_damage(r3 damage data, r4 object, r5 node,
// r6 region, r7 material, r8 normal): the damage data starts with the
// damage effect tag's datum (damage_data_new, sub_820F4318, fills it in).
// The first time each damage effect is seen, the damage data's first
// kDumpWords words are logged too.

#include "cache/cache_file.h"
#include "config/env.h"
#include "memory.h"
#include "tags/tags.h"

#include <ppc_recomp_shared.h>

#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

namespace
{
    // Game time globals (TLS slot kTlsGameTime): the current tick (u32).
    constexpr uint32_t kTlsGameTime = 188;
    constexpr uint32_t kGameTimeTick = 12;

    // Damage data words dumped for each damage effect's first hit.
    constexpr uint32_t kDumpWords = 24;
    constexpr uint32_t kSlotMask = 0xFFFF;

    // The damage effect tag's name, or its datum when no cache file is known.
    std::string TagName(uint32_t datum)
    {
        const auto* file = h3::native::tags::LoadedCacheFile();
        if (file == nullptr)
        {
            char hex[16];
            snprintf(hex, sizeof(hex), "%08X", datum);
            return hex;
        }
        return file->TagName(datum & kSlotMask);
    }
}

// object_cause_damage(r3 damage data, r4 object index, r5 node, ...).
PPC_FUNC_IMPL(__imp__sub_820F43C8);
PPC_FUNC(sub_820F43C8)
{
    static const bool enabled = h3::EnvFlag("H3_LOG_DAMAGE");
    if (enabled)
    {
        static std::set<uint32_t> dumped;
        uint32_t data = ctx.r3.u32;
        uint32_t effect = h3::GuestRef<uint32_t>(data);
        uint32_t gameTime = h3::GuestRef<uint32_t>(uint32_t(h3::GuestRef<uint32_t>(ctx.r13.u32)) + kTlsGameTime);
        uint32_t tick = h3::GuestRef<uint32_t>(gameTime + kGameTimeTick);
        fprintf(stderr, "[damage] tick %u: %s (%08X) -> object %08X node %d\n", tick, TagName(effect).c_str(), effect,
            ctx.r4.u32, int(ctx.r5.s16));
        if (dumped.insert(effect).second)
        {
            fprintf(stderr, "[damage]   data:");
            for (uint32_t i = 0; i < kDumpWords; i++)
                fprintf(stderr, " %08X", uint32_t(h3::GuestRef<uint32_t>(data + i * 4)));
            fprintf(stderr, "\n");
        }
    }
    __imp__sub_820F43C8(ctx, base);
}
