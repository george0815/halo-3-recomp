// xam.xex exports. Reference: Xenia's src/xenia/kernel/xam/.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "module.h"
#include "objects.h"
#include "thread.h"
#include "../input/input.h"
#include "../memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

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
// One controller, user 0, from the keyboard and the first SDL gamepad
// (input/input.cpp). Other users stay unplugged: answering success without
// filling in the state lets the game read stack garbage as button presses.

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kUserIndexAny = 0xFF;

static bool IsConnectedUser(uint32_t userIndex)
{
    return userIndex == 0 || userIndex == kUserIndexAny;
}

// XINPUT_GAMEPAD, 12 bytes big-endian.
static void WriteGamepad(uint32_t address, const input::GamepadState& state)
{
    GuestRef<uint16_t>(address) = state.buttons;
    *GuestPtr<uint8_t>(address + 2) = state.leftTrigger;
    *GuestPtr<uint8_t>(address + 3) = state.rightTrigger;
    GuestRef<uint16_t>(address + 4) = uint16_t(state.thumbLX);
    GuestRef<uint16_t>(address + 6) = uint16_t(state.thumbLY);
    GuestRef<uint16_t>(address + 8) = uint16_t(state.thumbRX);
    GuestRef<uint16_t>(address + 10) = uint16_t(state.thumbRY);
}

// DWORD XamInputGetCapabilities(DWORD userIndex, DWORD flags, PXINPUT_CAPABILITIES caps)
// XINPUT_CAPABILITIES: Type, SubType, WORD Flags, XINPUT_GAMEPAD, XINPUT_VIBRATION.
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    static const bool log = getenv("H3_LOG_INPUT") != nullptr;
    if (log)
    {
        static uint32_t calls = 0;
        if (calls++ < 20)
            fprintf(stderr, "[xam] XamInputGetCapabilities user %u flags %u (lr=0x%08X)\n", ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr));
    }
    if (!IsConnectedUser(ctx.r3.u32) || ctx.r5.u32 == 0)
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    uint32_t caps = ctx.r5.u32;
    *GuestPtr<uint8_t>(caps) = 1;      // XINPUT_DEVTYPE_GAMEPAD
    *GuestPtr<uint8_t>(caps + 1) = 1;  // XINPUT_DEVSUBTYPE_GAMEPAD
    GuestRef<uint16_t>(caps + 2) = 0;
    input::GamepadState all;           // every control present
    all.buttons = 0xFFFF;
    all.leftTrigger = all.rightTrigger = 0xFF;
    all.thumbLX = all.thumbLY = all.thumbRX = all.thumbRY = int16_t(0xFFC0);
    WriteGamepad(caps + 4, all);
    GuestRef<uint16_t>(caps + 16) = 0xFFFF;  // vibration motors
    GuestRef<uint16_t>(caps + 18) = 0xFFFF;
    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamInputGetState(DWORD userIndex, DWORD flags, PXINPUT_STATE state)
// XINPUT_STATE: DWORD dwPacketNumber, XINPUT_GAMEPAD.
PPC_FUNC(__imp__XamInputGetState)
{
    static const bool log = getenv("H3_LOG_INPUT") != nullptr;
    if (log)
    {
        static uint32_t calls = 0;
        if ((++calls & 1023) == 1)
            fprintf(stderr, "[xam] XamInputGetState #%u user %u flags %u (lr=0x%08X)\n", calls, ctx.r3.u32, ctx.r4.u32,
                uint32_t(ctx.lr));
    }
    if (!IsConnectedUser(ctx.r3.u32))
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }
    if (ctx.r5.u32 != 0)
    {
        input::GamepadState state = input::GetState();
        GuestRef<uint32_t>(ctx.r5.u32) = state.packetNumber;
        WriteGamepad(ctx.r5.u32 + 4, state);
    }
    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamInputSetState(DWORD userIndex, DWORD flags, PXINPUT_VIBRATION vibration)
// Rumble isn't forwarded.
PPC_FUNC(__imp__XamInputSetState)
{
    ctx.r3.u64 = IsConnectedUser(ctx.r3.u32) ? kErrorSuccess : kErrorDeviceNotConnected;
}
