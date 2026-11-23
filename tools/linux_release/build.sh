#!/usr/bin/env bash
# Inside the release container (Containerfile; run.sh starts it with the
# checkout at /src): the dependencies into /src/build/linux-compat-deps,
# the project into /src/build/linux-compat, then the package and archive.
# Re-running skips dependencies already built.
set -euo pipefail

SRC=/src
DEPS=$SRC/build/linux-compat-deps
BUILD=$SRC/build/linux-compat
JOBS=$(nproc)
QT_VERSION=6.12.0
export CC=clang CXX=clang++
# podman --userns=keep-id leaves HOME at /, which isn't writable (aqtinstall
# keeps its settings and downloads under ~/.local).
export HOME=$DEPS/home
mkdir -p "$DEPS" "$HOME"

# SDL3, from the source tools/build_windows_deps.sh fetched.
if [[ ! -f $DEPS/sdl3/lib/libSDL3.so ]]; then
    cmake -S "$SRC/build/windows-deps-src/SDL3-3.4.18" -B "$DEPS/sdl3-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS/sdl3" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    cmake --build "$DEPS/sdl3-build" -j"$JOBS" && cmake --install "$DEPS/sdl3-build"
fi

# shaderc (shared), from the same source (its third_party synced).
if [[ ! -f $DEPS/shaderc/lib/pkgconfig/shaderc.pc ]]; then
    cmake -S "$SRC/build/windows-deps-src/shaderc" -B "$DEPS/shaderc-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$DEPS/shaderc" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON -DSPIRV_SKIP_TESTS=ON \
        -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DBUILD_SHARED_LIBS=OFF
    cmake --build "$DEPS/shaderc-build" -j"$JOBS" && cmake --install "$DEPS/shaderc-build"
fi

# FFmpeg with the xmaframes decoder.
if [[ ! -f $DEPS/ffmpeg/lib/libavcodec.a ]]; then
    H3_FFMPEG_PREFIX="$DEPS/ffmpeg" H3_FFMPEG_WORK="$DEPS/ffmpeg-src" "$SRC/tools/build_ffmpeg.sh"
fi

# Qt (the launcher), Qt's own Linux build.
if [[ ! -d $DEPS/qt/$QT_VERSION/gcc_64 ]]; then
    # (from $DEPS: aqt writes aqtinstall.log to the working directory)
    (cd "$DEPS" && aqt install-qt linux desktop "$QT_VERSION" linux_gcc_64 -O "$DEPS/qt")
fi

export PKG_CONFIG_PATH="$DEPS/shaderc/lib/pkgconfig"
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$DEPS/sdl3;$DEPS/qt/$QT_VERSION/gcc_64" -DH3_FFMPEG_DIR="$DEPS/ffmpeg"
cmake --build "$BUILD" -j"$JOBS"

python3 "$SRC/tools/package.py" "$BUILD" "$SRC/build/package_linux_compat" --archive "$SRC/build/release_archives"
