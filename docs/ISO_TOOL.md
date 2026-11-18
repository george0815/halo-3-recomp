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

Front ends: `h3setup` (command line) and the launcher, which runs it on
first start ("Select your Halo 3 disc image") and shows its progress lines.

Input: a disc image (ISO) only. Supported: the original disc release,
`default.xex` SHA-1 `927a8c49062a55179d84116e33f8a5a0b3db13c1`
(docs/DUMPING.md). Other pressings and title updates aren't, yet.

## Steps

1. **check**: open the image (XDVDFS: XGD2/XGD1/XGD3 layouts, plain XISO),
   hash `default.xex`, compare with the supported builds. *Done.*
2. **extract**: copy the disc's files into the install's `game/` folder
   (maps, bink, the DLLs; about 6 GB, ~25 s here). *Done.*
3. **recompile**: dump the images (h3xexinfo `--dump`: `image.bin`,
   `modules/*.bin`), run XenonRecomp on `default.xex` and the four DLLs with
   the shipped configs (paths rewritten to the install), write the import
   stubs. The jump table files are shipped (they're addresses), so
   XenonAnalyse doesn't run on the player's machine.
4. **generate glue**: what H3RecompLib/CMakeLists.txt does at configure
   time: each module's mapping array renamed, the module registry, the
   list of stubbed imports. *Done* (tools/h3setup/build.cpp, Glue).
5. **compile**: the generated sources, in parallel, with the flags from a
   build manifest written when the release was built (compiler, defines,
   include directories, forced includes, `-ffp-contract=off`...). *Done*:
   tools/package.py takes this build's own commands (compile_commands.json,
   `ninja -t commands H3Recomp`) and rewrites them into runtime/build.txt
   templates; objects newer than their source are skipped (resumable).
6. **link**: the game objects with the shipped runtime (its object files /
   whole archive, so the hooks' strong definitions override the generated
   weak ones) and the shipped libraries (SDL3, shaderc, FFmpeg) into the
   executable. *Done* (Linux): H3Runtime is a static library linked
   whole-archive (H3Recomp/CMakeLists.txt); objects via a response file;
   shared libraries in lib/ next to the executable (rpath $ORIGIN/lib).

## Toolchain on the player's machine

- Linux: the system's clang (18+) (`$CXX`, or `clang++` on the PATH);
  h3setup says what to install if it's missing.
- Windows (user's choice, 2026-11-18): the release ships a trimmed LLVM
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

## Release layout (planned)

```
H3Launcher(.exe), h3setup(.exe), runtime/ (objects or archive, headers the
generated code includes, build manifest), recompiler/ (XenonRecomp,
h3xexinfo, configs + jump tables), libraries, game/ (created by h3setup),
H3Recomp(.exe) (created by h3setup)
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
  the SDK already present (the dev machine's xwin splat): 135 compiles with
  the shipped clang-cl, lld-link, 2 min 10 s; H3Recomp.exe boots to the main
  menu. The launcher asks for Microsoft's license on Windows before
  passing --accept-microsoft-license.
- Left: the launcher INSTALL page (user test), a real-Windows run (xwin's
  download included), Linux launcher's Qt (system Qt 6 for now), the
  release README, film check of the Windows-built executable.
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
- Open: the project's own license (LICENSE) — the user's choice.
