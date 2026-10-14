#pragma once

// Force-included into the recompiled game code before ppc_context.h:
//  - a jump table index outside its table aborts with the table's address
//    and the index, instead of being undefined behaviour (which shows up
//    later as corrupted host stacks);
//  - an indirect call whose lookup table entry is null aborts with the
//    target and how far the table around it is zeroed. Every slot without a
//    function holds a reporting stub (kernel/modules.cpp), so a null entry
//    means something overwrote the table (it lives in guest address space
//    after the image: a guest memset with a negative size once wiped it).
// Implemented in H3Recomp/kernel/imports.cpp.

#include <cstdint>

[[noreturn]] void H3SwitchUnreachable(uint32_t address, uint32_t value);

#define PPC_SWITCH_UNREACHABLE(address, value) H3SwitchUnreachable(address, value)

[[noreturn]] void H3IndirectCallToNull(uint32_t target);

#define PPC_CALL_INDIRECT_FUNC(x)                       \
    do                                                  \
    {                                                   \
        PPCFunc* h3_target = PPC_LOOKUP_FUNC(base, x);  \
        if (!h3_target)                                 \
            H3IndirectCallToNull(uint32_t(x));          \
        h3_target(ctx, base);                           \
    } while (0)
