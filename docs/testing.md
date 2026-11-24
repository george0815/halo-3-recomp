---
title: Testing
nav_order: 5
---

# How changes are tested

Halo 3 is a deterministic simulation: given the same inputs, it computes the
same game state, tick after tick. Its Theater mode saves exactly those
inputs as films. That makes every saved film a test.

## Film hashes

1. **Replay a film** (`tools/films/run_film.sh`): the game starts, the
   script navigates to Theater and plays the film; `H3_STATE_HASH` writes a
   checksum of the 4 MB game state for every tick (one line per tick).
2. **Mask the cosmetic state.** Some of the state legitimately differs
   between replays (timers driven by real time, render-side caches). Those
   parts are learned from repeated replays (`learn_mask.py`) and left out
   of the checksum.
3. **Compare with the reference** (`compare_hashes.py`): the first differing
   tick, and the stretches that differ, point at what changed. A difference
   that never re-converges is a simulation change; one that re-converges is
   usually cosmetic state that escaped the mask.
4. **The library** (`tools/films/run_library.sh`) replays every film (one per
   campaign mission, plus multiplayer) against its reference. It runs before
   every release, on both platforms, and after any change that could touch
   the simulation (60 Hz fixes, decompiled functions, the ISO tool's builds).

For the native port, `H3_CHECK_NATIVE=1` runs each hand-written replacement
alongside the original and logs any difference, on every call, during a
replay.

## Finding where it diverged

- `H3_STATE_DUMP=<tick>` saves the raw state at a tick; comparing two dumps
  shows which fields differ.
- `H3_STATE_RECORD` records every tick compactly, for the stretch around a
  difference.
- `H3_SNAPSHOT_LEVEL` saves screenshots at chosen moments of the film, so
  two builds can be compared at exactly the same moment.

## Pictures

Rendering isn't in the game state, so it's checked by eye: screenshots at
the same film moment (`H3_SNAPSHOT_LEVEL`), GPU captures (F9/F10, per draw)
and comparisons with the same frame in [Xenia](https://github.com/xenia-project/xenia).

The details are in `tools/films/README.md` in the repository.
