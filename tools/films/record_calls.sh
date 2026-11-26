#!/usr/bin/env bash
# Records function calls from the film library for replaying against
# decompiled replacements (docs/ROADMAP.md 6.1; H3Recomp/debug/call_record.h).
#
# Usage: tools/films/record_calls.sh <out dir> [--only <name>,...] [--points <n>]
#                                    [--ticks <t>,...] [--window <n>]
#   --only <names>  just these films (library.txt names)
#   --points <n>    snapshots per film, spread over its length (default 6)
#   --ticks <list>  snapshot at these level ticks instead (e.g. 1 for the
#                   map start)
#   --window <n>    ticks of calls recorded per snapshot (default 1); a
#                   wider window catches rarely called functions (spawns,
#                   game engine events, cinematics)
#
# Needs the recording build: a build directory configured with
# -DH3_RECORD_CALLS=ON (default build/record; H3_RECORD_BINARY overrides).
# The film runs with every replacement switched off (H3_ORIGINAL=*), so
# every function's calls are recorded from the original code. Writes
# <out dir>/<film>/calls/tick_<t>.mem (sparse guest memory snapshots, about
# 1 GB of data each) and tick_<t>.calls (distinct entry registers per
# function), plus the film run's own outputs.
#
# Then check replacements in seconds, as often as needed:
#   tools/films/replay_calls.sh <out dir> [<name|address>,...]
#
# The recordings stay valid as long as the recompiled code is the same
# (they hold original behavior only): re-record after re-running the
# recompiler, not after changing H3Native.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export H3_FILM_DATA="${H3_FILM_DATA:-$ROOT/private/films_data}"
FILMS="$ROOT/tools/films"
LIBRARY="$ROOT/private/films/library.txt"
CATEGORY=3
MENU_SECONDS=90
TICKS_PER_SECOND=30

usage() { sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[[ $# -ge 1 ]] || usage
OUT="$(realpath -m "$1")"; shift
ONLY="" POINTS=6 TICKS="" WINDOW=1
while [[ $# -gt 0 ]]; do
    case "$1" in
        --only) ONLY=",$2,"; shift 2 ;;
        --points) POINTS=$2; shift 2 ;;
        --ticks) TICKS=$2; shift 2 ;;
        --window) WINDOW=$2; shift 2 ;;
        *) usage ;;
    esac
done
export H3_FILM_BINARY="${H3_RECORD_BINARY:-$ROOT/build/record/H3Recomp/H3Recomp}"
[[ -x "$H3_FILM_BINARY" ]] || { echo "no recording build at $H3_FILM_BINARY"; exit 1; }
mkdir -p "$OUT"

while read -r name index seconds options; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    [[ -n "$ONLY" && "$ONLY" != *",$name,"* ]] && continue
    category=$CATEGORY wait="" fixes=0
    for option in $options; do
        case "$option" in
            category=*) category=${option#category=} ;;
            fixes) fixes=1 ;;
            *) wait=$option ;;
        esac
    done
    rate=$TICKS_PER_SECOND
    [[ $fixes == 1 ]] && rate=60
    # Points at the middle of POINTS equal parts of the film.
    ticks="$TICKS"
    for (( k = 0; k < POINTS && ${#TICKS} == 0; k++ )); do
        ticks+="${ticks:+,}$(( (2 * k + 1) * seconds * rate / (2 * POINTS) ))"
    done
    run=(--category $category --index "$index" --mask "$ROOT/private/films/$name.mask"
        --seconds $(( MENU_SECONDS + ${wait:-0} / 60 + seconds * 60 / TICKS_PER_SECOND )))
    [[ -n "$wait" ]] && run+=(--start-wait "$wait")
    if [[ $fixes == 1 ]]; then export H3_FILM_WITH_60HZ_FIXES=1; else unset H3_FILM_WITH_60HZ_FIXES; fi
    rm -rf "$OUT/$name"
    mkdir -p "$OUT/$name"
    start=$SECONDS
    H3_ORIGINAL='*' H3_RECORD_CALLS="$OUT/$name/calls" H3_RECORD_TICKS="$ticks" H3_RECORD_WINDOW=$WINDOW \
        "$FILMS/run_film.sh" "$OUT/$name" "${run[@]}" > /dev/null 2>&1
    recorded=$(ls "$OUT/$name/calls"/*.calls 2>/dev/null | wc -l)
    size=$(du -sh "$OUT/$name/calls" 2>/dev/null | cut -f1)
    echo "$name: $recorded of $(( $(tr -cd , <<< "$ticks" | wc -c) + 1 )) points recorded ($size, $(( SECONDS - start )) s; ticks $ticks)"
done < "$LIBRARY"
