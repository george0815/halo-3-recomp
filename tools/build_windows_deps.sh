#!/usr/bin/env bash
# Builds the Windows (x64) dependencies by cross-compiling from Linux with
# clang-cl (cmake/toolchains/windows-clang-cl.cmake; needs the MSVC CRT and
# Windows SDK from xwin in H3_XWIN_DIR, default ~/tools/xwin-sdk) into
# build/windows-deps/: SDL3, shaderc and FFmpeg with the xmaframes decoder
# (as tools/build_ffmpeg.sh), and Qt 6 for the launcher (official MSVC
# binaries via aqtinstall; the Linux Qt of the same version provides moc,
# uic and rcc), and the Visual C++ runtime DLLs (from Microsoft's
# vc_redist.x64.exe, shipped next to the executables). Run once before
# configuring the "windows-cross" preset.
#
# Usage: tools/build_windows_deps.sh [sdl3|shaderc|ffmpeg|qt|vcredist ...] (default: all)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
XWIN="${H3_XWIN_DIR:-$HOME/tools/xwin-sdk}"
TOOLCHAIN="$ROOT/cmake/toolchains/windows-clang-cl.cmake"
WORK="$ROOT/build/windows-deps-src"
PREFIX="$ROOT/build/windows-deps"
JOBS="$(nproc)"
mkdir -p "$WORK" "$PREFIX"

[[ -d "$XWIN/crt" ]] || { echo "No MSVC CRT/SDK in $XWIN (xwin --accept-license --arch x86_64 splat --output $XWIN)"; exit 1; }
for tool in curl 7z cabextract; do
    command -v "$tool" >/dev/null || { echo "needs $tool"; exit 1; }
done

fetch() {  # fetch <url> <file> <sha256>
    local url=$1 file=$2 sum=$3
    if [[ ! -f "$file" ]]; then
        curl -fsSL -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    echo "$sum  $file" | sha256sum -c --quiet
}

cmake_cross() {  # cmake_cross <source> <build> [options...]
    local src=$1 build=$2; shift 2
    cmake -S "$src" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DH3_XWIN_DIR="$XWIN" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_FIND_ROOT_PATH="$PREFIX" \
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL "$@" >/dev/null
    cmake --build "$build" -j"$JOBS" >/dev/null
    cmake --install "$build" >/dev/null
}

SDL_VERSION=3.4.18
SDL_SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
build_sdl3() {
    echo "== SDL3 $SDL_VERSION"
    local tarball="$WORK/SDL3-$SDL_VERSION.tar.gz"
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL3-$SDL_VERSION.tar.gz" "$tarball" "$SDL_SHA256"
    rm -rf "$WORK/SDL3-$SDL_VERSION"
    tar -xf "$tarball" -C "$WORK"
    cmake_cross "$WORK/SDL3-$SDL_VERSION" "$WORK/sdl3-build" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF
}

SHADERC_TAG=v2026.4
build_shaderc() {
    echo "== shaderc $SHADERC_TAG"
    local src="$WORK/shaderc"
    if [[ ! -d "$src" ]]; then
        git clone -q --depth 1 --branch "$SHADERC_TAG" https://github.com/google/shaderc.git "$src"
        # glslang, SPIRV-Tools and SPIRV-Headers at the commits the release pins.
        (cd "$src" && python3 utils/git-sync-deps >/dev/null)
    fi
    cmake_cross "$src" "$WORK/shaderc-build" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_SKIP_TESTS=ON \
        -DENABLE_GLSLANG_BINARIES=OFF -DSHADERC_ENABLE_SHARED_CRT=ON -DBUILD_SHARED_LIBS=OFF
}

# FFmpeg: the version, checksum and patch tools/build_ffmpeg.sh uses.
FFMPEG_VERSION=7.1
FFMPEG_SHA256=40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6
build_ffmpeg() {
    echo "== FFmpeg $FFMPEG_VERSION (xmaframes)"
    local tarball="$WORK/ffmpeg-$FFMPEG_VERSION.tar.xz" src="$WORK/ffmpeg-$FFMPEG_VERSION"
    fetch "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" "$tarball" "$FFMPEG_SHA256"
    rm -rf "$src"
    tar -xf "$tarball" -C "$WORK"
    patch -d "$src" -p1 --quiet < "$ROOT/tools/patches/ffmpeg-$FFMPEG_VERSION-xmaframes.patch"
    local cflags="--target=x86_64-pc-windows-msvc" ldflags=""
    for dir in crt/include sdk/include/ucrt sdk/include/um sdk/include/shared; do cflags+=" -imsvc $XWIN/$dir"; done
    for dir in crt/lib/x86_64 sdk/lib/um/x86_64 sdk/lib/ucrt/x86_64; do ldflags+=" -libpath:$XWIN/$dir"; done
    local asm=()
    command -v nasm >/dev/null || asm=(--disable-x86asm)
    (cd "$src" && ./configure --prefix="$PREFIX/ffmpeg" --toolchain=msvc --target-os=win64 --arch=x86_64 \
        --enable-cross-compile --cc=clang-cl --cxx=clang-cl --ld=lld-link --ar=llvm-ar --nm=llvm-nm --host-cc=clang \
        --extra-cflags="$cflags" --extra-ldflags="$ldflags" \
        --disable-everything --enable-decoder=xmaframes \
        --disable-programs --disable-doc --disable-network --disable-autodetect \
        --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
        --enable-static --disable-shared "${asm[@]}" >/dev/null)
    make -C "$src" -j"$JOBS" >/dev/null
    make -C "$src" install >/dev/null
}

# Qt: qtbase of the official MSVC 2022 build (the launcher needs Widgets
# only), straight from Qt's repository with the checksum Qt publishes next
# to each archive. Its moc/uic/rcc are Windows programs: the build runs the
# Linux Qt of the same version instead (QT_HOST_PATH, the preset).
QT_VERSION=6.12.0
QT_RELEASE=6.12.0-0-202609280346
build_qt() {
    echo "== Qt $QT_VERSION (qtbase, MSVC 2022)"
    local v=${QT_VERSION//./}
    local base="https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_$v/qt6_${v}_msvc2022_64/qt.qt6.$v.win64_msvc2022_64"
    local archive="$QT_RELEASE""qtbase-Windows-Windows_11_24H2-MSVC2022-Windows-Windows_11_24H2-X86_64.7z"
    local file="$WORK/qtbase-$QT_VERSION-msvc2022_64.7z"
    local sum
    sum=$(curl -fsL "$base/$archive.sha256" | cut -d' ' -f1)
    fetch "$base/$archive" "$file" "$sum"
    rm -rf "$PREFIX/qt"
    mkdir -p "$PREFIX/qt"
    7z x -y -o"$PREFIX/qt" "$file" >/dev/null
}

# aka.ms always serves the newest runtime: when Microsoft updates it the
# checksum fails; check the new version installs and runs, then update both.
VCREDIST_VERSION=14.44.35211
VCREDIST_SHA256=cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b
VCREDIST_DLLS=(msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait vcruntime140 vcruntime140_1)
build_vcredist() {
    echo "== Visual C++ runtime $VCREDIST_VERSION"
    local file="$WORK/vc_redist-$VCREDIST_VERSION.x64.exe" cabs="$WORK/vcredist-cabs" cab="" c dll
    fetch "https://aka.ms/vs/17/release/vc_redist.x64.exe" "$file" "$VCREDIST_SHA256"
    # The installer's payload cabinets are appended to the exe; the x64
    # runtime is the one holding msvcp140.dll_amd64.
    rm -rf "$cabs" && mkdir -p "$cabs"
    cabextract -q -d "$cabs" "$file" 2>/dev/null || true
    for c in "$cabs"/*; do
        if cabextract -l "$c" 2>/dev/null | grep -q ' msvcp140.dll_amd64$'; then cab=$c; break; fi
    done
    [[ -n "$cab" ]] || { echo "no x64 runtime cabinet in $file"; exit 1; }
    rm -rf "$PREFIX/vcredist" && mkdir -p "$PREFIX/vcredist"
    for dll in "${VCREDIST_DLLS[@]}"; do
        cabextract -q -F "$dll.dll_amd64" -d "$cabs/x64" "$cab" 2>/dev/null
        mv "$cabs/x64/$dll.dll_amd64" "$PREFIX/vcredist/$dll.dll"
    done
    rm -rf "$cabs"
}

steps=("$@")
(( ${#steps[@]} )) || steps=(sdl3 shaderc ffmpeg qt vcredist)
for step in "${steps[@]}"; do
    case "$step" in
        sdl3) build_sdl3 ;;
        shaderc) build_shaderc ;;
        ffmpeg) build_ffmpeg ;;
        qt) build_qt ;;
        vcredist) build_vcredist ;;
        *) echo "unknown step $step"; exit 1 ;;
    esac
done
echo "Done: $PREFIX"
