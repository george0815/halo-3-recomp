// Weapon barrels, decompiled in place (docs/ROADMAP.md 6.4): the fire-rate
// logic of a weapon's barrels, with MCC's fixes for running the game above
// 30 ticks per second.
//
// A barrel fires a round at most once per game tick. Its pace comes from
// two places in the weapon tag's barrel (weapon_barrels):
//   rounds per second  between the rounds of a burst: a round may fire once
//                      the barrel has idled ticks_per_round - 1 ticks
//   fire recovery time after a burst (a set of shots): ticks the barrel
//                      waits before it can fire again
// Both are converted to ticks with the game's tick rate, so they keep their
// pace at 60 ticks per second, except where 30 Hz rounding was part of the
// pace: at 30 a 20 rounds per second weapon gets 1.5 ticks per round, i.e.
// fires every second tick (15 per second), and a weapon with no recovery
// fires every tick (30 per second). At 60 those become 20 and 60 per second:
// Guilty Spark's beam (30 rounds per second, no recovery) fired twice as
// fast. MCC's Halo 3 (H3EK's halo3_tag_test.exe has the same code) keeps
// the 30 Hz pace: rates are quantized to whole 30 Hz ticks, and a barrel
// waits at least one 30 Hz tick between rounds. hooks::ContentFixes()
// turns that on; at 30 ticks per second, or with H3_NO_60HZ_FIXES, these
// compute exactly what the originals do (H3_CHECK_NATIVE compares).
//
// Replaced:
//   sub_8234BF00  weapon_barrel_can_fire (MCC FUN_140a83ad0)
// Wrapped (the original runs, the fix adjusts its result):
//   sub_82100858  weapon_barrel_start_recovery (MCC FUN_140a871b0)
// Names are descriptive: retail has no symbols.
//
// The port also makes the original's stack stores (saved registers, the
// int-to-float temporary) below the caller's stack pointer, in case later
// code reads that stale stack as an uninitialized local (a port of
// start_recovery matched call for call but not over a replay; replace.h's
// H3_NATIVE_SHADOW narrows such cases down).
//
// Layouts (retail; MCC's object data is the same 4 bytes earlier):
//   weapon object data + kWeaponBarrels: kWeaponBarrelCount barrel states
//   of kBarrelBytes; weapon tag (object + item + weapon parts, guest
//   layout) + kWeaponTagBarrels: the barrels block (count, elements), each
//   element kBarrelDefinitionBytes.

#include "objects/object_access.h"
#include "replace.h"
#include "tags/tags.h"

#include "config/env.h"
#include "hooks/tick_rate.h"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>

namespace
{
    namespace objects = h3::native::objects;
    using h3::GuestRef;
    using h3::native::GuestStores;
    using h3::native::Run;

    // Game time globals (TLS slot kTlsGameTime, hooks/tick_rate.cpp): ticks
    // per second (s16) at kGameTimeTicksPerSecond.
    constexpr uint32_t kTlsGameTime = 188;
    constexpr uint32_t kGameTimeTicksPerSecond = 4;

    // Weapon object data.
    constexpr uint32_t kWeaponAge = 400;               // float: 0 new .. 1 worn out
    constexpr uint32_t kWeaponItemInInventory = 353;   // u8: held by a unit
    constexpr uint32_t kWeaponObjectFlags = 374;       // u16
    constexpr uint16_t kWeaponCannotFire = 0x40;       // in kWeaponObjectFlags
    constexpr uint32_t kWeaponInventoryUnit = 364;     // object index of the unit holding it, or NONE
    constexpr uint32_t kWeaponBarrels = 436;
    constexpr uint32_t kBarrelBytes = 52;

    // A barrel's state (kBarrelBytes, at kWeaponBarrels + index * kBarrelBytes).
    constexpr uint32_t kBarrelIdleTicks = 0;      // s8: ticks since its last round
    constexpr uint32_t kBarrelState = 1;          // s8: kBarrelIdle, kBarrelFiring, kBarrelRecovering*
    constexpr uint32_t kBarrelTimer = 2;          // s16: recovery ticks left
    constexpr uint32_t kBarrelFlags = 4;          // u16
    constexpr uint16_t kBarrelHeld = 0x40;        // in kBarrelFlags: can't fire (e.g. firing is being held back)
    constexpr uint32_t kBarrelShotsFired = 6;     // u16: rounds fired in the current burst
    constexpr uint32_t kBarrelRateFraction = 16;  // float: 0..1 between the rounds per second bounds
    constexpr uint32_t kBarrelBonusRounds = 36;   // float: fraction of extra rounds owed (rates over one per tick)
    constexpr int8_t kBarrelFiring = 1;
    constexpr int8_t kBarrelRecovering = 2;        // the first recovering state (start_recovery sets 2 or 3)
    constexpr int8_t kBarrelRecoveringLast = 4;    // the last recovering state

    // Unit object data: which of its weapon slots hold the two weapons it
    // wields (s8 slot index, or -1), indexing objects::kUnitWeapons.
    constexpr uint32_t kUnitWieldedSlots[2] = { 602, 603 };

    // Weapon tag (guest layout from the tag's start).
    constexpr uint32_t kWeaponTagAgeRateOfFirePenalty = 948;  // float: Age Rate Of Fire Penalty
    constexpr uint32_t kWeaponTagBarrelCount = 1116;          // Barrels block: count
    constexpr uint32_t kWeaponTagBarrelElements = 1120;       // Barrels block: elements
    constexpr uint32_t kBarrelDefinitionBytes = 308;

    // Barrel definition (weapon_barrels).
    constexpr uint32_t kBarrelDefFlags = 0;
    constexpr uint32_t kBarrelDefRoundsPerSecond = 4;  // real bounds: at rate fraction 0 and 1
    constexpr uint32_t kBarrelDefShotsPerFire = 28;    // short integer bounds: a burst's minimum and maximum (0: none)
    constexpr uint32_t kCantFireWhenOthersFiring = 1u << 9;     // in kBarrelDefFlags
    constexpr uint32_t kCantFireWhenOthersRecovering = 1u << 10;

    // Rounds per second at or below this count as none (the original's).
    constexpr float kNoRate = 1e-4f;

    // The originals' stack frames (offsets from the caller's r1).
    // weapon_barrel_can_fire saves r22-r31 (__savegprlr_22: r22 at -88 ..
    // r31 at -16) and the return address (-8), and converts through one
    // doubleword at kCanFireTemp.
    constexpr uint32_t kSavedGprBytes = 8;
    constexpr uint32_t kSavedReturnAddress = 8;
    constexpr uint32_t kCanFireSavedR22 = 88;
    constexpr uint32_t kCanFireTemp = 96;

    // H3_CHECK_NATIVE watches output bytes only at plausible addresses
    // (the first guest page is never mapped).
    constexpr uint32_t kLowestGuestAddress = 0x10000;

    // MCC quantizes rates to the original tick rate's whole ticks.
    constexpr float kOriginalTicksPerSecond = 30.0f;

    // Big-endian guest loads.
    uint8_t Load8(uint32_t address) { return GuestRef<uint8_t>(address); }
    uint16_t Load16(uint32_t address) { return GuestRef<uint16_t>(address); }
    int8_t Load8s(uint32_t address) { return int8_t(Load8(address)); }
    int16_t Load16s(uint32_t address) { return int16_t(Load16(address)); }
    float LoadFloat(uint32_t address) { return GuestRef<float>(address); }

    // The tag data of a datum, as most of the game finds it: straight from
    // the tag table, without tag_get's checks.
    uint32_t TagDataUnchecked(uint32_t datum)
    {
        return h3::native::tags::TagDataBySlot(datum & 0xFFFF);
    }

    uint32_t TicksPerSecond(const PPCContext& ctx)
    {
        uint32_t gameTime = objects::Load32(objects::Load32(ctx.r13.u32) + kTlsGameTime);
        return uint32_t(int32_t(int16_t(Load16(gameTime + kGameTimeTicksPerSecond))));
    }

    // A weapon's barrel: its state and its definition (guest addresses).
    struct Barrel
    {
        uint32_t weapon;      // object data
        uint32_t state;       // barrel state
        uint32_t tag;         // weapon tag data
        uint32_t definition;  // barrel definition

        Barrel(const PPCContext& ctx, uint32_t weaponIndex, int32_t index)
        {
            weapon = objects::ObjectData(ctx, weaponIndex);
            state = weapon + kWeaponBarrels + uint32_t(index) * kBarrelBytes;
            tag = TagDataUnchecked(objects::Load32(weapon));
            definition = objects::Load32(tag + kWeaponTagBarrelElements) + uint32_t(index) * kBarrelDefinitionBytes;
        }
    };

    // Ticks between a barrel's rounds at its current rate (0: no rate),
    // stretched by the weapon's age. With fixes (MCC), the rate is first
    // rounded down to what whole 30 Hz ticks allow.
    float TicksPerRound(const PPCContext& ctx, const Barrel& b, bool fixes, bool& convertedRate)
    {
        float low = LoadFloat(b.definition + kBarrelDefRoundsPerSecond);
        float high = LoadFloat(b.definition + kBarrelDefRoundsPerSecond + 4);
        float rate = std::fmaf(high - low, LoadFloat(b.state + kBarrelRateFraction), low);
        if (fixes && rate > kNoRate)
            rate = kOriginalTicksPerSecond / std::ceil(kOriginalTicksPerSecond / rate);
        convertedRate = rate > kNoRate;
        float ticks = convertedRate ? float(int32_t(TicksPerSecond(ctx))) / rate : 0.0f;
        float penalty = LoadFloat(b.tag + kWeaponTagAgeRateOfFirePenalty);
        if (penalty > 0.0f)
            ticks *= std::fmaf(LoadFloat(b.weapon + kWeaponAge), penalty, 1.0f);
        return ticks;
    }

    // H3_LOG_BARRELS=1 (testing the fire rates): counts each weapon
    // definition's rounds (weapon_barrel_can_fire saying yes) and logs, every
    // kLogSeconds of game time, those that fired: rounds per second, and how
    // many ticks apart each weapon's consecutive rounds were (1..kGaps-1,
    // longer gaps not counted: they're between bursts).
    void LogRound(const PPCContext& ctx, uint32_t definition, uint32_t weaponIndex)
    {
        static const bool enabled = h3::EnvFlag("H3_LOG_BARRELS");
        if (!enabled)
            return;
        constexpr uint32_t kLogSeconds = 5;
        constexpr uint32_t kGameTimeTick = 12;
        constexpr uint32_t kGaps = 6;
        struct Counts
        {
            uint32_t rounds = 0;
            uint32_t gaps[kGaps] = {};
        };
        static std::map<uint32_t, Counts> counts;
        static std::map<uint32_t, uint32_t> lastRound;  // weapon object -> tick
        static uint32_t windowStart = 0;
        uint32_t gameTime = objects::Load32(objects::Load32(ctx.r13.u32) + kTlsGameTime);
        uint32_t tick = objects::Load32(gameTime + kGameTimeTick);
        uint32_t window = kLogSeconds * TicksPerSecond(ctx);
        if (tick < windowStart || tick >= windowStart + window)
        {
            for (auto& [tag, c] : counts)
                fprintf(stderr, "[barrels] tick %u: weapon %08X %.1f rounds/s, gaps 1-5: %u %u %u %u %u\n", windowStart, tag,
                    double(c.rounds) / kLogSeconds, c.gaps[1], c.gaps[2], c.gaps[3], c.gaps[4], c.gaps[5]);
            counts.clear();
            windowStart = tick - tick % window;
        }
        Counts& c = counts[definition];
        c.rounds++;
        auto last = lastRound.find(weaponIndex);
        if (last != lastRound.end() && tick > last->second && tick - last->second < kGaps)
            c.gaps[tick - last->second]++;
        lastRound[weaponIndex] = tick;
    }

    // The other barrel of a two-barrel weapon (barrel 1 for barrel 0, else 0).
    uint32_t OtherBarrel(const Barrel& b, int32_t index)
    {
        return b.weapon + kWeaponBarrels + (index == 0 ? kBarrelBytes : 0);
    }

    // Whether the weapon the same unit wields in its other hand, if it is
    // the same kind of weapon, has its same barrel firing right now (at most
    // one of the two fires in a tick; with fixes, within one 30 Hz tick).
    bool DualPartnerJustFired(const PPCContext& ctx, uint32_t weaponIndex, const Barrel& b, int32_t index, bool fixes)
    {
        if (!Load8(b.weapon + kWeaponItemInInventory))
            return false;
        uint32_t unit = objects::Load32(b.weapon + kWeaponInventoryUnit);
        if (unit == objects::kNone)
            return false;
        uint32_t unitData = objects::ObjectData(ctx, unit);
        uint32_t wielded[2];
        for (int hand = 0; hand < 2; hand++)
        {
            int8_t slot = Load8s(unitData + kUnitWieldedSlots[hand]);
            wielded[hand] = slot == -1 ? objects::kNone : objects::Load32(unitData + objects::kUnitWeapons + uint32_t(slot) * 4);
        }
        uint32_t other = objects::kNone;
        if (weaponIndex == wielded[0])
            other = wielded[1];
        if (weaponIndex == wielded[1])
            other = wielded[0];
        if (other == objects::kNone)
            return false;
        uint32_t otherData = objects::ObjectData(ctx, other);
        if (objects::Load32(otherData) != objects::Load32(b.weapon))
            return false;
        uint32_t partner = otherData + kWeaponBarrels + uint32_t(index) * kBarrelBytes;
        if (Load8s(partner + kBarrelState) != kBarrelFiring)
            return false;
        if (fixes)
            return Load8s(partner + kBarrelIdleTicks) < h3::hooks::TicksPerOriginalTick();
        return Load8(partner + kBarrelIdleTicks) == 0;
    }
}

// weapon_barrel_can_fire(r3 weapon, r4 s16 barrel, r5 u8* burst ok,
// r6 u8* burst full, r7 u8* held) -> whether the barrel fires a round this
// tick. Also owes the barrel bonus rounds when its rate is over one per
// tick. The outputs: r5 = the burst may go on (its shot count is within the
// definition's shots per fire), r6 = the burst reached its maximum, r7 set
// (with r6) when the barrel is held.
H3_REPLACE_STORES(8234BF00, weapon_barrel_can_fire, {
    bool fixes = run == Run::Game && h3::hooks::ContentFixes();
    uint32_t weaponIndex = ctx.r3.u32;
    int32_t index = int16_t(ctx.r4.u32);
    uint32_t burstOk = ctx.r5.u32, burstFull = ctx.r6.u32, held = ctx.r7.u32;
    Barrel b(ctx, weaponIndex, index);
    uint32_t sp = ctx.r1.u32;
    if (run == Run::Check)
    {
        stores.Watch(sp - kCanFireTemp, kCanFireTemp);
        stores.Watch(b.state, kBarrelBytes);
        for (uint32_t output : { burstOk, burstFull, held })
            if (output >= kLowestGuestAddress)
                stores.Watch(output, 1);
    }

    const PPCRegister saved[] = { ctx.r22, ctx.r23, ctx.r24, ctx.r25, ctx.r26, ctx.r27, ctx.r28, ctx.r29, ctx.r30, ctx.r31 };
    for (uint32_t i = 0; i < std::size(saved); i++)
        stores.Store64(sp - kCanFireSavedR22 + i * kSavedGprBytes, saved[i].u64);
    stores.Store32(sp - kSavedReturnAddress, uint32_t(ctx.lr));

    bool convertedRate = false;
    float ticksPerRound = TicksPerRound(ctx, b, fixes, convertedRate);
    if (convertedRate)
        stores.Store64(sp - kCanFireTemp, uint64_t(int64_t(int32_t(TicksPerSecond(ctx)))));

    // Barrels that wait for the weapon's other barrel.
    uint32_t flags = objects::Load32(b.definition + kBarrelDefFlags);
    bool blocked = false;
    if ((flags & (kCantFireWhenOthersFiring | kCantFireWhenOthersRecovering)) &&
        int32_t(objects::Load32(b.tag + kWeaponTagBarrelCount)) > 1)
    {
        int8_t other = Load8s(OtherBarrel(b, index) + kBarrelState);
        if (other == kBarrelFiring && (flags & kCantFireWhenOthersFiring))
            blocked = true;
        if (other >= kBarrelRecovering && other <= kBarrelRecoveringLast && (flags & kCantFireWhenOthersRecovering))
            blocked = true;
    }

    // A burst's first round fires at once; the next ones once the barrel
    // has idled a round's worth of ticks (with fixes, at least one 30 Hz
    // tick: idle ticks count from 0 the tick after a round).
    bool fire = false;
    uint16_t shots = Load16(b.state + kBarrelShotsFired);
    int8_t idle = Load8s(b.state + kBarrelIdleTicks);
    if (!blocked && shots != 0)
        stores.Store64(sp - kCanFireTemp, uint64_t(int64_t(idle)));
    if (!blocked && (!fixes || idle >= h3::hooks::TicksPerOriginalTick() - 1))
        fire = shots == 0 || !(float(idle) + 1.0f < ticksPerRound);

    // Shots per fire: below the minimum the burst can't stop yet; at the
    // maximum it must.
    int16_t minimum = Load16s(b.definition + kBarrelDefShotsPerFire);
    int16_t maximum = Load16s(b.definition + kBarrelDefShotsPerFire + 2);
    if (minimum != 0 && int32_t(shots) < minimum)
    {
        stores.Store8(burstOk, 0);
        stores.Store8(burstFull, 0);
    }
    else if (maximum != 0 && int32_t(shots) >= maximum)
    {
        fire = false;
        stores.Store8(burstOk, 0);
        stores.Store8(burstFull, 1);
    }
    else
    {
        stores.Store8(burstOk, 1);
        stores.Store8(burstFull, 0);
    }
    if (Load16(b.state + kBarrelFlags) & kBarrelHeld)
    {
        fire = false;
        stores.Store8(burstFull, 1);
        stores.Store8(held, 1);
    }
    if (fire && (Load16(b.weapon + kWeaponObjectFlags) & kWeaponCannotFire))
    {
        fire = false;
        stores.Store8(burstOk, 1);
        stores.Store8(burstFull, 0);
    }
    if (fire && DualPartnerJustFired(ctx, weaponIndex, b, index, fixes))
        fire = false;

    // Rates over one round per tick: the extra rounds accumulate.
    if (fire && ticksPerRound < 1.0f && ticksPerRound > kNoRate)
        stores.StoreFloat(b.state + kBarrelBonusRounds, 1.0f / ticksPerRound + LoadFloat(b.state + kBarrelBonusRounds));
    if (fire && run == Run::Game)
        LogRound(ctx, objects::Load32(b.weapon), weaponIndex);
    return uint32_t(fire);
})

// weapon_barrel_start_recovery(r3 weapon, r4 s16 barrel, r5 u8 flag): after
// a burst, sets the barrel's recovery timer from the definition's fire
// recovery time (the soft share of it not counted, fractional ticks carried
// over between bursts) and its state to recovering. With fixes (MCC), the
// recovery lasts at least one 30 Hz tick.
//
// The original runs, and the fix only raises its timer afterwards. MCC also
// raises the recovery's ticks before the soft share and the carry are taken
// from them; for the timer that comes to the same: recoveries of at least
// one 30 Hz tick aren't changed, shorter ones end up at exactly one. (A
// native port matched the original call for call but not over a whole
// replay: private/re/ports/weapon_barrel_start_recovery.cpp.txt.)
PPC_FUNC_IMPL(__imp__sub_82100858);
PPC_FUNC(sub_82100858)
{
    if (!h3::hooks::ContentFixes())
    {
        __imp__sub_82100858(ctx, base);
        return;
    }
    uint32_t weaponIndex = ctx.r3.u32;
    int32_t index = int16_t(ctx.r4.u32);
    __imp__sub_82100858(ctx, base);
    Barrel b(ctx, weaponIndex, index);
    int16_t minimum = int16_t(h3::hooks::TicksPerOriginalTick());
    if (Load16s(b.state + kBarrelTimer) < minimum)
        GuestRef<uint16_t>(b.state + kBarrelTimer) = uint16_t(minimum);
}
