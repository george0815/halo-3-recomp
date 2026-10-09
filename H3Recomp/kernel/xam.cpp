// xam.xex exports. Reference: Xenia's src/xenia/kernel/xam/.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "module.h"
#include "objects.h"
#include "thread.h"
#include "../memory.h"

#include <algorithm>
#include <cstdio>

using namespace h3;
using namespace h3::kernel;

// --- Tasks -----------------------------------------------------------------

// DWORD XamTaskSchedule(PVOID callback, PVOID message, PDWORD flags, PHANDLE handle)
//
// Runs callback(message) asynchronously. Halo 3 schedules disk-cache work
// this way and then waits on an event the callback signals, so the task has
// to really run. Like Xenia, each task gets its own guest thread.
PPC_FUNC(__imp__XamTaskSchedule)
{
    uint32_t stackSize = std::max(0x4000u, (g_module.stackSize + 0xFFF) & ~0xFFFu);
    auto thread = StartGuestThread(stackSize, 0, ctx.r3.u32, ctx.r4.u32, false);
    if (ctx.r6.u32 != 0)
        GuestRef<uint32_t>(ctx.r6.u32) = CreateHandle(thread);
    ctx.r3.u64 = kStatusSuccess;
}

// BOOL XamTaskCloseHandle(HANDLE handle)
PPC_FUNC(__imp__XamTaskCloseHandle)
{
    ctx.r3.u64 = CloseHandle(ctx.r3.u32) ? 1 : 0;
}

// --- Loader ------------------------------------------------------------------

// void XamShowDirtyDiscErrorUI(DWORD userIndex)
// The game hit a disc read it couldn't recover from; on the console this
// shows an error and never returns.
PPC_FUNC(__imp__XamShowDirtyDiscErrorUI)
{
    GuestFatal("disc read error (XamShowDirtyDiscErrorUI)", ctx);
}

// void XamLoaderLaunchTitle(LPCSTR path, DWORD flags)
// Relaunching isn't supported; on the console this doesn't return.
PPC_FUNC(__imp__XamLoaderLaunchTitle)
{
    const char* path = ctx.r3.u32 != 0 ? GuestPtr<const char>(ctx.r3.u32) : "(null)";
    fprintf(stderr, "[xam] title asked to launch %s\n", path);
    GuestFatal("XamLoaderLaunchTitle", ctx);
}

// --- Input -------------------------------------------------------------------
//
// No controllers yet (XInput -> SDL gamepad is roadmap phase 4). Reporting
// them as unplugged matters: answering success without filling in the state
// lets the game read stack garbage as button presses (which skips the intro
// movie, among other things).

constexpr uint32_t kErrorDeviceNotConnected = 0x48F;

// DWORD XamInputGetCapabilities(DWORD userIndex, DWORD flags, PXINPUT_CAPABILITIES caps)
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    ctx.r3.u64 = kErrorDeviceNotConnected;
}

// DWORD XamInputGetState(DWORD userIndex, DWORD flags, PXINPUT_STATE state)
PPC_FUNC(__imp__XamInputGetState)
{
    ctx.r3.u64 = kErrorDeviceNotConnected;
}

// DWORD XamInputSetState(DWORD userIndex, DWORD flags, PXINPUT_VIBRATION vibration)
PPC_FUNC(__imp__XamInputSetState)
{
    ctx.r3.u64 = kErrorDeviceNotConnected;
}
