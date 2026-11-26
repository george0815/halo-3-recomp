// Function-level call recording and replay (docs/ROADMAP.md 6.1): checks
// decompiled replacements against their recompiled originals in seconds,
// without running the game.
//
// Recording (a build with -DH3_RECORD_CALLS=ON, H3_ORIGINAL=*;
// tools/films/record_calls.sh):
//
//   H3_RECORD_CALLS=<dir>         where to write
//   H3_RECORD_TICKS=<t>[,<t>...]  level times (state_hash.h) to record at
//   H3_RECORD_WINDOW=<n>          ticks of calls per snapshot (default 1)
//
// At each listed tick the whole guest memory is saved (<dir>/tick_<t>.mem,
// a sparse file: byte offset = guest address) and, for the next n ticks,
// the entry registers of every distinct call of every function
// (<dir>/tick_<t>.calls; up to kMaxCallsPerFunction per function). Calls
// from later ticks of a window replay on the snapshot from its start: both
// versions still see the same memory, but rarer functions get covered.
//
// Replay (any build; H3Recomp <xex> --replay-calls <dir> [<name|address>,...]):
// for each snapshot, loads the memory, rebuilds the host tag cache, and
// calls every registered replacement with each recorded call's registers
// under H3_CHECK_NATIVE (native body, then the original, compared). Both
// run on the same snapshot, so the original is the reference and nothing
// about what it reads needs recording. Each function runs in a child
// process (stores and hangs stay contained). Prints per replacement: calls
// checked, differences (the first ones described), or no recorded calls.
//
// Linux only (pagemap, fork); elsewhere recording is off and replay says so.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace h3::debug
{
    // Called by OnGameStateCopy each tick with the level time: opens and
    // closes the recording windows (H3_RECORD_CALLS).
    void OnRecordTick(uint32_t levelTime);

    // H3Recomp <xex> --replay-calls: returns the process exit code (0 when
    // every replacement with calls matched, 1 otherwise). only is a comma
    // list of replacement names or hex addresses (empty: all).
    int ReplayCalls(const std::filesystem::path& directory, const std::string& only);
}
