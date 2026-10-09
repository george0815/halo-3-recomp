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
H3RecompLib/config/  XenonRecomp config + jump tables for Halo 3
H3RecompLib/ppc/     generated C++ (ignored)
H3RecompLib/modules/ generated C++ for the DLLs the game loads (ignored)
tools/h3xexinfo/     XEX inspector: layout, imports, helper addresses, stubs
tools/recompile.sh   full XEX -> C++ pipeline
tools/xdvdfs_extract.py  disc image -> private/game
tools/h3analysis.py  jump table / function boundary fixups
thirdparty/          XenonRecomp (submodule)
private/             your game files (ignored)
docs/                dumping guide, roadmap
```

## Building

Requires CMake 3.20+, Ninja and Clang 18+.

```sh
git submodule update --init --recursive

# Tools only (no game files needed)
cmake --preset release
cmake --build --preset release

# Put your default.xex in private/, then:
build/release/tools/h3xexinfo/h3xexinfo private/default.xex
# paste the printed addresses into H3RecompLib/config/halo3.toml
tools/recompile.sh
cmake --build --preset release
build/release/H3Recomp/H3Recomp private/default.xex
```

The game's hard drive (cache partitions, saves) is created in `private/hdd/`.
Delete that folder to start from a clean console.

### Debugging

Environment variables:

| Variable | Effect |
|---|---|
| `H3_STRICT_IMPORTS=1` | Abort on the first unimplemented import instead of logging it |
| `H3_LOG_FILES=1` | Log file opens, attribute queries and symbolic links |
| `H3_LOG_READS=1` | Log every file read (path, offset, length, destination) |
| `H3_LOG_MEMORY=1` | Log physical memory allocations |
| `H3_LOG_GPU=1` | Log ring buffer setup, the first use of each PM4 opcode, and frame counts |

CMake option `-DH3_TRACE_MODULE_CALLS=ON` logs every function call in the
recompiled DLLs with arguments and return values (very slow).

The runtime also reports locks held for more than two seconds and GPU waits
that last more than a second.

## Status

Boots through kernel setup, the Waves audio DLLs, the intro video and map
loading (`mainmenu.map`, `shared.map`), presenting frames through a
stand-in GPU that draws nothing. See [docs/ROADMAP.md](docs/ROADMAP.md).
