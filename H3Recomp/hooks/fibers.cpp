// Halo 3's fibers. The title links XAPI's CreateFiber/SwitchToFiber/
// DeleteFiber statically and runs some work (map decompression) on fibers
// with their own stacks.
//
// The real SwitchToFiber saves the caller's registers into the current
// fiber, loads the target fiber's (including r1 and LR) and returns into the
// target. Recompiled code returns through the host call stack, not the guest
// LR, so that can't be translated. Instead each guest fiber runs on a host
// fiber of its own (own host stack, own PPCContext), and switching swaps
// host contexts. The guest-visible state the real switch maintains (the
// thread's current fiber and the KPCR stack bounds) is kept up to date.

#include <ppc_recomp_shared.h>

#include "../kernel/imports.h"
#include "../kernel/thread.h"
#include "../memory.h"

#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#endif

using namespace h3;
using namespace h3::kernel;

namespace
{
    // XAPI functions in default.xex.
    constexpr uint32_t kSwitchToFiber = 0x8259FE40;  // void SwitchToFiber(FIBER* fiber)
    constexpr uint32_t kDeleteFiber = 0x8259FDE0;    // void DeleteFiber(FIBER* fiber)

    // KTHREAD field holding the current fiber.
    constexpr uint32_t kKThreadFiber = 0x164;

    // FIBER: fiber data, stack top (twice), stack limit, then the saved
    // context the real switch fills in: LR, r1, r14-r31, CR.
    constexpr uint32_t kFiberStackBase = 0x08;
    constexpr uint32_t kFiberStackLimit = 0x0C;
    constexpr uint32_t kFiberSavedLr = 0x1C;
    constexpr uint32_t kFiberSavedR1 = 0x30;
    constexpr uint32_t kFiberSavedR14 = 0x98;

    // Host stack for each new fiber. Recompiled frames are larger than the
    // guest's, so this is generous; untouched pages cost nothing.
    constexpr size_t kHostStackSize = 8 * 1024 * 1024;

    struct HostFiber
    {
        uint32_t guestFiber = 0;
        bool started = false;
        PPCContext startContext{};  // template for a fiber's first run
#ifdef _WIN32
        void* handle = nullptr;
#else
        ucontext_t context{};
        std::unique_ptr<uint8_t[]> stack;
#endif
    };

    std::mutex g_mutex;
    std::unordered_map<uint32_t, std::unique_ptr<HostFiber>> g_fibers;

    // A new fiber's first run, on its own host stack: load the saved
    // registers the real switch would restore and call the saved LR (the
    // fiber start thunk).
    [[noreturn]] void RunFiber(HostFiber& fiber)
    {
        PPCContext ctx = fiber.startContext;
        uint32_t guest = fiber.guestFiber;
        ctx.r1.u64 = GuestRef<uint64_t>(guest + kFiberSavedR1);
        constexpr PPCRegister PPCContext::* kNonVolatile[] = { &PPCContext::r14, &PPCContext::r15,
            &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19, &PPCContext::r20,
            &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24, &PPCContext::r25,
            &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29, &PPCContext::r30,
            &PPCContext::r31 };
        for (uint32_t i = 0; i < 18; i++)
            (ctx.*kNonVolatile[i]).u64 = GuestRef<uint64_t>(guest + kFiberSavedR14 + i * 8);
        uint32_t entry = GuestRef<uint32_t>(guest + kFiberSavedLr);
        (PPC_LOOKUP_FUNC(g_base, entry))(ctx, g_base);

        // The start thunk ends the thread instead of returning.
        GuestFatal("fiber returned", ctx);
    }

    // Where a new fiber's host stack starts: runs the guest fiber routine.
#ifdef _WIN32
    void WINAPI FiberEntry(void* parameter)
    {
        RunFiber(*static_cast<HostFiber*>(parameter));
    }
#else
    thread_local HostFiber* t_startingFiber = nullptr;

    void FiberEntry()
    {
        RunFiber(*t_startingFiber);
    }
#endif

    // The host fiber for a guest fiber. A guest fiber seen for the first time
    // as the *current* fiber is the thread's own (ConvertThreadToFiber): it
    // adopts the running host context. Otherwise it's a new fiber that gets
    // its own host stack.
    HostFiber& Lookup(uint32_t guestFiber, bool current)
    {
        std::lock_guard lock(g_mutex);
        auto& slot = g_fibers[guestFiber];
        if (slot)
            return *slot;

        slot = std::make_unique<HostFiber>();
        slot->guestFiber = guestFiber;
        slot->started = current;
#ifdef _WIN32
        if (current)
        {
            thread_local bool converted = false;
            if (!converted)
            {
                ConvertThreadToFiber(nullptr);
                converted = true;
            }
            slot->handle = GetCurrentFiber();
        }
        else
        {
            slot->handle = CreateFiber(kHostStackSize, FiberEntry, slot.get());
        }
#else
        if (!current)
        {
            slot->stack = std::make_unique_for_overwrite<uint8_t[]>(kHostStackSize);
            getcontext(&slot->context);
            slot->context.uc_stack.ss_sp = slot->stack.get();
            slot->context.uc_stack.ss_size = kHostStackSize;
            slot->context.uc_link = nullptr;
            makecontext(&slot->context, FiberEntry, 0);
        }
#endif
        return *slot;
    }
}

// void SwitchToFiber(FIBER* fiber)
PPC_FUNC(sub_8259FE40)
{
    static_assert(kSwitchToFiber == 0x8259FE40);
    uint32_t target = ctx.r3.u32;
    GuestThread* thread = CurrentThread();
    uint32_t current = GuestRef<uint32_t>(thread->kthread + kKThreadFiber);
    if (current == 0)
        GuestFatal("SwitchToFiber on a thread that isn't a fiber", ctx);
    if (current == target)
        return;

    HostFiber& from = Lookup(current, true);
    HostFiber& to = Lookup(target, false);

    // What the real switch leaves behind (via KeSetCurrentStackPointers).
    GuestRef<uint32_t>(thread->kthread + kKThreadFiber) = target;
    GuestRef<uint32_t>(thread->pcr + kPcrStackBase) = GuestRef<uint32_t>(target + kFiberStackBase).get();
    GuestRef<uint32_t>(thread->pcr + kPcrStackLimit) = GuestRef<uint32_t>(target + kFiberStackLimit).get();

    if (!to.started)
    {
        to.started = true;
        to.startContext = ctx;
    }

#ifdef _WIN32
    SwitchToFiber(to.handle);
#else
    t_startingFiber = &to;
    swapcontext(&from.context, &to.context);
#endif
    // Back on this fiber: someone switched to it. Its registers are this
    // call's ctx, untouched.
}

// void DeleteFiber(FIBER* fiber)
PPC_FUNC_IMPL(__imp__sub_8259FDE0);
PPC_FUNC(sub_8259FDE0)
{
    static_assert(kDeleteFiber == 0x8259FDE0);
    uint32_t fiber = ctx.r3.u32;
    __imp__sub_8259FDE0(ctx, base);

    std::lock_guard lock(g_mutex);
    auto it = g_fibers.find(fiber);
    if (it != g_fibers.end())
    {
#ifdef _WIN32
        if (it->second->handle != nullptr && it->second->handle != GetCurrentFiber())
            DeleteFiber(it->second->handle);
#endif
        g_fibers.erase(it);
    }
}
