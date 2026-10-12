// xboxkrnl threading exports: TLS, critical sections, critical regions,
// interlocked singly linked lists, spin locks and IRQL.
// Reference: Xenia's xboxkrnl_threading.cc and xboxkrnl_rtl.cc.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "config/env.h"
#include "objects.h"
#include "thread.h"
#include "../memory.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <mutex>
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
// Lock count is -1 or 0 and waiters aren't counted in the guest structure,
// which keeps every field consistent for guest code that peeks at them.
// Contended entries spin briefly, then sleep on a host condition variable
// (hashed by address) that the leaving thread signals.
namespace
{
    constexpr uint32_t kCsLockCount = 0x10;
    constexpr uint32_t kCsRecursionCount = 0x14;
    constexpr uint32_t kCsOwningThread = 0x18;

    struct CsWaitBucket
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::atomic<uint32_t> waiters{ 0 };
    };

    // The wait bucket for a guest critical section. Contended sections block on a host
    // condition variable; sections hash into a fixed set of buckets rather than each
    // getting its own.
    CsWaitBucket& WaitBucket(uint32_t cs)
    {
        static CsWaitBucket buckets[64];
        return buckets[(cs >> 3) % 64];
    }

    // Atomic access to a 32-bit word of guest memory (stored big-endian; callers
    // compare against byte-swapped values).
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
    // Takes the critical section for `thread` if it's free or already owned by it
    // (recursion). Returns false if another thread owns it.
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
    SetThreadWait(thread, "critical section", cs, uint32_t(GuestRef<uint32_t>(cs + kCsOwningThread)), uint32_t(ctx.lr));
    for (uint32_t spins = 1; spins < 64; spins++)
    {
        if (TryEnterCriticalSection(cs, thread))
        {
            ClearThreadWait(thread);
            return;
        }
        std::this_thread::yield();
    }

    CsWaitBucket& bucket = WaitBucket(cs);
    std::unique_lock lock(bucket.mutex);
    bucket.waiters++;
    while (!TryEnterCriticalSection(cs, thread))
    {
        bucket.condition.wait_for(lock, std::chrono::milliseconds(50));
        if (!reported && std::chrono::steady_clock::now() - start > std::chrono::seconds(2))
        {
            fprintf(stderr, "[kernel] thread %08X stuck entering critical section %08X (owner %08X, recursion %d, lr=0x%08X)\n",
                thread, cs, uint32_t(GuestRef<uint32_t>(cs + kCsOwningThread)),
                int32_t(GuestRef<int32_t>(cs + kCsRecursionCount)), uint32_t(ctx.lr));
            DumpThreadWaits();
            DumpHostBacktrace(uint32_t(GuestRef<uint32_t>(cs + kCsOwningThread)));
            static const bool trapOnStuck = EnvFlag("H3_TRAP_ON_STUCK");
            if (trapOnStuck)  // debugging: stop in an attached debugger
                raise(SIGTRAP);
            reported = true;
        }
    }
    bucket.waiters--;
    lock.unlock();
    ClearThreadWait(thread);
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

    CsWaitBucket& bucket = WaitBucket(cs);
    if (bucket.waiters.load() != 0)
    {
        std::lock_guard lock(bucket.mutex);
        bucket.condition.notify_all();
    }
}

// SLIST_HEADER (8 bytes, big-endian): next entry (0 when empty), depth,
// sequence. Read as one big-endian 64-bit word: next in the high half, depth
// in bits 16-31, sequence in bits 0-15. Titles push inline with ldarx/stdcx.
// on the whole word, so pops and flushes must swap the same 64 bits.
// SINGLE_LIST_ENTRY is just the next pointer.
namespace
{
    // Atomic access to a guest SLIST_HEADER (8 bytes, swapped as one 64-bit value).
    std::atomic_ref<uint64_t> SListHeader(uint32_t address)
    {
        return std::atomic_ref<uint64_t>(*GuestPtr<uint64_t>(address));
    }

    uint64_t SListNext(uint64_t header) { return header >> 32; }
    uint64_t SListDepth(uint64_t header) { return (header >> 16) & 0xFFFF; }
    uint64_t SListSequence(uint64_t header) { return header & 0xFFFF; }

    // Packs an SLIST_HEADER: next entry pointer, depth and sequence number.
    uint64_t MakeSListHeader(uint64_t next, uint64_t depth, uint64_t sequence)
    {
        return (next << 32) | ((depth & 0xFFFF) << 16) | (sequence & 0xFFFF);
    }
}

// PSINGLE_LIST_ENTRY InterlockedPopEntrySList(PSLIST_HEADER list)
PPC_FUNC(__imp__InterlockedPopEntrySList)
{
    auto header = SListHeader(ctx.r3.u32);
    uint64_t raw = header.load();
    for (;;)
    {
        uint64_t old = __builtin_bswap64(raw);
        uint32_t entry = uint32_t(SListNext(old));
        if (entry == 0)
        {
            ctx.r3.u64 = 0;
            return;
        }
        uint64_t next = GuestRef<uint32_t>(entry);
        uint64_t updated = MakeSListHeader(next, SListDepth(old) - 1, SListSequence(old) + 1);
        if (header.compare_exchange_weak(raw, __builtin_bswap64(updated)))
        {
            ctx.r3.u64 = entry;
            return;
        }
    }
}

// PSINGLE_LIST_ENTRY InterlockedFlushSList(PSLIST_HEADER list)
PPC_FUNC(__imp__InterlockedFlushSList)
{
    auto header = SListHeader(ctx.r3.u32);
    uint64_t raw = header.load();
    for (;;)
    {
        uint64_t old = __builtin_bswap64(raw);
        uint64_t updated = MakeSListHeader(0, 0, SListSequence(old) + 1);
        if (header.compare_exchange_weak(raw, __builtin_bswap64(updated)))
        {
            ctx.r3.u64 = SListNext(old);
            return;
        }
    }
}

// Spin locks hold the owner's KPCR address (as Xenia does), 0 when free.
//
// IRQL is tracked per thread. Crossing DISPATCH_LEVEL takes or releases the
// interrupt lock (see EnterInterrupt in thread.h), which is what keeps the
// GPU interrupt handler from running in the middle of code that raised IRQL
// to exclude it. Only one thread at a time can be at raised IRQL, so spin
// locks (always taken at DISPATCH_LEVEL) are never contended.
namespace
{
    constexpr uint32_t kDispatchLevel = 2;
    constexpr uint32_t kInterruptLevel = 4;  // what interrupt handlers run at

    thread_local uint32_t t_irql = 0;
    std::recursive_mutex g_interruptLock;

    // Sets the calling thread's IRQL. Going to DISPATCH_LEVEL or above takes the
    // process-wide interrupt lock (see thread.h), dropping below releases it.
    void SetIrql(uint32_t level)
    {
        bool wasRaised = t_irql >= kDispatchLevel;
        bool raised = level >= kDispatchLevel;
        if (!wasRaised && raised)
            g_interruptLock.lock();
        t_irql = level;
        if (wasRaised && !raised)
            g_interruptLock.unlock();
    }

    // Raises the IRQL (never lowers it) and returns the old level, as KfRaiseIrql does.
    uint32_t RaiseIrql(uint32_t level)
    {
        uint32_t old = t_irql;
        SetIrql(std::max(old, level));
        return old;
    }

    // Spins until the guest spin lock word is free, then stores the owner (the KPCR
    // address) in it.
    void AcquireSpinLock(uint32_t lock)
    {
        auto word = GuestAtomic(lock);
        uint32_t owner = __builtin_bswap32(CurrentThread()->pcr);
        for (;;)
        {
            uint32_t expected = 0;
            if (word.compare_exchange_weak(expected, owner))
                return;
            std::this_thread::yield();
        }
    }

    // Releases a guest spin lock.
    void ReleaseSpinLock(uint32_t lock)
    {
        GuestAtomic(lock).store(0);
    }
}

void h3::kernel::EnterInterrupt()
{
    g_interruptLock.lock();
    t_irql = kInterruptLevel;
}

void h3::kernel::LeaveInterrupt()
{
    t_irql = 0;
    g_interruptLock.unlock();
}

// KIRQL KfAcquireSpinLock(PKSPIN_LOCK lock)
PPC_FUNC(__imp__KfAcquireSpinLock)
{
    uint32_t old = RaiseIrql(kDispatchLevel);
    AcquireSpinLock(ctx.r3.u32);
    ctx.r3.u64 = old;
}

// void KfReleaseSpinLock(PKSPIN_LOCK lock, KIRQL oldIrql)
PPC_FUNC(__imp__KfReleaseSpinLock)
{
    ReleaseSpinLock(ctx.r3.u32);
    SetIrql(ctx.r4.u8);
}

// void KeAcquireSpinLockAtRaisedIrql(PKSPIN_LOCK lock)
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    AcquireSpinLock(ctx.r3.u32);
}

// BOOLEAN KeTryToAcquireSpinLockAtRaisedIrql(PKSPIN_LOCK lock)
PPC_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql)
{
    uint32_t expected = 0;
    ctx.r3.u64 = GuestAtomic(ctx.r3.u32).compare_exchange_strong(expected, __builtin_bswap32(CurrentThread()->pcr));
}

// void KeReleaseSpinLockFromRaisedIrql(PKSPIN_LOCK lock)
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    ReleaseSpinLock(ctx.r3.u32);
}

// KIRQL KeRaiseIrqlToDpcLevel()
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)
{
    ctx.r3.u64 = RaiseIrql(kDispatchLevel);
}

// void KfLowerIrql(KIRQL newIrql)
PPC_FUNC(__imp__KfLowerIrql)
{
    SetIrql(ctx.r3.u8);
}
