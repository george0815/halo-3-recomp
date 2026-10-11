#!/usr/bin/env bash
# Builds the FFmpeg the XMA decoder needs: FFmpeg 7.1 with Xenia's
# "xmaframes" decoder (single XMA frames; stock FFmpeg only decodes whole
# packets and buffers samples, which doesn't map onto the XMA hardware's
# contexts). Static libavcodec + libavutil with only that decoder, installed
# to build/ffmpeg/. Run once before configuring; CMake picks it up.
#
# Usage: tools/build_ffmpeg.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="7.1"
SHA256="40973d44970dbc83ef302b0609f2e74982be2d85916dd2ee7472d30678a7abe6"
WORK="$ROOT/build/ffmpeg-src"
PREFIX="$ROOT/build/ffmpeg"
TARBALL="$WORK/ffmpeg-$VERSION.tar.xz"

mkdir -p "$WORK"
if [[ ! -f "$TARBALL" ]]; then
    echo "== Downloading FFmpeg $VERSION"
    curl -fL -o "$TARBALL.part" "https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz"
    mv "$TARBALL.part" "$TARBALL"
fi
echo "$SHA256  $TARBALL" | sha256sum -c --quiet

SRC="$WORK/ffmpeg-$VERSION"
rm -rf "$SRC"
tar -xf "$TARBALL" -C "$WORK"
patch -d "$SRC" -p1 --quiet < "$ROOT/tools/patches/ffmpeg-$VERSION-xmaframes.patch"

echo "== Configuring"
ASM=()
command -v nasm >/dev/null || ASM=(--disable-x86asm)
(cd "$SRC" && ./configure --prefix="$PREFIX" \
    --disable-everything --enable-decoder=xmaframes \
    --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
    --enable-static --disable-shared --enable-pic "${ASM[@]}" >/dev/null)

echo "== Building"
make -C "$SRC" -j"$(nproc)" >/dev/null
make -C "$SRC" install >/dev/null
echo "Done: $PREFIX"
