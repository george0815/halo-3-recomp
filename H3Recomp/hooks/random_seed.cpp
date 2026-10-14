// H3_RANDOM_SEED=<n>: makes the game's clock-based random seeds the same
// run after run, so two replays of a film can be compared word by word
// (docs/ROADMAP.md 6.1; XNetRandom honors the same switch, kernel/xnet.cpp).
//
// sub_821F6C58 makes a seed from the clock:
//
//   seed = tick count (KeTimeStampBundle +16, ms since boot)
//        ^ time(nullptr) (sub_82095DB8: system time in seconds)
//        ^ rand() (sub_82590950, the CRT's per-thread generator)
//
// sub_823A1298 (random bytes) falls back to it, through an LCG, while the
// network isn't up, which is how the game state gets its random placement:
// sub_8219A978 offsets it by (random << 7) & 0xFF80 bytes into its 16 MB
// region (every pointer into the state then differs between runs). Five
// other places call it directly.
//
// sub_821F6BA0 (once, at startup) sets the two seeds of the random module:
// the deterministic one (thread-local, always 30888; gameplay uses it, and
// films rely on it) and the "local" one at kLocalRandomSeed, made from the
// clock like the above (inlined). Cosmetic randomness reads the local one:
// effects copy it into their own seed (effect datum +0x84; sub_82234E78
// steps either) at creation.
//
// The local seed is shared by two threads: the main thread (effects, object
// creation) and a second one (particles and effects: sub_821440C8,
// sub_82145358, sub_82230150) that steps it a timing-dependent number of
// times. Which values the main thread draws then varies from run to run,
// and with them effect seeds and a few object fields set at load. That's
// harmless cosmetic randomness, but it breaks word-by-word comparison.
//
// The main thread also steps it per rendered frame, so the seed a level
// starts with depends on how many frames its loading screen took.
//
// With the switch, each make_clock_seed call returns the n-th value of a
// fixed sequence instead, the local seed starts from a fixed value and goes
// back to it when a level's game time starts (ResetLocalRandomSeed), and
// the second thread's three uses get a seed of their own
// (LocalRandomSeedBase, mid-asm hooks in H3RecompLib/config/halo3.toml).
// With H3_LOCKSTEP (hooks/tick_rate.cpp) for one tick per frame, two
// replays of a film then match word for word.

#include "hooks/random_seed.h"
#include "config/env.h"
#include "kernel/thread.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <atomic>
#include <cstdint>

namespace
{
    constexpr uint32_t kLocalRandomSeed = 0x827E3648;
    // The hooked code reaches the seed as 13896(rX) after lis rX,0x827E.
    constexpr uint32_t kLocalRandomSeedLow = 13896;

    // The thread that reset the seeds at startup (the main thread).
    std::atomic<uint32_t> g_mainThread{ 0 };

    // splitmix64's finalizer: spreads consecutive inputs over all bits.
    uint64_t Mix(uint64_t value)
    {
        value += 0x9E3779B97F4A7C15ull;
        value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
        return value ^ (value >> 31);
    }

    // Whether H3_RANDOM_SEED is set, and its value.
    bool Fixed()
    {
        static const bool fixed = h3::EnvString("H3_RANDOM_SEED") != nullptr;
        return fixed;
    }

    uint64_t Seed()
    {
        static const uint64_t seed = h3::EnvNumber("H3_RANDOM_SEED");
        return seed;
    }
}

// uint32_t make_clock_seed()
PPC_FUNC_IMPL(__imp__sub_821F6C58);
PPC_FUNC(sub_821F6C58)
{
    if (!Fixed())
    {
        __imp__sub_821F6C58(ctx, base);
        return;
    }
    static std::atomic<uint64_t> calls{ 0 };
    ctx.r3.u64 = uint32_t(Mix(Seed() ^ (calls++ << 32)));
}

// void random_seeds_reset()
PPC_FUNC_IMPL(__imp__sub_821F6BA0);
PPC_FUNC(sub_821F6BA0)
{
    __imp__sub_821F6BA0(ctx, base);
    if (Fixed())
    {
        h3::hooks::ResetLocalRandomSeed();
        g_mainThread = h3::kernel::CurrentThread()->kthread;
    }
}

void h3::hooks::ResetLocalRandomSeed()
{
    if (Fixed())
        h3::GuestRef<uint32_t>(kLocalRandomSeed) = uint32_t(Mix(~Seed()));
}

// Mid-asm hook after "lis rX,0x827E" in the second thread's uses of the
// local seed: off the main thread, points rX at a seed of that thread's own
// (allocated on first use) so that 13896(rX) reads and writes it.
void LocalRandomSeedBase(PPCRegister& reg)
{
    if (!Fixed() || h3::kernel::CurrentThread()->kthread == g_mainThread)
        return;
    thread_local uint32_t seed = [] {
        uint32_t address = h3::SystemAlloc(4);
        h3::GuestRef<uint32_t>(address) = uint32_t(Mix(Seed() + 1));
        return address;
    }();
    reg.u64 = seed - kLocalRandomSeedLow;
}
