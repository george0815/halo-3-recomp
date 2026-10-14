# halo-3-recomp

An in-progress static recompilation of Halo 3 (Xbox 360) to native PC code,
with decompilation of individual subsystems as a long-term goal.

**No game data is included.** You need your own legally dumped copy of the
game. See [docs/DUMPING.md](docs/DUMPING.md).

## How it works

1. [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) translates every
   PowerPC function in the game's `default.xex` into C++
   (`H3RecompLib/ppc/`, generated and git-ignored).
2. The runtime (`H3Recomp/`) provides what the 360 used to: guest memory,
   kernel/XAM imports, graphics, audio, input.
3. Over time, recompiled functions get replaced with hand-written code.

This is the same approach as [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp),
which is the best reference for what a finished runtime looks like.
[Xenia](https://github.com/xenia-project/xenia) is the reference for kernel
and GPU behaviour.

## Layout

```
H3Recomp/            runtime: entry point, guest memory, kernel imports
H3Launcher/          Qt launcher: settings, profile, controls (optional)
H3RecompLib/config/  XenonRecomp config + jump tables for Halo 3
H3RecompLib/ppc/     generated C++ (ignored)
H3RecompLib/modules/ generated C++ for the DLLs the game loads (ignored)
tools/h3xexinfo/     XEX inspector: layout, imports, helper addresses, stubs
tools/recompile.sh   full XEX -> C++ pipeline
tools/build_ffmpeg.sh  FFmpeg 7.1 + XMA frame decoder (tools/patches/), static
tools/xdvdfs_extract.py  disc image -> private/game
tools/h3analysis.py  jump table / function boundary fixups
thirdparty/          XenonRecomp (submodule)
private/             your game files (ignored)
docs/                dumping guide, roadmap
```

## Building

Requires CMake 3.20+, Ninja, Clang 18+, SDL3, shaderc and a Vulkan driver.
The launcher additionally needs Qt 6 (Widgets); without it, it is skipped.
XMA audio needs a patched FFmpeg, built once by `tools/build_ffmpeg.sh`
(downloads FFmpeg 7.1; needs curl, make and a C compiler).

```sh
git submodule update --init --recursive

# Tools only (no game files needed)
cmake --preset release
cmake --build --preset release

# Put your default.xex in private/, then:
build/release/tools/h3xexinfo/h3xexinfo private/default.xex
# paste the printed addresses into H3RecompLib/config/halo3.toml
tools/build_ffmpeg.sh
tools/recompile.sh
cmake --build --preset release
build/release/H3Recomp/H3Recomp private/default.xex
```

The game's hard drive (cache partitions, saves) is created in `private/hdd/`,
and compiled host shaders are cached in `private/shadercache/` (safe to delete),
along with `pipelines.txt`, the list of shaders and pipelines this PC has used,
which the game rebuilds in the background at start (launcher: Video >
Precompile shaders) so areas you've already played don't stutter.
Delete `private/hdd/` to start from a clean console (this also clears the
saved profile settings, `private/hdd/profile_settings.bin`).

### Launcher

`build/release/H3Launcher/H3Launcher` edits the settings and starts the game.
Keyboard, mouse (right-click goes back) or a gamepad; the layout follows the
window size.
It finds `private/default.xex` by itself (or use GAME LOCATION, or pass the
path as its argument). Its menus use Halo 3's own font, read from the game's
`maps/fonts/font_package.bin`.

- **Video**: window size, fullscreen (F11 or Alt+Enter toggles it in game),
  vsync, the render resolution (1x native 1280x720, 2x or 3x), anti-aliasing
  (FXAA; Halo 3 itself has none), precompiling shaders at start, and background shader compilation (no stutter the first
  time something new appears, but it can be missing for a few frames).
- **Controls**: up to three keys or mouse buttons per action, mouse
  sensitivity and inversion. Gamepads always use the standard layout.
- **Debug**: the logging and tracing switches below (set in `settings.ini`
  and applied by the game at start, so they work without the launcher too),
  writing the console output to `private/h3recomp.log`, and unlocking every
  campaign mission.
- **Profile**: gamertag, a gamer picture (shown on the launcher's profile
  card; Halo 3 itself shows your emblem), and the profile options Halo 3
  copies into a new player profile (invert look, vibration, auto-aim,
  auto-center).

Settings are saved to `settings.ini` next to `default.xex` (plain text, safe
to edit by hand; see `H3Recomp/config/settings.h`), and the game reads them
at start. Whatever the game saves to your profile (its own settings, controls
and emblem) persists in `private/hdd/profile_settings.bin`.

### Controls

Controller 0 is the keyboard and mouse merged with the first gamepad SDL
finds. The keyboard and mouse bindings are set in the launcher; the
defaults:

| Key | Controller |
|---|---|
| W A S D | Left stick (move) |
| I J K L | Right stick (look) |
| Arrow keys | D-pad (menus) |
| Space, Enter | A |
| Backspace, V | B |
| F | X |
| Q | Y |
| G / R | Left / right bumper |
| Left Shift | Left trigger |
| Left Ctrl, E | Right trigger |
| C / Z | Left / right stick click |
| Escape | Start |
| Tab | Back |
| Mouse | Right stick (look); click the window to capture the mouse, F1 releases it |
| Left mouse button | Right trigger (fire) |
| Right mouse button | Right stick click (zoom) |
| Middle mouse button | B (melee) |
| Mouse wheel | Y (switch weapons) |

`H3_MOUSE_SENSITIVITY=<factor>` overrides the launcher's mouse sensitivity.

`H3_INPUT_SCRIPT="40-40.4:down,f900-905:a"` holds buttons between two
times (seconds since start, or presented frames with `f`) for testing
without a keyboard (names: a b x y start
back up down left right lb rb ls rs; sticks and triggers: lup ldown lleft
lright rup rdown rleft rright lt rt). `H3_LOG_INPUT=1` logs state changes.

### Debugging

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
| `H3_STATE_HASH=<file>` | Append `<copy> <tick> <hash>` for every per-tick game state copy: a checksum of the 4 MB game state per tick (`tools/films/`) |
| `H3_STATE_MASK=<file>` | Leave the cosmetic state that file names (data arrays, object fields, words; learned by `tools/films/learn_mask.py`) out of `H3_STATE_HASH` |
| `H3_STATE_DUMP=<tick>[,...]` | With `H3_STATE_HASH`: save the raw game state at those ticks as `state_<tick>.bin` |
| `H3_ORIGINAL=<name>[,...]` | Run the recompiled originals instead of those decompiled replacements (`tag_get`, `character_get_general_properties`, ...; `H3Native/`) |
| `H3_CHECK_NATIVE=1` | Run each decompiled replacement and its original, log differences (`[native]`); the game gets the original's result |
| `H3_CHECK_TAGS=1` | Compare the loaded tag index with the native cache file reader's, log the tag cache, and ~10 s after a map loads report tag bytes the game changed at runtime (`[tags]`) |
| `H3_STATE_RECORD=<file>` | With `H3_STATE_HASH`: record every tick's game state compactly (words changed since the previous tick; ~90 KB a tick) |

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

## Status

Boots through kernel setup, the Waves audio DLLs, the intro video and map
loading (`mainmenu.map`, `shared.map`) to the main menu, which renders
through the host GPU backend (Xenos command stream -> SDL_GPU/Vulkan): the
lit menu scene, the menu and its text, the logos. Keyboard/gamepad input
works, a local profile is signed in, and Campaign and multiplayer games start:
levels load and run with the HUD, though in-game frames are still washed
out. The intro movie shows through a debug shortcut that reads Bink's
decoded frames from guest memory (`H3Recomp/video/bink_view.cpp`). The
window title shows frames presented.
See [docs/ROADMAP.md](docs/ROADMAP.md).
