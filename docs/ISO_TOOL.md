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

- Linux: the system's clang (18+) and lld or the system linker; h3setup
  finds them and says what to install if missing.
- Windows: h3setup downloads LLVM (clang-cl, lld-link) and runs xwin to
  fetch Microsoft's CRT and Windows SDK (the player accepts Microsoft's
  license there), into a cache folder. No Visual Studio needed. The same
  toolchain cross-builds our Windows release (cmake/toolchains/
  windows-clang-cl.cmake), so the flags carry over.

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
