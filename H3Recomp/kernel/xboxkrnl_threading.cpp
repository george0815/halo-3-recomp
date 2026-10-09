// xboxkrnl threading exports: TLS, critical sections, critical regions.
// Reference: Xenia's xboxkrnl_threading.cc and xboxkrnl_rtl.cc.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "thread.h"
#include "../memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace h3;
using namespace h3::kernel;

// DWORD KeTlsAlloc()
PPC_FUNC(__imp__KeTlsAlloc)
{
    ctx.r3.u64 = TlsAlloc();
}

// BOOL KeTlsFree(DWORD slot)
PPC_FUNC(__imp__KeTlsFree)
{
    ctx.r3.u64 = TlsFree(ctx.r3.u32);
}

// PVOID KeTlsGetValue(DWORD slot)
PPC_FUNC(__imp__KeTlsGetValue)
{
    ctx.r3.u64 = GuestRef<uint32_t>(CurrentThread()->tlsSlots + ctx.r3.u32 * 4).get();
}

// BOOL KeTlsSetValue(DWORD slot, PVOID value)
PPC_FUNC(__imp__KeTlsSetValue)
{
    GuestRef<uint32_t>(CurrentThread()->tlsSlots + ctx.r3.u32 * 4) = ctx.r4.u32;
    ctx.r3.u64 = 1;
}

// Critical regions only block kernel APCs, which the runtime doesn't deliver.
PPC_FUNC(__imp__KeEnterCriticalRegion)
{
}

PPC_FUNC(__imp__KeLeaveCriticalRegion)
{
}

// RTL_CRITICAL_SECTION (0x1C bytes, big-endian):
//   0x00 DISPATCHER_HEADER: type, absolute (spin count / 256), size,
//        inserted, signal state, wait list (flink, blink)
//   0x10 lock count       (-1 when free)
//   0x14 recursion count
//   0x18 owning thread    (KTHREAD pointer, 0 when free)
//
// For now this is a yielding spin lock: lock count is -1 or 0 and waiters
// aren't counted. That keeps every field consistent for guest code that
// peeks at them, and it's correct while the guest only has one thread.
// Real blocking waits come with the dispatcher object work.
namespace
{
    constexpr uint32_t kCsLockCount = 0x10;
    constexpr uint32_t kCsRecursionCount = 0x14;
    constexpr uint32_t kCsOwningThread = 0x18;

    std::atomic_ref<uint32_t> GuestAtomic(uint32_t address)
    {
        return std::atomic_ref<uint32_t>(*GuestPtr<uint32_t>(address));
    }
}

// Also used for the VdHSIOCalibrationLock kernel variable.
void h3::kernel::InitCriticalSection(uint32_t cs, uint32_t spinCount)
{
    *GuestPtr<uint8_t>(cs + 0) = 1;  // EventSynchronizationObject
    *GuestPtr<uint8_t>(cs + 1) = uint8_t(std::min((spinCount + 255) >> 8, 255u));
    *GuestPtr<uint8_t>(cs + 2) = 0;
    *GuestPtr<uint8_t>(cs + 3) = 0;
    GuestRef<uint32_t>(cs + 4) = 0;
    GuestRef<uint32_t>(cs + 8) = cs + 8;  // empty wait list points at itself
    GuestRef<uint32_t>(cs + 12) = cs + 8;
    GuestRef<int32_t>(cs + kCsLockCount) = -1;
    GuestRef<int32_t>(cs + kCsRecursionCount) = 0;
    GuestRef<uint32_t>(cs + kCsOwningThread) = 0;
}

namespace
{
    bool TryEnterCriticalSection(uint32_t cs, uint32_t thread)
    {
        if (GuestRef<uint32_t>(cs + kCsOwningThread) == thread)
        {
            auto& recursion = GuestRef<int32_t>(cs + kCsRecursionCount);
            recursion = recursion + 1;
            return true;
        }

        uint32_t expected = ByteSwap(uint32_t(-1));
        if (GuestAtomic(cs + kCsLockCount).compare_exchange_strong(expected, 0))
        {
            GuestRef<uint32_t>(cs + kCsOwningThread) = thread;
            GuestRef<int32_t>(cs + kCsRecursionCount) = 1;
            return true;
        }
        return false;
    }
}

// void RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION cs)
PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    InitCriticalSection(ctx.r3.u32, 0);
}

// NTSTATUS RtlInitializeCriticalSectionAndSpinCount(PRTL_CRITICAL_SECTION cs, ULONG spinCount)
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    InitCriticalSection(ctx.r3.u32, ctx.r4.u32);
    ctx.r3.u64 = kStatusSuccess;
}

// void RtlEnterCriticalSection(PRTL_CRITICAL_SECTION cs)
PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    uint32_t thread = CurrentThread()->kthread;
    if (TryEnterCriticalSection(cs, thread))
        return;

    // Report a lock that stays held for seconds: on hardware that's almost
    // always a deadlock, and here it's often a kernel bug.
    auto start = std::chrono::steady_clock::now();
    bool reported = false;
    for (uint32_t spins = 1; !TryEnterCriticalSection(cs, thread); spins++)
    {
        if (!reported && (spins & 0x3FF) == 0 && std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
        {
            fprintf(stderr, "[kernel] thread %08X stuck entering critical section %08X (owner %08X, recursion %d, lr=0x%08X)\n",
                thread, cs, uint32_t(GuestRef<uint32_t>(cs + kCsOwningThread)),
                int32_t(GuestRef<int32_t>(cs + kCsRecursionCount)), uint32_t(ctx.lr));
            reported = true;
        }
        std::this_thread::yield();
    }
}

// BOOL RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION cs)
PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = TryEnterCriticalSection(ctx.r3.u32, CurrentThread()->kthread);
}

// void RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION cs)
PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    uint32_t cs = ctx.r3.u32;
    auto& recursion = GuestRef<int32_t>(cs + kCsRecursionCount);
    recursion = recursion - 1;
    if (recursion != 0)
        return;

    GuestRef<uint32_t>(cs + kCsOwningThread) = 0;
    GuestAtomic(cs + kCsLockCount).store(ByteSwap(uint32_t(-1)));
}
