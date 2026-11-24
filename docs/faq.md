---
title: FAQ
nav_order: 2
---

# FAQ
{: .no_toc }

1. TOC
{:toc}

## Playing

### What do I need?

Your own Halo 3 disc dumped to a disc image (.iso; see [Dumping your
disc](DUMPING.html)), a 64-bit PC with a Vulkan graphics card, and about
8 GB of disk space. On Windows, Windows 10 (version 1903 or later) or 11;
on Linux, clang 18 or newer (the installer compiles the game with it). Only the
original disc release is supported for now (no title updates).

### Why do I need my own disc? Can't you ship the game?

The game's code and data belong to Microsoft. The release contains only
this project's own code; the installer reads *your* disc image, translates
the game's code and compiles it on your PC. That's also why installing
takes a few minutes.

### Why does Windows download something from Microsoft?

Compiling needs Microsoft's C runtime and Windows SDK, which may not be
redistributed. The installer has [xwin](https://github.com/Jake-Shadle/xwin)
download them straight from Microsoft (about 1 GB, once), after asking you
to accept Microsoft's license terms. Visual Studio isn't needed. The
compiler itself (LLVM's clang-cl) comes with the release.

### Is this an emulator? Is it a native port?

Partly the first, not yet the second. An emulator like
[Xenia](https://github.com/xenia-project/xenia) interprets or JIT-compiles
the 360's PowerPC code while the game runs; here every function of the game
was translated to C++ ahead of time and compiled into a normal program for
your CPU, so the game's own logic runs directly. But everything around it is
still emulated at a high level: the Xenos GPU's command stream and shaders,
XMA audio, the kernel and system software. That part owes a great deal to
Xenia, whose source is the reference for how the console behaves.

A *native port* would be the game as readable, hand-written PC code, with no
Xbox 360 underneath. That's where the project is heading
([native port](NATIVE_PORT_STATUS.html)), one subsystem at a time.

### Does it run at 60 fps? Doesn't Halo 3 break at 60?

It does break: the 360 version ties many things to its 30 ticks per second
(fire rates, physics, camera shake, animations, AI timing). The project
runs the game at 60 ticks and fixes those, porting MCC's own fixes where it
has them, and checks the result against the original with recorded films
([testing](testing.html)).

### Can I play with friends?

Yes: split screen with up to four players on one PC, and system link over a
LAN or the internet (one UDP port, which the host forwards). Everyone needs
the same release, the same game version and the same frame rate setting.
See the [player guide](guide.html).

### Will my MCC or 360 saves work?

No; the game keeps its own saves (an emulated 360 hard drive in `game/hdd/`).

## How the project works

### What is static recompilation?

A tool ([XenonRecomp](https://github.com/hedge-dev/XenonRecomp)) reads the
game executable (`default.xex`), finds every function, and writes each one
as C++ that does exactly what the PowerPC instructions did, on a model of
the CPU's registers and the console's memory. Halo 3 has about 22,400
functions, plus the four audio DLLs it loads. The C++ is compiled normally.
The game then runs at native speed, but it still expects an Xbox 360
around it, which the runtime provides.

### What does the runtime do?

Everything the 360 did: memory laid out like the console's, the kernel and
XAM system calls (threads, files, users, networking), the Xenos GPU (its
command stream and shaders, translated to Vulkan), XMA audio decoding and
input.
On top of that, *hooks* replace or adjust individual game functions: the
60 Hz fixes, mouse look, the menu cursor, PC text, system link. See the
[architecture](ARCHITECTURE.html).

### What's the native port?

Replacing recompiled functions with hand-written, readable C++, subsystem by
subsystem, each checked against the original with the film tests. It's how
the project gets from "the 360's code, translated" to "a PC game's source".
See the [roadmap](ROADMAP.html) and the [status](NATIVE_PORT_STATUS.html).

### How do you know a change didn't break the game?

Recorded films. Halo 3's Theater replays a game from its inputs, so a film
is a deterministic test: the project checksums the game state every tick
during a replay and compares it with a reference. Any change to the
simulation shows up as the first differing tick. See [testing](testing.html).

### Why compile on the player's machine instead of shipping binaries?

A compiled game executable would contain Halo 3's code, translated. Building
it locally from your own disc means nothing the project distributes contains
Microsoft's code.

## Building and contributing

### How do I build it myself?

See [Building](building.html). In short: CMake, Ninja, Clang 18+, SDL3,
shaderc, a Vulkan driver, Qt 6 for the launcher, your `default.xex`, then
`tools/recompile.sh` and a CMake build.

### Something looks wrong. How do I report it?

Open an issue with what you saw (a screenshot helps), where it happened
(mission and spot) and your system. Many fixes have a switch to turn them off
([Debugging](debugging.html), "Turning a fix off"); saying whether one of
them changes the problem helps a lot. A saved Theater film that shows the
problem is the best bug report there is.

### The install failed. What now?

The launcher shows the reason. The logs are in `build/` (`build.log`,
`compile_*.log`, `recompile.log`, `shaders.log`) and `toolchain/xwin.log`.
Common causes: not enough disk space, clang missing (Linux), or a disc image
that isn't the supported version. Running INSTALL again continues where it
stopped.
