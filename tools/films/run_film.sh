#!/usr/bin/env bash
# Plays one saved film in Theater and logs a checksum of the game state per
# game tick (docs/ROADMAP.md 6.1; how to use it: tools/films/README.md).
#
# Usage: tools/films/run_film.sh <out dir> [options]
#   --category <n>   Theater film category: 0 recent films (default),
#                    1 file share, 2 film clips, 3 campaign, 4 multiplayer,
#                    5 forge
#   --index <n>      film within the category, 0 = the top one (default 0)
#   --seconds <n>    how long to run before stopping the game (default 120;
#                    the menus take ~30 s, then one tick per frame)
#   --mask <file>    H3_STATE_MASK (cosmetic state left out of the hash)
#   --record         also write record.bin (H3_STATE_RECORD, ~90 KB per
#                    tick) for learn_mask.py --record
#   --dump <ticks>   also save raw states at these ticks (H3_STATE_DUMP)
#   --snapshots      also save a screenshot every 600 frames (to check the
#                    menu navigation reached the film)
#
# Writes <out dir>/hash.txt ("<copy> <tick> <hash>" per game tick, menus
# included; compare_hashes.py compares the last level), run.log, and the
# optional outputs. Extra H3_* variables in the environment pass through.
#
# The runs are deterministic only with the switches this sets:
# H3_RANDOM_SEED (fixed random seeds, the game state at one address) and
# H3_LOCKSTEP (one game tick per frame).
#
# Expects (paths overridable by environment):
#   H3_FILM_BINARY  the game (build/release/H3Recomp/H3Recomp)
#   H3_FILM_DATA    the game files: default.xex, game/, hdd/, modules/,
#                   image.bin, shadercache/, settings.ini (private/)
#   H3_FILM_PREFS   a preferences.dat that leaves the main menu cursor on
#                   THEATER; restored into hdd/Cache0 before each run
#                   (private/bench/hddstate/Cache0/preferences.dat)
# The run uses its own copy of settings.ini (windowed, 60 Hz tick, scale 1).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BINARY="${H3_FILM_BINARY:-$ROOT/build/release/H3Recomp/H3Recomp}"
DATA="${H3_FILM_DATA:-$ROOT/private}"
PREFS="${H3_FILM_PREFS:-$ROOT/private/bench/hddstate/Cache0/preferences.dat}"

# Kill the game when free memory drops below this (kB): a parallel game
# plus the browser once pushed the machine into a global OOM.
MIN_AVAILABLE_KB=2000000

# Menu timing in presented frames (60 per second): the main menu is up by
# MENU_FRAME, the Theater lobby's film loaded by LOBBY_FRAME; presses are
# STEP frames apart and held for 5.
MENU_FRAME=1300
LOBBY_FRAME=1650
STEP=60

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[[ $# -ge 1 ]] || usage
OUT="$1"; shift
CATEGORY=0 INDEX=0 SECONDS_TO_RUN=120 MASK="" RECORD=0 DUMP="" SNAPSHOTS=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --category) CATEGORY="$2"; shift 2 ;;
        --index) INDEX="$2"; shift 2 ;;
        --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
        --mask) MASK="$(realpath "$2")"; shift 2 ;;
        --record) RECORD=1; shift ;;
        --dump) DUMP="$2"; shift 2 ;;
        --snapshots) SNAPSHOTS=1; shift ;;
        *) usage ;;
    esac
done

# The input script: THEATER on the main menu; in the lobby up to "FILM:",
# open the chooser, down to the category, into its list, down to the film,
# select it (back in the lobby, on FILM:), down to START FILM, start.
frame=$LOBBY_FRAME
script="f$MENU_FRAME-$((MENU_FRAME + 5)):a"
press() { script+=",f$frame-$((frame + 5)):$1"; frame=$((frame + STEP)); }
press up
press a
for ((i = 0; i < CATEGORY; i++)); do press down; done
press a
for ((i = 0; i < INDEX; i++)); do press down; done
press a
press down
press a

mkdir -p "$OUT/game"
OUT="$(realpath "$OUT")"
for entry in default.xex game hdd modules image.bin image.bin.json shadercache; do
    ln -sfn "$DATA/$entry" "$OUT/game/$entry"
done
sed -e 's/^fullscreen *=.*/fullscreen = false/' -e 's/^width *=.*/width = 1280/' \
    -e 's/^height *=.*/height = 720/' -e 's/^render_scale *=.*/render_scale = 1/' \
    -e 's/^frame_rate *=.*/frame_rate = 60/' "$DATA/settings.ini" > "$OUT/game/settings.ini"
if [[ -f "$PREFS" ]]; then
    cp -a "$PREFS" "$DATA/hdd/Cache0/preferences.dat"
else
    echo "warning: no $PREFS; the menu cursor must already be on THEATER" >&2
fi

env=(H3_RANDOM_SEED=1 H3_LOCKSTEP=1 H3_NO_WARMUP=1 H3_STATE_HASH="$OUT/hash.txt" H3_INPUT_SCRIPT="$script")
[[ -n "$MASK" ]] && env+=(H3_STATE_MASK="$MASK")
[[ $RECORD == 1 ]] && env+=(H3_STATE_RECORD="$OUT/record.bin")
[[ -n "$DUMP" ]] && env+=(H3_STATE_DUMP="$DUMP")
[[ $SNAPSHOTS == 1 ]] && env+=(H3_SNAPSHOT_EVERY=600)

# State dumps and snapshots land in the working directory.
cd "$OUT"
env "${env[@]}" timeout "$SECONDS_TO_RUN" "$BINARY" "$OUT/game/default.xex" > "$OUT/run.log" 2>&1 &
pid=$!
while kill -0 "$pid" 2>/dev/null; do
    available=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
    if (( available < MIN_AVAILABLE_KB )); then
        echo "low memory ($available kB available): stopping the game" >&2
        kill "$pid"
    fi
    sleep 2
done
wait "$pid" || true
echo "$OUT: $(wc -l < "$OUT/hash.txt") ticks logged"
