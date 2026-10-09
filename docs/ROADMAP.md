# Roadmap

Rough phases. Each one is a large amount of work; that's fine.

## 0. Ground truth
- [x] Own dump of `default.xex` in `private/`; SHA-1 recorded
- [x] `h3xexinfo` runs: sections, entry point, imports listed
- [ ] XEX loaded into Ghidra (XEX loader plugin) for manual analysis

## 1. Recompile
- [x] Fill save/restore helper addresses (`h3xexinfo` output)
- [ ] Locate `setjmp`/`longjmp` in Ghidra (or confirm they're unused)
- [x] XenonAnalyse jump tables: check the results and fix missed patterns.
      `tools/h3analysis.py tables` re-reads every inline table (23 misread,
      156 missed by XenonAnalyse). 9 tables still log errors: their
      unreachable cases point at the padding word after the function, which
      XenonRecomp turns into `return`.
- [x] Manual `functions = [...]` boundaries for anything misanalysed
      (`tools/h3analysis.py functions`)
- [x] Missing PPC instructions in XenonRecomp (12 added; not yet in a fork)
- [x] `tools/recompile.sh` completes; `H3Recomp` links

## 2. Boot (kernel/XAM)
- [x] KPCR / TLS setup (`r13`), main thread state
- [x] Memory: `NtAllocateVirtualMemory`, `MmAllocatePhysicalMemoryEx`, heaps
- [x] Threads, events, critical sections, `KeWaitFor*`, XAM tasks
- [x] Kernel variable imports (`KeTimeStampBundle`, `VdHSIOCalibrationLock`, ...)
- [x] DLLs loaded with `XexLoadImage`: the four Waves audio DLLs are
      recompiled separately (`H3RecompLib/config/modules/`) and linked in;
      boot gets past Waves init
- [x] File I/O: disc (`d:`, `game:`) read-only from `private/game/`, hard
      drive (`cache0:`, `cache1:`, saves) writable in `private/hdd/`;
      completion ports, directory listing, symbolic links
- [x] XeCrypt: SHA-1, RSA public-key and PKCS#1 checks (map signatures)
- [x] User APCs (Bink's file reads), fibers (map decompression; native
      host fibers in `H3Recomp/hooks/fibers.cpp`)
- [x] Reach the first map load: `mainmenu.map` and `shared.map` load and
      verify

## 3. Graphics
- [x] Stand-in command processor (`H3Recomp/gpu/`): consumes the PM4 ring
      buffer, performs fences, waits, scratch/memory writes and interrupts,
      60 Hz vblank. Draws nothing, but the game presents frames
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
