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

#include "config/env.h"
#include "input/input.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <cstdio>
#include <algorithm>
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

    // The lowest weapon turn rate returned during the current look call
    // (only player 1's call collects).
    thread_local bool t_collecting = false;
    thread_local float t_weaponRate = 0;

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
    bool mouse = user == 0 && args[2] != 0 && h3::input::NativeMouseLook();
    t_collecting = mouse;
    t_weaponRate = 1e9f;
    __imp__sub_82211EA8(ctx, base);
    t_collecting = false;
    if (mouse)
    {
        float yaw, pitch;
        h3::input::TakeMouseLook(yaw, pitch);
        if (yaw != 0 || pitch != 0)
        {
            float yawScale = std::min(1.0f, 2 * t_weaponRate / kDefaultYawRate);
            float pitchScale = std::min(1.0f, t_weaponRate / kDefaultPitchRate);
            static const bool log = h3::EnvFlag("H3_LOG_MOUSE");
            if (log)
                fprintf(stderr, "[mouse] yaw %.5f pitch %.5f, weapon rate %.4f -> scale %.3f / %.3f\n", yaw, pitch, t_weaponRate,
                    yawScale, pitchScale);
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
