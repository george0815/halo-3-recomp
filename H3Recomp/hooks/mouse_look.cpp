// Native mouse look (private/re/mouse_look.md). The look function,
// sub_82211EA8 (MCC FUN_140448a70), turns a player's right stick into this
// tick's change of view through a response curve and an acceleration ramp,
// and writes it to its output (r8): yaw at +0x10 (radians, growing to the
// left), pitch at +0x14 (growing up). With native mouse look (input.h) the
// mouse no longer drives player 1's stick; its movement since the last
// tick is added to those two after the original runs, the way MCC's PC
// version skips the curve and acceleration for the mouse. The game then
// applies them as it does the stick's: pitch limits, aim assist.
//
// Zoom: the look function caps the turn rates by the unit's weapons
// (sub_82098B50, called for both weapon slots, returns a weapon's rate:
// pitch at most that, yaw at most twice that). MCC scales the mouse by the
// capped rates over the defaults (120 and 60 degrees a second), so a
// zoomed weapon turns slower under the mouse too; the lowest weapon rate
// seen during the call gives the same scale here.
//
// H3_LOG_LOOK=<call>,<call>,... (research): snapshots what the function can
// reach through its pointer arguments at those calls (one per tick, counted
// from the first) and logs the words that changed.

#include "hooks/mouse_look.h"
#include "config/env.h"
#include "config/settings.h"
#include "input/input.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kOutputYaw = 0x10;
    constexpr uint32_t kOutputPitch = 0x14;
    constexpr float kDefaultYawRate = 2.0943951f;    // 120 degrees a second
    constexpr float kDefaultPitchRate = 1.0471976f;  // 60

    // Whether a player sits in a vehicle or turret, as the look function
    // itself tells (it reads the seat's turn rates then): the player's
    // unit (player control record: r13's game-state globals +120, 236 bytes
    // a player, +16; the unit at +0) has a parent object (+16) and a seat
    // (+582, halfword) in the object table (globals +168: data at +68,
    // 16-byte entries, the object at +12).
    bool Seated(const PPCContext& ctx, uint32_t user)
    {
        using h3::GuestRef;
        uint32_t globals = GuestRef<uint32_t>(ctx.r13.u32);
        if (globals == 0)
            return false;
        uint32_t controls = GuestRef<uint32_t>(globals + 120);
        uint32_t objects = GuestRef<uint32_t>(globals + 168);
        if (controls == 0 || objects == 0)
            return false;
        uint32_t unit = GuestRef<uint32_t>(controls + user * 236 + 16);
        if (unit == 0xFFFFFFFF)
            return false;
        uint32_t data = GuestRef<uint32_t>(objects + 68);
        uint32_t object = data ? uint32_t(GuestRef<uint32_t>(data + (unit & 0xFFFF) * 16 + 12)) : 0;
        if (object == 0)
            return false;
        static const bool log = h3::EnvFlag("H3_LOG_MOUSE");
        static uint32_t logged = 0;
        if (log && logged++ % 120 == 0)
            fprintf(stderr, "[mouse] player %u unit %08X object %08X parent %08X seat %d\n", user, unit, object,
                uint32_t(GuestRef<uint32_t>(object + 16)), int(int16_t(GuestRef<uint16_t>(object + 582))));
                return GuestRef<uint32_t>(object + 16) != 0xFFFFFFFF && int16_t(GuestRef<uint16_t>(object + 582)) != -1;
    }

    // Player 1's unit object (0 if none), by the same chain as Seated.
    uint32_t UnitObject(const PPCContext& ctx, uint32_t user)
    {
        using h3::GuestRef;
        uint32_t globals = GuestRef<uint32_t>(ctx.r13.u32);
        uint32_t controls = globals ? uint32_t(GuestRef<uint32_t>(globals + 120)) : 0;
        uint32_t objects = globals ? uint32_t(GuestRef<uint32_t>(globals + 168)) : 0;
        if (controls == 0 || objects == 0)
            return 0;
        uint32_t unit = GuestRef<uint32_t>(controls + user * 236 + 16);
        uint32_t data = GuestRef<uint32_t>(objects + 68);
        return unit == 0xFFFFFFFF || data == 0 ? 0 : uint32_t(GuestRef<uint32_t>(data + (unit & 0xFFFF) * 16 + 12));
    }

    // H3_LOG_UNIT (research): player 1's unit object, the halfwords and bytes
    // that switch between -1 and 0..7, when they do (looking for zoom).
    void LogUnitFlips(const PPCContext& ctx, uint32_t calls)
    {
        static const bool log = h3::EnvFlag("H3_LOG_UNIT");
        static std::vector<uint8_t> last;
        if (!log)
            return;
        constexpr uint32_t kSpan = 0x800;
        // H3_LOG_UNIT=control: player 1's player control record instead.
        static const bool control = h3::EnvString("H3_LOG_UNIT") && strcmp(h3::EnvString("H3_LOG_UNIT"), "control") == 0;
        uint32_t object = UnitObject(ctx, 0);
        if (control)
        {
            uint32_t globals = h3::GuestRef<uint32_t>(ctx.r13.u32);
            uint32_t controls = globals ? uint32_t(h3::GuestRef<uint32_t>(globals + 120)) : 0;
            object = controls ? controls + 16 : 0;  // player 1's record, and the next ones
        }
        if (object < 0x40000000 || object > 0xC0000000 - kSpan)
            return;  // no unit (menus): the chain gives junk
        std::vector<uint8_t> now(h3::GuestPtr<uint8_t>(object), h3::GuestPtr<uint8_t>(object) + kSpan);
        if (last.size() == kSpan)
            for (uint32_t i = 0; i + 1 < kSpan; i++)
            {
                int a = int8_t(last[i]), b = int8_t(now[i]);
                if (a != b && ((a == -1 && b >= 0 && b < 8) || (b == -1 && a >= 0 && a < 8)))
                    fprintf(stderr, "[unit] call %u byte +%03X %d -> %d\n", calls, i, a, b);
            }
        last = std::move(now);
    }

    // The lowest weapon turn rate returned during the current look call
    // (only player 1's call collects).
    thread_local bool t_collecting = false;
    thread_local float t_weaponRate = 0;
    // A unit's zoom level: int16 at +0x290 of its object, -1 unzoomed, 0..
    // the weapon's zoom levels (found by diffing the object across scripted
    // zooms, private/re/mouse_look.md).
    constexpr uint32_t kUnitZoomLevel = 0x290;
    std::atomic<bool> g_player1Zoomed{ false };
    // The requested zoom level in a player's player control record
    // (int16; globals +120, 236 bytes a player, record at +16): what zoom
    // presses step through, and the unit follows. Found the same way.
    constexpr uint32_t kControlZoomLevel = 0x2A;
    std::atomic<bool> g_unzoomRequested{ false };

    constexpr uint32_t kSpan = 0x400;  // bytes snapshot behind each pointer

    struct Snapshot
    {
        uint32_t address = 0;
        std::vector<uint32_t> words;
    };

    Snapshot Take(uint32_t address)
    {
        Snapshot s{ address, std::vector<uint32_t>(kSpan / 4) };
        if (address >= 0x10000 && address < 0xFFFF0000)
            for (uint32_t i = 0; i < kSpan / 4; i++)
                s.words[i] = h3::GuestRef<uint32_t>(address + i * 4);
        return s;
    }
}

bool h3::hooks::Player1Zoomed()
{
    return g_player1Zoomed.load();
}

void h3::hooks::RequestPlayer1Unzoom()
{
    g_unzoomRequested.store(true);
}

PPC_FUNC_IMPL(__imp__sub_82211EA8);
PPC_FUNC(sub_82211EA8)
{
    static const char* list = h3::EnvString("H3_LOG_LOOK");
    static std::vector<uint32_t> calls;
    static bool parsed = false;
    if (!parsed)
    {
        parsed = true;
        if (list)
        {
            std::stringstream text(list);
            std::string item;
            while (std::getline(text, item, ','))
                calls.push_back(uint32_t(std::stoul(item)));
        }
    }
    uint32_t user = ctx.r3.u32;
    uint32_t args[3] = { ctx.r4.u32, ctx.r7.u32, ctx.r8.u32 };
    // Hold to zoom's release (and H3_TEST_UNZOOM_AT=<call>, for testing):
    // player 1's requested zoom level straight to none.
    static const uint32_t testUnzoomAt = h3::EnvNumber("H3_TEST_UNZOOM_AT");
    static uint32_t lookCalls = 0;
    if (user == 0 && (++lookCalls == testUnzoomAt || g_unzoomRequested.exchange(false)))
    {
        uint32_t globals = h3::GuestRef<uint32_t>(ctx.r13.u32);
        uint32_t controls = globals ? uint32_t(h3::GuestRef<uint32_t>(globals + 120)) : 0;
        if (controls >= 0x40000000 && controls < 0xC0000000)
            h3::GuestRef<uint16_t>(controls + 16 + kControlZoomLevel) = 0xFFFF;
    }
    bool mouse = user == 0 && args[2] != 0 && h3::input::NativeMouseLook();
    t_collecting = user == 0;  // player 1's weapon rate: the mouse's zoom scale, and Player1Zoomed
    t_weaponRate = 1e9f;
    __imp__sub_82211EA8(ctx, base);
    t_collecting = false;
    if (user == 0)
    {
        uint32_t object = UnitObject(ctx, 0);
        g_player1Zoomed.store(object >= 0x40000000 && object < 0xC0000000 &&
            int16_t(h3::GuestRef<uint16_t>(object + kUnitZoomLevel)) != -1);
        // H3_LOG_WEAPON_RATE: player 1's weapon turn cap whenever it
        // changes (weapon switches, zoom levels), with the look call count.
        static const bool logRate = h3::EnvFlag("H3_LOG_WEAPON_RATE");
        static float lastRate = -1;
        static uint32_t calls = 0;
        calls++;
        LogUnitFlips(ctx, calls);
        // Research: the look function's output halfword +32 (zoom level?).
        static int lastOut32 = 99;
        int out32 = args[2] ? int(int16_t(h3::GuestRef<uint16_t>(args[2] + 32))) : 99;
        if (logRate && out32 != lastOut32)
        {
            fprintf(stderr, "[weapon] call %u output +32 = %d\n", calls, out32);
            lastOut32 = out32;
        }
        static bool lastZoomed = false;
        if (logRate && g_player1Zoomed.load() != lastZoomed)
        {
            lastZoomed = g_player1Zoomed.load();
            fprintf(stderr, "[weapon] call %u %s\n", calls, lastZoomed ? "zoomed in" : "zoomed out");
        }
        if (logRate && t_weaponRate != lastRate)
        {
            fprintf(stderr, "[weapon] call %u rate %.4f%s\n", calls, t_weaponRate, g_player1Zoomed.load() ? " (zoomed)" : "");
            lastRate = t_weaponRate;
        }
    }
    if (mouse)
    {
        float yaw, pitch;
        h3::input::TakeMouseLook(yaw, pitch);
        if (yaw != 0 || pitch != 0)
        {
            float vehicle = Seated(ctx, user) ? h3::config::Active().controls.vehicleMouseSensitivity : 1.0f;
            float yawScale = std::min(1.0f, 2 * t_weaponRate / kDefaultYawRate) * vehicle;
            float pitchScale = std::min(1.0f, t_weaponRate / kDefaultPitchRate) * vehicle;
            static const bool log = h3::EnvFlag("H3_LOG_MOUSE");
            if (log)
                fprintf(stderr, "[mouse] yaw %.5f pitch %.5f, weapon rate %.4f%s -> scale %.3f / %.3f\n", yaw, pitch,
                    t_weaponRate, vehicle != 1.0f ? " (seated)" : "", yawScale, pitchScale);
            h3::GuestRef<float>(args[2] + kOutputYaw) = float(h3::GuestRef<float>(args[2] + kOutputYaw)) + yaw * yawScale;
            h3::GuestRef<float>(args[2] + kOutputPitch) = float(h3::GuestRef<float>(args[2] + kOutputPitch)) + pitch * pitchScale;
        }
    }
    if (calls.empty() || user != 0)
        return;
    static uint32_t count = 0;
    static std::map<int, Snapshot> last;
    count++;
    if (std::find(calls.begin(), calls.end(), count) == calls.end())
        return;
    static const char* names[3] = { "r4", "r7", "r8" };
    for (int a = 0; a < 3; a++)
    {
        Snapshot now = Take(args[a]);
        auto previous = last.find(a);
        if (previous != last.end() && previous->second.address == now.address)
        {
            for (uint32_t i = 0; i < kSpan / 4; i++)
                if (now.words[i] != previous->second.words[i])
                {
                    float before, after;
                    memcpy(&before, &previous->second.words[i], 4);
                    memcpy(&after, &now.words[i], 4);
                    fprintf(stderr, "[look] call %u %s+%03X: %08X -> %08X (%g -> %g)\n", count, names[a], i * 4,
                        previous->second.words[i], now.words[i], before, after);
                }
        }
        else
            fprintf(stderr, "[look] call %u %s = %08X\n", count, names[a], args[a]);
        last[a] = std::move(now);
    }
}

// float weapon turn rate (sub_82098B50): see the zoom note above.
PPC_FUNC_IMPL(__imp__sub_82098B50);
PPC_FUNC(sub_82098B50)
{
    __imp__sub_82098B50(ctx, base);
    if (t_collecting && ctx.f1.f64 > 0)
        t_weaponRate = std::min(t_weaponRate, float(ctx.f1.f64));
}
