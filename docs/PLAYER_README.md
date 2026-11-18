Halo 3 Recomp
=============

A PC port of Halo 3 (Xbox 360), made by statically recompiling the game's
own code. This download contains no part of Halo 3: you need your own copy
of the game as a disc image, and the game is built from it on your PC.

What you need
-------------

- Your own Halo 3 disc image (.iso), dumped from your own disc
  (see https://github.com/george0815/halo-3-recomp/blob/main/docs/DUMPING.md).
  Supported: the original disc release (default.xex SHA-1
  927a8c49062a55179d84116e33f8a5a0b3db13c1). Title updates and other
  pressings aren't yet.
- A 64-bit PC with a Vulkan graphics card.
- About 8 GB of free disk space where you unpack this.
- Windows: Windows 10 (version 1903 or later) or 11, and an internet
  connection the first time (see below).
- Linux: clang 18 or newer, to build the game once:
    Ubuntu/Debian: sudo apt install clang
    Fedora:        sudo dnf install clang
    Arch:          sudo pacman -S clang
    openSUSE:      sudo zypper install clang

Installing
----------

1. Unpack this archive into a folder of its own.
2. Start H3Launcher (H3Launcher.exe on Windows).
3. Select INSTALL and choose your Halo 3 disc image. The launcher checks the
   disc, copies the game's files (about 6 GB), translates the game's code
   and compiles it. This takes a few minutes; the progress shows at the
   bottom.
   On Windows, it first asks whether you accept Microsoft's license terms:
   building on Windows needs Microsoft's C runtime and Windows SDK, which
   can't be shipped here, so they are downloaded from Microsoft (about
   1 GB, once) by xwin.
4. When it says the game is installed, select LAUNCH.

The same from a terminal: h3setup install <your disc image> <this folder>
(add --accept-microsoft-license on Windows if you accept Microsoft's terms).

Playing
-------

The launcher sets the window size, graphics, controls, profile and
system link. Keyboard and mouse or controllers; in menus the mouse points
and clicks. Split screen: plug in more controllers and press START.

If something goes wrong
-----------------------

- The install stops with a message saying what failed. Its logs are in
  build/ (build.log, compile_*.log, recompile.log) and toolchain/xwin.log.
  Running INSTALL again continues where it stopped.
- The game's own log: turn on "Log to file" in the launcher's DEBUG page;
  it writes h3recomp.log in game/.

Uninstalling: delete the folder. Settings and saves live in game/.

Legal
-----

Halo 3 is a trademark of Microsoft. This project is not affiliated with or
endorsed by Microsoft, Bungie or 343 Industries. It contains no game code or
data; use it only with a copy of the game you own.

Halo 3 Recomp itself is under the MIT license (LICENSE.txt). Third-party
software included and its licenses: THIRD-PARTY.txt, licenses/.
