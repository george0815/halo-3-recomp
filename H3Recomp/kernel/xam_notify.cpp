// xam.xex notification exports: listeners and XNotifyGetNext.
// Reference: Xenia's src/xenia/kernel/xam/xam_notify.cc.
//
// A listener is a kernel object, signaled while it has notifications
// queued. New listeners get the console's current state: who is signed in
// and the system UI closed. Titles track sign-in through these (Halo keeps
// its controller -> user map from XN_SYS_SIGNINCHANGED).

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "config/env.h"
#include "objects.h"
#include "../memory.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

using namespace h3;
using namespace h3::kernel;

namespace
{
    // Notification ids carry their area in bits 25-30; the listener mask
    // has one bit per area.
    constexpr uint32_t kXnSysUi = 0x00000009;
    constexpr uint32_t kXnSysSigninChanged = 0x0000000A;

    // The mask bit a notification id's area maps to.
    uint32_t AreaBit(uint32_t id) { return 1u << ((id >> 25) & 0x3F); }

    // XNotifyCreateListener's object: a queue of notifications for the
    // areas in its mask. Waiting on it waits for one to arrive.
    struct Listener : KernelObject
    {
        // Acts as a notification event: signaled while notifications are queued.
        explicit Listener(uint64_t mask) : KernelObject(ObjectType::NotificationEvent), mask(mask) {}
        bool IsSignaled(uint32_t) const override { return !queue.empty(); }
        int32_t SignalState() const override { return !queue.empty(); }

        // One queued notification: its id and parameter.
        struct Notification
        {
            uint32_t id;
            uint32_t param;
        };
        uint64_t mask;
        std::deque<Notification> queue;  // guarded by DispatcherLock()
    };

    // Every listener created, for BroadcastNotification.
    std::mutex g_listenersMutex;
    std::vector<std::weak_ptr<Listener>> g_listeners;

    // H3_LOG_NOTIFY=1: log notification listener calls.
    bool LogNotify()
    {
        static const bool log = EnvFlag("H3_LOG_NOTIFY");
        return log;
    }
}

// HANDLE XamNotifyCreateListener(QWORD areas, DWORD version)
PPC_FUNC(__imp__XamNotifyCreateListener)
{
    uint64_t mask = ctx.r3.u64;
    auto listener = std::make_shared<Listener>(mask);
    {
        auto lock = DispatcherLock();
        for (Listener::Notification n : { Listener::Notification{ kXnSysUi, 0 }, Listener::Notification{ kXnSysSigninChanged, SignedInUsers() } })
            if (mask & AreaBit(n.id))
                listener->queue.push_back(n);
        listener->SyncHeader();
        NotifyDispatcher();
    }
    {
        std::lock_guard lock(g_listenersMutex);
        std::erase_if(g_listeners, [](const std::weak_ptr<Listener>& l) { return l.expired(); });
        g_listeners.push_back(listener);
    }
    if (LogNotify())
        fprintf(stderr, "[xam] XamNotifyCreateListener areas %llX\n", static_cast<unsigned long long>(mask));
    ctx.r3.u64 = CreateHandle(listener);
}

// Queues a notification (sign-in changed, system UI shown...) on every
// listener whose mask includes its area, and wakes their waiters.
void h3::kernel::BroadcastNotification(uint32_t id, uint32_t param)
{
    std::vector<std::shared_ptr<Listener>> listeners;
    {
        std::lock_guard lock(g_listenersMutex);
        for (const std::weak_ptr<Listener>& weak : g_listeners)
            if (auto listener = weak.lock())
                listeners.push_back(listener);
    }
    auto lock = DispatcherLock();
    for (const auto& listener : listeners)
        if (listener->mask & AreaBit(id))
        {
            listener->queue.push_back({ id, param });
            listener->SyncHeader();
        }
    NotifyDispatcher();
    if (LogNotify())
        fprintf(stderr, "[xam] notification %08X (%08X) to %zu listeners\n", id, param, listeners.size());
}

// BOOL XNotifyGetNext(HANDLE listener, DWORD matchId, PDWORD id, PULONG_PTR param)
// Pops the next notification, or the first one with matchId if that's set.
PPC_FUNC(__imp__XNotifyGetNext)
{
    auto listener = LookupHandleAs<Listener>(ctx.r3.u32);
    uint32_t matchId = ctx.r4.u32, idOut = ctx.r5.u32, paramOut = ctx.r6.u32;
    ctx.r3.u64 = 0;
    if (!listener)
        return;
    Listener::Notification found{};
    bool have = false;
    {
        auto lock = DispatcherLock();
        for (auto it = listener->queue.begin(); it != listener->queue.end(); ++it)
        {
            if (matchId != 0 && it->id != matchId)
                continue;
            found = *it;
            listener->queue.erase(it);
            have = true;
            break;
        }
        listener->SyncHeader();
    }
    if (!have)
        return;
    if (LogNotify())
        fprintf(stderr, "[xam] XNotifyGetNext -> %08X (%08X)\n", found.id, found.param);
    if (idOut != 0)
        GuestRef<uint32_t>(idOut) = found.id;
    if (paramOut != 0)
        GuestRef<uint32_t>(paramOut) = found.param;
    ctx.r3.u64 = 1;
}

// DWORD XNotifyDelayUI(DWORD milliseconds) / XNotifyPositionUI(DWORD position)
// No system UI to move or delay.
PPC_FUNC(__imp__XNotifyDelayUI)
{
    ctx.r3.u64 = 0;
}

// Returns success: nothing to position.
PPC_FUNC(__imp__XNotifyPositionUI)
{
    ctx.r3.u64 = 0;
}
