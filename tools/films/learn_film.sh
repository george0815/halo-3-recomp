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
#   --dumps <n>     replays dumped at full speed when the records agree
#                   (default 4; 0 = stop there instead)
#   --jobs <n>      replays run at once (default 1), each with its own data
#                   directory from H3_FILM_DATA_SLOTS (colon-separated; the
#                   game writes to hdd/ and shadercache/, so slots can't
#                   share them). A parallel full pair that matches is
#                   confirmed by one more replay run alone, at full speed
#                   (some races only show there); dumps always run alone.
#
# Each round: two full replays with the current mask; if they match on
# every tick, done. Otherwise record <records> replays of the differing
# stretches (H3_STATE_RECORD_TICKS spans from compare_hashes.py --spans, up
# to --window level ticks; those runs stop right after the last), learn
# rules for what differs there (learn_mask.py --known), and go again.
# When the records all agree (recording slows a replay down, and some races
# only go both ways at full speed: Floodgate, Ark), dump raw states at a
# few level times in each stretch from <dumps> unrecorded replays and learn
# from those instead (learn_mask.py dumps).
# Ticks here are level time (compare_hashes.py: through checkpoint reverts). Writes private/films/<name>.mask, <name>.hash.txt (the
# reference: the first replay of the last round) and <name>.rules.log (the
# rules each round added, to read before trusting the mask). Work files go
# to private/films_work/<name>/ (records are deleted once learned).
#
# Uses H3_FILM_DATA (default private/films_data: a copy of the game files
# and hdd/, so test runs never touch the real save data) for replays run
# alone, and H3_FILM_DATA_SLOTS (default H3_FILM_DATA, then
# private/films_data2) for --jobs.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export H3_FILM_DATA="${H3_FILM_DATA:-$ROOT/private/films_data}"
IFS=: read -ra SLOTS <<< "${H3_FILM_DATA_SLOTS:-$H3_FILM_DATA:$ROOT/private/films_data2}"
FILMS="$ROOT/tools/films"
CATEGORY=3

# Seconds of run time: ~60 for the menus and level load, then ~37 game
# ticks per second (one tick per frame), with headroom; replays sharing the
# machine run slower (PARALLEL_TICKS_PER_SECOND). These only bound a run:
# each stops on its own at the film's end or after its record window.
MENU_SECONDS=90
TICKS_PER_SECOND=30
PARALLEL_TICKS_PER_SECOND=20

usage() { sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }
[[ $# -ge 3 ]] || usage
NAME="$1" INDEX="$2" FILM_SECONDS="$3"; shift 3
START="$ROOT/private/films/generic.mask" WINDOW=600 RECORDS=3 ROUNDS=10 FIRST="" MAX_NEW=60 DUMPS=4 JOBS=1
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
        --dumps) DUMPS="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        *) usage ;;
    esac
done

WORK="$ROOT/private/films_work/$NAME"
MASK="$ROOT/private/films/$NAME.mask"
RULES="$ROOT/private/films/$NAME.rules.log"
mkdir -p "$WORK"
[[ -f "$MASK" ]] || cp "$START" "$MASK"
log() { echo "[$(date +%H:%M:%S)] $NAME: $*" | tee -a "$WORK/learn.log"; }
(( JOBS > ${#SLOTS[@]} )) && JOBS=${#SLOTS[@]}
(( JOBS > 1 )) && RUN_TICKS_PER_SECOND=$PARALLEL_TICKS_PER_SECOND || RUN_TICKS_PER_SECOND=$TICKS_PER_SECOND

# Runs "<function> <item>" for each item, at most JOBS at once, each with
# H3_FILM_DATA set to a slot no other running one has (every replay restores
# and writes its data directory's hdd/). Returns 1 if any failed.
run_slots() {
    local fn="$1" item pid done_pid failed=0; shift
    local -a free=("${SLOTS[@]:0:JOBS}")
    local -A slot_of=()
    for item in "$@"; do
        if (( ${#free[@]} == 0 )); then
            wait -n -p done_pid "${!slot_of[@]}" || failed=1
            free+=("${slot_of[$done_pid]}")
            unset "slot_of[$done_pid]"
        fi
        H3_FILM_DATA="${free[0]}" "$fn" "$item" &
        slot_of[$!]="${free[0]}"
        free=("${free[@]:1}")
    done
    for pid in "${!slot_of[@]}"; do
        wait "$pid" || failed=1
    done
    return $failed
}

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
# Whether the replay that logged hash.txt reached the film's end.
end_reached() {
    local last
    last=$("$FILMS/compare_hashes.py" --length "$1" 2>/dev/null || echo 0)
    (( last + END_SLACK >= FILM_SECONDS * 60 )) && return 0
    (( $(end_held "$1") >= END_HOLD_COPIES && last * 100 >= FILM_SECONDS * 60 * END_MIN_PERCENT ))
}
# full_run <dir> [<seconds>]: one full replay into dir, retried once when
# it falls short of the film's end; bounded by FULL_SECONDS, or seconds.
full_run() {
    local dir="$1" seconds="${2:-$FULL_SECONDS}" attempt last
    for attempt in 1 2; do
        rm -rf "$dir"
        "$FILMS/run_film.sh" "$dir" --category $CATEGORY --index "$INDEX" \
            --seconds "$seconds" --mask "$MASK" --snapshots "${WAIT[@]}" >> "$WORK/learn.log" 2>&1
        end_reached "$dir/hash.txt" && return 0
        last=$("$FILMS/compare_hashes.py" --length "$dir/hash.txt" 2>/dev/null || echo 0)
        log "$(basename "$dir") stopped at level time $last of ~$(( FILM_SECONDS * 60 )) (attempt $attempt)"
    done
    return 1
}

# Full replay seconds: menus plus the film at 60 ticks per film second
# (FULL_SECONDS run alone, PARALLEL_FULL_SECONDS sharing the machine).
FULL_SECONDS=$(( MENU_SECONDS + FILM_SECONDS * 60 / TICKS_PER_SECOND ))
PARALLEL_FULL_SECONDS=$(( MENU_SECONDS + FILM_SECONDS * 60 / RUN_TICKS_PER_SECOND ))
full_pair_run() { full_run "$WORK/full_$1" "$PARALLEL_FULL_SECONDS"; }

# record_run <n>: replay n of a round, recording $spans into rec<n>.
record_run() {
    rm -rf "$WORK/rec$1"
    H3_STATE_RECORD_TICKS="$spans" "$FILMS/run_film.sh" "$WORK/rec$1" --category $CATEGORY \
        --index "$INDEX" --seconds "$seconds" --record "${WAIT[@]}" >> "$WORK/learn.log" 2>&1
}

# Dumps per stretch: its first differing level time (record spans start
# SPAN_MARGIN before it, compare_hashes.py), its middle and its end, for the
# first DUMP_SPANS stretches (4 MB per state per replay).
SPAN_MARGIN=10
DUMP_SPANS=6

# Level times to dump for record spans "a-b,c-d,...", comma-separated.
dump_times() {
    tr ',' '\n' <<< "$1" | head -n "$DUMP_SPANS" | awk -F- -v margin=$SPAN_MARGIN '
        { last = ($2 == "") ? $1 : $2; first = $1 + margin; if (first > last) first = last
          print first; print int((first + last) / 2); print last }' | sort -nu | paste -sd,
}

# Learns new rules into $MASK: learn_mask.py <mode> <inputs>, the rules log
# headed <what>. Sets before/after (rule counts); exits on a failure or a
# burst (a real divergence).
learn() {
    local mode="$1" what="$2" burst; shift 2
    before=$(grep -vc '^#' "$MASK")
    cp "$MASK" "$WORK/mask.before"
    "$FILMS/learn_mask.py" "$mode" "$MASK.new" "$@" --known "$MASK" > "$WORK/learned.txt" 2>&1
    cat "$WORK/learned.txt" >> "$WORK/learn.log"
    if [[ ! -s "$MASK.new" ]]; then
        log "learn_mask.py $mode failed (see learn.log); stopping"
        exit 1
    fi
    # learn_mask.py lists each new rule as "<first tick> <rule>". The 64-byte
    # blocks of one gap ("after <name> @ <offset>") count once: one effect
    # in a big gap is many blocks (Cortana: 122 of "effect geometry sample").
    burst=$(awk '$1 ~ /^[0-9]+$/ {rule = $0; sub(/ @ .*/, "", rule); if (!seen[rule]++) n[$1]++}
        END {m = 0; for (t in n) if (n[t] > m) {m = n[t]; at = t} print m + 0, at + 0}' "$WORK/learned.txt")
    if (( ${burst% *} > MAX_NEW )); then
        log "${burst% *} new rules first differ at tick ${burst#* } (over $MAX_NEW): replays diverge there? Kept the mask; new one in $MASK.new for review; stopping"
        exit 1
    fi
    mv "$MASK.new" "$MASK"
    after=$(grep -vc '^#' "$MASK")
    { echo "== round $round, $what"; diff <(grep -v '^#' "$WORK/mask.before") <(grep -v '^#' "$MASK") | grep '^>' ; } >> "$RULES"
    log "mask: $before -> $after rules"
}

# Records take ~90-180 KB per level tick each (Covenant: 7.2 GB for 40000
# ticks), all <records> of a round on disk at once: a full disk (2026-10-23)
# left records cut short and learn_mask.py's output empty. Each round
# records at most what fits in the free space less DISK_RESERVE_BYTES.
RECORD_BYTES_PER_TICK=200000
DISK_RESERVE_BYTES=$(( 5 * 1000 * 1000 * 1000 ))
MIN_RECORD_TICKS=300

# Sets budget: the level ticks to record this round, --window or less to
# fit the disk; stops when even MIN_RECORD_TICKS don't fit. Deletes the last
# round's records first (superseded by this round's full pair; kept until
# now only when that round stopped).
record_budget() {
    local free
    rm -f "$WORK"/rec*/record.bin
    free=$(df --output=avail -B1 "$WORK" | tail -n 1)
    budget=$(( (free - DISK_RESERVE_BYTES) / (RECORDS * RECORD_BYTES_PER_TICK) ))
    if (( budget < MIN_RECORD_TICKS )); then
        log "disk: $(( free / 1000000000 )) GB free, room for $budget ticks of $RECORDS records (under $MIN_RECORD_TICKS); stopping"
        exit 1
    fi
    if (( budget < WINDOW )); then
        log "disk: $(( free / 1000000000 )) GB free: recording $budget ticks, not $WINDOW"
    else
        budget=$WINDOW
    fi
}

for ((round = 1; round <= ROUNDS; round++)); do
  if [[ $round == 1 && -n "$FIRST" ]]; then
    first=$FIRST
    record_budget
    spans="$first-$(( first + budget - 1 ))"
    log "round 1: first differing tick given: $first"
  else
    log "round $round: two full replays ($PARALLEL_FULL_SECONDS s max each, $JOBS at once)"
    run_slots full_pair_run a b
    for r in a b; do
        if ! end_reached "$WORK/full_$r/hash.txt"; then
            log "full replay $r didn't reach the film's end twice; stopping"
            exit 1
        fi
    done
    result=$("$FILMS/compare_hashes.py" "$WORK/full_a/hash.txt" "$WORK/full_b/hash.txt")
    status=$?
    log "$result"
    # A pair run side by side ran slower than full speed, where some races
    # only go both ways (Floodgate, Ark): a match is confirmed by a replay
    # run alone; if that one differs, it is this round's second replay.
    if [[ $status == 0 && $JOBS -gt 1 ]]; then
        log "pair matches; confirming with a replay run alone ($FULL_SECONDS s max)"
        if ! full_run "$WORK/full_c"; then
            log "full replay c didn't reach the film's end twice; stopping"
            exit 1
        fi
        result=$("$FILMS/compare_hashes.py" "$WORK/full_a/hash.txt" "$WORK/full_c/hash.txt")
        status=$?
        log "alone: $result"
        if [[ $status != 0 ]]; then
            rm -rf "$WORK/full_b" && mv "$WORK/full_c" "$WORK/full_b"
        fi
    fi
    if [[ $status == 0 ]]; then
        cp "$WORK/full_a/hash.txt" "$ROOT/private/films/$NAME.hash.txt"
        log "done: replays match; reference private/films/$NAME.hash.txt"
        exit 0
    fi
    first=$(sed -n 's/.*first at tick \([0-9]*\).*/\1/p' <<< "$result")
    record_budget
    spans=$("$FILMS/compare_hashes.py" --spans "$budget" "$WORK/full_a/hash.txt" "$WORK/full_b/hash.txt")
    if [[ -z "$first" || -z "$spans" ]]; then
        log "no differing tick in the comparison (runs too short?); stopping"
        exit 1
    fi
  fi
    last=${spans##*-}
    seconds=$(( MENU_SECONDS + last / RUN_TICKS_PER_SECOND + 20 ))
    log "recording ticks $spans in $RECORDS replays ($seconds s each, $JOBS at once)"
    run_slots record_run $(seq 1 "$RECORDS")
    records=()
    for ((r = 1; r <= RECORDS; r++)); do
        records+=("$WORK/rec$r/record.bin")
        # Empty when the replay never reached the level (run_film.sh gave
        # up retrying the START FILM press) or stopped before the window.
        if [[ ! -s "$WORK/rec$r/record.bin" ]]; then
            log "rec$r recorded nothing (no level? see its hash.txt); stopping"
            exit 1
        fi
    done
    learn learn "ticks $spans" "${records[@]}"
    rm -f "$WORK"/rec*/record.bin
    if [[ $after == "$before" && $DUMPS -ge 2 ]]; then
        times=$(dump_times "$spans")
        seconds=$(( MENU_SECONDS + ${times##*,} / TICKS_PER_SECOND + 20 ))
        log "records agree: dumping level times $times from $DUMPS full-speed replays ($seconds s each)"
        dumps=()
        for ((r = 1; r <= DUMPS; r++)); do
            rm -rf "$WORK/dump$r"
            "$FILMS/run_film.sh" "$WORK/dump$r" --category $CATEGORY --index "$INDEX" \
                --seconds "$seconds" --mask "$MASK" --dump "$times" "${WAIT[@]}" >> "$WORK/learn.log" 2>&1
            dumps+=("$WORK/dump$r")
            if ! compgen -G "$WORK/dump$r/state_*.bin" > /dev/null; then
                log "dump$r dumped nothing (no level? see its hash.txt); stopping"
                exit 1
            fi
        done
        learn dumps "dumps at $times" "${dumps[@]}"
        rm -f "$WORK"/dump*/state_*.bin
    fi
    if [[ $after == "$before" ]]; then
        log "no new rules for a differing tick: the replays really diverge at $first (not cosmetic?); stopping"
        exit 1
    fi
done
log "gave up after $ROUNDS rounds"
exit 1
