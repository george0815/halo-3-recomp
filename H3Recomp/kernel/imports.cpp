#include "imports.h"

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
        static const bool strict = getenv("H3_STRICT_IMPORTS") != nullptr;

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

    uint32_t StackArgument(const PPCContext& ctx, uint32_t index)
    {
        return GuestRef<uint32_t>(ctx.r1.u32 + 0x54 + (index - 8) * 8);
    }

    void GuestFatal(const char* what, PPCContext& ctx)
    {
        fprintf(stderr, "[kernel] %s (r3=0x%08X r4=0x%08X lr=0x%08llX), exiting\n", what,
            ctx.r3.u32, ctx.r4.u32, static_cast<unsigned long long>(ctx.lr));
        fflush(stdout);
        _Exit(1);
    }
}
