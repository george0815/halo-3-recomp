#pragma once

// Function-level call recording (docs/ROADMAP.md 6.1): with
// -DH3_RECORD_CALLS=ON, every function in the recompiled executable hands
// its entry registers to h3RecordCall (H3Recomp/debug/call_record.cpp)
// while a recording window is open. Outside the windows a call costs one
// relaxed load. Force-included into the executable's library before
// ppc_context.h.
//
// The prologue runs in the recompiled body (__imp__sub_*), so a decompiled
// replacement records only when its original runs: recording runs set
// H3_ORIGINAL=* (tools/films/record_calls.sh).

#include <atomic>
#include <cstdint>

extern "C" void h3RecordCall(uint32_t* address, const char* name, const void* ctx);
extern std::atomic<bool> g_h3RecordCallsOpen;

#define PPC_FUNC_PROLOGUE()                                                                          \
    do                                                                                               \
    {                                                                                                \
        static uint32_t h3Address = 0;                                                               \
        if (g_h3RecordCallsOpen.load(std::memory_order_relaxed))                                     \
            h3RecordCall(&h3Address, __func__, &ctx);                                                \
    } while (0)
