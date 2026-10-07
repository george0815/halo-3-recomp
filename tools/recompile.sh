#!/usr/bin/env bash
# Runs the whole recompilation pipeline against the XEX in private/:
#   1. build the tools
#   2. XenonAnalyse -> jump table TOML (only if it doesn't exist yet)
#   3. XenonRecomp  -> H3RecompLib/ppc/*.cpp
#   4. h3xexinfo    -> H3RecompLib/ppc/import_stubs.cpp
#   5. reconfigure so the game targets show up
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
fi

echo "== Recompiling"
"$XENON_RECOMP" "$CONFIG" "$PPC_CONTEXT"

echo "== Generating import stubs"
"$H3XEXINFO" "$TARGET_XEX" --stubs "$PPC_DIR/import_stubs.cpp" | tail -n 1

echo "== Reconfiguring"
cmake --preset "$PRESET" >/dev/null

echo "Done. Build the game with: cmake --build --preset $PRESET"
