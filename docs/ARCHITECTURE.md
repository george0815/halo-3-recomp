# Architecture

This document is the map of the project: what the pieces are, how they talk
to each other, and where to start reading. Each source file also starts with
a comment describing its own job.

## The big picture

Halo 3 is an Xbox 360 game: PowerPC code (`default.xex`) running on the
360's kernel, talking to the Xenos GPU and the XMA audio hardware. This
project turns the game's PowerPC code into C++ ahead of time (*static
recompilation*) and supplies everything around it on the PC:

```
             build time                               run time
 default.xex ──XenonRecomp──▶ H3RecompLib/ppc/*.cpp ─┐
 (your dump)                  (recompiled game code)  │   links into
                                                      ▼
                                   ┌────────── H3Recomp (the executable) ──────────┐
                                   │ recompiled game code                           │
                                   │   │ calls kernel exports (imports)             │
                                   │   ▼                                            │
                                   │ kernel/  (high-level emulation of the 360 OS)  │
                                   │   │ GPU ring buffer      │ audio frames         │
                                   │   ▼                      ▼                      │
                                   │ gpu/ (Xenos → Vulkan)   apu/ (XAudio, XMA)      │
                                   │   │                      │                      │
                                   │   ▼                      ▼                      │
                                   │ video/window (SDL3)     SDL3 audio              │
                                   └────────────────────────────────────────────────┘
```

Nothing from the retail game is in the repository: you supply `private/`
(see `docs/DUMPING.md`), and everything derived from it (the recompiled C++,
shader caches) stays local.

## Directory layout

| Path | What lives there |
|---|---|
| `H3Recomp/` | The executable: runtime, kernel, GPU, audio, input, window |
| `H3Recomp/kernel/` | The 360 kernel (`xboxkrnl`) and system library (`xam`) exports the game imports, plus threads, kernel objects, file system and module loading |
| `H3Recomp/gpu/` | Xenos GPU: the command processor (reads the game's GPU commands) and the renderer (draws them with SDL_GPU / Vulkan) |
| `H3Recomp/gpu/renderer/` | The renderer's internals, split by job (shaders, copies, render targets, resolves, textures, draws, debug dumps); `renderer/internal.h` explains the split |
| `H3Recomp/gpu/shader/` | Xenos shader microcode → GLSL translator |
| `H3Recomp/apu/` | Audio: the XAudio render driver (output) and the XMA hardware decoder |
| `H3Recomp/input/` | XInput emulation from keyboard, mouse and gamepads |
| `H3Recomp/video/` | The SDL window, presentation, event loop |
| `H3Recomp/hooks/` | Host replacements for specific game functions (fibers, XMA registers) |
| `H3Recomp/debug/` | Optional debugging tools (profiler, write watch, tracing) |
| `H3Recomp/config/` | Runtime settings (`env.h`: the `H3_*` environment switches) |
| `H3RecompLib/` | Build glue for the recompiled code: recompiler configs, generated sources (ignored) |
| `tools/` | Pipeline scripts (recompile, FFmpeg build), XEX inspector, shader test tool |
| `thirdparty/XenonRecomp/` | The recompiler (submodule, with local patches; see `private/`) |
| `docs/` | This file, the roadmap, the dumping guide |

## How a frame of the game runs

1. **Boot** (`H3Recomp/main.cpp`): reserve the 4 GiB guest address space
   (`memory.cpp`), open the window, start the GPU and XMA workers, load the
   XEX (`kernel/modules.cpp`), then run the game's entry point `_xstart` on
   a thread of its own. The host main thread runs the SDL event loop.
2. **Game code** is the recompiled C++. Every guest function is a C++
   function `sub_XXXXXXXX(PPCContext& ctx, uint8_t* base)`; `ctx` holds the
   PowerPC registers and `base` is the start of guest memory. Guest pointers
   are 32-bit offsets from `base` (`GuestPtr`, `GuestRef` in `memory.h`),
   and guest data is big-endian (`be<T>`).
3. **Kernel calls**: the game imports functions from the 360's kernel. Each
   import is a weak stub in the generated `import_stubs.cpp`; the files in
   `kernel/` override the ones we implement with `PPC_FUNC(__imp__Name)`.
   Unimplemented ones log once and return 0 (`kernel/imports.cpp`).
4. **Threads** created by the game are host threads with guest state (KPCR,
   KTHREAD, stack, TLS; `kernel/thread.cpp`). Kernel objects (events,
   semaphores, mutants, timers, threads) and waits are in
   `kernel/objects.cpp`.
5. **Graphics**: the game's Direct3D library (compiled into the game) writes
   GPU commands (PM4 packets) into a ring buffer in memory.
   `gpu/command_processor.cpp` reads them on its own thread, like the real
   GPU front end: register writes, constant loads, fences, interrupts,
   draws and resolves. Draws and resolves go to the renderer
   (`gpu/renderer.h`, internals in `gpu/renderer/`), which translates shaders
   (`gpu/shader/`), decodes textures from guest memory (`gpu/texture.cpp`),
   and renders with SDL_GPU. On a swap the finished frame is handed to the
   window (`video/window.cpp`).
6. **Audio**: the game mixes audio itself and hands 5.1 frames to the
   XAudio render driver (`apu/audio_system.cpp`), which sends them to SDL.
   Compressed sounds and music are XMA: the game fills in hardware decoder
   contexts and the XMA decoder (`apu/xma_decoder.cpp`) decodes them with
   FFmpeg (built by `tools/build_ffmpeg.sh`).
7. **Input**: XInput calls read the state that `input/input.cpp` builds from
   SDL keyboard, mouse and gamepad events.

## Key concepts

- **Guest memory**: one 4 GiB host reservation. The 360's physical memory
  is visible at three addresses (0xA0000000, 0xC0000000, 0xE0000000); all
  three are mapped onto the same host memory (`memory.cpp`).
- **Big-endian**: the 360 is big-endian, x86 is little-endian. Anything read
  from guest memory as a multi-byte value goes through `be<T>` or a byte
  swap.
- **High-level emulation**: the kernel and hardware are not emulated at the
  register level; instead their behaviour as seen by the game is
  reimplemented (with Xenia's source as the reference).
- **Hooks**: when the recompiled version of a game function can't work on
  the PC (fibers that switch stacks, MMIO register writes), a host function
  replaces it (`hooks/`).
- **EDRAM**: Xenos renders into 10 MB of embedded memory, then *resolves*
  (copies) results to main memory. The renderer keeps render targets as host
  textures and resolves into host textures standing in for guest memory.

## Debugging

Most tools are switched on with environment variables; the README lists
them all (`H3_GPU_CAPTURE`, `H3_DUMP_FRAME`, `H3_LOG_FPS`, `H3_PROFILE`,
`H3_WATCH_WRITES`, `H3_AUDIO_DUMP`, ...). `private/SESSION_NOTES.md` records
how past bugs were found, which is a good guide to the techniques.

Performance ideas that haven't been tried yet are marked `// PERF:` in the
source and listed in `private/TODO.md`.

## Where to start reading

1. `H3Recomp/main.cpp`: boot order.
2. `H3Recomp/memory.h`: guest memory and how guest pointers work.
3. `H3Recomp/kernel/xboxkrnl.cpp` and `kernel/imports.cpp`: how kernel
   exports are written.
4. `H3Recomp/gpu/command_processor.h` then `gpu/renderer.h`: the graphics
   pipeline.
5. `docs/ROADMAP.md`: what's done and what's next.
