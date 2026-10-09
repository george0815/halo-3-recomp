#pragma once

#include <cstdint>
#include <memory>

struct PPCContext;

namespace h3::kernel
{
    struct Thread;

    // Guest-visible state of one guest thread. Layouts follow Xenia's
    // X_KPCR and X_KTHREAD (src/xenia/kernel/xthread.h).
    struct GuestThread
    {
        uint32_t pcr = 0;         // r13 points here
        uint32_t kthread = 0;     // X_KTHREAD, also the "thread id" critical sections store
        uint32_t stackBase = 0;   // high address
        uint32_t stackLimit = 0;  // low address
        uint32_t tlsStatic = 0;   // __declspec(thread) data, KPCR.tls_ptr
        uint32_t tlsSlots = 0;    // KeTlsGetValue/KeTlsSetValue slots
    };

    // KPCR field offsets the runtime fills in.
    constexpr uint32_t kPcrTlsPtr = 0x000;
    constexpr uint32_t kPcrSelf = 0x030;
    constexpr uint32_t kPcrStackBase = 0x070;
    constexpr uint32_t kPcrStackLimit = 0x074;
    constexpr uint32_t kPcrCurrentThread = 0x100;
    constexpr uint32_t kPcrCurrentCpu = 0x10C;
    constexpr uint32_t kPcrSize = 0x2D8;
    constexpr uint32_t kKThreadSize = 0xAB0;

    // Allocates the KPCR, KTHREAD, stack and TLS for a guest thread.
    GuestThread* AllocateGuestThread(uint32_t stackSize);

    // Binds a guest thread to the calling host thread and points ctx.r1/r13
    // at it.
    void BindGuestThread(GuestThread* thread, PPCContext& ctx);

    // The guest thread bound to the calling host thread.
    GuestThread* CurrentThread();

    // Sets up the calling host thread as a guest thread with its own Thread
    // object: the process's main thread, or runtime threads that call guest
    // code (GPU interrupts).
    std::shared_ptr<Thread> AdoptHostThread(PPCContext& ctx, uint32_t stackSize);

    // ExCreateThread. With xapiStartup set, the thread runs
    // xapiStartup(start, context); otherwise start(context).
    std::shared_ptr<Thread> StartGuestThread(uint32_t stackSize, uint32_t xapiStartup, uint32_t start,
        uint32_t context, bool suspended);

    // Returns the previous suspend count.
    uint32_t ResumeThread(Thread& thread);

    // Runs the calling thread's queued user APCs (after an alertable wait
    // returned STATUS_USER_APC). ctx's registers are preserved.
    void DeliverUserApcs(PPCContext& ctx);

    // Ends the calling guest thread (ExTerminateThread). Unwinds the host
    // stack back to the thread's entry point.
    [[noreturn]] void ExitCurrentThread(uint32_t exitCode);

    // Process-wide dynamic TLS slot allocation.
    uint32_t TlsAlloc();  // 0xFFFFFFFF when none are left
    bool TlsFree(uint32_t slot);
}
