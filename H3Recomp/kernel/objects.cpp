#include "objects.h"
#include "thread.h"

#include "../memory.h"

#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>

namespace h3::kernel
{
    namespace
    {
        std::mutex g_dispatcherMutex;
        std::condition_variable g_dispatcherCv;

        std::mutex g_handleMutex;
        std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_handles;
        uint32_t g_nextHandle = 0xF8000000;

        std::mutex g_nameMutex;
        std::unordered_map<std::string, std::weak_ptr<KernelObject>> g_names;

        // Guarded by the dispatcher lock.
        std::unordered_map<uint32_t, std::shared_ptr<KernelObject>> g_guestObjects;

        thread_local std::shared_ptr<Thread> t_threadObject;

        constexpr uint32_t kHeaderSignalState = 4;
        constexpr uint32_t kSemaphoreLimit = 0x10;
    }

    void KernelObject::SyncHeader() const
    {
        if (guestHeader != 0)
            GuestRef<int32_t>(guestHeader + kHeaderSignalState) = SignalState();
    }

    std::unique_lock<std::mutex> DispatcherLock()
    {
        return std::unique_lock(g_dispatcherMutex);
    }

    void NotifyDispatcher()
    {
        g_dispatcherCv.notify_all();
    }

    void WaitDispatcher(std::unique_lock<std::mutex>& lock, const std::function<bool()>& ready)
    {
        g_dispatcherCv.wait(lock, ready);
    }

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
        struct ThreadWait
        {
            const char* what;
            uint32_t object, detail, lr;
        };
        std::mutex g_threadWaitMutex;
        std::map<uint32_t, ThreadWait> g_threadWaits;
    }

    void SetThreadWait(uint32_t thread, const char* what, uint32_t object, uint32_t detail, uint32_t lr)
    {
        std::lock_guard lock(g_threadWaitMutex);
        g_threadWaits[thread] = { what, object, detail, lr };
    }

    void ClearThreadWait(uint32_t thread)
    {
        std::lock_guard lock(g_threadWaitMutex);
        g_threadWaits.erase(thread);
    }

    void DumpThreadWaits()
    {
        std::lock_guard lock(g_threadWaitMutex);
        for (const auto& [thread, wait] : g_threadWaits)
            fprintf(stderr, "[kernel]   thread %08X waits on %s %08X (%08X) lr=0x%08X\n", thread, wait.what, wait.object, wait.detail, wait.lr);
    }

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

    uint32_t CreateHandle(std::shared_ptr<KernelObject> object)
    {
        std::lock_guard lock(g_handleMutex);
        uint32_t handle = g_nextHandle;
        g_nextHandle += 4;
        g_handles.emplace(handle, std::move(object));
        return handle;
    }

    std::shared_ptr<KernelObject> LookupHandle(uint32_t handle)
    {
        if (handle == kCurrentThreadHandle)
            return CurrentThreadObject();

        std::lock_guard lock(g_handleMutex);
        auto it = g_handles.find(handle);
        return it != g_handles.end() ? it->second : nullptr;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(g_handleMutex);
        return g_handles.erase(handle) != 0;
    }

    std::shared_ptr<KernelObject> FindNamedObject(const std::string& name)
    {
        std::lock_guard lock(g_nameMutex);
        auto it = g_names.find(name);
        return it != g_names.end() ? it->second.lock() : nullptr;
    }

    void RegisterNamedObject(const std::shared_ptr<KernelObject>& object)
    {
        std::lock_guard lock(g_nameMutex);
        g_names[object->name] = object;
    }

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

    void QueueUserApc(Thread& thread, const Thread::UserApc& apc)
    {
        auto lock = DispatcherLock();
        thread.userApcs.push_back(apc);
        NotifyDispatcher();
    }

    void PostCompletion(IoCompletion& port, const IoCompletion::Packet& packet)
    {
        auto lock = DispatcherLock();
        port.packets.push_back(packet);
        NotifyDispatcher();
    }

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

    void BindGuestObject(const std::shared_ptr<KernelObject>& object, uint32_t header)
    {
        auto lock = DispatcherLock();
        object->guestHeader = header;
        *GuestPtr<uint8_t>(header) = uint8_t(object->type);
        object->SyncHeader();
        g_guestObjects[header] = object;
    }

    uint32_t GuestObjectFor(const std::shared_ptr<KernelObject>& object)
    {
        if (object->guestHeader == 0)
            BindGuestObject(object, SystemAlloc(0x20));
        return object->guestHeader;
    }

    std::shared_ptr<Thread> CurrentThreadObject()
    {
        return t_threadObject;
    }

    void RegisterThreadObject(const std::shared_ptr<Thread>& thread)
    {
        t_threadObject = thread;
    }
}
