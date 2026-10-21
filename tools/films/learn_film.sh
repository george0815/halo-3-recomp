#!/usr/bin/env bash
# Learns one film's mask iteratively and keeps its reference hashes
# (tools/films/README.md, "Adding a film", automated for long films).
#
# Usage: tools/films/learn_film.sh <name> <index> <film seconds> [options]
#   <name>          the film's name in the library (private/films/<name>.*)
#   <index>         its index in Theater's CAMPAIGN category (category 3)
#   <film seconds>  its length (Theater's LENGTH; bounds the full runs)
#   --start <mask>  mask to start from (default private/films/generic.mask)
#   --window <n>    level ticks recorded per round (default 600): the
#                   differing stretches in order, from just before each
#   --records <n>   replays recorded per divergence (default 3)
#   --rounds <n>    give up after this many rounds (default 10)
#   --first <tick>  the first differing tick is already known: start the
#                   first round by recording there
#   --max-new <n>   stop for review when more than this many new rules
#                   first differ at the same tick (default 60): cosmetic
#                   state comes in a few fields at a time, a burst means the
#                   replays really diverged (or a broken record)
#   --start-wait <n> run_film.sh --start-wait: frames the lobby needs to
#                   load the film's map before START FILM (library.txt's
#                   fourth column; default run_film.sh's)
#
# Each round: two full replays with the current mask; if they match on
# every tick, done. Otherwise record <records> replays of the differing
# stretches (H3_STATE_RECORD_TICKS spans from compare_hashes.py --spans, up
# to --window level ticks; those runs stop right after the last), learn
# rules for what differs there (learn_mask.py --known), and go again.
# Ticks here are level time (compare_hashes.py: through checkpoint reverts). Writes private/films/<name>.mask, <name>.hash.txt (the
# reference: the first replay of the last round) and <name>.rules.log (the
# rules each round added, to read before trusting the mask). Work files go
# to private/films_work/<name>/ (records are deleted once learned).
#
# Uses H3_FILM_DATA (default private/films_data: a copy of the game files
# and hdd/, so test runs never touch the real save data).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export H3_FILM_DATA="${H3_FILM_DATA:-$ROOT/private/films_data}"
FILMS="$ROOT/tools/films"
CATEGORY=3

# Seconds of run time: ~60 for the menus and level load, then ~37 game
# ticks per second (one tick per frame), with headroom.
MENU_SECONDS=90
TICKS_PER_SECOND=30

usage() { sed -n '2,33p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[[ $# -ge 3 ]] || usage
NAME="$1" INDEX="$2" FILM_SECONDS="$3"; shift 3
START="$ROOT/private/films/generic.mask" WINDOW=600 RECORDS=3 ROUNDS=10 FIRST="" MAX_NEW=60
WAIT=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --start) START="$(realpath "$2")"; shift 2 ;;
        --window) WINDOW="$2"; shift 2 ;;
        --records) RECORDS="$2"; shift 2 ;;
        --rounds) ROUNDS="$2"; shift 2 ;;
        --first) FIRST="$2"; shift 2 ;;
        --max-new) MAX_NEW="$2"; shift 2 ;;
        --start-wait) WAIT=(--start-wait "$2"); shift 2 ;;
        *) usage ;;
    esac
done

WORK="$ROOT/private/films_work/$NAME"
MASK="$ROOT/private/films/$NAME.mask"
RULES="$ROOT/private/films/$NAME.rules.log"
mkdir -p "$WORK"
[[ -f "$MASK" ]] || cp "$START" "$MASK"
log() { echo "[$(date +%H:%M:%S)] $NAME: $*" | tee -a "$WORK/learn.log"; }

# A full replay must reach the film's end: a run cut short (low memory, a
# crash, the time limit) would otherwise compare "alike" over the ticks it
# has. It has when its level time (game ticks played through any checkpoint
# reverts) is within END_SLACK of the film's length at 60 ticks per second,
# or when it ends in Theater's end hold (END_HOLD_COPIES copies of the last
# tick) past END_MIN_PERCENT of that length: library.txt's length can run
# long (Crow's Nest: 1264 s listed, 71583 ticks played).
END_SLACK=120
END_HOLD_COPIES=300
END_MIN_PERCENT=80

# Copies of hash.txt's last tick at its end (Theater's end hold).
end_held() {
    awk '{ tick[NR] = $2 } END { n = 0; for (i = NR; i > 0 && tick[i] == tick[NR]; i--) n++; print n }' "$1" 2>/dev/null || echo 0
}
full_run() {
    local dir="$1" attempt last
    for attempt in 1 2; do
        rm -rf "$dir"
        "$FILMS/run_film.sh" "$dir" --category $CATEGORY --index "$INDEX" \
            --seconds "$FULL_SECONDS" --mask "$MASK" --snapshots "${WAIT[@]}" >> "$WORK/learn.log" 2>&1
        last=$("$FILMS/compare_hashes.py" --length "$dir/hash.txt" 2>/dev/null || echo 0)
        (( last + END_SLACK >= FILM_SECONDS * 60 )) && return 0
        (( $(end_held "$dir/hash.txt") >= END_HOLD_COPIES &&
           last * 100 >= FILM_SECONDS * 60 * END_MIN_PERCENT )) && return 0
        log "$(basename "$dir") stopped at level time $last of ~$(( FILM_SECONDS * 60 )) (attempt $attempt)"
    done
    return 1
}

# Full replay seconds: menus plus the film at 60 ticks per film second.
FULL_SECONDS=$(( MENU_SECONDS + FILM_SECONDS * 60 / TICKS_PER_SECOND ))

for ((round = 1; round <= ROUNDS; round++)); do
  if [[ $round == 1 && -n "$FIRST" ]]; then
    first=$FIRST
    spans="$first-$(( first + WINDOW - 1 ))"
    log "round 1: first differing tick given: $first"
  else
    log "round $round: two full replays ($FULL_SECONDS s max each)"
    for r in a b; do
        if ! full_run "$WORK/full_$r"; then
            log "full replay $r didn't reach the film's end twice; stopping"
            exit 1
        fi
    done
    result=$("$FILMS/compare_hashes.py" "$WORK/full_a/hash.txt" "$WORK/full_b/hash.txt")
    status=$?
    log "$result"
    if [[ $status == 0 ]]; then
        cp "$WORK/full_a/hash.txt" "$ROOT/private/films/$NAME.hash.txt"
        log "done: replays match; reference private/films/$NAME.hash.txt"
        exit 0
    fi
    first=$(sed -n 's/.*first at tick \([0-9]*\).*/\1/p' <<< "$result")
    spans=$("$FILMS/compare_hashes.py" --spans "$WINDOW" "$WORK/full_a/hash.txt" "$WORK/full_b/hash.txt")
    if [[ -z "$first" || -z "$spans" ]]; then
        log "no differing tick in the comparison (runs too short?); stopping"
        exit 1
    fi
  fi
    last=${spans##*-}
    seconds=$(( MENU_SECONDS + last / TICKS_PER_SECOND + 20 ))
    log "recording ticks $spans in $RECORDS replays ($seconds s each)"
    records=()
    for ((r = 1; r <= RECORDS; r++)); do
        rm -rf "$WORK/rec$r"
        H3_STATE_RECORD_TICKS="$spans" "$FILMS/run_film.sh" "$WORK/rec$r" --category $CATEGORY \
            --index "$INDEX" --seconds "$seconds" --record "${WAIT[@]}" >> "$WORK/learn.log" 2>&1
        records+=("$WORK/rec$r/record.bin")
        # Empty when the replay never reached the level (run_film.sh gave
        # up retrying the START FILM press) or stopped before the window.
        if [[ ! -s "$WORK/rec$r/record.bin" ]]; then
            log "rec$r recorded nothing (no level? see its hash.txt); stopping"
            exit 1
        fi
    done
    before=$(grep -vc '^#' "$MASK")
    cp "$MASK" "$WORK/mask.before"
    "$FILMS/learn_mask.py" learn "$MASK.new" "${records[@]}" --known "$MASK" > "$WORK/learned.txt" 2>&1
    cat "$WORK/learned.txt" >> "$WORK/learn.log"
    if [[ ! -s "$MASK.new" ]]; then
        log "learn_mask.py failed (see learn.log); stopping"
        exit 1
    fi
    # learn_mask.py lists each new rule as "<first tick> <rule>".
    burst=$(awk '$1 ~ /^[0-9]+$/ {n[$1]++} END {m = 0; for (t in n) if (n[t] > m) {m = n[t]; at = t} print m + 0, at + 0}' "$WORK/learned.txt")
    if (( ${burst% *} > MAX_NEW )); then
        log "${burst% *} new rules first differ at tick ${burst#* } (over $MAX_NEW): replays diverge there? Kept the mask; new one in $MASK.new for review; stopping"
        exit 1
    fi
    mv "$MASK.new" "$MASK"
    after=$(grep -vc '^#' "$MASK")
    { echo "== round $round, ticks $spans"; diff <(grep -v '^#' "$WORK/mask.before") <(grep -v '^#' "$MASK") | grep '^>' ; } >> "$RULES"
    log "mask: $before -> $after rules"
    rm -f "$WORK"/rec*/record.bin
    if [[ $after == "$before" ]]; then
        log "no new rules for a differing tick: the replays really diverge at $first (not cosmetic?); stopping"
        exit 1
    fi
done
log "gave up after $ROUNDS rounds"
exit 1
