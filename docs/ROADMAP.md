# Roadmap

Rough phases. Each one is a large amount of work; that's fine.

## 0. Ground truth
- [ ] Own dump of `default.xex` in `private/`; SHA-1 recorded
- [ ] `h3xexinfo` runs: sections, entry point, imports listed
- [ ] XEX loaded into Ghidra (XEX loader plugin) for manual analysis

## 1. Recompile
- [ ] Fill save/restore helper addresses (`h3xexinfo` output)
- [ ] Locate `setjmp`/`longjmp` in Ghidra (or confirm they're unused)
- [ ] XenonAnalyse jump tables: check the results and fix missed patterns.
      XenonAnalyse's detection was tuned for Sonic Unleashed, so expect
      Halo 3 to need work here.
- [ ] Manual `functions = [...]` boundaries for anything misanalysed
- [ ] `tools/recompile.sh` completes; `H3Recomp` links

## 2. Boot (kernel/XAM)
- [ ] KPCR / TLS setup (`r13`), main thread state
- [ ] Memory: `NtAllocateVirtualMemory`, `MmAllocatePhysicalMemoryEx`, heaps
- [ ] Threads, events, critical sections, `KeWaitFor*`
- [ ] File I/O redirected into `private/game/`
- [ ] Reach the first map load

## 3. Graphics
- [ ] Hook the game's D3D9-on-Xenos layer. Replacing it at the API level is
      far easier than emulating raw PM4 GPU command packets.
- [ ] Shaders via XenosRecomp
- [ ] Render-target / EDRAM resolve semantics
- [ ] Menu renders → mainmenu.map renders → a campaign level renders

## 4. Audio, input, saves
- [ ] XMA decoding, XAudio
- [ ] XInput → SDL gamepad
- [ ] Content/profile/save emulation (XAM content APIs)

## 5. Native improvements
- [ ] Resolution/framerate unlocks (engine tick vs render decoupling)
- [ ] Replace hot recompiled functions with hand-written source

## 6. Decompilation (optional, long-term)
- [ ] Identify subsystems (tags, cache files, scenario, AI, physics) and
      name functions in Ghidra
- [ ] Replace recompiled subsystems with readable C++ one at a time,
      using the recompiled original as an oracle for testing
