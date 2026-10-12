// Native replacement for the game's VMX block copy, sub_825A18C0 (a
// hand-written copy in 128-byte blocks: lvx loads, dcbz128 on each
// destination line, stvlx stores).
//
// Halo 3 copies its whole 4 MB game state with it once per game tick (the
// caller at 0x821423C0: the game state's copies 4 MB apart, kept for
// checkpoints and revert). Recompiled, the byte-wise stvlx stores made it
// the hottest function in gameplay (~12% of all samples, ~70% of the main
// thread at 60 ticks). Copying the bytes natively gives the same memory:
// lvx/stvlx byte-swap on load and swap back on store, and dcbz's zeroed
// lines are fully overwritten.
//
// Only the shape the game state copy uses is replaced: both pointers
// aligned to the copy's granularity and a size of whole 128-byte blocks,
// at least kMinNativeBytes. Anything else runs the original.

#include <ppc_recomp_shared.h>

#include <cstring>

namespace
{
    // The original's block size and alignment: dcbz128 zeroes the 128-byte
    // destination line, lvx reads 16-byte aligned quadwords.
    constexpr uint32_t kBlockBytes = 128;
    constexpr uint32_t kSourceAlignment = 16;
    // The original's main loop moves 1024 bytes per pass; smaller copies
    // take its tail path, which is left alone.
    constexpr uint32_t kMinNativeBytes = 1024;
}

// r3 = destination, r4 = source, r5 = bytes.
PPC_FUNC_IMPL(__imp__sub_825A18C0);
PPC_FUNC(sub_825A18C0)
{
    uint32_t destination = ctx.r3.u32, source = ctx.r4.u32, bytes = ctx.r5.u32;
    if (bytes < kMinNativeBytes || bytes % kBlockBytes != 0 || destination % kBlockBytes != 0 || source % kSourceAlignment != 0)
    {
        __imp__sub_825A18C0(ctx, base);
        return;
    }
    memmove(base + destination, base + source, bytes);
    // Leave the pointer registers as the original does: both advanced past
    // the copy, no bytes left.
    ctx.r3.u64 = destination + bytes;
    ctx.r4.u64 = source + bytes;
    ctx.r5.u64 = 0;
}
