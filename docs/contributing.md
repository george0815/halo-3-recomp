---
title: Contributing
nav_order: 8
---

# Contributing
{: .no_toc }

A guide for programmers who want to work on the project, written so you can
get started without anyone to ask. You need ordinary C++ (classes, lambdas,
`std::` containers) and to be comfortable building a CMake project. You do
**not** need to know PowerPC assembly, the Xbox 360 or Halo's internals to
begin: most changes start from an existing example in the code, and this
page points you at them.

1. TOC
{:toc}

## Getting set up

1. Build the project from source: [Building](building.html). You need your
   own copy of the game (`private/default.xex` and the disc files in
   `private/game/`, see [Dumping](DUMPING.html)); nothing from the game is
   in the repository.
2. Run it: `build/release/H3Recomp/H3Recomp private/default.xex`, or start
   `build/release/H3Launcher/H3Launcher`.
3. Read the [architecture](ARCHITECTURE.html) page once. It explains the
   pieces and where to start reading.

## The project in five sentences

1. The game's PowerPC code was translated to C++ ahead of time by
   XenonRecomp: every game function is a C++ function named after its
   address, `sub_82205690(PPCContext& ctx, uint8_t* base)`. Those files are
   generated (`H3RecompLib/ppc/`); never edit them.
2. `ctx` holds the PowerPC registers: arguments arrive in `ctx.r3`, `ctx.r4`
   ... (`ctx.f1`... for floats) and the result goes back in `ctx.r3`
   (`ctx.f1`). `ctx.lr` is the return address: who called.
3. The game's memory is one 4 GB block (*guest memory*). Guest addresses are
   32-bit numbers; `GuestRef<T>(address)` reads or writes a value there and
   swaps the byte order for you (the 360 is big-endian). See `H3Recomp/memory.h`.
4. Everything the console provided (its kernel, GPU, audio, controllers) is
   reimplemented by the *runtime*, `H3Recomp/`.
5. To change what the game does, you *hook* one of its functions: your C++
   runs instead of (or around) the recompiled one. Hooks live in
   `H3Recomp/hooks/`.

## How the code is written

Follow what's there; the main rules:

- **Comments.** Every file starts with a comment saying what it's for.
  Every function and struct has a comment above it saying what it does and,
  where it isn't obvious, why. Explain every magic number: where an address
  or an offset comes from (`// player control record: +0x2A requested zoom
  level`). Write for someone who has never seen the code. When you find out
  something non-obvious while debugging, put it in a comment next to the
  code it explains.
- **Names.** Functions and types `PascalCase`; locals and parameters
  `camelCase`; constants `kPascalCase`; globals `g_name`; thread-locals
  `t_name`; class members `m_name` (the launcher, a Qt program, uses
  `name_`). Game functions keep their address names (`sub_8216AE48`) with a
  comment naming what they are.
- **Formatting.** Four spaces, no tabs, braces on their own lines, about
  120-150 columns at most.
- **Switches, not deletions.** A change to the game's behavior gets an
  environment switch that turns it back off (`H3_NO_...`), listed on the
  [Debugging](debugging.html) page. It makes A/B comparisons and bug reports
  much easier.
- **No game code or data in the repository.** Addresses, offsets and your
  own notes are fine; bytes copied out of the game are not.

## Recipes

Each recipe follows a real example in the code; read the example alongside.

### Add a setting to settings.ini and the launcher

Example: `centered_crosshair` (search the repository for `centeredCrosshair`).

1. **The field**: add it to the right struct in `H3Recomp/config/settings.h`
   (`VideoSettings`, `ControlSettings`, ...) with its default and a comment.
2. **Reading**: in `H3Recomp/config/settings.cpp`, add an
   `else if (key == "your_key")` line in that section's parser (see
   `centered_crosshair` there).
3. **Writing**: add the matching line to `Save()` in the same file, so the
   launcher keeps it.
4. **The launcher**: add a `MenuItem` on the right page in
   `H3Launcher/launcher_window.cpp` (`VideoPage()` etc.): a label, a
   description, `value` (what to show) and `step` (what left/right does).
5. **Using it**: in the game, `h3::config::Active().video.yourField`
   (settings are read once at start, so read it into a `static const` if
   it's read often).
6. **Docs**: describe it in `docs/guide.md`.

### Add a debugging or escape-hatch switch

Example: `H3_NO_SNAP_CENTERS` in `H3Recomp/gpu/renderer/draw.cpp`.

```cpp
// H3_NO_YOUR_FIX=1: the old behavior (to compare).
static const bool disabled = h3::EnvFlag("H3_NO_YOUR_FIX");
if (disabled)
    return;
```

`EnvFlag` (set at all), `EnvString` and `EnvNumber` are in
`H3Recomp/config/env.h`. Cache the value in a `static const` (reading the
environment on every call is slow). Then add a row to the right table in
`docs/debugging.md`.

### Hook a game function

Example: `H3Recomp/hooks/crosshair.cpp` (short), then
`H3Recomp/hooks/exposure.cpp` (a 60 Hz fix, explained in detail at the top).

```cpp
// What sub_82205690 is, and what this hook changes.
PPC_FUNC_IMPL(__imp__sub_82205690);  // the recompiled original, renamed
PPC_FUNC(sub_82205690)               // your replacement: the game now calls this
{
    // Before: read or change the arguments (ctx.r3, ctx.r4, ...).
    __imp__sub_82205690(ctx, base);  // run the original (or don't)
    // After: read or change the result (ctx.r3).
}
```

- Put it in a new file in `H3Recomp/hooks/` and add the file to the
  `H3Runtime` source list in `H3Recomp/CMakeLists.txt` (hooks are listed
  there one by one).
- A function called from many places can be hooked for one caller only:
  compare `uint32_t(ctx.lr)` with the return address of the call you want
  (see `kRisingCall` in `exposure.cpp`).
- Finding the function to hook is the hard part. The notes in the existing
  hooks show how earlier ones were found: Ghidra on the XEX, comparing with
  the PC version (MCC) and the Halo 3 Editing Kit, and the runtime's own
  tools (`H3_WATCH_WRITES` finds who writes an address, `H3_SCAN_FLOAT` finds
  a value in memory; [Debugging](debugging.html)).
- A fix for something that only goes wrong at 60 Hz should only run when
  `h3::hooks::ContentFixes()` is true (`H3Recomp/hooks/tick_rate.h`).

### Implement a kernel or system call

Example: `KeTlsSetValue` in `H3Recomp/kernel/xboxkrnl_threading.cpp`.

```cpp
// BOOL KeTlsSetValue(DWORD slot, PVOID value)
PPC_FUNC(__imp__KeTlsSetValue)
{
    GuestRef<uint32_t>(CurrentThread()->tlsSlots + ctx.r3.u32 * 4) = ctx.r4.u32;
    ctx.r3.u64 = 1;  // TRUE
}
```

Every import the game uses already has a generated fallback that logs
`[kernel] unimplemented import: Name` once and returns 0 or a failure. To
implement one, write `PPC_FUNC(__imp__Name)` in the matching file in
`H3Recomp/kernel/` (`xboxkrnl_*` for the kernel, `xam*` for the system
library); yours replaces the fallback at link time. Put the C signature in
the comment above it. [Xenia](https://github.com/xenia-project/xenia)'s
source is the reference for what each call does.

### Read the game's data

- `GuestRef<uint32_t>(address)`, `GuestRef<float>(address)`: a value in
  guest memory, byte-swapped automatically (read and write).
- `GuestPtr<uint8_t>(address)`: a raw pointer to the bytes (not swapped:
  swap them yourself).
- `ctx.r13.u32` points at the current thread's TLS block; many of the game's
  globals hang off it (search the hooks for `ctx.r13` for examples).
- The GPU and the XMA audio hardware work with *physical* addresses
  (0 to 512 MB). That memory is visible to the game at `0xA0000000` and up;
  `PhysicalToGuest(physical)` gives the guest address to use with
  `GuestRef`.

## Testing your change

- **Build both ways if you can**: Linux, and Windows (natively or
  cross-compiled from Linux, `cmake --preset windows-cross`).
- **Play the part of the game you changed.** Press F9 to save a GPU capture
  of a frame (for rendering bugs), and save a Theater film when something
  goes wrong: a film replays exactly, so it reproduces the bug every time.
- **If your change can affect gameplay** (anything in a hook that runs during
  play), the film library must still match: `tools/films/run_library.sh
  <out dir>` replays every film and checks the game state tick by tick
  ([Testing](testing.html)). The library is made of films recorded on the
  maintainer's machine (`private/films/`); if you don't have it, say so when
  you submit and describe what you played.
- **Rendering changes** can't be checked by the films: compare screenshots
  of the same film moment before and after (`H3_SNAPSHOT_LEVEL`), and with
  the original in Xenia where possible.

## Submitting

Open a pull request on GitHub with: what you changed and why, how you tested
it (what you played, which films you ran), and screenshots for anything
visible. Smaller pull requests are reviewed faster.

## Words you'll see

| Word | Meaning |
|---|---|
| guest / host | The Xbox 360 side (the game, its memory, its addresses) / the PC side (our code, real pointers). |
| `sub_XXXXXXXX` | A recompiled game function, named after its address in the original executable. |
| `ctx`, `base` | The PowerPC registers of the current call, and the start of guest memory. |
| `lr` | Link register: the return address, i.e. which call site called this function. |
| import | A function the game calls in the console's kernel or system library (`__imp__Name`). |
| hook | Our C++ running in place of a game function. |
| tick | One step of the game simulation: 30 a second originally, 60 with the 60 Hz option. |
| content fixes | Hooks that keep things tuned for 30 ticks (fire rates, camera shake...) right at 60. |
| MCC | Halo: The Master Chief Collection, the PC version; its fixes are the model for ours. |
| tag | One piece of game data in a map (a weapon, a shader...), found by its *datum* index. |
| EDRAM, resolve | The 360 GPU's on-chip render memory, and copying from it to main memory. |
| film | A Theater recording: the inputs of a game, which replay exactly. |
| native port | Replacing recompiled functions with hand-written C++ ([roadmap](ROADMAP.html)). |
