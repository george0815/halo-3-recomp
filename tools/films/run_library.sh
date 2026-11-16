#!/usr/bin/env bash
# Replays every film of the library against its reference hashes: the
# regression test for a changed build (docs/ROADMAP.md 6.1).
#
# Usage: tools/films/run_library.sh <out dir> [--native] [--only <name>,...]
#   --native        also check every decompiled replacement against its
#                   original on every call (H3_CHECK_NATIVE=1, as
#                   check_native.sh)
#   --only <names>  just these films
#
# The library is private/films/library.txt, one film per line:
#   <name> <index> <film seconds> [<start wait>] [category=<n>] [fixes]
# (start wait: run_film.sh --start-wait, for a film whose map the lobby
# takes longer to load; category: its Theater category, default 3
# CAMPAIGN, 4 for multiplayer; fixes: replay with the 60 Hz content fixes,
# for a film recorded with them, as learn_film.sh --fixes)
# with private/films/<name>.mask and <name>.hash.txt from learn_film.sh.
# Prints one line per film (match, or the first differing tick) and, with
# --native, the replacement summary; exits 1 if any film differs. Uses
# H3_FILM_DATA (default private/films_data, a copy of the game files and
# hdd/).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export H3_FILM_DATA="${H3_FILM_DATA:-$ROOT/private/films_data}"
FILMS="$ROOT/tools/films"
LIBRARY="$ROOT/private/films/library.txt"
CATEGORY=3
# Run time bound per film: menus, then ~30 ticks per second (learn_film.sh).
MENU_SECONDS=90
TICKS_PER_SECOND=30

usage() { sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[[ $# -ge 1 ]] || usage
OUT="$1"; shift
NATIVE=0 ONLY=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --native) NATIVE=1; shift ;;
        --only) ONLY=",$2,"; shift 2 ;;
        *) usage ;;
    esac
done
mkdir -p "$OUT"

failed=0
while read -r name index seconds options; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    [[ -n "$ONLY" && "$ONLY" != *",$name,"* ]] && continue
    reference="$ROOT/private/films/$name.hash.txt"
    if [[ ! -f "$reference" ]]; then
        echo "$name: no reference (learn_film.sh first)"
        failed=1
        continue
    fi
    category=$CATEGORY wait="" fixes=0
    for option in $options; do
        case "$option" in
            category=*) category=${option#category=} ;;
            fixes) fixes=1 ;;
            *) wait=$option ;;
        esac
    done
    # The start wait's frames come on top of the menus (60 per second).
    run=(--category $category --index "$index" --mask "$ROOT/private/films/$name.mask"
        --seconds $(( MENU_SECONDS + ${wait:-0} / 60 + seconds * 60 / TICKS_PER_SECOND )))
    [[ -n "$wait" ]] && run+=(--start-wait "$wait")
    if [[ $fixes == 1 ]]; then export H3_FILM_WITH_60HZ_FIXES=1; else unset H3_FILM_WITH_60HZ_FIXES; fi
    rm -rf "$OUT/$name"
    if [[ $NATIVE == 1 ]]; then
        summary=$("$FILMS/check_native.sh" "$OUT/$name" "${run[@]}" 2>&1)
    else
        "$FILMS/run_film.sh" "$OUT/$name" "${run[@]}" > /dev/null 2>&1
        summary=""
    fi
    if result=$("$FILMS/compare_hashes.py" "$reference" "$OUT/$name/hash.txt"); then
        echo "$name: match ($result)"
    else
        echo "$name: DIFFERS ($result)"
        failed=1
    fi
    [[ -n "$summary" ]] && echo "$summary"
done < "$LIBRARY"
exit $failed
