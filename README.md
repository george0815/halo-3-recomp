# Halo 3 Recomp

**Halo 3 on PC, Linux and Windows,** made by statically recompiling the Xbox
360 game's own code to C++. The game's PowerPC code is translated ahead of
time and runs directly on your CPU, but **this is not a native port (yet)**:
the systems around it, the 360's GPU, XMA audio and operating system, are
emulated at a high level by the runtime, built on what
[Xenia](https://github.com/xenia-project/xenia) taught the world about the
console. Replacing the recompiled code with hand-written C++, a true native
port, is the project's long-term goal.

**No game data is included.** You need your own Halo 3 disc, dumped to a
disc image ([how](docs/DUMPING.md)). The installer builds the game from it on
your PC; nothing from Halo 3 is ever distributed.

Project site (FAQ, guides, how it works, roadmap, updates):
**https://george0815.github.io/halo-3-recomp/**

## Features

- **The whole game:** the campaign (solo and two-player co-op), multiplayer
  Custom Games, Forge and Theater, in Halo 3's own engine and rules.
- **60 frames per second,** with fixes for what the 360 version tied to
  30 (weapon fire rates, physics, camera shake, AI, animations), checked
  tick by tick against the original with recorded films.
- **Higher resolutions:** 2x and 3x the 360's 1280x720, FXAA, any window size,
  fullscreen.
- **Keyboard and mouse that feel native:** direct mouse look like MCC's,
  zoom-aware mouse speed, a mouse pointer in the menus, rebindable keys shown
  in the game's own prompts ("[Space] Select"), toggle crouch, hold to zoom.
- **PC options:** field of view, centered crosshair, vehicle mouse
  sensitivity, skip the intro, the 360's original text and icons if you
  prefer them.
- **Split screen** for up to four players (any mix of controllers and the
  keyboard).
- **System link** over your LAN or the internet with friends, no Xbox Live
  needed.
- **Linux and Windows,** from one code base.

## Installing

1. Download the release for your system (Releases on this page) and unpack
   it into a folder of its own.
2. Start **H3Launcher**, select **INSTALL** and choose your Halo 3 disc image.
   It checks the disc, copies the game's files (about 6 GB), translates and
   compiles the game's code (a few minutes), and then it's ready: **LAUNCH**.

You need a 64-bit PC with a Vulkan graphics card and about 8 GB of disk
space. On Windows (10 version 1903 or later), the installer downloads
Microsoft's C runtime and Windows SDK once, after asking you to accept
Microsoft's terms. On Linux, install clang 18 or newer first
(`sudo apt install clang`, `sudo dnf install clang`, `sudo pacman -S clang`).

Supported: the original disc release of Halo 3 (`default.xex` SHA-1
`927a8c49062a55179d84116e33f8a5a0b3db13c1`). Title updates and other
pressings aren't yet.

The [player guide](docs/guide.md) covers the launcher, the controls, split
screen and system link in detail; the [FAQ](docs/faq.md) answers the usual
questions.

## Default keys

| Key | Action |
|---|---|
| W A S D / Mouse | Move / look |
| Left / right mouse button | Fire / zoom |
| Space | Jump (A) |
| Left Shift | Grenade |
| Middle mouse, V | Melee |
| Mouse wheel, Q | Switch weapons |
| R | Reload / pick up |
| G | Switch grenades |
| F | Use equipment |
| C | Crouch |
| E, Left Ctrl | Fire |
| Escape / Tab | Start (menu) / Back (scoreboard) |
| In menus | The mouse points and clicks; right click goes back |

Every key can be changed in the launcher (CONTROLS). Controllers use the
standard Halo 3 layout.

## How it works

1. **[XenonRecomp](https://github.com/hedge-dev/XenonRecomp)** translates
   every PowerPC function in the game's `default.xex` (and the DLLs it loads)
   into C++.
2. **The runtime** (`H3Recomp/`) provides what the Xbox 360 did: guest memory,
   the kernel and XAM system calls, the Xenos GPU (translated to Vulkan), XMA
   audio, input, networking.
3. **Hooks** replace or adjust individual game functions: the 60 Hz fixes,
   mouse look, the menu cursor, PC text, system link.
4. **The ISO tool** (`h3setup`) does steps 1 and the build on your machine,
   from your disc, so the release itself contains only our code.

Over time, recompiled functions get replaced with hand-written C++ (the
native port). The approach follows [Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp);
[Xenia](https://github.com/xenia-project/xenia) is the reference for the
360's kernel and GPU. More on the [architecture](docs/ARCHITECTURE.md) and
how the project [tests itself with recorded films](docs/testing.md).

## Building from source

```sh
git submodule update --init --recursive
cmake --preset release && cmake --build --preset release   # tools only
# With your default.xex in private/:
tools/build_ffmpeg.sh && tools/recompile.sh && cmake --build --preset release
build/release/H3Launcher/H3Launcher
```

Needs CMake, Ninja, Clang 18+, SDL3, shaderc, a Vulkan driver and (for the
launcher) Qt 6. Details, the source layout and release packaging:
[Building](docs/building.md). Debugging switches: [Debugging](docs/debugging.md).

## Status

Version 0.75: the campaign, multiplayer, Forge and Theater play through at
60 Hz on Linux and Windows. Next is the native port: replacing the
recompiled code with readable C++, subsystem by subsystem. See the
[roadmap](docs/ROADMAP.md).

## Legal

Halo 3 is a trademark of Microsoft. This project is not affiliated with or
endorsed by Microsoft, Bungie or 343 Industries. It contains no game code or
data; use it only with a copy of the game you own.

The project is under the MIT license (LICENSE). Third-party code keeps its
own licenses (thirdparty/, and THIRD-PARTY.txt in releases).
