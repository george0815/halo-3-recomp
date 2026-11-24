// Kernel objects: the KernelObject base, handles, object names, and waiting
// (WaitForObjects) for events, semaphores, mutants, timers, threads and I/O
// completion ports. The xboxkrnl exports built on them are in
// xboxkrnl_objects.cpp.

#include "objects.h"
#include "config/env.h"
#include "thread.h"

#include "../memory.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace h3::kernel
{
    namespace
    {
        std::mutex g_dispatcherMutex;
        std::condition_variable g_dispatcherCv;

        std::mutex g_handleMutex;
        std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
        // Handles start high so they never look like small integers or
        // valid guest pointers (Xenia starts at 0xF8000000 too).
        constexpr uint32_t kFirstHandle = 0xF8000000;
        uint32_t g_nextHandle = kFirstHandle;

        std::mutex g_nameMutex;
        std::unordered_map<std::string, std::weak_ptr<KernelObject>> g_names;

        // Guarded by the dispatcher lock.
        std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_guestObjects;

        thread_local std::shared_ptr<Thread> t_threadObject;

        constexpr uint32_t kHeaderSignalState = 4;
        constexpr uint32_t kSemaphoreLimit = 0x10;
    }

    // Mirrors the host state into the guest's DISPATCHER_HEADER (when the
    // object has one), so guest code that reads it directly sees the truth.
    void KernelObject::SyncHeader() const
    {
        if (guestHeader != 0)
            GuestRef<int32_t>(guestHeader + kHeaderSignalState) = SignalState();
    }

    // One global lock for all kernel object state, like the console's
    // dispatcher lock: simple, and waits are rare enough not to contend.
    std::unique_lock<std::mutex> DispatcherLock()
    {
        return std::unique_lock(g_dispatcherMutex);
    }

    // PERF: every state change wakes every waiting thread, which then
    // re-checks its own objects (one global condition variable). Per-object
    // wait lists would wake only the threads that can proceed.
    void NotifyDispatcher()
    {
        g_dispatcherCv.notify_all();
    }

    // Sleeps on the dispatcher's condition variable until ready() is true
    // (NotifyDispatcher wakes everyone to re-check).
    void WaitDispatcher(std::unique_lock<std::mutex>& lock, const std::function<bool()>& ready)
    {
        g_dispatcherCv.wait(lock, ready);
    }

    // KeWaitForMultipleObjects' core. Loops under the dispatcher lock: queued
    // user APCs end an alertable wait first; then, wait-all succeeds when
    // every object is signaled (all of them are satisfied together), wait-any
    // when one is (the lowest index wins, returned as STATUS_WAIT_0 + index).
    // Otherwise it sleeps until a state change or the deadline.
    uint32_t WaitForObjects(const std::vector<std::shared_ptr<KernelObject>>& objects, bool waitAll,
        Deadline deadline, uint32_t thread, const Thread* alertableThread)
    {
        auto lock = DispatcherLock();
        while (true)
        {
            if (alertableThread != nullptr && !alertableThread->userApcs.empty())
                return kStatusUserApc;

            if (waitAll)
            {
                bool all = true;
                for (const auto& object : objects)
                    all = all && object->IsSignaled(thread);
                if (all)
                {
                    for (const auto& object : objects)
                    {
                        object->Satisfy(thread);
                        object->SyncHeader();
                    }
                    return 0;
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); i++)
                {
                    if (objects[i]->IsSignaled(thread))
                    {
                        objects[i]->Satisfy(thread);
                        objects[i]->SyncHeader();
                        return uint32_t(i);
                    }
                }
            }

            if (!deadline)
                g_dispatcherCv.wait(lock);
            else if (g_dispatcherCv.wait_until(lock, *deadline) == std::cv_status::timeout)
                return kStatusTimeout;
        }
    }

    namespace
    {
        // What one thread is blocked on (see SetThreadWait), and since when.
        struct ThreadWait
        {
            const char* what;
            uint32_t object, detail, lr, caller;
            std::chrono::steady_clock::time_point start;
        };
        std::mutex g_threadWaitMutex;
        std::map<uint32_t, ThreadWait> g_threadWaits;

        // H3_LOG_WAITS: time spent blocked, per thread, caller and object.
        const bool g_logWaits = EnvFlag("H3_LOG_WAITS");
        struct WaitTotal
        {
            const char* what;
            double seconds = 0;
            uint32_t count = 0;
        };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, WaitTotal> g_waitTotals;  // (thread, lr, caller, object)
    }

    // Records what a thread is about to block on (for DumpThreadWaits); with
    // H3_LOG_WAITS also when it started.
    void SetThreadWait(uint32_t thread, const char* what, uint32_t object, uint32_t detail, uint32_t lr, uint32_t caller)
    {
        std::lock_guard lock(g_threadWaitMutex);
        g_threadWaits[thread] = { what, object, detail, lr, caller, g_logWaits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point() };
    }

    // The thread stopped waiting; with H3_LOG_WAITS, adds the time it waited to
    // the totals for that call site and object.
    void ClearThreadWait(uint32_t thread)
    {
        std::lock_guard lock(g_threadWaitMutex);
        auto it = g_threadWaits.find(thread);
        if (it == g_threadWaits.end())
            return;
        if (g_logWaits)
        {
            WaitTotal& total = g_waitTotals[{ thread, it->second.lr, it->second.caller, it->second.object }];
            total.what = it->second.what;
            total.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - it->second.start).count();
            total.count++;
        }
        g_threadWaits.erase(it);
    }

    // H3_LOG_WAITS: the 16 call sites and objects threads spent the most time
    // blocked on since the last report, then starts over.
    void DumpWaitTotals()
    {
        if (!g_logWaits)
            return;
        std::lock_guard lock(g_threadWaitMutex);
        std::vector<std::pair<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, WaitTotal>> totals(g_waitTotals.begin(), g_waitTotals.end());
        std::sort(totals.begin(), totals.end(), [](const auto& a, const auto& b) { return a.second.seconds > b.second.seconds; });
        fprintf(stderr, "[waits] since the last report:\n");
        for (size_t i = 0; i < totals.size() && i < 16; i++)
        {
            const auto& [key, total] = totals[i];
            fprintf(stderr, "[waits]   %7.3f s %6u x thread %08X lr %08X caller %08X %s %08X\n", total.seconds, total.count,
                std::get<0>(key), std::get<1>(key), std::get<2>(key), total.what, std::get<3>(key));
        }
        g_waitTotals.clear();
    }

    namespace
    {
        // Guest thread names (MSVC's set-thread-name exception), by KTHREAD.
        std::mutex g_threadNameMutex;
        std::unordered_map<uint32_t, std::string> g_threadNames;
    }

    // Remembers a thread's name for logs (the game names its threads).
    void SetThreadName(uint32_t kthread, const std::string& name)
    {
        std::lock_guard lock(g_threadNameMutex);
        g_threadNames[kthread] = name;
    }

    // A thread's name, or "" if it has none.
    std::string ThreadName(uint32_t kthread)
    {
        std::lock_guard lock(g_threadNameMutex);
        auto it = g_threadNames.find(kthread);
        return it != g_threadNames.end() ? it->second : std::string();
    }

    // Prints what every blocked guest thread waits on (for a hang: which
    // threads wait on what, from where).
    void DumpThreadWaits()
    {
        std::lock_guard lock(g_threadWaitMutex);
        for (const auto& [thread, wait] : g_threadWaits)
        {
            std::string name = ThreadName(thread);
            fprintf(stderr, "[kernel]   thread %08X%s%s%s waits on %s %08X (%08X) lr=0x%08X\n", thread, name.empty() ? "" : " (",
                name.c_str(), name.empty() ? "" : ")", wait.what, wait.object, wait.detail, wait.lr);
        }
    }

    // A guest timeout as a host deadline. NT timeouts are 100 ns units:
    // negative (or zero) means relative to now, positive is an absolute
    // FILETIME (100 ns since 1601), converted through the system clock.
    Deadline DeadlineFromGuest(uint32_t timeoutPtr)
    {
        if (timeoutPtr == 0)
            return std::nullopt;

        int64_t timeout = GuestRef<int64_t>(timeoutPtr);
        auto now = std::chrono::steady_clock::now();
        if (timeout <= 0)
            return now + std::chrono::nanoseconds(-timeout * 100);

        // Absolute FILETIME.
        constexpr int64_t kUnixEpochAsFileTime = 116444736000000000ll;
        int64_t nowFileTime = kUnixEpochAsFileTime + std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() / 100;
        return now + std::chrono::nanoseconds(std::max<int64_t>(timeout - nowFileTime, 0) * 100);
    }

    // Hands out the next handle (multiples of 4, as NT's are) for an object.
    // The handle table keeps the object alive until CloseHandle.
    uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
    {
        std::lock_guard lock(g_handleMutex);
        uint32_t handle = g_nextHandle;
        g_nextHandle += 4;
        g_handles.emplace(handle, std::move(object));
        return handle;
    }

    // The object behind a handle; the current-thread pseudo handle gives the
    // calling thread. Null for an unknown handle.
    std::shared_ptr<KernelObject> LookupHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return CurrentThreadObject();

        std::lock_guard lock(g_handleMutex);
        auto it = g_handles.find(handle);
        return it != g_handles.end() ? it->second : nullptr;
    }

    // Drops a handle (the object lives on while something else holds it).
    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(g_handleMutex);
        return g_handles.erase(handle) != 0;
    }

    // An existing named object (opening "the same" event from two places
    // gives the same object); null if none, or if it was destroyed.
    std::shared_ptr<KernelObject> FindNamedObject(const std::string& name)
    {
        std::lock_guard lock(g_nameMutex);
        auto it = g_names.find(name);
        return it != g_names.end() ? it->second.lock() : nullptr;
    }

    // Makes a named object findable by its name. Held weakly: the name doesn't
    // keep the object alive.
    void RegisterNamedObject(const std::shared_ptr<KernelObject>& object)
    {
        std::lock_guard lock(g_nameMutex);
        g_names[object->name] = object;
    }

    // An object's name from the OBJECT_ATTRIBUTES a create/open call passes.
    std::string ObjectAttributesName(uint32_t attributes)
    {
        // OBJECT_ATTRIBUTES: RootDirectory, PANSI_STRING ObjectName, Attributes.
        if (attributes == 0)
            return {};
        uint32_t name = GuestRef<uint32_t>(attributes + 4);
        if (name == 0)
            return {};
        uint16_t length = GuestRef<uint16_t>(name);
        uint32_t buffer = GuestRef<uint32_t>(name + 4);
        return std::string(GuestPtr<const char>(buffer), length);
    }

    // KeSetEvent / KeResetEvent: changes the state, mirrors it to the guest
    // header and, when set, wakes waiters.
    uint32_t SetEvent(Event& event, bool signaled)
    {
        auto lock = DispatcherLock();
        uint32_t previous = event.signaled;
        event.signaled = signaled;
        event.SyncHeader();
        if (signaled)
            NotifyDispatcher();
        return previous;
    }

    // Queues an APC; waking the dispatcher lets an alertable wait in that
    // thread return STATUS_USER_APC so it runs it.
    void QueueUserApc(Thread& thread, const Thread::UserApc& apc)
    {
        auto lock = DispatcherLock();
        thread.userApcs.push_back(apc);
        NotifyDispatcher();
    }

    // Adds a finished I/O to a completion port and wakes its waiters.
    void PostCompletion(IoCompletion& port, const IoCompletion::Packet& packet)
    {
        auto lock = DispatcherLock();
        port.packets.push_back(packet);
        NotifyDispatcher();
    }

    // The game often uses kernel objects it allocated itself (a KEVENT inside
    // its own struct) through Ke* calls. Those are looked up by header
    // address; a host object is made the first time, from the header's type
    // and signal state, and resynced if the guest re-initialized the header.
    std::shared_ptr<KernelObject> ObjectForGuest(uint32_t header)
    {
        auto lock = DispatcherLock();
        auto type = ObjectType(*GuestPtr<uint8_t>(header));
        int32_t state = GuestRef<int32_t>(header + kHeaderSignalState);

        auto it = g_guestObjects.find(header);
        if (it != g_guestObjects.end() && it->second->type == type)
        {
            // We mirror our state into the header after every change, so a
            // mismatch means the guest re-initialised the object itself.
            auto& object = it->second;
            if (state != object->SignalState())
            {
                if (auto* event = dynamic_cast<Event*>(object.get()))
                    event->signaled = state != 0;
                else if (auto* semaphore = dynamic_cast<Semaphore*>(object.get()))
                    semaphore->count = state;
            }
            return object;
        }

        std::shared_ptr<KernelObject> object;
        switch (type)
        {
        case ObjectType::NotificationEvent:
        case ObjectType::SynchronizationEvent:
            object = std::make_shared<Event>(type == ObjectType::NotificationEvent, state != 0);
            break;
        case ObjectType::Semaphore:
            object = std::make_shared<Semaphore>(state, GuestRef<int32_t>(header + kSemaphoreLimit));
            break;
        case ObjectType::Mutant:
            object = std::make_shared<Mutant>();
            break;
        default:
            fprintf(stderr, "[kernel] unsupported guest dispatcher object type %u at 0x%08X\n", unsigned(type), header);
            return nullptr;
        }

        object->guestHeader = header;
        g_guestObjects[header] = object;
        return object;
    }

    // Ties a host object to a guest DISPATCHER_HEADER (writes its type and
    // state there), so lookups by header find it.
    void BindGuestObject(const std::shared_ptr<KernelObject>& object, uint32_t header)
    {
        auto lock = DispatcherLock();
        object->guestHeader = header;
        *GuestPtr<uint8_t>(header) = uint8_t(object->type);
        object->SyncHeader();
        g_guestObjects[header] = object;
    }

    // A guest header for an object made through a handle (Nt* calls), for
    // ObReferenceObjectByHandle, which returns a guest pointer.
    uint32_t GuestObjectFor(const std::shared_ptr<KernelObject>& object)
    {
        if (object->guestHeader == 0)
            BindGuestObject(object, SystemAlloc(0x20));
        return object->guestHeader;
    }

    // The calling guest thread's object (thread-local).
    std::shared_ptr<Thread> CurrentThreadObject()
    {
        return t_threadObject;
    }

    // Called on each guest thread at its start, for CurrentThreadObject.
    void RegisterThreadObject(const std::shared_ptr<Thread>& thread)
    {
        t_threadObject = thread;
    }
}
