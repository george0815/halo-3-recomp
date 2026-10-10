// xboxkrnl object exports: handles, events, semaphores, mutants, I/O
// completion ports, threads and waits. Reference: Xenia's
// xboxkrnl_threading.cc, xboxkrnl_ob.cc and xboxkrnl_io.cc.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "module.h"
#include "objects.h"
#include "thread.h"
#include "../memory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace h3;
using namespace h3::kernel;

namespace
{
    constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
    constexpr uint32_t kStatusObjectTypeMismatch = 0xC0000024;
    constexpr uint32_t kStatusMutantNotOwned = 0xC0000046;
    constexpr uint32_t kStatusSemaphoreLimitExceeded = 0xC0000047;
    constexpr uint32_t kStatusObjectNameExists = 0x40000000;

    uint32_t CurrentKThread()
    {
        return CurrentThread()->kthread;
    }

    // Shared by the NtCreate* functions: opens an existing object with the
    // same name, or registers a new one. Returns the NTSTATUS.
    template <typename T, typename Make>
    uint32_t CreateObject(uint32_t handlePtr, uint32_t attributes, Make make)
    {
        std::string name = ObjectAttributesName(attributes);
        if (!name.empty())
        {
            if (auto existing = FindNamedObject(name))
            {
                if (!std::dynamic_pointer_cast<T>(existing))
                    return kStatusObjectTypeMismatch;
                GuestRef<uint32_t>(handlePtr) = CreateHandle(existing);
                return kStatusObjectNameExists;
            }
        }

        std::shared_ptr<T> object = make();
        if (!name.empty())
        {
            object->name = name;
            RegisterNamedObject(object);
        }
        GuestRef<uint32_t>(handlePtr) = CreateHandle(object);
        return kStatusSuccess;
    }

    // An alertable wait returns STATUS_USER_APC after running the thread's
    // queued user APCs.
    uint32_t Wait(std::vector<std::shared_ptr<KernelObject>> objects, bool waitAll, uint32_t timeoutPtr,
        bool alertable, PPCContext& ctx)
    {
        for (const auto& object : objects)
        {
            if (!object)
                return kStatusInvalidHandle;
        }
        auto thread = alertable ? CurrentThreadObject() : nullptr;
        uint32_t kthread = CurrentKThread();
        SetThreadWait(kthread, objects.size() > 1 ? (waitAll ? "all of" : "any of") : "object", objects[0]->guestHeader,
            uint32_t(objects[0]->type) | uint32_t(objects.size()) << 8, uint32_t(ctx.lr));
        uint32_t status = WaitForObjects(objects, waitAll, DeadlineFromGuest(timeoutPtr), kthread, thread.get());
        ClearThreadWait(kthread);
        if (status == kStatusUserApc)
            DeliverUserApcs(ctx);
        return status;
    }

    std::shared_ptr<Thread> ThreadFromGuest(uint32_t kthread)
    {
        return std::dynamic_pointer_cast<Thread>(ObjectForGuest(kthread));
    }
}

// --- Handles ---------------------------------------------------------------

// NTSTATUS NtClose(HANDLE handle)
PPC_FUNC(__imp__NtClose)
{
    ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? kStatusSuccess : kStatusInvalidHandle;
}

// NTSTATUS NtDuplicateObject(HANDLE source, PHANDLE target, ULONG options)
PPC_FUNC(__imp__NtDuplicateObject)
{
    constexpr uint32_t kDuplicateCloseSource = 1;
    auto object = LookupHandle(ctx.r3.u32);
    if (!object)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (ctx.r4.u32 != 0)
        GuestRef<uint32_t>(ctx.r4.u32) = CreateHandle(object);
    if (ctx.r5.u32 & kDuplicateCloseSource)
        CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS ObReferenceObjectByHandle(HANDLE handle, POBJECT_TYPE type, PVOID* object)
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    auto object = LookupHandle(ctx.r3.u32);
    if (!object)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (ctx.r5.u32 != 0)
        GuestRef<uint32_t>(ctx.r5.u32) = GuestObjectFor(object);
    ctx.r3.u64 = kStatusSuccess;
}

// Host objects are kept alive by their handles; guest references don't count.
PPC_FUNC(__imp__ObReferenceObject)
{
}

PPC_FUNC(__imp__ObDereferenceObject)
{
}

// --- Events ----------------------------------------------------------------

// NTSTATUS NtCreateEvent(PHANDLE handle, POBJECT_ATTRIBUTES attributes,
//     EVENT_TYPE type, BOOLEAN initialState)
// EVENT_TYPE 0 is NotificationEvent (manual reset), 1 SynchronizationEvent.
PPC_FUNC(__imp__NtCreateEvent)
{
    bool manualReset = ctx.r5.u32 == 0;
    bool initialState = ctx.r6.u32 != 0;
    ctx.r3.u64 = CreateObject<Event>(ctx.r3.u32, ctx.r4.u32,
        [&] { return std::make_shared<Event>(manualReset, initialState); });
}

// NTSTATUS NtSetEvent(HANDLE handle, PLONG previousState)
PPC_FUNC(__imp__NtSetEvent)
{
    auto event = LookupHandleAs<Event>(ctx.r3.u32);
    if (!event)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t previous = SetEvent(*event, true);
    if (ctx.r4.u32 != 0)
        GuestRef<uint32_t>(ctx.r4.u32) = previous;
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtClearEvent(HANDLE handle)
PPC_FUNC(__imp__NtClearEvent)
{
    auto event = LookupHandleAs<Event>(ctx.r3.u32);
    if (!event)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    SetEvent(*event, false);
    ctx.r3.u64 = kStatusSuccess;
}

// LONG KeSetEvent(PKEVENT event, KPRIORITY increment, BOOLEAN wait)
PPC_FUNC(__imp__KeSetEvent)
{
    auto event = std::dynamic_pointer_cast<Event>(ObjectForGuest(ctx.r3.u32));
    ctx.r3.u64 = event ? SetEvent(*event, true) : 0;
}

// LONG KeResetEvent(PKEVENT event)
PPC_FUNC(__imp__KeResetEvent)
{
    auto event = std::dynamic_pointer_cast<Event>(ObjectForGuest(ctx.r3.u32));
    ctx.r3.u64 = event ? SetEvent(*event, false) : 0;
}

// --- Semaphores ------------------------------------------------------------

// NTSTATUS NtCreateSemaphore(PHANDLE handle, POBJECT_ATTRIBUTES attributes,
//     LONG initialCount, LONG maximumCount)
PPC_FUNC(__imp__NtCreateSemaphore)
{
    int32_t count = ctx.r5.s32;
    int32_t limit = ctx.r6.s32;
    ctx.r3.u64 = CreateObject<Semaphore>(ctx.r3.u32, ctx.r4.u32,
        [&] { return std::make_shared<Semaphore>(count, limit); });
}

// Returns the NTSTATUS; the previous count goes to *previous.
static uint32_t ReleaseSemaphore(Semaphore& semaphore, int32_t count, int32_t* previous)
{
    auto lock = DispatcherLock();
    *previous = semaphore.count;
    if (count <= 0 || semaphore.count + count > semaphore.limit)
        return kStatusSemaphoreLimitExceeded;
    semaphore.count += count;
    semaphore.SyncHeader();
    NotifyDispatcher();
    return kStatusSuccess;
}

// NTSTATUS NtReleaseSemaphore(HANDLE handle, LONG releaseCount, PLONG previousCount)
PPC_FUNC(__imp__NtReleaseSemaphore)
{
    auto semaphore = LookupHandleAs<Semaphore>(ctx.r3.u32);
    if (!semaphore)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    int32_t previous;
    uint32_t status = ReleaseSemaphore(*semaphore, ctx.r4.s32, &previous);
    if (ctx.r5.u32 != 0)
        GuestRef<int32_t>(ctx.r5.u32) = previous;
    ctx.r3.u64 = status;
}

// void KeInitializeSemaphore(PKSEMAPHORE semaphore, LONG count, LONG limit)
// KSEMAPHORE: DISPATCHER_HEADER, then LONG Limit at 0x10.
PPC_FUNC(__imp__KeInitializeSemaphore)
{
    uint32_t header = ctx.r3.u32;
    auto semaphore = std::make_shared<Semaphore>(ctx.r4.s32, ctx.r5.s32);
    GuestRef<uint32_t>(header + 4) = 0;
    GuestRef<uint32_t>(header + 8) = header + 8;
    GuestRef<uint32_t>(header + 12) = header + 8;
    GuestRef<int32_t>(header + 0x10) = ctx.r5.s32;
    BindGuestObject(semaphore, header);
}

// LONG KeReleaseSemaphore(PKSEMAPHORE semaphore, KPRIORITY increment, LONG adjustment, BOOLEAN wait)
PPC_FUNC(__imp__KeReleaseSemaphore)
{
    auto semaphore = std::dynamic_pointer_cast<Semaphore>(ObjectForGuest(ctx.r3.u32));
    int32_t previous = 0;
    if (semaphore)
        ReleaseSemaphore(*semaphore, ctx.r5.s32, &previous);
    ctx.r3.u64 = uint32_t(previous);
}

// --- Mutants ---------------------------------------------------------------

// NTSTATUS NtCreateMutant(PHANDLE handle, POBJECT_ATTRIBUTES attributes, BOOLEAN initialOwner)
PPC_FUNC(__imp__NtCreateMutant)
{
    bool initialOwner = ctx.r5.u32 != 0;
    uint32_t thread = CurrentKThread();
    ctx.r3.u64 = CreateObject<Mutant>(ctx.r3.u32, ctx.r4.u32, [&]
    {
        auto mutant = std::make_shared<Mutant>();
        if (initialOwner)
        {
            mutant->owner = thread;
            mutant->recursion = 1;
        }
        return mutant;
    });
}

// NTSTATUS NtReleaseMutant(HANDLE handle, PLONG previousCount)
PPC_FUNC(__imp__NtReleaseMutant)
{
    auto mutant = LookupHandleAs<Mutant>(ctx.r3.u32);
    if (!mutant)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    auto lock = DispatcherLock();
    if (mutant->owner != CurrentKThread())
    {
        ctx.r3.u64 = kStatusMutantNotOwned;
        return;
    }
    int32_t previous = mutant->SignalState();
    if (--mutant->recursion == 0)
    {
        mutant->owner = 0;
        NotifyDispatcher();
    }
    mutant->SyncHeader();
    if (ctx.r4.u32 != 0)
        GuestRef<int32_t>(ctx.r4.u32) = previous;
    ctx.r3.u64 = kStatusSuccess;
}

// --- I/O completion ports --------------------------------------------------

// NTSTATUS NtCreateIoCompletion(PHANDLE handle, ACCESS_MASK access,
//     POBJECT_ATTRIBUTES attributes, ULONG concurrentThreads)
PPC_FUNC(__imp__NtCreateIoCompletion)
{
    ctx.r3.u64 = CreateObject<IoCompletion>(ctx.r3.u32, ctx.r5.u32,
        [] { return std::make_shared<IoCompletion>(); });
}

// NTSTATUS NtRemoveIoCompletion(HANDLE handle, PVOID* keyContext, PVOID* apcContext,
//     PIO_STATUS_BLOCK ioStatus, PLARGE_INTEGER timeout)
PPC_FUNC(__imp__NtRemoveIoCompletion)
{
    auto port = LookupHandleAs<IoCompletion>(ctx.r3.u32);
    if (!port)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    uint32_t status = WaitForObjects({ port }, false, DeadlineFromGuest(ctx.r7.u32), CurrentKThread());
    if (status == kStatusTimeout)
    {
        ctx.r3.u64 = status;
        return;
    }

    IoCompletion::Packet packet;
    {
        auto lock = DispatcherLock();
        packet = port->packets.front();
        port->packets.pop_front();
    }
    if (ctx.r4.u32 != 0)
        GuestRef<uint32_t>(ctx.r4.u32) = packet.key;
    if (ctx.r5.u32 != 0)
        GuestRef<uint32_t>(ctx.r5.u32) = packet.apcContext;
    if (ctx.r6.u32 != 0)
    {
        GuestRef<uint32_t>(ctx.r6.u32) = packet.status;
        GuestRef<uint32_t>(ctx.r6.u32 + 4) = packet.information;
    }
    ctx.r3.u64 = kStatusSuccess;
}

// --- Waits -----------------------------------------------------------------

// NTSTATUS NtWaitForSingleObjectEx(HANDLE handle, KPROCESSOR_MODE mode,
//     BOOLEAN alertable, PLARGE_INTEGER timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    ctx.r3.u64 = Wait({ LookupHandle(ctx.r3.u32) }, false, ctx.r6.u32, ctx.r5.u8 != 0, ctx);
}

// NTSTATUS NtWaitForMultipleObjectsEx(ULONG count, PHANDLE handles, WAIT_TYPE type,
//     KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER timeout)
// WAIT_TYPE 0 is WaitAll, 1 is WaitAny.
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
        objects.push_back(LookupHandle(GuestRef<uint32_t>(ctx.r4.u32 + i * 4)));
    ctx.r3.u64 = Wait(std::move(objects), ctx.r5.u32 == 0, ctx.r8.u32, ctx.r7.u8 != 0, ctx);
}

// NTSTATUS KeWaitForSingleObject(PVOID object, KWAIT_REASON reason,
//     KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER timeout)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    ctx.r3.u64 = Wait({ ObjectForGuest(ctx.r3.u32) }, false, ctx.r7.u32, ctx.r6.u8 != 0, ctx);
}

// NTSTATUS KeWaitForMultipleObjects(ULONG count, PVOID* objects, WAIT_TYPE type,
//     KWAIT_REASON reason, KPROCESSOR_MODE mode, BOOLEAN alertable,
//     PLARGE_INTEGER timeout, PKWAIT_BLOCK waitBlocks)
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    std::vector<std::shared_ptr<KernelObject>> objects;
    for (uint32_t i = 0; i < ctx.r3.u32; i++)
        objects.push_back(ObjectForGuest(GuestRef<uint32_t>(ctx.r4.u32 + i * 4)));
    ctx.r3.u64 = Wait(std::move(objects), ctx.r5.u32 == 0, ctx.r9.u32, ctx.r8.u8 != 0, ctx);
}

// --- Threads ---------------------------------------------------------------

// NTSTATUS ExCreateThread(PHANDLE handle, DWORD stackSize, LPDWORD threadId,
//     PVOID xapiThreadStartup, PVOID startAddress, PVOID startContext, DWORD flags)
PPC_FUNC(__imp__ExCreateThread)
{
    constexpr uint32_t kCreateSuspended = 0x1;
    // Hands back the X_KTHREAD pointer instead of a handle (Xenia does the
    // same); the game then waits on it as a dispatcher object.
    constexpr uint32_t kReturnKThread = 0x80;

    uint32_t handlePtr = ctx.r3.u32;
    uint32_t stackSize = ctx.r4.u32 ? ctx.r4.u32 : g_module.stackSize;
    uint32_t threadIdPtr = ctx.r5.u32;
    uint32_t flags = ctx.r9.u32;

    auto thread = StartGuestThread(stackSize, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, flags & kCreateSuspended);
    if (handlePtr != 0)
        GuestRef<uint32_t>(handlePtr) = (flags & kReturnKThread) ? thread->guest->kthread : CreateHandle(thread);
    if (threadIdPtr != 0)
        GuestRef<uint32_t>(threadIdPtr) = thread->threadId;
    ctx.r3.u64 = kStatusSuccess;
}

// void ExTerminateThread(DWORD exitCode)
PPC_FUNC(__imp__ExTerminateThread)
{
    ExitCurrentThread(ctx.r3.u32);
}

// NTSTATUS NtResumeThread(HANDLE handle, PULONG suspendCount)
PPC_FUNC(__imp__NtResumeThread)
{
    auto thread = LookupHandleAs<Thread>(ctx.r3.u32);
    if (!thread)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    uint32_t previous = ResumeThread(*thread);
    if (ctx.r4.u32 != 0)
        GuestRef<uint32_t>(ctx.r4.u32) = previous;
    ctx.r3.u64 = kStatusSuccess;
}

// ULONG KeResumeThread(PKTHREAD thread)
PPC_FUNC(__imp__KeResumeThread)
{
    auto thread = ThreadFromGuest(ctx.r3.u32);
    ctx.r3.u64 = thread ? ResumeThread(*thread) : 0;
}

// KAFFINITY KeSetAffinityThread(PKTHREAD thread, KAFFINITY affinity, PKAFFINITY previous)
// The host scheduler decides; just remember what the guest asked for.
PPC_FUNC(__imp__KeSetAffinityThread)
{
    auto thread = ThreadFromGuest(ctx.r3.u32);
    uint32_t previous = thread ? thread->affinity : 0;
    if (thread)
        thread->affinity = ctx.r4.u32;
    if (ctx.r5.u32 != 0)
        GuestRef<uint32_t>(ctx.r5.u32) = previous;
    ctx.r3.u64 = previous;
}

// LONG KeSetBasePriorityThread(PKTHREAD thread, LONG increment)
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    auto thread = ThreadFromGuest(ctx.r3.u32);
    int32_t previous = thread ? thread->priority : 0;
    if (thread)
        thread->priority = ctx.r4.s32;
    ctx.r3.u64 = uint32_t(previous);
}

// NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE mode, BOOLEAN alertable, PLARGE_INTEGER interval)
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    Deadline deadline = DeadlineFromGuest(ctx.r5.u32);
    if (ctx.r4.u8 != 0)
    {
        // Alertable: an empty wait ends at the deadline or on a user APC.
        if (!deadline)
            deadline = std::chrono::steady_clock::now();
        auto thread = CurrentThreadObject();
        if (WaitForObjects({}, false, deadline, CurrentKThread(), thread.get()) == kStatusUserApc)
        {
            DeliverUserApcs(ctx);
            ctx.r3.u64 = kStatusUserApc;
            return;
        }
    }
    else if (deadline)
        std::this_thread::sleep_until(*deadline);
    else
        std::this_thread::yield();
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueueApcThread(HANDLE thread, PVOID routine, PVOID context, PVOID arg1, PVOID arg2)
// Queues a user APC: routine(context, arg1, arg2).
PPC_FUNC(__imp__NtQueueApcThread)
{
    auto thread = ctx.r3.u32 == kCurrentThreadHandle ? CurrentThreadObject() : LookupHandleAs<Thread>(ctx.r3.u32);
    if (!thread)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    QueueUserApc(*thread, { ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32 });
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtYieldExecution()
PPC_FUNC(__imp__NtYieldExecution)
{
    std::this_thread::yield();
    ctx.r3.u64 = kStatusSuccess;
}

// --- Timers ------------------------------------------------------------------
//
// A host thread per armed timer sleeps until the due time, signals the timer
// and queues the optional APC on the thread that set it; periodic timers go
// around again. Setting or cancelling bumps the generation, which retires
// the previous thread.

namespace
{
    struct Timer : KernelObject
    {
        explicit Timer(bool synchronization)
            : KernelObject(synchronization ? ObjectType::SynchronizationTimer : ObjectType::NotificationTimer) {}

        bool IsSignaled(uint32_t) const override { return signaled; }
        void Satisfy(uint32_t) override
        {
            if (type == ObjectType::SynchronizationTimer)
                signaled = false;
        }
        int32_t SignalState() const override { return signaled; }

        bool signaled = false;
        std::atomic<uint32_t> generation{ 0 };
    };
}

// NTSTATUS NtCreateTimer(PHANDLE handle, POBJECT_ATTRIBUTES attributes, TIMER_TYPE type)
// TIMER_TYPE 0 is NotificationTimer, 1 SynchronizationTimer.
PPC_FUNC(__imp__NtCreateTimer)
{
    bool synchronization = ctx.r5.u32 == 1;
    ctx.r3.u64 = CreateObject<Timer>(ctx.r3.u32, ctx.r4.u32, [&] { return std::make_shared<Timer>(synchronization); });
}

// NTSTATUS NtSetTimerEx(HANDLE handle, PLARGE_INTEGER dueTime, PTIMERAPCROUTINE routine, KPROCESSOR_MODE mode,
//     PVOID context, BOOLEAN resume, LONG period, PBOOLEAN previousState)
PPC_FUNC(__imp__NtSetTimerEx)
{
    auto timer = LookupHandleAs<Timer>(ctx.r3.u32);
    if (!timer)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    Deadline due = DeadlineFromGuest(ctx.r4.u32);
    uint32_t routine = ctx.r5.u32, context = ctx.r7.u32;
    int32_t period = int32_t(ctx.r9.u32);
    uint32_t previousState = ctx.r10.u32;
    std::shared_ptr<Thread> setter = routine != 0 ? CurrentThreadObject() : nullptr;

    uint32_t generation;
    {
        auto lock = DispatcherLock();
        if (previousState != 0)
            *GuestPtr<uint8_t>(previousState) = timer->signaled;
        timer->signaled = false;
        timer->SyncHeader();
        generation = ++timer->generation;
    }
    auto when = due.value_or(std::chrono::steady_clock::now());
    std::thread([timer, generation, when, period, routine, context, setter]() mutable {
        for (;;)
        {
            std::this_thread::sleep_until(when);
            if (timer->generation.load() != generation)
                return;
            {
                auto lock = DispatcherLock();
                timer->signaled = true;
                timer->SyncHeader();
                NotifyDispatcher();
            }
            if (setter)
            {
                // The APC gets the fire time as a FILETIME, low and high parts.
                constexpr int64_t kUnixEpochAsFileTime = 116444736000000000ll;
                int64_t now = kUnixEpochAsFileTime + std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() / 100;
                QueueUserApc(*setter, { routine, context, uint32_t(now), uint32_t(uint64_t(now) >> 32) });
            }
            if (period <= 0)
                return;
            when += std::chrono::milliseconds(period);
        }
    }).detach();
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtCancelTimer(HANDLE handle, PBOOLEAN currentState)
PPC_FUNC(__imp__NtCancelTimer)
{
    auto timer = LookupHandleAs<Timer>(ctx.r3.u32);
    if (!timer)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    auto lock = DispatcherLock();
    timer->generation++;
    if (ctx.r4.u32 != 0)
        *GuestPtr<uint8_t>(ctx.r4.u32) = timer->signaled;
    ctx.r3.u64 = kStatusSuccess;
}
