#!/usr/bin/env bash
# Checks the decompiled replacements against calls recorded by
# record_calls.sh: each recorded call runs through the replacement and its
# recompiled original on the same memory snapshot and the results are
# compared (H3_CHECK_NATIVE). No game run; seconds per film.
#
# Usage: tools/films/replay_calls.sh <recordings dir> [<name|address>,...]
#   <recordings dir>  record_calls.sh's output (one directory per film)
#   names             only these replacements (H3_REPLACE names or hex
#                     addresses); default all
#
# Uses the build with the replacements (H3_FILM_BINARY, default
# build/dev/H3Recomp/H3Recomp) and the film data's game files (for the map
# files the tag cache is rebuilt from). Prints one block per film and exits
# 1 if any replacement differed, hung or crashed.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DATA="${H3_FILM_DATA:-$ROOT/private/films_data}"
BINARY="${H3_FILM_BINARY:-$ROOT/build/dev/H3Recomp/H3Recomp}"

[[ $# -ge 1 ]] || { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
DIR="$1"
ONLY="${2:-}"

# A directory of its own: the game's xex and files, no settings.ini (the
# film data's sends output to a log file).
RUN="$DIR/.replay"
mkdir -p "$RUN"
ln -sfn "$DATA/default.xex" "$RUN/default.xex"
ln -sfn "$DATA/game" "$RUN/game"

failed=0
for calls in "$DIR"/*/calls; do
    [[ -d "$calls" ]] || continue
    film=$(basename "$(dirname "$calls")")
    echo "$film:"
    "$BINARY" "$RUN/default.xex" --replay-calls "$calls" $ONLY 2> "$calls/../replay.log" || failed=1
done
exit $failed
