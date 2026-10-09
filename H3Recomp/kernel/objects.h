#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace h3::kernel
{
    struct GuestThread;

    // Dispatcher object types, numbered as in DISPATCHER_HEADER.type.
    enum class ObjectType : uint8_t
    {
        NotificationEvent = 0,
        SynchronizationEvent = 1,
        Mutant = 2,
        Semaphore = 5,
        Thread = 6,
        NotificationTimer = 8,
        SynchronizationTimer = 9,
        IoCompletion = 0x80,  // not a dispatcher type on the 360; host-only
        File = 0x81,          // host-only
    };

    // A kernel object. All dispatcher state is guarded by DispatcherLock().
    // Objects may have a DISPATCHER_HEADER in guest memory (guestHeader):
    // either the guest's own KEVENT/KSEMAPHORE used through Ke* calls, or one
    // the runtime allocated so ObReferenceObjectByHandle can return it. Its
    // signal state is kept in sync for guest code that reads it directly.
    struct KernelObject
    {
        explicit KernelObject(ObjectType type) : type(type) {}
        virtual ~KernelObject() = default;

        // Can a wait by this thread (KTHREAD address) be satisfied now?
        virtual bool IsSignaled(uint32_t thread) const = 0;
        // Applies the side effect of a satisfied wait (auto-reset, count--...).
        virtual void Satisfy(uint32_t thread) {}
        virtual int32_t SignalState() const = 0;

        void SyncHeader() const;

        ObjectType type;
        uint32_t guestHeader = 0;
        std::string name;
    };

    struct Event : KernelObject
    {
        Event(bool manualReset, bool signaled)
            : KernelObject(manualReset ? ObjectType::NotificationEvent : ObjectType::SynchronizationEvent), signaled(signaled) {}

        bool IsSignaled(uint32_t) const override { return signaled; }
        void Satisfy(uint32_t) override
        {
            if (type == ObjectType::SynchronizationEvent)
                signaled = false;
        }
        int32_t SignalState() const override { return signaled; }

        bool signaled;
    };

    struct Semaphore : KernelObject
    {
        Semaphore(int32_t count, int32_t limit) : KernelObject(ObjectType::Semaphore), count(count), limit(limit) {}

        bool IsSignaled(uint32_t) const override { return count > 0; }
        void Satisfy(uint32_t) override { count--; }
        int32_t SignalState() const override { return count; }

        int32_t count;
        int32_t limit;
    };

    struct Mutant : KernelObject
    {
        Mutant() : KernelObject(ObjectType::Mutant) {}

        bool IsSignaled(uint32_t thread) const override { return owner == 0 || owner == thread; }
        void Satisfy(uint32_t thread) override
        {
            owner = thread;
            recursion++;
        }
        int32_t SignalState() const override { return 1 - recursion; }

        uint32_t owner = 0;
        int32_t recursion = 0;
    };

    struct IoCompletion : KernelObject
    {
        struct Packet
        {
            uint32_t key;
            uint32_t apcContext;
            uint32_t status;
            uint32_t information;
        };

        IoCompletion() : KernelObject(ObjectType::IoCompletion) {}

        bool IsSignaled(uint32_t) const override { return !packets.empty(); }
        int32_t SignalState() const override { return int32_t(packets.size()); }

        std::deque<Packet> packets;
    };

    struct Thread : KernelObject
    {
        Thread() : KernelObject(ObjectType::Thread) {}

        bool IsSignaled(uint32_t) const override { return exited; }
        int32_t SignalState() const override { return exited; }

        // User APCs: routine(arg1, arg2, arg3), run on this thread during an
        // alertable wait. Guarded by DispatcherLock().
        struct UserApc
        {
            uint32_t routine;
            uint32_t arg1;
            uint32_t arg2;
            uint32_t arg3;
        };

        GuestThread* guest = nullptr;
        std::deque<UserApc> userApcs;
        uint32_t threadId = 0;
        uint32_t suspendCount = 0;
        int32_t priority = 0;
        uint32_t affinity = 0xFF;
        bool exited = false;
        uint32_t exitCode = 0;
    };

    // Sets or clears an event and wakes waiters. Returns the previous state.
    uint32_t SetEvent(Event& event, bool signaled);

    // Queues a packet on a completion port and wakes waiters.
    void PostCompletion(IoCompletion& port, const IoCompletion::Packet& packet);

    std::unique_lock<std::mutex> DispatcherLock();
    // Wakes waiters after a state change. Call with the lock held.
    void NotifyDispatcher();
    // Blocks until ready() holds, re-checking after every NotifyDispatcher.
    void WaitDispatcher(std::unique_lock<std::mutex>& lock, const std::function<bool()>& ready);

    // Waits for any (waitAll = false) or all of the objects. Returns the
    // NTSTATUS: STATUS_WAIT_0 + index, STATUS_TIMEOUT, or STATUS_USER_APC
    // when alertableThread has user APCs queued (the caller delivers them).
    // No deadline waits forever.
    using Deadline = std::optional<std::chrono::steady_clock::time_point>;
    uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
        Deadline deadline, uint32_t thread, const Thread* alertableThread = nullptr);

    // Queues a user APC on a thread and wakes it if it's in an alertable wait.
    void QueueUserApc(Thread& thread, const Thread::UserApc& apc);

    // Converts a guest PLARGE_INTEGER timeout (100 ns units; negative is
    // relative, positive is absolute FILETIME, null is infinite).
    Deadline DeadlineFromGuest(uint32_t timeoutPtr);

    constexpr uint32_t kStatusUserApc = 0x000000C0;
    constexpr uint32_t kStatusTimeout = 0x00000102;
    constexpr uint32_t kStatusAbandonedWait0 = 0x00000080;

    // Handles. Pseudo handles: 0xFFFFFFFE is the current thread.
    constexpr uint32_t kCurrentThreadHandle = 0xFFFFFFFE;
    uint32_t CreateHandle(std::shared_ptr<KernelObject> object);
    std::shared_ptr<KernelObject> LookupHandle(uint32_t handle);
    bool CloseHandle(uint32_t handle);

    template <typename T>
    std::shared_ptr<T> LookupHandleAs(uint32_t handle)
    {
        return std::dynamic_pointer_cast<T>(LookupHandle(handle));
    }

    // Named objects (OBJECT_ATTRIBUTES with a name).
    std::shared_ptr<KernelObject> FindNamedObject(const std::string& name);
    void RegisterNamedObject(const std::shared_ptr<KernelObject>& object);
    // Reads the name from a guest OBJECT_ATTRIBUTES pointer ("" if none).
    std::string ObjectAttributesName(uint32_t attributes);

    // Host object for a DISPATCHER_HEADER in guest memory, created from the
    // header's type and signal state on first use.
    std::shared_ptr<KernelObject> ObjectForGuest(uint32_t header);
    // Associates a host object with a guest header (threads, re-inits).
    void BindGuestObject(const std::shared_ptr<KernelObject>& object, uint32_t header);
    // Guest header for a host object, allocating one if needed.
    uint32_t GuestObjectFor(const std::shared_ptr<KernelObject>& object);

    // Threads.
    std::shared_ptr<Thread> CurrentThreadObject();
    void RegisterThreadObject(const std::shared_ptr<Thread>& thread);
}
