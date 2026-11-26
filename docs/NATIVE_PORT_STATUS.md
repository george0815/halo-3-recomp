---
title: Native port
nav_order: 9
---

# Native port: where things stand (2026-11-26)

Working notes on the decompilation and native port ([roadmap](ROADMAP.html)
phase 6): read this first, then the roadmap. The files under `private/`
mentioned here are local data (game files, films, generated layouts) that
aren't in the repository.

## Progress

As of 2026-11-26: **228 of the game's 22,400 functions rewritten** (1.02%),
**186 of them verified** by replaying recorded calls through both versions
(the rest have no recorded calls yet). Charts, history and the numbers per
subsystem: [Port progress](progress.html). Regenerate the data with
`python3 -I tools/re/progress.py --json docs/_data/progress.json --verified
<replay_calls.sh outputs>`.

## Next steps

1. **Film library (6.1): done.** Ten films (the nine playable campaign
   missions, Sierra 117 to Halo, and an Oddball match recorded with the
   60 Hz fixes), each with a learned mask and reference hashes in
   `private/films/`; `tools/films/run_library.sh` replays them all
   (`--native` adds `check_native.sh`), and does before every release and
   after any change that could touch the simulation ([Testing](testing.html)).
   Last full run (2026-11-24): every film matches its reference on every
   tick, except The Covenant's known start-of-level race on ticks 0-61.
2. **Checking ports (6.1).** While porting, check against recorded calls:
   `tools/films/replay_calls.sh private/calls` (all replacements, a few
   seconds per film; recordings from `tools/films/record_calls.sh`, see
   [tools/films/README.md](https://github.com/george0815/halo-3-recomp/blob/main/tools/films/README.md)).
   The full film library with `--native` is the final check once a
   subsystem is done, and before merging into main.
3. **Tag readers (6.3).** 224 of ~1,590 ported (`H3_REPLACE` readers plus
   the decompiled tag system); 186 checked by call replay (10 films +
   2 wide-window recordings), 0 differed. Continue with the leaf,
   result-only ones: `tools/re/tag_readers.py <defs> --list
   --leaf-pure`, most-called first. Workflow below.
4. Then the roadmap's next items (6.2 foundations / 6.4 simulation), with
   readers ported as part of each subsystem.

## What exists

| Piece | Where | Notes |
|---|---|---|
| Film determinism | `hooks/random_seed.cpp`, `hooks/tick_rate.cpp` (`H3_LOCKSTEP`), `debug/state_hash.cpp` | `H3_RANDOM_SEED` + `H3_LOCKSTEP` + a learned mask: Sierra 117 replays match on all 3,274 ticks |
| Film tools | `tools/films/` | `run_film.sh`, `compare_hashes.py`, `learn_mask.py`, `check_native.sh`, `record_calls.sh` / `replay_calls.sh` (call replay, `H3Recomp/debug/call_record.cpp`), README |
| Cache file reader | `H3Native/cache/cache_file.h`, `tools/h3tags` | header, sections, string ids (all sets), tag names, index |
| Tag layouts | `tools/tagdefs/` -> `H3Native/tags/generated/` (git-ignored, regenerate) | H3EK (via Wine + gdb) merged with Assembly 360 plugins; README has the 5-command pipeline |
| Host tags | `tag_host.h`, `tag_convert.cpp` (generated), `H3Native/tags/tag_host_types.h` | Block<T> keeps host pointer, guest address, storedCount; per-group `<name>_tag` structs and part offsets (`TagPart`) |
| Tag cache | `H3Native/tags/tag_cache.{h,cpp}` | whole map converted at load, lock-free published snapshot, `TagBySlotOf<T>`, `TagPartBySlot<Part>`, `GlobalScenario()`, `GlobalGameGlobals()`; `H3_CHECK_TAGS` reports |
| Decompiled tag system | `H3Native/tags/tags.cpp` (`tag_get` 82087AF0), `H3Native/cache/cache_file_tags.cpp` (load 821A11D0 / unload 821A17E8) | `H3_ORIGINAL=tag_get`, `=cache_file_tags` |
| Calling recompiled code | `H3Native/guest_call.h` | `Call`, `CallIndirect`, `GuestFrame` |
| Replacements | `H3Native/replace.h`, `H3Native/native.h` | `H3_REPLACE_INT/FLOAT(address, name, { body })`: body returns optional (nullopt = run the original) |
| Readers | `H3Native/{ai,cinematics,effects,game,interface,items,objects,render,scenario,sound,structures,units,vehicles,weapons}/` (`objects/object_access.h`, `objects/object_reads.h`: shared guest reads) | 224 functions |
| RE tools | `tools/re/tag_readers.py` (annotates a function's tag reads), `tools/re/layout_query.py` (which groups have a field at an offset); `tools/re/progress.py` (progress per subsystem, the [progress page](progress.html)'s data) | |

All of `H3Native/*.cpp` is globbed into the game (H3Recomp/CMakeLists.txt),
except `cache/cache_file.cpp` (H3NativeCache library) and
`tags/generated/` (H3NativeTags library, only if generated).

## Workflow for a reader

1. `python3 -I tools/re/tag_readers.py private/tagdefs/halo3_360.json <addr>`
   and read the listing (`H3RecompLib/ppc`). The annotator is one-pass: it
   loses values across branch merges; confirm groups with
   `layout_query.py`.
2. Write it with `H3_REPLACE_INT`: read host tags; game state (objects,
   players, globals) stays in guest memory (`GuestRef`). Where the original
   reads past a block's end or through null, return `std::nullopt` (the
   original runs). Mind signed/unsigned compares, `slw` shifts >= 32, and
   no-salt slot lookups (`TagBySlotOf`).
3. **Never write guest memory from a reader**, and convert `GuestRef` to a
   plain integer before `&`/`|` (XenonUtils `be<T>` operators used to store
   their result; they're deleted now, so it no longer compiles).
4. Build, then `tools/films/replay_calls.sh private/calls <name>`: every
   recorded call checked, 0 differed. "original hangs on N snapshot(s)"
   isn't a failure: a wide recording window replays later ticks' calls on
   its first tick's memory, and the original itself can loop on that stale
   state (call_record.cpp reruns a hung replacement with only originals). "no recorded calls" means no
   recorded film reaches it: record one that does (or check it with
   `tools/films/check_native.sh` on a film that calls it).

## Known issues

- One intermittent crash: a null indirect call in a GPU interrupt callback
  (sub_821951F8 via DeliverInterrupt), 1 in ~7 check-mode runs, not
  reproduced since. Recompiled indirect calls now abort with
  `[recomp] indirect call to ... lookup entry is null; entries ... are zero`
  if the function table was overwritten: that message tells which.
- Unverified readers: 39 have no calls in the 10 recorded films (all
  `multiplayer_*` globals readers among them: oddball_lr's snapshots are
  mid-game; the readers probably run at map load). Record a point at map load, or
  check them with `tools/films/check_native.sh`. List:
  `tools/films/replay_calls.sh private/calls | grep 'no recorded'`.
- Runtime-change check (game writing tag memory) only ran on mainmenu and
  Sierra 117; run `H3_CHECK_TAGS=1` on the new films.
- XenonRecomp submodule has local patches (incl. the `be<T>` change):
  backup `private/xenonrecomp-halo3.patch`; never `git submodule update`.

## Test assets (private/, not in git)

- `private/films_test/mask4.txt`: Sierra 117 recent-film-0 mask;
  `private/films_test/run_h/hash.txt`: its reference hashes (masked).
- `private/films/sierra117_recent0.mask`: the same mask, kept.
- `private/tagdefs/halo3.json` (H3EK raw), `halo3_360.json` (merged; the
  one to generate from); `private/assembly_ref/plugins` (Assembly plugins).
- Regenerating layouts needs the H3EK tool.exe dump (scratch Wine prefix,
  deleted): `tools/tagdefs/dump_tool_memory.sh "<H3EK>" <work dir>`.
