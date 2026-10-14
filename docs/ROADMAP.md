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
      the 4 MB per-tick game state copy, `hooks/block_copy.cpp`); this
      continues as phase 6

## 6. Decompilation and native port

Goal: every function of the game as readable C++, running on host data
structures, with no recompiled code, no guest memory and no console
emulation left. The recompiled game stays the reference (the *oracle*)
until the very end: each piece is replaced only when it behaves the same.

Scale: ~22,400 functions in the recompiled image (H3RecompLib/ppc), about
6.9 MB of code; third-party libraries (Havok, Direct3D 9, XAPI/CRT, Bink,
XAudio) included. Progress is counted by functions and by code bytes,
per subsystem (6.0).

The work goes in two stages per subsystem:

- **Decompiled in place**: readable C++ that still uses the guest's data
  layout (big-endian `be<T>` fields, 32-bit guest pointers, guest
  memory) and the recompiler's calling convention at its edges, so it
  drops in for the recompiled functions one at a time and calls whatever
  is still recompiled.
- **Native**: once a subsystem and everything that touches its data is
  C++, its data becomes host layout (native endian, host pointers,
  64-bit clean), converted at the remaining boundaries (cache file loading
  above all).

Order: leaves before callers, data owners before data users, and the
subsystems with the best references first.

### 6.0 Infrastructure (before replacing anything)
- [~] Symbol database: `private/re/symbols.csv` built by
      `private/re/build_symbols.py` (HaloScript table alignment against
      H3EK's `halo3_tag_test.exe`, handler callees, hand-identified
      functions; 1,305 names), applied to the Ghidra project
- [~] Subsystem map: which address ranges are which source files
      (`private/re/README.md`; from `__FILE__` strings and named functions)
- [ ] Bring the tools into the repo (`tools/re/`): symbol pipeline, call
      graph, string references, Ghidra scripts; data stays in `private/`
- [ ] Type database: structures as C++ headers (`H3Native/types/`), from
      `halo3_tag_test.exe` (tag field definitions, asserts naming fields)
      and the retail code; one source of truth, exported to Ghidra
- [ ] Replacement registry: each decompiled function declares the guest
      address it replaces (`H3_REPLACES(0x8250AFA8)`); a generated table
      overrides the recompiled symbol (as `hooks/` do today). Each one
      can be switched back to the recompiled version at runtime
      (`H3_ORIGINAL=<name|address|subsystem>`) for A/B runs
- [ ] Progress report: functions and bytes replaced per subsystem, and
      what's left (from the registry, the call graph and the symbol
      database); shown in the README
- [ ] Naming, round 2: callers and callees of named functions, RTTI
      vtables (217 classes), tag group constants (4-character group tags
      in code), string references; matching against `halo3_tag_test.exe`
      by strings, constants and call shape (it keeps asserts and
      `__FILE__` paths)

### 6.1 Verification harness (the oracle)
- [x] First check that the recompiled build replays a film identically
      run after run, and fix what doesn't (`tools/films/`): the game state
      is placed at a clock-seeded random offset and cosmetic randomness
      depends on frame timing and a second thread; `H3_RANDOM_SEED` and
      `H3_LOCKSTEP` fix those, and a mask learned from a few replays
      leaves out what depends on asynchronous load timing (effects, HUD,
      a few object fields). Sierra 117: fresh replays match on every tick
- [ ] Deterministic replays: saved films replay the same game from the
      same inputs. A per-tick checksum of the game state (the 4 MB game
      state block and the object/AI data arrays) from the recompiled
      build, recorded once per film, is the reference; a replacement that
      changes any tick's checksum fails, at that tick
- [ ] Film library: a few films per mission covering combat, vehicles,
      cutscenes and multiplayer variants, played headless at maximum
      speed in CI-style batch runs (`tools/films/run_film.sh`; masks and
      reference checksums per film)
- [ ] Function-level traces: record a function's calls in a real run
      (arguments, the guest memory it reads, its writes and result) and
      replay them against the replacement, so a mismatch points at the
      exact call rather than a tick
- [ ] Differential mode: run both versions of a function on copies of
      the state and compare (for pure and nearly pure functions)
- [ ] Rendering checks: frame captures (`H3_GPU_CAPTURE`, snapshots)
      compared image by image against the oracle for render-side work

### 6.2 Foundations (cseries and engine core)
The library layer everything else uses; small, well-specified functions,
the first real decompilation and where the conventions get settled.
- [ ] cseries: memory (heaps, pools), strings, `csprintf`-style
      formatting, math and vector helpers, real/int conversions, bit
      vectors, CRC/hashes, random numbers (deterministic: films depend on
      them)
- [ ] Data structures: data arrays (datum handles: index + salt), linked
      lists, hash tables, bitstreams (also used by networking and films)
- [ ] Engine core: game time (`game_time_*`), the main loop
      (`main_loop`, already mapped), game state allocation and the
      per-tick state copy (`hooks/block_copy.cpp` folds into this),
      threads and synchronization wrappers
- [ ] CRT and XAPI pieces the game calls (memset/memcpy/strings, fibers):
      host equivalents

### 6.3 Content: cache files, tags and resources
Best references (H3EK tag definitions, ElDorito's cache structures,
`halo3_tag_test.exe`), self-contained, and everything else reads tags.
- [~] Cache file header, section and partition layout; map loading and
      the tag index (`tag_get`, group tags). Native reader
      `H3Native/cache/cache_file.h` (header, sections, partitions, string
      ids, tag names, groups, tags, global tags; `tools/h3tags`
      lists them); it matches the game's loaded index slot for slot
      (`H3_CHECK_TAGS=1`). `tag_get` (sub_82087AF0) decompiled in place
      (`H3Native/tags/tags.cpp`; `H3_CHECK_NATIVE=1` runs both, Sierra
      film identical). Left: map loading itself
- [~] Tag layouts as typed C++ structures for every group the game reads
      (generated from the tag definitions where possible): `tools/tagdefs`
      extracts H3EK's definitions (175 groups, 1,364 structs) and
      generates packed big-endian structs; validated against the retail
      maps and Assembly's 360 plugins (106 of 127 groups the same size).
      Left: reconcile the ~70 structs MCC changed (sounds, compiled
      shaders, physics shapes, resource gestalt, ...)
- [ ] Resource system (sound, bitmap, geometry, animation resources);
      streaming and the I/O paths (DVD/HDD cache partitions)
- [ ] Strings (unicode string lists, localization), fonts
- [ ] **Native milestone A**: tags loaded into host-layout structures,
      converted from the big-endian cache file once at load; decompiled
      code reads those; recompiled readers get the guest copy until
      they're gone

### 6.4 Simulation
The core gameplay; the largest share of the code. Films keep it honest.
- [ ] Scenario and zone sets, BSP and structure data, PVS
- [ ] Objects: object types, creation/deletion, attachments, the object
      update loop; then items, weapons, projectiles, equipment, units,
      bipeds, vehicles, devices, scenery, crates
- [ ] Damage, effects (gameplay side), impacts
- [ ] Physics glue: collision queries, the game's use of Havok (see 6.8)
- [ ] Animation: animation graphs, playback, interpolation
- [ ] AI: actors, squads, behaviors, perception, pathfinding, flocks,
      command scripts (`cs_*`)
- [ ] HaloScript: the compiler-free runtime (threads, evaluation, the
      ~1,470 functions' handlers, already named), recordings
- [ ] Game engines and campaign: game_engine (variants, scoring,
      respawns), campaign progress, checkpoints and revert, saved films
- [ ] Players, cameras, controls (the input side of units), cinematics
      (game side)
- [ ] Simulation for networking (prediction, entity replication: needed
      for films and splitscreen even offline)
- [ ] **Native milestone B**: game state in host layout (the 4 MB game
      state block, data arrays, object memory); checkpoints/films save
      and load it through explicit serialization (keeps old saves and
      films readable)

### 6.5 Presentation: rendering
Today rendering is emulated at the GPU level (PM4 commands, Xenos
shaders, EDRAM). A native renderer replaces that from the top down.
- [ ] Decompile the rasterizer layer (`rasterizer_*`, render methods,
      lights, water, particles, decorators, screen effects, the frame's
      pass structure: G-buffer, lighting, transparents, post-processing)
      in place, still issuing D3D calls to the emulated device
- [ ] Shaders: Halo's shader tags hold compiled Xenos microcode; keep the
      translator (gpu/shader) as the shader source, then move to the
      HLSL-equivalent sources reconstructed from it per render method
- [ ] Native render backend: the rasterizer calls a host renderer
      (SDL_GPU or Vulkan) directly with typed state, instead of D3D 9 ->
      PM4 -> command processor; textures and geometry from tag resources
      without guest memory or EDRAM emulation
- [ ] **Native milestone C**: the PM4 command processor, EDRAM emulation,
      memexport compute variants and texture detiling are gone (kept
      behind a switch until the native path matches the captures)
- [ ] Then real improvements: native resolution without sample-space
      targets, proper MSAA, high refresh rates, widescreen FOV, ultrawide

### 6.6 Presentation: interface, audio, input
- [ ] UI: the GUI screen system, widgets, CHUD (HUD), menus, the main
      menu scene; text and fonts
- [ ] Audio: the sound system (mixer, classes, environments, dialogue,
      music state); decode XMA (and Bink audio) natively through FFmpeg
      without the emulated XMA hardware contexts and XAudio driver
- [ ] Video: Bink playback (FFmpeg's Bink decoder, or a decompiled
      player) without the recompiled Bink library
- [ ] Input: controller/keyboard/mouse straight into the game's input
      layer (no emulated XInput)

### 6.7 Platform layer
The emulated Xbox 360 goes away as its callers become native.
- [ ] Saves, profiles and content: host files (no emulated XContent,
      cache partitions or NT file APIs)
- [ ] Networking: LAN/online sessions over host sockets, or offline only
      (Xbox Live services can't come back; system link could)
- [ ] Threads, synchronization, timing: host primitives (no emulated
      kernel objects, IRQL, interrupts, fibers)
- [ ] Remove the HLE kernel/XAM layers piece by piece as their last
      callers disappear (track imports still called)

### 6.8 Third-party libraries
Statically linked into the executable; each needs its own answer.
- [ ] Direct3D 9 (static): disappears with the native renderer (6.5)
- [ ] CRT / XAPI: host standard library (6.2, 6.7)
- [ ] XAudio / XMA: FFmpeg (6.6)
- [ ] Bink: FFmpeg or decompiled (6.6)
- [ ] Havok (physics, collision; ~8% of the code; Havok 4.x, before the
      2007 `hkp` renaming, as is the MCC PC build): **decided: stays the
      last recompiled island.** The recompiled translation units are
      already native x64 code; game code calls them through a thin
      interface. To replace it last: decompile with the x64 Havok in
      `halo3_tag_test.exe` as the readable reference (same 4.x generation:
      34 of ~40 reflected class names match, no `hkp` names in either)
      and the PowerPC code as the authority on exact arithmetic (fused
      multiply-adds, rounding), checked against films like the rest. Off-the-shelf PC Havok SDKs don't
      fit: commercial license, newer versions, and Halo's tags hold data
      baked by this exact version (MOPP codes, shapes)

### 6.9 Nativization and the end of the recompiled game
- [ ] Data layout: host endianness and pointers everywhere (milestones A,
      B, C done; the rest of the guest structures converted), 64-bit
      clean
- [ ] Remove the guest memory reservation (4 GB) once no code uses
      guest addresses; remove `be<T>` from game code
- [ ] **Native milestone D**: no recompiled game function linked; only
      the Havok island (6.8) remains recompiled, behind its interface
- [ ] Havok last: decompiled (from the x64 reference) or otherwise
      replaced; then H3RecompLib, XenonRecomp and the PPC context are
      dropped from the build
- [ ] The game still needs the user's own disc data (maps, sounds,
      videos): an asset loader reads them from the user's copy; no game
      content in the repository
- [ ] Portability: Windows and macOS builds (SDL already abstracts the
      window, input and GPU), ARM64 (no x86 intrinsics left in game code)
- [ ] Afterwards (the point of a native port): mod support (tags from
      H3EK), new features, fixes the console version couldn't have
