#include "imports.h"

#include <ppc_recomp_shared.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
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

        ctx.r3.u64 = 0;
    }
}
