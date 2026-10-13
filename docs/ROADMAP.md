# Roadmap

Rough phases. Each one is a large amount of work; that's fine.

## 0. Ground truth
- [x] Own dump of `default.xex` in `private/`; SHA-1 recorded
- [x] `h3xexinfo` runs: sections, entry point, imports listed
- [x] XEX loaded into Ghidra (XEX loader plugin) for manual analysis
      (`~/tools/ghidra_*`, project in `private/ghidra/`)

## 1. Recompile
- [x] Fill save/restore helper addresses (`h3xexinfo` output)
- [x] Locate `setjmp`/`longjmp`: `sub_826F3AF0`/`sub_826F36B0` (only library
      error paths call them); XenonRecomp emits host setjmp/longjmp
- [x] XenonAnalyse jump tables: check the results and fix missed patterns.
      `tools/h3analysis.py tables` re-reads every inline table (23 misread,
      156 missed by XenonAnalyse). 9 tables still log errors: their
      unreachable cases point at the padding word after the function, which
      XenonRecomp turns into `return`.
- [x] Manual `functions = [...]` boundaries for anything misanalysed
      (`tools/h3analysis.py functions`)
- [x] Missing PPC instructions in XenonRecomp (12 added; not yet in a fork),
      plus `blrl`, `vupkd3d128` float16/short4 formats and `vpkd3d128`
      float16_2/normpacked32
- [x] Jump table entries that tail-call another function (both
      `h3analysis.py` and XenonRecomp); out-of-range table indexes abort
      with the table address (`H3RecompLib/debug/switch_check.h`)
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
- [x] Interlocked SLists, spin locks, IRQL (raised IRQL holds off the GPU
      interrupt, as on the console)
- [x] Physical memory aliased at 0xA0000000, 0xC0000000 and 0xE0000000
- [x] Runs past loading at a steady 60 fps (nothing drawn yet)

## 3. Graphics
- [x] Stand-in command processor (`H3Recomp/gpu/`): consumes the PM4 ring
      buffer, performs fences, waits, scratch/memory writes and interrupts,
      60 Hz vblank. Draws nothing, but the game presents frames
- [x] PM4 predication (bin mask/select) for D3D's predicated tiling, and
      command processor interrupts delivered synchronously (as Xenia does);
      without them the main menu's tiled frames replay forever
- [x] Intro movie shown by reading Bink's decoded frames (debug shortcut)
- [x] Command stream state: constants (SET_CONSTANT, LOAD_ALU_CONSTANT),
      shader loads (IM_LOAD), draws (DRAW_INDX) and resolves tracked;
      `H3_GPU_CAPTURE` dumps a frame. (Hooking D3D was dropped: its setters
      only write Xenos register shadows and its draws are inlined into the
      game, so rendering works from the command stream instead.)
- [x] Host backend (SDL_GPU / Vulkan, `H3Recomp/gpu/renderer.cpp`): draws,
      EDRAM render targets, resolves into host textures, presents the swap's
      front buffer. The main menu renders
- [x] Xenos microcode -> GLSL -> SPIR-V shader translator
      (`H3Recomp/gpu/shader/`, test with `tools/h3shader`); vertex fetch from
      fetch constants; rectangle lists in the vertex shader
- [x] 2D textures from memory, base level (`H3Recomp/gpu/texture.cpp`:
      untiling, endian swaps, DXT/DXN/CTX1 and packed formats; fetch
      constant swizzle, sign and exponent in the shader). The main menu's
      text and logos render
- [x] Cube textures from memory; depth resolves (sampled as 24_8 or as the
      depth word's bytes); fetch offsets; resolve exponent bias; gamma render
      targets (encoded on resolve, decoded on fetch, both approximated as
      gamma 2); render target exponent bias; alpha test; blend constants
- [x] Mipmaps (2D textures, decoded from the guest's mip chain), stacked
      (2D array) textures, anisotropic filtering
- [x] 3D textures (base level), mips for cube maps, exact piecewise-linear
      gamma (Xenia's curve; `H3_APPROX_GAMMA=1` for the old gamma 2)
- [x] Mips for stacked textures (hosted as 2D arrays; the shader picks the
      array or 3D binding per fetch constant and blends layers itself).
      True 3D textures keep their base level only (none seen in Halo 3)
- [x] Culling (PA_SU_SC_MODE_CNTL), polygon offset (decals, shadow maps)
- [x] MSAA: guest MSAA surfaces live in sample space (the menu's 4x
      G-buffer, D3D's 4x fast clears); resolves honour the sample select
      (depth from sample 0). Halo 3 has no AA of its own: optional FXAA on
      the finished frame (launcher Video > Anti-aliasing)
- [x] Point sizes: point lists take PA_SU_POINT_SIZE's diameter times the
      render scale (Halo 3 draws a few one-pixel point lists per frame;
      no shader exports its own size). Needs Vulkan largePoints
- [ ] Batch draws better (one render pass per render target change today).
      Profiled 2026-10-13: ~54 draw passes per frame, and draw submission is a
      few percent of the command processor thread, which spends a third of
      its time waiting on the game; not the bottleneck at the moment
- [x] EDRAM render targets and resolves (Halo 3 renders 1152x640 in four
      predicated tiles, deferred: G-buffer, shadow maps, post-processing)
- [x] EDRAM aliasing between formats at one base: 7e3 read as 2_10_10_10
      is reinterpreted bit for bit, other formats convert by value
- [ ] EDRAM tile layout: surfaces of different pitches at one base don't
      alias the way they do in tiled EDRAM
- [x] Menu renders → mainmenu.map renders → a campaign level runs
- [x] In-game frames are washed out (stale render target contents where
      nothing draws; confirmed fixed 2026-10-12)

## 4. Audio, input, saves
- [x] XAudio render driver (`apu/audio_system.cpp`) and the XMA hardware
      decoder (`apu/xma_decoder.cpp`, FFmpeg's xmaframes decoder built by
      `tools/build_ffmpeg.sh`)
- [x] XInput → keyboard and SDL gamepad (`H3Recomp/input/`); no rumble
- [x] Local profile signed in on user 0 (`kernel/xam_user.cpp`), profile
      setting defaults, notifications, content packages as host folders
      (`kernel/xam_content.cpp`), offline XNet basics (`kernel/xnet.cpp`)
- [x] Persist profile settings (`hdd/profile_settings.bin`); gamertag and
      profile defaults from the launcher
- [x] Saved films: hard drive files honour NT share access (the autosave
      queue's *.temp cleanup used to delete the film being recorded) and
      renames (*.temp -> *.film); Theater lists and plays campaign films
- [x] Launcher (`H3Launcher/`, Qt): video, controls, profile; writes
      `settings.ini`, which the runtime reads (`H3Recomp/config/settings.h`)
- [ ] Xbox Live features

## 5. Native improvements
- [x] Internal resolution scale (1x-3x render targets and resolves;
      screen-space draws snap to guest pixels), window size, fullscreen
- [~] Framerate unlock: experimental 60 Hz game tick (launcher Video >
      Frame rate; `hooks/tick_rate.cpp`). Swaps present at once instead of
      waiting for the next vblank (D3D's present interval 1 turned every
      frame slower than 16.7 ms into a 33 ms one): Sierra gameplay went from
      a locked 30 fps to 35-45. Needs play-testing (physics, animation, AI,
      cutscenes) and more headroom to hold 60. The command processor
      thread went from ~21.5 to ~10 ms of CPU per frame for ~1770 draws in
      Sierra (staging written straight into upload buffers, float constants
      in a per-flush storage buffer instead of per-draw uniform pushes,
      lazy AVX2 content hashing, a texture lookup cache, batched export
      scatters; H3_LOG_FPS `[cp]` lines). GPU time went
      from ~27 to ~11 ms a frame (memory export dispatches recorded in
      batches, resident vertex data, compact constants; draws that can't
      write anything skipped): the Sierra, Tsavo and Storm starts hold 60
      fps. Heavier scenes untested
- [ ] Replace hot recompiled functions with hand-written source (started:
      the 4 MB per-tick game state copy, `hooks/block_copy.cpp`)

## 6. Decompilation (optional, long-term)
- [ ] Identify subsystems (tags, cache files, scenario, AI, physics) and
      name functions in Ghidra
- [ ] Replace recompiled subsystems with readable C++ one at a time,
      using the recompiled original as an oracle for testing
