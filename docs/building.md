---
title: Building
nav_order: 3
---

# Building from source

This page is for working on the project. Players don't need it: the
release's launcher builds the game from your disc image (see the
[README](https://github.com/george0815/halo-3-recomp#installing) and
[the ISO tool](ISO_TOOL.html)).

## Layout

```
H3Recomp/            runtime: entry point, guest memory, kernel, GPU, audio,
                     input, networking, and the hooks on game functions
H3Native/            decompiled replacements for recompiled functions
                     (the native port) and the host tag reader
H3Launcher/          Qt launcher: install, settings, profile, controls
H3RecompLib/config/  XenonRecomp config + jump tables for Halo 3
H3RecompLib/ppc/     generated C++ (ignored)
H3RecompLib/modules/ generated C++ for the DLLs the game loads (ignored)
assets/              launcher art, the icon, shipped shader manifests (assets/shaders)
tools/h3setup/       the ISO tool: check, extract, recompile, build, shaders
tools/h3xexinfo/     XEX inspector: layout, imports, helper addresses, stubs
tools/h3tags/        map (cache file) inspector: header, tags, shader dumps
tools/h3shader/      translates shader microcode files to GLSL (debugging)
tools/shaders/       builds the shipped shader manifest
tools/films/         the film regression tests (docs: Testing)
tools/package.py     a build -> a release package and archive
tools/linux_release/ the Linux release's Ubuntu 22.04 container build
tools/recompile.sh   full XEX -> C++ pipeline
tools/build_ffmpeg.sh  FFmpeg 7.1 + XMA frame decoder (tools/patches/), static
tools/build_windows_deps.sh  the Windows cross-build's libraries
tools/xdvdfs_extract.py  disc image -> private/game
tools/h3analysis.py  jump table / function boundary fixups
thirdparty/          XenonRecomp (submodule)
private/             your game files (ignored)
docs/                this site
```

## Build steps

Requires CMake 3.20+, Ninja, Clang 18+, SDL3, shaderc and a Vulkan driver.
The launcher also needs Qt 6 (Widgets); without it, the launcher isn't built.
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

The game's hard drive (cache partitions, saves) is created in
`private/hdd/`. Compiled shaders are cached in `private/shadercache/` (safe
to delete), together with `pipelines.txt`: the shaders and pipelines this PC
has used, which the game rebuilds in the background at start (launcher:
VIDEO > Precompile shaders), so places you've already played don't stutter.
To start from a clean console, delete `private/hdd/` (this also clears the
saved profile settings, `private/hdd/profile_settings.bin`).

## Release packages

`tools/package.py <build dir> <package dir> --archive <dir>` turns a build
into a release (described on the [ISO tool](ISO_TOOL.html) page). The Linux release
is built in an Ubuntu 22.04 container (`tools/linux_release/run.sh`) so it
runs on distributions with glibc 2.35 and later; the Windows release is
cross-compiled from Linux with clang-cl and xwin (`cmake --preset
windows-cross`, dependencies from `tools/build_windows_deps.sh`).
