---
title: The ISO tool
nav_order: 6
---

# ISO tool (h3setup)

Turns the player's own Halo 3 disc image into a playable install, on the
player's machine. Nothing derived from the game is distributed: the release
contains our runtime (built), the recompiler and its config, and h3setup;
the game's code is recompiled and compiled locally from the player's
`default.xex`.

Front ends: `h3setup` (command line) and the launcher, whose INSTALL item
runs `h3setup install` and shows its progress lines (`progress <stage>
<percent>`).

Input: a disc image (ISO) only. Supported: the original disc release,
`default.xex` SHA-1 `927a8c49062a55179d84116e33f8a5a0b3db13c1`
(docs/DUMPING.md). Other pressings and title updates aren't, yet.

## Steps

`h3setup install <iso> <dir>` runs them in order (each is also a command of
its own); every step can be rerun and continues where it stopped.

1. **check**: open the image (XDVDFS: XGD2/XGD1/XGD3 layouts, plain XISO),
   hash `default.xex`, compare with the supported builds.
2. **sdk** (Windows only, before the 6 GB copy): xwin downloads Microsoft's
   CRT and Windows SDK once the player accepts Microsoft's terms (below).
3. **extract**: copy the disc's files into the install's `game/` folder
   (maps, bink, the DLLs; about 6 GB, about 25 s on a fast drive). Files
   already there are kept.
4. **recompile**: run XenonRecomp on `default.xex` and the four DLLs with
   the shipped configs (paths rewritten to the install), write the import
   stubs (h3xexinfo `--stubs`). The jump table files are shipped (they're
   addresses), so XenonAnalyse doesn't run on the player's machine.
5. **build**: what H3RecompLib/CMakeLists.txt does at configure time (each
   module's mapping array renamed, the module registry, the list of stubbed
   imports; tools/h3setup/build.cpp, Glue), then the compile, in parallel,
   with the flags from the build manifest written when the release was built
   (tools/package.py takes this build's own commands, compile_commands.json
   and `ninja -t commands H3Recomp`, and rewrites them into
   runtime/build.txt templates; objects newer than their source are
   skipped), then the link: the game objects with the shipped runtime
   (H3Runtime, a static library linked whole-archive, so the hooks' strong
   definitions override the generated weak ones) and libraries. Linux:
   shared libraries in lib/ next to the executable (rpath $ORIGIN/lib).
   Windows: the DLLs next to it, and runtime/utf8.res: the UTF-8 manifest
   and the Halo 3 icon (assets/icon.ico) as resources.
6. **shaders**: the built game compiles every shader in the maps, and the
   variants listed in the shipped manifest, into its shader cache
   (`H3Recomp <xex> --precompile-shaders <package>/shaders`;
   assets/shaders/README.md), and copies the shipped pipeline list there
   for the first start's warm-up. Optional: if it fails, the game compiles
   shaders as it meets them.

## Toolchain on the player's machine

- Linux: the system's clang (18+) (`$CXX`, or `clang++` on the PATH);
  h3setup says what to install if it's missing.
- Windows: the release ships a trimmed LLVM
  for Windows (toolchain/llvm: clang-cl, lld-link and clang's headers from
  the official LLVM release matching our clang, ~200 MB; Apache 2.0 with
  LLVM exceptions, licenses included) and xwin (toolchain/xwin.exe,
  MIT/Apache). Microsoft's CRT and Windows SDK can't be redistributed:
  `h3setup sdk <dir> --accept-microsoft-license` (also the first step of
  `install` on Windows, before the 6 GB copy) has xwin download them into
  <install>/toolchain/sdk once the player accepts Microsoft's terms (the
  launcher asks). No Visual Studio needed. The same toolchain cross-builds
  our Windows release (cmake/toolchains/windows-clang-cl.cmake), so the
  flags carry over ({sdk} in runtime/build.txt).

## Release layout

```
H3Launcher(.exe), h3setup(.exe), README.txt, LICENSE.txt, THIRD-PARTY.txt,
licenses/, lib/ or the DLLs (Qt, SDL3, ...), runtime/ (the H3Runtime
archive, the headers the generated code includes, build.txt), recompiler/
(configs + jump tables), tools/ (XenonRecomp, h3xexinfo), shaders/ (the
shader and pipeline manifests), toolchain/ (Windows: trimmed LLVM, xwin)

created by INSTALL: game/ (the disc's files, settings.ini, hdd/,
shadercache/), build/ (generated code, objects, logs),
H3Recomp(.exe), toolchain/sdk/ (Windows)
```

## Status

- 2026-11-18: h3setup check/list/extract (tools/h3setup); extraction
  identical to private/game for the reference image (6.1 GB, 25 s).
- 2026-11-18: h3setup recompile: configs rewritten into <install>/build/
  config (paths relative to the config: XenonRecomp joins them as strings),
  jump tables copied beside them; the generated code for default.xex and
  the four DLLs is identical to the dev tree's (4 s). Next: the build
  (steps 4-6): split the runtime into a library the release ships, a
  build manifest, the compile/link driver.
- 2026-11-18: film check: the h3setup-built Linux executable matches
  Storm's reference on every tick (78563), so it simulates exactly like the
  dev build. (Link order matters: the game objects go where the dev build
  links the game's libraries, after the runtime, so shared inline functions
  come from the same copy; {exe_objects} / {game_objects}.)
- 2026-11-18: Windows end to end under wine: build/package_windows (307 MB,
  trimmed LLVM 23.1.4 + xwin 0.10.0 included), `h3setup.exe install` with
  the SDK already downloaded: 135 compiles with
  the shipped clang-cl, lld-link, 2 min 10 s; H3Recomp.exe boots to the main
  menu. The launcher asks for Microsoft's license on Windows before
  passing --accept-microsoft-license.
- 2026-11-18 later: release packaging: Linux bundles Qt (plugins, qt.conf)
  and every non-system library the launcher and game need (lib/,
  runtime/lib; RPATH via --disable-new-dtags); licenses/ + THIRD-PARTY.txt
  (XenonRecomp's disassembler is GPL: XenonRecomp ships with the GPL text;
  the runtime doesn't use it and libdisasm isn't shipped); README.txt
  (docs/PLAYER_README.md); --archive writes halo3-recomp-<version>-
  {linux,windows}-x86_64.{tar.gz,zip} (39 MB / 107 MB). h3setup: UTF-8
  paths everywhere (Windows: UTF-16 command line; programs run without a
  shell); Windows tools and H3Recomp.exe run with the UTF-8 code page
  (cmake/utf8.manifest; runtime/utf8.res for the player's link): a full
  install into a non-ASCII folder works under wine; free-space check;
  extraction resumes (files already there are kept); clang install hints.
- 2026-11-18: MIT license (LICENSE).
- 2026-11-23: real Windows: friends installed from the Windows release and
  played system link with Linux (four players over the internet).
- 2026-11-24: the shaders step (precompile.cpp); the Halo 3 icon on both
  executables and windows (assets/icon.png, .ico).
