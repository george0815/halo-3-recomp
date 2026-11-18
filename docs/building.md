---
title: Building
nav_order: 3
---

# Building from source

Players don't need this: the release's launcher builds the game from your
disc image (see the [README](https://github.com/george0815/halo-3-recomp#installing) and [the ISO tool](ISO_TOOL.html)).
This is for working on the project.

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

## Build steps

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

## Release packages

`tools/package.py <build dir> <package dir> --archive <dir>` turns a build
into a release (docs on the [ISO tool](ISO_TOOL.html) page). The Linux release
is built in an Ubuntu 22.04 container (`tools/linux_release/run.sh`) so it
runs on distributions with glibc 2.35 and later; the Windows release is
cross-compiled from Linux with clang-cl and xwin (`cmake --preset
windows-cross`, dependencies from `tools/build_windows_deps.sh`).
