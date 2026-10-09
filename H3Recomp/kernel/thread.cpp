#include "thread.h"
#include "module.h"
#include "objects.h"

#include "../memory.h"

#include <ppc_recomp_shared.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace h3::kernel
{
    namespace
    {
        thread_local GuestThread* t_currentThread = nullptr;

        std::mutex g_tlsMutex;
        std::vector<bool> g_tlsUsed;

        std::atomic<uint32_t> g_nextThreadId{ 1 };

        // Thrown by ExitCurrentThread, caught at the thread's entry point.
        struct ThreadExit
        {
            uint32_t exitCode;
        };

        std::shared_ptr<Thread> MakeThreadObject(GuestThread* guest)
        {
            auto thread = std::make_shared<Thread>();
            thread->guest = guest;
            thread->threadId = g_nextThreadId++;
            BindGuestObject(thread, guest->kthread);
            return thread;
        }
    }

    GuestThread* AllocateGuestThread(uint32_t stackSize)
    {
        auto* thread = new GuestThread();

        stackSize = (std::max(stackSize, 0x4000u) + kPageSize4K - 1) & ~(kPageSize4K - 1);
        thread->stackLimit = SystemAlloc(stackSize);
        thread->stackBase = thread->stackLimit + stackSize;

        // Static TLS first, then the dynamic slots (Xenia's layout). The
        // static block starts as a copy of the XEX's TLS template.
        uint32_t staticSize = (g_module.tlsDataSize + 15) & ~15u;
        thread->tlsStatic = SystemAlloc(staticSize + g_module.tlsSlotCount * 4);
        thread->tlsSlots = thread->tlsStatic + staticSize;
        if (g_module.tlsRawDataAddress != 0)
        {
            memcpy(GuestPtr<uint8_t>(thread->tlsStatic), GuestPtr<uint8_t>(g_module.tlsRawDataAddress),
                g_module.tlsRawDataSize);
        }

        thread->kthread = SystemAlloc(kKThreadSize);

        thread->pcr = SystemAlloc(kPcrSize);
        GuestRef<uint32_t>(thread->pcr + kPcrTlsPtr) = thread->tlsStatic;
        GuestRef<uint32_t>(thread->pcr + kPcrSelf) = thread->pcr;
        GuestRef<uint32_t>(thread->pcr + kPcrStackBase) = thread->stackBase;
        GuestRef<uint32_t>(thread->pcr + kPcrStackLimit) = thread->stackLimit;
        GuestRef<uint32_t>(thread->pcr + kPcrCurrentThread) = thread->kthread;
        *GuestPtr<uint8_t>(thread->pcr + kPcrCurrentCpu) = 0;

        return thread;
    }

    void BindGuestThread(GuestThread* thread, PPCContext& ctx)
    {
        // Leave room for the back chain and LR save area above r1.
        ctx.r1.u64 = thread->stackBase - 0x100;
        ctx.r13.u64 = thread->pcr;
        t_currentThread = thread;
    }

    GuestThread* CurrentThread()
    {
        return t_currentThread;
    }

    std::shared_ptr<Thread> AdoptHostThread(PPCContext& ctx, uint32_t stackSize)
    {
        GuestThread* guest = AllocateGuestThread(stackSize);
        BindGuestThread(guest, ctx);
        auto thread = MakeThreadObject(guest);
        RegisterThreadObject(thread);
        return thread;
    }

    std::shared_ptr<Thread> StartGuestThread(uint32_t stackSize, uint32_t xapiStartup, uint32_t start,
        uint32_t context, bool suspended)
    {
        auto thread = MakeThreadObject(AllocateGuestThread(stackSize));
        thread->suspendCount = suspended ? 1 : 0;

        std::thread([thread, xapiStartup, start, context]
        {
            RegisterThreadObject(thread);

            {
                auto lock = DispatcherLock();
                WaitDispatcher(lock, [&] { return thread->suspendCount == 0; });
            }

            PPCContext ctx{};
            BindGuestThread(thread->guest, ctx);

            uint32_t exitCode;
            try
            {
                uint32_t entry = xapiStartup ? xapiStartup : start;
                ctx.r3.u64 = xapiStartup ? start : context;
                ctx.r4.u64 = context;
                (PPC_LOOKUP_FUNC(g_base, entry))(ctx, g_base);
                exitCode = ctx.r3.u32;
            }
            catch (const ThreadExit& exit)
            {
                exitCode = exit.exitCode;
            }

            auto lock = DispatcherLock();
            thread->exited = true;
            thread->exitCode = exitCode;
            thread->SyncHeader();
            NotifyDispatcher();
        }).detach();

        return thread;
    }

    void DeliverUserApcs(PPCContext& ctx)
    {
        auto thread = CurrentThreadObject();
        for (;;)
        {
            Thread::UserApc apc;
            {
                auto lock = DispatcherLock();
                if (thread->userApcs.empty())
                    return;
                apc = thread->userApcs.front();
                thread->userApcs.pop_front();
            }
            PPCContext apcCtx = ctx;
            apcCtx.r3.u64 = apc.arg1;
            apcCtx.r4.u64 = apc.arg2;
            apcCtx.r5.u64 = apc.arg3;
            (PPC_LOOKUP_FUNC(g_base, apc.routine))(apcCtx, g_base);
        }
    }

    uint32_t ResumeThread(Thread& thread)
    {
        auto lock = DispatcherLock();
        uint32_t previous = thread.suspendCount;
        if (thread.suspendCount > 0 && --thread.suspendCount == 0)
            NotifyDispatcher();
        return previous;
    }

    void ExitCurrentThread(uint32_t exitCode)
    {
        throw ThreadExit{ exitCode };
    }

    uint32_t TlsAlloc()
    {
        std::lock_guard lock(g_tlsMutex);
        g_tlsUsed.resize(g_module.tlsSlotCount);
        for (uint32_t slot = 0; slot < g_tlsUsed.size(); slot++)
        {
            if (!g_tlsUsed[slot])
            {
                g_tlsUsed[slot] = true;
                return slot;
            }
        }
        return 0xFFFFFFFF;
    }

    bool TlsFree(uint32_t slot)
    {
        std::lock_guard lock(g_tlsMutex);
        if (slot >= g_tlsUsed.size() || !g_tlsUsed[slot])
            return false;
        g_tlsUsed[slot] = false;
        return true;
    }
}
