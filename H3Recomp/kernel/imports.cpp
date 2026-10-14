// Implementation of the shared export helpers in imports.h, plus the handler
// the recompiled code calls when a jump table is indexed out of range.

#include "imports.h"
#include "config/env.h"

#include <ppc_recomp_shared.h>

#include "../memory.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

namespace h3::kernel
{
    void UnimplementedImport(const char* name, PPCContext& ctx)
    {
        static std::mutex mutex;
        static std::unordered_set<std::string> reported;
        static const bool strict = EnvFlag("H3_STRICT_IMPORTS");

        {
            std::lock_guard lock(mutex);
            if (reported.insert(name).second)
                fprintf(stderr, "[kernel] unimplemented import: %s (lr=0x%08llX)\n", name,
                    static_cast<unsigned long long>(ctx.lr));
        }

        if (strict)
            abort();

        // Nt*/Io* return an NTSTATUS. Reporting success for a call that did
        // nothing sends the game down paths that read output it never got
        // (a garbage NtQueryDirectoryFile entry once overran a lock), so
        // fail those. Everything else gets 0.
        std::string_view view(name);
        bool ntStatus = view.starts_with("Nt") || view.starts_with("Io");
        ctx.r3.u64 = ntStatus ? kStatusNotImplemented : 0;
    }

    // PowerPC ABI: the caller's parameter save area starts at r1 + 0x50 and
    // holds one 8-byte slot per argument (the first eight are passed in
    // registers but still have slots). A 32-bit argument sits in the low,
    // big-endian half of its slot, hence + 4.
    constexpr uint32_t kParameterSaveArea = 0x50;
    constexpr uint32_t kRegisterArguments = 8;
    constexpr uint32_t kArgumentSlotSize = 8;

    uint32_t StackArgument(const PPCContext& ctx, uint32_t index)
    {
        return GuestRef<uint32_t>(ctx.r1.u32 + kParameterSaveArea + 4 + (index - kRegisterArguments) * kArgumentSlotSize);
    }

    void GuestFatal(const char* what, PPCContext& ctx)
    {
        fprintf(stderr, "[kernel] %s (r3=0x%08X r4=0x%08X lr=0x%08llX), exiting\n", what,
            ctx.r3.u32, ctx.r4.u32, static_cast<unsigned long long>(ctx.lr));
        fflush(stdout);
        _Exit(1);
    }
}

// See H3RecompLib/debug/switch_check.h.
void H3SwitchUnreachable(uint32_t address, uint32_t value)
{
    fprintf(stderr, "[recomp] jump table at 0x%08X indexed out of range (%u)\n", address, value);
    fflush(stderr);
    abort();
}

// See H3RecompLib/debug/switch_check.h.
void H3IndirectCallToNull(uint32_t target)
{
    // How much of the lookup table around the entry is zero: a wiped range
    // points at whatever overwrote it.
    auto entry = [](uint64_t address) { return *reinterpret_cast<uint64_t*>(h3::g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (address - PPC_CODE_BASE) * 2); };
    uint64_t low = target & ~3u, high = target & ~3u;
    while (low > PPC_CODE_BASE && entry(low - 4) == 0)
        low -= 4;
    while (high + 4 < PPC_CODE_BASE + PPC_CODE_SIZE && entry(high + 4) == 0)
        high += 4;
    fprintf(stderr, "[recomp] indirect call to 0x%08X: its function lookup entry is null; entries for 0x%08llX-0x%08llX are zero "
        "(the table, at guest 0x%08llX-, was overwritten)\n", target, (unsigned long long)low, (unsigned long long)high,
        (unsigned long long)(PPC_IMAGE_BASE + PPC_IMAGE_SIZE));
    fflush(stderr);
    abort();
}
