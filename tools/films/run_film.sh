#!/usr/bin/env bash
# Plays one saved film in Theater and logs a checksum of the game state per
# game tick (docs/ROADMAP.md 6.1; how to use it: tools/films/README.md).
#
# Usage: tools/films/run_film.sh <out dir> [options]
#   --category <n>   Theater film category: 0 recent films (default),
#                    1 file share, 2 film clips, 3 campaign, 4 multiplayer,
#                    5 forge
#   --index <n>      film within the category, 0 = the top one (default 0)
#   --seconds <n>    how long to run at most (default 120; the menus take
#                    ~30 s, then one tick per frame, ~37 ticks/s; the run
#                    also stops once the film is over)
#   --mask <file>    H3_STATE_MASK (cosmetic state left out of the hash)
#   --record         also write record.bin (H3_STATE_RECORD, ~90 KB per
#                    tick) for learn_mask.py --record
#   --dump <times>   also save raw states at these level times, comma-
#                    separated (H3_STATE_DUMP; compare_hashes.py's ticks)
#   --snapshots      also save a screenshot every 600 frames (to check the
#                    menu navigation reached the film)
#   --start-wait <n> extra frames before pressing START FILM (default 0;
#                    3000 for a library film, category 3 and up: the lobby
#                    ignores the press while it's still loading the film's
#                    map, "Loading... n%"; Floodgate's and Crow's Nest's
#                    are Ready ~2000-2500 frames after the press would be)
#
# Writes <out dir>/hash.txt ("<copy> <tick> <hash>" per game tick, menus
# included; compare_hashes.py compares the last level), run.log, and the
# optional outputs. Extra H3_* variables in the environment pass through.
#
# The runs are deterministic only with the switches this sets:
# H3_RANDOM_SEED (fixed random seeds, the game state at one address) and
# H3_LOCKSTEP (one game tick per frame). It also sets H3_NO_60HZ_FIXES: the
# library's films were recorded without the 60 Hz content fixes
# (hooks/tick_rate.h), and a film replays its inputs only, so changed game
# behavior would take it somewhere else (H3_FILM_WITH_60HZ_FIXES=1 leaves
# them on, to see what they change).
#
# Expects (paths overridable by environment):
#   H3_FILM_BINARY  the game (build/release/H3Recomp/H3Recomp)
#   H3_FILM_DATA    the game files: default.xex, game/, hdd/, modules/,
#                   image.bin, shadercache/, settings.ini (private/)
#   H3_FILM_DATA/hdd.pristine (optional) the hdd/ to start every run from:
#                   the game writes to hdd/ (its map cache, checkpoints),
#                   which changes later runs' loading (lost menu presses,
#                   different load timing); what changed is put back first
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

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[[ $# -ge 1 ]] || usage
OUT="$1"; shift
CATEGORY=0 INDEX=0 SECONDS_TO_RUN=120 MASK="" RECORD=0 DUMP="" SNAPSHOTS=0 START_WAIT=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --category) CATEGORY="$2"; shift 2 ;;
        --index) INDEX="$2"; shift 2 ;;
        --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
        --mask) MASK="$(realpath "$2")"; shift 2 ;;
        --record) RECORD=1; shift ;;
        --dump) DUMP="$2"; shift 2 ;;
        --snapshots) SNAPSHOTS=1; shift ;;
        --start-wait) START_WAIT="$2"; shift 2 ;;
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
[[ -z "$START_WAIT" ]] && START_WAIT=$(( CATEGORY >= 3 ? 3000 : 0 ))
frame=$((frame + START_WAIT))
press a

mkdir -p "$OUT/game"
OUT="$(realpath "$OUT")"
for entry in default.xex game hdd modules image.bin image.bin.json shadercache; do
    ln -sfn "$DATA/$entry" "$OUT/game/$entry"
done
sed -e 's/^fullscreen *=.*/fullscreen = false/' -e 's/^width *=.*/width = 1280/' \
    -e 's/^height *=.*/height = 720/' -e 's/^render_scale *=.*/render_scale = 1/' \
    -e 's/^frame_rate *=.*/frame_rate = 60/' "$DATA/settings.ini" > "$OUT/game/settings.ini"
# Puts back the files the game changed in hdd/ since the last restore (the
# .restored stamp; the first time, all of hdd/ is replaced).
restore_hdd() {
    local hdd="$DATA/hdd" pristine="$DATA/hdd.pristine" file relative
    [[ -d "$pristine" ]] || return 0
    if [[ ! -f "$hdd/.restored" ]]; then
        rm -rf "$hdd" && cp -a "$pristine" "$hdd"
    else
        while IFS= read -r -d '' file; do
            relative="${file#"$hdd"/}"
            if [[ -e "$pristine/$relative" ]]; then
                cp -a "$pristine/$relative" "$file"
            else
                rm -f "$file"
            fi
        done < <(find "$hdd" -type f -newer "$hdd/.restored" ! -name .restored -print0)
    fi
    touch "$hdd/.restored"
}
restore_hdd
if [[ -f "$PREFS" ]]; then
    cp -a "$PREFS" "$DATA/hdd/Cache0/preferences.dat"
else
    echo "warning: no $PREFS; the menu cursor must already be on THEATER" >&2
fi

env=(H3_RANDOM_SEED=1 H3_LOCKSTEP=1 H3_NO_WARMUP=1 H3_STATE_HASH="$OUT/hash.txt" H3_INPUT_SCRIPT="$script")
[[ -n "${H3_FILM_WITH_60HZ_FIXES:-}" ]] || env+=(H3_NO_60HZ_FIXES=1)
[[ -n "$MASK" ]] && env+=(H3_STATE_MASK="$MASK")
[[ $RECORD == 1 ]] && env+=(H3_STATE_RECORD="$OUT/record.bin")
[[ -n "$DUMP" ]] && env+=(H3_STATE_DUMP="$DUMP")
[[ $SNAPSHOTS == 1 ]] && env+=(H3_SNAPSHOT_EVERY=600)

# State dumps and snapshots land in the working directory.
cd "$OUT"
# The film is over when the game tick stops advancing for STALL_SECONDS
# after the level started (Theater holds the last moment): stop then rather
# than at --seconds. The level has started when the tick counter drops (the
# menus count too); if that hasn't happened by LEVEL_DEADLINE the START FILM
# press was lost (it sometimes is, however long the wait before it): try
# again, up to ATTEMPTS times.
# Not shorter: a shader compile can hold the tick 20 s or more mid-film
# (Floodgate, tick 1376, the watchdog's "stuck entering critical section"
# with the render thread in rasterizer_wait_for_fence). A deep frame
# capture (H3_CAPTURE_DEEP) holds it for minutes: raise it with
# H3_FILM_STALL_SECONDS.
STALL_SECONDS=${H3_FILM_STALL_SECONDS:-60}
# Menus run at ~60 frames per second: a longer --start-wait moves the press
# (and the level) later.
LEVEL_DEADLINE=$(( 150 + START_WAIT / 60 ))
ATTEMPTS=3
for ((attempt = 1; attempt <= ATTEMPTS; attempt++)); do
    rm -f "$OUT"/hash.txt "$OUT"/record.bin "$OUT"/frame_*.png "$OUT"/state_*.bin
    if (( attempt > 1 )); then
        restore_hdd
        [[ -f "$PREFS" ]] && cp -a "$PREFS" "$DATA/hdd/Cache0/preferences.dat"
    fi
    env "${env[@]}" timeout "$SECONDS_TO_RUN" "$BINARY" "$OUT/game/default.xex" > "$OUT/run.log" 2>&1 &
    pid=$!
    # Never leave the game running if this script stops early.
    trap 'kill "$pid" 2>/dev/null || true' EXIT
    last_tick="" stalled=0 elapsed=0 in_level=0 lost=0
    while kill -0 "$pid" 2>/dev/null; do
        available=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
        if (( available < MIN_AVAILABLE_KB )); then
            echo "low memory ($available kB available): stopping the game" >&2
            kill "$pid"
        fi
        tick=$(tail -n 1 "$OUT/hash.txt" 2>/dev/null | awk '{print $2}' || true)
        if [[ -n "$tick" && -n "$last_tick" ]] && (( tick < last_tick )); then
            in_level=1
        fi
        if (( !in_level && elapsed >= LEVEL_DEADLINE )); then
            echo "attempt $attempt: no level after ${LEVEL_DEADLINE} s (START FILM press lost?): retrying" >&2
            lost=1
            kill "$pid"
            break
        fi
        if [[ -n "$tick" && "$tick" == "$last_tick" && "$tick" -gt 1000 ]]; then
            stalled=$((stalled + 2))
            if (( stalled >= STALL_SECONDS )); then
                echo "film over (tick $tick held ${STALL_SECONDS} s): stopping the game" >&2
                kill "$pid"
            fi
        else
            stalled=0
        fi
        last_tick="$tick"
        sleep 2
        elapsed=$((elapsed + 2))
    done
    wait "$pid" || true
    (( lost )) || break
done
echo "$OUT: $(wc -l < "$OUT/hash.txt") ticks logged"
