#!/usr/bin/env bash
# Replays the test film with H3_CHECK_NATIVE=1 (every decompiled
# replacement runs next to its original) and summarizes: per replacement,
# calls checked and differences; any difference's first reports.
#
# Usage: tools/films/check_native.sh <out dir> [run_film.sh options]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="$1"; shift
H3_CHECK_NATIVE=1 "$ROOT/tools/films/run_film.sh" "$OUT" "$@" > /dev/null
# "[native] <name>: <n> calls checked, <d> differed": keep the last per name.
grep -o '^\[native\] [a-z0-9_]*: [0-9]* calls checked, [0-9]* differed' "$OUT/run.log" |
    awk '{name=$2; sub(":", "", name); last[name]=$3 " calls, " $6 " differed"} END {for (n in last) print "  " n ": " last[n]}' | sort
grep '^\[native\]' "$OUT/run.log" | grep -v 'calls checked' | head -20 || true
