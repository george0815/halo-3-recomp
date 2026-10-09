// xboxkrnl.exe exports. Each PPC_FUNC(__imp__Name) defined here overrides the
// generated weak stub of the same name. Xenia's src/xenia/kernel/xboxkrnl/ is
// the reference for what each export is expected to do.
//
// Arguments arrive in r3..r10 and the result goes in r3.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "module.h"
#include "../memory.h"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace h3;
using namespace h3::kernel;

// void DbgPrint(const char* format, ...)
// Prints the raw format string only; enough to see what the game is saying.
PPC_FUNC(__imp__DbgPrint)
{
    fprintf(stderr, "[guest] %s", GuestPtr<const char>(ctx.r3.u32));
    ctx.r3.u64 = 0;
}

// void HalReturnToFirmware(FIRMWARE_REENTRY routine)
// How a title exits. The CRT also lands here on fatal startup errors.
PPC_FUNC(__imp__HalReturnToFirmware)
{
    GuestFatal("HalReturnToFirmware", ctx);
}

// void KeBugCheck(ULONG code)
PPC_FUNC(__imp__KeBugCheck)
{
    GuestFatal("KeBugCheck", ctx);
}

// void KeBugCheckEx(ULONG code, ULONG_PTR p1, ULONG_PTR p2, ULONG_PTR p3, ULONG_PTR p4)
PPC_FUNC(__imp__KeBugCheckEx)
{
    GuestFatal("KeBugCheckEx", ctx);
}

// ULONG KeGetCurrentProcessType()
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    constexpr uint32_t kProcessTypeUser = 1;
    ctx.r3.u64 = kProcessTypeUser;
}

// BOOL XexCheckExecutablePrivilege(ULONG privilege)
// The privilege is a bit index into the XEX system flags.
PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    uint32_t privilege = ctx.r3.u32;
    ctx.r3.u64 = privilege < 32 ? (g_module.systemFlags >> privilege) & 1 : 0;
}

// void KeQuerySystemTime(PLARGE_INTEGER time)
// FILETIME: 100 ns intervals since 1601-01-01.
PPC_FUNC(__imp__KeQuerySystemTime)
{
    constexpr uint64_t kUnixEpochAsFileTime = 116444736000000000ull;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t fileTime = kUnixEpochAsFileTime + std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100;
    if (ctx.r3.u32 != 0)
        GuestRef<uint64_t>(ctx.r3.u32) = fileTime;
}

// LONGLONG KeQueryPerformanceFrequency()
// XenonRecomp compiles mftb to the host's __rdtsc(), so the frequency the
// game divides by has to be the host TSC's, not the 360's 50 MHz time base.
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    static const uint64_t frequency = []
    {
        auto start = std::chrono::steady_clock::now();
        uint64_t ticks = __rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint64_t elapsedTicks = __rdtsc() - ticks;
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        return uint64_t(elapsedTicks / seconds);
    }();
    ctx.r3.u64 = frequency;
}
