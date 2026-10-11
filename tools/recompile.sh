#!/usr/bin/env bash
# Runs the whole recompilation pipeline against the XEX in private/:
#   1. build the tools
#   2. XenonAnalyse -> jump table TOML (only if it doesn't exist yet),
#      corrected by h3analysis.py
#   3. XenonRecomp  -> H3RecompLib/ppc/*.cpp
#   4. h3xexinfo    -> H3RecompLib/ppc/import_stubs.cpp
#   5. the same for each DLL in H3RecompLib/config/modules/ (the game loads
#      them at runtime) -> H3RecompLib/modules/<name>/
#   6. reconfigure so the game targets show up
#
# Usage: tools/recompile.sh [preset]   (default: release)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="${1:-release}"
BUILD="$ROOT/build/$PRESET"
CONFIG_DIR="$ROOT/H3RecompLib/config"
CONFIG="$CONFIG_DIR/halo3.toml"
SWITCH_TABLES="$CONFIG_DIR/halo3_switch_tables.toml"
PPC_DIR="$ROOT/H3RecompLib/ppc"

XEX="$ROOT/private/default.xex"
XEXP="$ROOT/private/default.xexp"
PATCHED_XEX="$ROOT/private/default_patched.xex"

if [[ ! -f "$XEX" ]]; then
    echo "Missing $XEX. See docs/DUMPING.md." >&2
    exit 1
fi

echo "== Building tools ($PRESET)"
cmake --preset "$PRESET" >/dev/null
cmake --build --preset "$PRESET" --target XenonAnalyse XenonRecomp h3xexinfo

XENON_ANALYSE="$BUILD/thirdparty/XenonRecomp/XenonAnalyse/XenonAnalyse"
XENON_RECOMP="$BUILD/thirdparty/XenonRecomp/XenonRecomp/XenonRecomp"
H3XEXINFO="$BUILD/tools/h3xexinfo/h3xexinfo"
PPC_CONTEXT="$ROOT/thirdparty/XenonRecomp/XenonUtils/ppc_context.h"

if grep -Eq '^(rest|save)[a-z]+_[0-9]+_address = 0x0$' "$CONFIG"; then
    echo "Save/restore helper addresses are unset in $CONFIG." >&2
    echo "Run: $H3XEXINFO $XEX" >&2
    echo "and paste the addresses it prints." >&2
    exit 1
fi

mkdir -p "$PPC_DIR"

# Runs XenonRecomp for one config without leaving stale sources behind: when
# a run splits the code into fewer files, the old high-numbered
# ppc_recomp.N.cpp files would stay and link outdated copies of functions.
# The previous outputs are set aside first; files that come out identical
# get their old timestamps back so the build stays incremental.
recompile_into() {
    local config="$1" dir="$2" old
    old="$(mktemp -d)"
    shopt -s nullglob
    local previous=("$dir"/ppc_recomp.*.cpp)
    shopt -u nullglob
    [[ ${#previous[@]} -gt 0 ]] && mv "${previous[@]}" "$old"/
    "$XENON_RECOMP" "$config" "$PPC_CONTEXT"
    local file name
    for file in "$dir"/ppc_recomp.*.cpp; do
        name="$(basename "$file")"
        if [[ -f "$old/$name" ]] && cmp -s "$file" "$old/$name"; then
            touch -r "$old/$name" "$file"
        fi
    done
    rm -rf "$old"
}

# With a title update, XenonRecomp creates the patched XEX on its first run,
# and the jump tables have to come from the patched code. Bootstrap with an
# empty table file, then analyse the patched XEX.
TARGET_XEX="$XEX"
if [[ -f "$XEXP" ]] && grep -q '^patch_file_path' "$CONFIG"; then
    TARGET_XEX="$PATCHED_XEX"
    if [[ ! -f "$PATCHED_XEX" ]]; then
        echo "== Creating patched XEX"
        [[ -f "$SWITCH_TABLES" ]] || : > "$SWITCH_TABLES"
        "$XENON_RECOMP" "$CONFIG" "$PPC_CONTEXT"
        rm -f "$SWITCH_TABLES"
    fi
fi

if [[ ! -s "$SWITCH_TABLES" ]]; then
    echo "== Analysing jump tables"
    "$XENON_ANALYSE" "$TARGET_XEX" "$SWITCH_TABLES"
    "$H3XEXINFO" "$TARGET_XEX" --dump "$ROOT/private/image.bin" >/dev/null
    python3 "$ROOT/tools/h3analysis.py" tables "$ROOT/private/image.bin" "$SWITCH_TABLES"
fi

echo "== Recompiling"
recompile_into "$CONFIG" "$PPC_DIR"

echo "== Generating import stubs"
"$H3XEXINFO" "$TARGET_XEX" --stubs "$PPC_DIR/import_stubs.cpp" | tail -n 1

# DLLs the game loads with XexLoadImage. Each config's file_path points at the
# DLL under private/game/.
for MODULE_CONFIG in "$CONFIG_DIR"/modules/*.toml; do
    [[ "$MODULE_CONFIG" == *_switch_tables.toml ]] && continue
    NAME="$(basename "$MODULE_CONFIG" .toml)"
    MODULE_TABLES="$CONFIG_DIR/modules/${NAME}_switch_tables.toml"
    MODULE_DIR="$ROOT/H3RecompLib/modules/$NAME"
    MODULE_FILE="$(sed -n 's/^file_path = "\(.*\)"$/\1/p' "$MODULE_CONFIG")"
    MODULE_XEX="$(cd "$CONFIG_DIR/modules" && realpath -m "$MODULE_FILE")"
    MODULE_IMAGE="$ROOT/private/modules/$NAME.bin"

    if [[ ! -f "$MODULE_XEX" ]]; then
        echo "Missing $MODULE_XEX (needed by $NAME). Copy the disc contents to private/game/." >&2
        exit 1
    fi

    echo "== Module $NAME"
    mkdir -p "$MODULE_DIR" "$ROOT/private/modules"
    if [[ ! -s "$MODULE_TABLES" ]]; then
        "$XENON_ANALYSE" "$MODULE_XEX" "$MODULE_TABLES"
        "$H3XEXINFO" "$MODULE_XEX" --dump "$MODULE_IMAGE" >/dev/null
        python3 "$ROOT/tools/h3analysis.py" tables "$MODULE_IMAGE" "$MODULE_TABLES"
    fi
    recompile_into "$MODULE_CONFIG" "$MODULE_DIR"
    "$H3XEXINFO" "$MODULE_XEX" --stubs "$MODULE_DIR/import_stubs.cpp" | tail -n 1
done

echo "== Reconfiguring"
cmake --preset "$PRESET" >/dev/null

echo "Done. Build the game with: cmake --build --preset $PRESET"
