---
title: Debugging
nav_order: 5
---

# Debugging switches

The runtime's environment variables, for developers and bug reports. Set
them in the environment, or in the launcher's DEBUG page (`[debug]` in
settings.ini), which the game applies at start.

## Input scripts and test controllers

`H3_MOUSE_SENSITIVITY=<factor>` overrides the launcher's mouse sensitivity.
`H3_TEST_MENU_CURSOR=<frame>:<x>,<y>;...` and `H3_TEST_MENU_CLICKS=<frame>,...`
place and click the menu cursor without a mouse (frame fractions).

`H3_INPUT_SCRIPT="40-40.4:down,f900-905:a"` holds buttons between two
times (seconds since start, or presented frames with `f`) for testing
without a keyboard (names: a b x y start
back up down left right lb rb ls rs; sticks and triggers: lup ldown lleft
lright rup rdown rleft rright lt rt). A `p2.` to `p4.` prefix presses it
on another controller (`f2300-2306:p2.start` joins player 2), and `unplug`
disconnects that controller while held. `H3_VIRTUAL_PADS=<n>` connects
controllers 2 to n+1 with no gamepad behind them, for split screen tests.
`H3_LOG_INPUT=1` logs state changes; `H3_LOG_USER=1` sign-ins and profile
calls.

## Logging, captures and experiments

Environment variables:

| Variable | Effect |
|---|---|
| `H3_STRICT_IMPORTS=1` | Abort on the first unimplemented import instead of logging it |
| `H3_LOG_FILES=1` | Log file opens (with share access), attribute queries, information changes, renames and symbolic links |
| `H3_LOG_READS=1` | Log every file read (path, offset, length, destination) |
| `H3_LOG_MEMORY=1` | Log physical memory allocations |
| `H3_LOG_GPU=1` | Log ring buffer setup, the first use of each PM4 opcode, and frame counts |
| `H3_RENDER=0` | Turn host rendering off (the GPU command stream is still processed) |
| `H3_RENDER_SCALE=<1-3>` | Override the launcher's render resolution |
| `H3_AA=off\|fxaa` | Override the launcher's anti-aliasing (FXAA on the finished frame) |
| `H3_ASYNC_SHADERS=1\|0` | Override background shader compilation |
| `H3_NO_WARMUP=1` | Don't rebuild the shader cache's pipelines at start (the list of them, `pipelines.txt` in the shader cache, starts over when the translator's output changes for every shader) |
| `H3_LAUNCHER_SCREENSHOT=out.png` | (H3Launcher) render the launcher window to a PNG and exit; `H3_LAUNCHER_KEYS="Down,Return"` sends keys first. Works with `QT_QPA_PLATFORM=offscreen` |
| `H3_NO_TEXTURES=1` | Bind gray placeholders instead of textures decoded from memory |
| `H3_NO_WRITEBACK=1` | Don't copy small resolves back to guest memory |
| `H3_DUMP_FRAME=<n>` | Save the n-th presented frame as `frame_<n>.png` |
| `H3_DUMP_RESOLVE=<n>\|all` | With `H3_DUMP_FRAME`: save that frame's n-th resolve (or all of them) as PNGs |
| `H3_LOG_RESOLVES=1` | Log every resolve (source, rectangle, destination, format) |
| `H3_LOG_USER=1`, `H3_LOG_CONTENT=1`, `H3_LOG_NOTIFY=1` | Log profile, content and notification calls |
| F10 (key) | Like F9, plus what each draw of the frame changed in color target 0: a crop of the changed rectangle per draw (`draw_<frame>_<n>_<x>_<y>.png`), listed in `deep_capture.txt`. The game freezes until it finishes (up to a minute or so); let it finish |
| F9 (key) / `H3_CAPTURE_AT=<frame>[,<frame>...]` | Capture the next frame (or those frames, one folder each) into `captures/capture_<n>/`: `gpu_capture.txt`, the shaders, the presented frame and every resolve |
| `H3_GPU_CAPTURE=<frame>` | Write `gpu_capture.txt`: every draw, resolve and shader load in that frame (counted in swaps) with the render state |
| `H3_DUMP_DRAWS=<first>-<last>` | With `H3_DUMP_FRAME`: save color target 0 after each of those draws (numbered like the capture's `draw #n`), 2304x1280 (render targets are in sample space: a 4x surface's pixel is 2x2 texels) |
| `H3_SKIP_DRAWS=<first>-<last>[,...]` | Drop those draws (numbered like the capture's `draw #n`) in every frame, to find which draw causes an artifact |
| `H3_NO_MEMEXPORT=1` | Don't run vertex shaders' memory exports (GPU particles, occlusion tests, water factors) |
| `H3_LOG_MEMEXPORT=1` | Log memory export dispatches, their records (first few values) and buffer use |
| `H3_TICK_RATE=<n>` | Game ticks (frames) per second, overriding the launcher's frame rate (30 original, 60 experimental) |
| `H3_TICK_RATE_SKIP=<n>` | Leave the first n game time setups at 30 ticks (1: menus at 30, levels at `H3_TICK_RATE`; for frame-timed input scripts) |
| `H3_PRESENT_INTERVAL=<n>` | Vblanks each swap waits for (Halo 3 asks for 1; with the 60 Hz tick it is 0, swaps present at once). Set 1 to compare |
| `H3_LOG_TICK_RATE=1` | Log the game's time globals (tick rate, tick count) every 300 frames |
| `H3_POKE=<frame>:<address>:<hex bytes>[,...]` | Write bytes into guest memory at that swap (addresses as for `H3_DUMP_MEMORY`), for experiments |
| `H3_LOG_WAIT_DETAIL=1` | Log every WAIT_REG_MEM (address, function, reference, current value) |
| `H3_APPROX_GAMMA=1` | Approximate the 8_8_8_8_GAMMA piecewise-linear curve as gamma 2 (the old behaviour), to compare |
| `H3_LOG_CARRYOVER=1` | Log every EDRAM format switch (base, formats, rectangle carried over) |
| `H3_FULL_CARRYOVER=1` | Carry whole render targets over on EDRAM format switches instead of only what changed, to compare |
| `H3_FAKE_ZPD=1` | Report every occlusion query as visible (the old behaviour) instead of counting samples (tac lights, light glows) |
| `H3_MEMEXPORT_ASYNC=1` | Don't wait for earlier frames' memory exports before reading their data (the old behaviour) |
| `H3_NO_EXPORT_SCATTER=1` | Don't copy a memory export's results into the data later dispatches in the same frame read (GPU particles spawned and updated in one frame: muzzle flashes, splashes) |
| `H3_NO_EXPORT_BATCHING=1` | Record each memory export dispatch on its own (the old way) instead of in batches of independent dispatches |
| `H3_HIGHLIGHT_VS=<hash>` | Draw that vertex shader's draws (hash prefix of a captured `gpu_shaders/vs_<hash>.bin`; `list` prints them) in magenta with depth test off, and log their vertex fetch data |
| `H3_LOG_VS_DATA=<hash>[:<dwords>]` | Log each draw of that vertex shader (hash prefix as for `H3_HIGHLIGHT_VS`): primitive, clip control, some VS constants, its vertex fetch ranges, which of this frame's memory exports write them, and their records (record size in dwords, default 16; contrail draws log exactly the points they read); `H3_LOG_VS_DATA_FRAMES=<first>-<last>` limits it to those frames |
| `H3_VERIFY_VS_DATA=1` | With `H3_LOG_VS_DATA`: also read those records back from the GPU after the export scatter (all-zero records, `JUMP` for a stray point) |
| `H3_DEBUG_VS_POSITIONS=1` | With `H3_LOG_VS_DATA`: run the draw's vertex shader as compute and log every vertex's clip-space position (`ODD`: behind the eye or far off screen). Auto-indexed draws; slow |
| `H3_TRACE_EXPORTS=<a>-<b>` | Log every memory export record applied in that physical range (frame, address, value, shader); `H3_TRACE_EXPORTS_VS=<8 hex>[,...]` only those shaders, `H3_TRACE_EXPORTS_AT=<modulus>:<remainder>` only those offsets. Output can be gigabytes |
| `H3_LOG_STREAMS=<8 hex>` | Log that memexport shader's stream constants (base, format, endian) |
| `H3_CHECK_EXPORT_DUPES=1` | Report dispatches that export one address twice with different values |
| `H3_NO_STAGING_REUSE=1` | Copy every vertex buffer and convert every index list again on each flush (the old way), instead of reusing what earlier flushes this frame staged when the guest memory is unchanged |
| `H3_NO_RESIDENT=1` | Stage every vertex buffer into each frame's upload buffer (the old way) instead of keeping ones that don't change in a resident GPU buffer |
| `H3_NO_CONSTANT_REUSE=1` | Snapshot every draw's float constants, instead of sharing the previous draw's snapshot when the command stream wrote no constants in between |
| `H3_UNIFORM_CONSTANTS=1` | Push draws' float constants as uniform data draw by draw (the old way), instead of one storage buffer per flush that shaders index through the draw's first instance. Translates shaders differently, so they compile again the first time |
| `H3_LOG_VOLUMES=1` | Log each 3D or stacked texture once (size, layers, mips) |
| `H3_DUMP_TEXTURE=<physical address>` | Save that texture's decoded levels and layers (RGBA8 formats) as `tex_<address>_l<level>_<layer>.png` |
| `H3_NO_CULL=1` | Don't cull faces (PA_SU_SC_MODE_CNTL), to compare |
| `H3_NO_POLY_OFFSET=1` | Ignore polygon offset (depth bias: Halo's decals and shadow maps) |
| `H3_NO_STENCIL_RESTORE=1` | Don't turn stencil drawn through a color view (Halo's dither pattern) into stencil |
| `H3_LOG_ZPD=1` | Log occlusion query (EVENT_WRITE_ZPD) events |
| `H3_DUMP_PRIM=<type>` | With `H3_DUMP_FRAME`: save color target 0 before and after every draw of that primitive type (17: tessellated triangle patches, Halo's water) |
| `H3_DUMP_RESTORE=<frame>` | After each depth restore in that frame (Halo rewriting depth through a color view), save the color bytes and the resulting depth (`restore_<frame>_<n>_*.png`; depth also as raw floats in `.f32`, as are all 32-bit float dumps) |
| `H3_DUMP_SOURCE=1` | With `H3_DUMP_RESOLVE`: also save each resolve's source render target as `source_<frame>_<n>.png` |
| `H3_DUMP_VIEWS=1` | With `H3_DUMP_FRAME`: save each part of a resolved texture copied out for sampling at another size (`view_*.png`) |
| `H3_DUMP_SCALE=<n>` | Divide 16-bit float colors by n in dumps (Halo's final 7e3 target holds 32x values) |
| `H3_SNAPSHOT_EVERY=<n>` | Save every n-th presented frame as `frame_<n>.png` (from frame `H3_SNAPSHOT_FROM` to `H3_SNAPSHOT_TO`, if set) |
| `H3_SNAPSHOT_ASYNC=1` | Write snapshot PNGs on a background thread, so saving every frame doesn't slow frames down (real-time-driven animation) |
| `H3_LOG_CLEARS=1` | Log D3D clears (screen-space rectangle draws) with their rectangles, color and surface |
| `H3_LOG_AUDIO=1` | Log audio frames submitted and their peak level, once a second |
| `H3_AUDIO_DUMP=<file>` | Also write the audio output as raw 6-channel 48 kHz float (`ffmpeg -f f32le -ac 6 -ar 48000 -i <file> out.wav`) |
| `H3_LOG_FPS=1` | Log presented frames per second and the slowest frame, once a second; also `[vram]` lines with the video memory held by render targets, resolves, textures and buffers (every 150 frames, and whenever it moves by 64 MB), `[passes]` lines: GPU passes per frame by kind (draw, copy, depth/stencil restore, blit) with the host pixels they cover, and `[cp]` lines: per frame, how long the command processor thread executes packets (wall and CPU time; the gap is time blocked) and sits idle. A busy thread with little idle time is what limits the frame rate |
| `H3_LOG_SLOW=1` | Log shader translations, pipeline creations and texture uploads that take 20 ms or more (pipelines with their shaders' cache names) |
| `H3_DUMP_GLSL=1` | Write each translated shader's GLSL to the working directory as `<cache name>.glsl` |
| `H3_DUMP_MEMORY=<frame>:<address>:<bytes>[,...]` | Save guest memory at that swap as `mem_<frame>_<address>.bin` (hex address; below 0x80000000 physical, otherwise a guest address such as the executable's data) |
| `H3_WATCH_WRITES=<first>-<last>:<physical address>:<bytes>` | Linux: write-protect that range during those frames and log every host instruction that writes it to `write_watch.txt` (offsets into the executable; `addr2line`/`nm` them) |
| `H3_PROFILE=<first>-<last>` | Sample host CPU time between those frames into `profile.txt`; `tools/h3profile.py` symbolizes it. With `H3_PROFILE_STACKS=1` it also attributes time in libraries to the calling function |
| `H3_PROFILE_WALL=1` | With `H3_PROFILE`: sample only the command processor thread, 1000 times per wall-clock second whether it runs or not, so time it spends blocked (GPU driver, fences, locks) shows up in the blocking call |
| `H3_GPU_TIMING=<frame>` | From that swap on, run each draw flush and each labelled piece of other GPU work (resolves, copies, restores, carry-overs, memexport) alone on the GPU and wait for it, logging GPU ms per frame by flush and label (`[gpu timing]`). Slow; trustworthy for large passes, while waiting after many small dispatches inflates their numbers |
| `H3_GPU_TIMING_COARSE=1` | With `H3_GPU_TIMING`: no waits around labelled work; everything between two flushes is timed together, and the longest of those gaps are listed by the flush they precede |
| `H3_GPU_TIMING_LABELS=<label>[,...]` | With `H3_GPU_TIMING`: wait around those labels only (e.g. `memexport,resolve`) |
| `H3_WATCH_READS=<first>-<last>:<address>:<bytes>` | Like `H3_WATCH_WRITES`, but every access (reads too); writes `read_watch.txt` |
| `H3_TRAP_ON_STUCK=1` | Raise SIGTRAP when a thread is stuck on a critical section (for an attached debugger) |
| `H3_RANDOM_SEED=<n>` | Fixed random seeds (XNetRandom, the game's clock-based seeds, the local random seed at level start, a seed of its own for the particle thread): the game state lands at the same address and replays compare word for word (`tools/films/`) |
| `H3_LOCKSTEP=1` | Exactly one game tick per frame (the game runs as fast as frames finish), so per-frame state replays alike |
| `H3_LOCKSTEP_TICKS=<n>` | Exactly n game ticks per frame (testing multi-tick frames) |
| `H3_CAPTURE_LEVEL_TICK=<n>` | F9 capture (F10 with `H3_CAPTURE_DEEP=1`) when the first level's game tick reaches n: live runs drift by hundreds of frames, level ticks don't (one deep capture per run: cutscenes keep real time through the freeze) |
| `H3_STATE_HASH=<file>` | Append `<copy> <tick> <hash>` for every per-tick game state copy: a checksum of the 4 MB game state per tick (`tools/films/`) |
| `H3_STATE_MASK=<file>` | Leave the cosmetic state that file names (data arrays, object fields, words; learned by `tools/films/learn_mask.py`) out of `H3_STATE_HASH` |
| `H3_STATE_DUMP=<tick>[,...]` | With `H3_STATE_HASH`: save the raw game state at those ticks as `state_<tick>.bin` |
| `H3_ORIGINAL=<name>[,...]` | Run the recompiled originals instead of those decompiled replacements (`tag_get`, `character_get_general_properties`, ...; `H3Native/`) |
| `H3_CHECK_NATIVE=1` | Run each decompiled replacement and its original, log differences (`[native]`); the game gets the original's result |
| `H3_CHECK_TAGS=1` | Compare the loaded tag index with the native cache file reader's, log the tag cache, and ~10 s after a map loads report tag bytes the game changed at runtime (`[tags]`) |
| `H3_STATE_RECORD=<file>` | With `H3_STATE_HASH`: record every tick's game state compactly (words changed since the previous tick; ~90 KB a tick) |
| `H3_CAPTURE_LEVEL=<t>,...` / `H3_CAPTURE_LEVEL_TICK=<n>,...` | GPU frame capture (as F9, or F10 with `H3_CAPTURE_DEEP`) at those level times / game ticks |
| `H3_CAPTURE_HOLD_TICKS=1` | With `H3_CAPTURE_LEVEL_TICK`: hold the game until each capture is written, so consecutive ticks land on their own frames |
| `H3_CAPTURE_PS_CONST=<n>` | In captures, also log every write to pixel shader constant c<n> |
| `H3_DEBUG_PS=<hash prefix>@<case>=<GLSL vec4>[;...]` | Debug variants of one pixel shader: deep captures (F10) replay each draw once per expression and save its value per pixel (`tools`: `pixtrace.py`) |
| `H3_DUMP_ADDRESS=<hex>` / `H3_DUMP_TARGET=<0-3>` | With `H3_DUMP_FRAME`: dump the resolved texture at that address / that color target |
| `H3_SNAPSHOT_LEVEL=<first>-<last>[/<every>]` | Save the presented frame at those level times (`level_<time>.png`; film screenshot A/Bs) |
| `H3_STATE_RECORD_TICKS=<first>-<last>[,...]` | With `H3_STATE_RECORD`: record only those ticks |
| `H3_NATIVE_SHADOW=1` | Run each decompiled replacement's original first, then the replacement on a copy, to find effects besides memory |
| `H3_FRAME_DELAY_MS=<n>` | Sleep n ms per main loop iteration (simulates a slow machine: more than one tick per frame) |
| `H3_POLY_SLOPE_SCALE=<factor>` | Scale the slope part of the polygon offset |
| `H3_NO_DEPTH=1` / `H3_NO_STENCIL=1` / `H3_DEPTH_FLIP=1` | No depth and stencil tests / no stencil test / swap less and greater depth compares |
| `H3_BORDER_CLAMP=1` | Emulate clamp-to-border in the shader (experiment) |
| `H3_ALIAS_KEEP_HIGHER=<ps hash high 32 bits>` | For that pixel shader, the higher of two aliased color outputs wins (experiment) |
| `H3_CHECK_TRIG=1` | Compare the game's sin and cos with the host's on every call; log each new worst error |
| `H3_LOG_DEPTH=1` / `H3_LOG_SCISSOR=1` | Log each distinct depth state / window offsets and the scissors they give |
| `H3_LOG_REG_WRITES=<first>-<last>` | Log writes to that GPU register range (hex indices) |
| `H3_LOG_STREAMS_AT=<hex physical address>\|all` | Log each distinct memexport stream holding that address (shader, first frame) |
| `H3_LOG_TEXTURE_DECODES=1` | Log every texture decode, and textures bound without their base level per frame |
| `H3_LOG_TICKS=1` | Game tick and wall-clock time at each tick and presented frame |
| `H3_LOG_WAITS=1` | Every 150 frames, the time threads spent blocked, by object |
| `H3_LOG_TRYLOCK=1` | Failed critical section tries, with the caller |
| `H3_LOG_XMA=1` | Per-context XMA diagnostics (loops, buffer swaps, bad frames, exactly repeated frames, fields changed mid-decode) |
| `H3_LOG_XCONFIG=1` | Log console configuration queries |
| `H3_LOG_EXPOSURE=1` / `H3_LOG_AI_BURST=1` / `H3_LOG_BARRELS=1` / `H3_LOG_DAMAGE=1` / `H3_LOG_CAMERA=1` | 60 Hz fix checks: exposure updates per tick / AI bursts the minimum raised / ticks between each weapon's rounds / every damage call / cinematic camera modes |
| `H3_LOG_OBJLIGHT=<first>[-<last>]` / `H3_LOG_ATOC=<calls>` / `H3_LOG_EMOTION=1` / `H3_NO_EMOTION=1` | Object lighting calls in that tick range / alpha-to-coverage and blend mode calls / facial emotion calls / drop facial emotions |
| `H3_LAUNCHER_SIZE=<w>x<h>` | The launcher's window size |
| `H3_SCAN_FLOAT=<frame>:<value>[,...]` / `H3_POKE_FLOAT=<frame>:<hex address>=<value>[,...]` | Log every guest address holding a float near those values at that frame / write floats from that frame on (finding game state by value: the FOV) |
| `H3_FOV=<degrees>` / `H3_LOG_FOV=1` | Override the FOV setting / log the unit FOVs scaled per map |
| `H3_LOG_PC_TEXT=1` / `H3_LOG_NET=1` / `H3_LOG_LOOK=<calls>` / `H3_LOG_MOUSE=1` | How many English strings were reworded / system link calls and datagrams / what player 1's look function changed at those calls (research) / native mouse look per tick (movement, weapon rate, zoom scale) |

## Turning a fix off

Each of these turns one fix or improvement back off. They're there for
comparison and as escape hatches: if something looks or plays wrong, try the
switch for the area it's in and report whether it changes.

| Variable | Turns off |
|---|---|
| `H3_NO_60HZ_FIXES=1` | The 60 Hz content fixes (weapon fire rates, camera shake, exposure, physics step, AI bursts...); the tick rate stays |
| `H3_NO_STRUCTURED_SHADERS=1` | Structured shader control flow (back to the slower-compiling state machine) |
| `H3_NO_REGISTER_LOD=1` | Explicit texture LOD (shaders that pick their own mip level) |
| `H3_NO_WEIGHTS_OFFSET=1` / `H3_NO_SNAP_WEIGHTED_TAPS=1` | The shadow filtering fixes (weights offset; taps snapped to guest texels at a render scale) |
| `H3_NO_CLIP_PLANES=1` | User clip planes |
| `H3_DEPTH_ONLY_PS=1` | Skipping the pixel shader's kills in depth-only passes (shadow maps) |
| `H3_CTX1_LOW_R=1` | The CTX1 normal map channel order fix |
| `H3_NO_DEPTH_RESTORE=1` | Restoring depth written as color |
| `H3_NO_DEPTH_BYTES_SWIZZLE=1` | The byte order fix for depth read back as color |
| `H3_KEEP_ALIASED_TARGETS=1` | Unbinding a color target bound to two slots |
| `H3_NO_FORCED_FILTERS=1` | Filters forced by shader instructions (samplers follow the fetch constant only) |
| `H3_NO_KEPT_BASES=1` | Drawing a demoted texture (base level evicted, being read back) at full size, as MCC does: back to the 360's half-resolution popping |
| `H3_NO_STANDIN_MIPS=1` | Mips under a stand-in level (a texture whose base isn't resident yet draws its largest resident level alone) |
| `H3_NO_GAMMA=1` | The display gamma ramp (front buffer shown as is) |
| `H3_XMA_SUBFRAMES=0` | The XMA subframe model (music notes repeating) |
| `H3_NO_PC_TEXT=1` | PC wording in the English text ("this PC" for "this Xbox 360 console", the controller, sign-out tips) |
| `H3_MEMEXPORT_FENCE_ASYNC=1` | Waiting for memexport fences (they land at once, as before) |
| `H3_SYNC_WRITEBACK=1` | Asynchronous resolve write-back (waits instead) |
| `H3_SPIN_WAITS=1` | Sleeping in long GPU waits (always spins) |

CMake option `-DH3_TRACE_D3D=ON` counts and samples calls into the game's
statically linked D3D library and writes `d3d_trace.txt` at frame 150 (or
`H3_TRACE_D3D_FRAME`), sampling the 50 frames before it.

CMake option `-DH3_TRACE_FIRST_CALLS=ON` writes `first_calls.txt`: the first
call of every recompiled function with the frame it happened in; with
`H3_TRACE_WINDOW=<first>-<last>` also `window_calls.txt`, every function
called in that frame range. Use a separate build directory; it slows things
down.

CMake option `-DH3_TRACE_MODULE_CALLS=ON` logs every function call in the
recompiled DLLs with arguments and return values (very slow).

The runtime also reports locks held for more than two seconds and GPU waits
that last more than a second.