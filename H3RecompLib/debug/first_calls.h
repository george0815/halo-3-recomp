#pragma once

// Debug aid: with -DH3_TRACE_FIRST_CALLS=ON, every function in the
// recompiled executable reports its first call to h3FirstCall (in the
// runtime, H3Recomp/debug/first_calls.cpp), which appends it to
// first_calls.txt with the frame number; while H3_TRACE_WINDOW is open,
// every call goes to h3WindowCall too. Costs a relaxed load or two per call.
// Force-included into the executable's library before ppc_context.h.

#include <atomic>

extern "C" void h3FirstCall(const char* name);
extern "C" void h3WindowCall(const char* name);
extern std::atomic<bool> g_h3TraceWindowOpen;

#define PPC_FUNC_PROLOGUE()                                                                          \
    do                                                                                               \
    {                                                                                                \
        static std::atomic<bool> h3Seen{ false };                                                    \
        if (!h3Seen.load(std::memory_order_relaxed) && !h3Seen.exchange(true))                       \
            h3FirstCall(__func__);                                                                   \
        if (g_h3TraceWindowOpen.load(std::memory_order_relaxed))                                     \
            h3WindowCall(__func__);                                                                  \
    } while (0)
