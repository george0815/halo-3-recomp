#pragma once

// Debug aid: with -DH3_TRACE_MODULE_CALLS=ON, every function in the
// recompiled DLLs logs its entry (r3-r5) and return value (r3) to stderr.
// Force-included into the module libraries before ppc_context.h.

#include <cstdint>
#include <cstdio>

struct H3TraceScope
{
    static inline thread_local int depth = 0;
    const char* name;
    const uint64_t* r3;

    template <typename Context>
    H3TraceScope(const char* name, Context& ctx) : name(name), r3(&ctx.r3.u64)
    {
        fprintf(stderr, "%*s> %s(%08X, %08X, %08X)\n", depth * 2, "", name + 7, uint32_t(ctx.r3.u64),
            uint32_t(ctx.r4.u64), uint32_t(ctx.r5.u64));
        depth++;
    }

    ~H3TraceScope()
    {
        depth--;
        fprintf(stderr, "%*s< %s = %08X\n", depth * 2, "", name + 7, uint32_t(*r3));
    }
};

#define PPC_FUNC_PROLOGUE() H3TraceScope h3TraceScope(__func__, ctx)
