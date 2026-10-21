#!/usr/bin/env python3
"""Compares two films' per-tick state checksums (run_film.sh hash.txt).

    compare_hashes.py <reference hash.txt> <hash.txt>
    compare_hashes.py --length <hash.txt>
    compare_hashes.py --spans <budget> <hash.txt> <hash.txt>

Compares the last level of each log (from the last tick counter drop to
below LEVEL_START_TICKS, so the menus don't count, up to where Theater steps
back at the film's end) by level time: game ticks played, counting on
through checkpoint reverts (which send the tick counter back; state_hash.cpp
H3_STATE_RECORD_TICKS uses the same). Prints the first differing level time
and the differing stretches; exits 1 if any differs. Both runs must use the
same mask (H3_STATE_MASK) for the hashes to mean the same thing. --length
prints the level time a log reached (to check a run got to the film's end).
--spans prints the differing stretches as H3_STATE_RECORD_TICKS spans (each
from SPAN_MARGIN before), in order, until they cover <budget> level ticks:
what learn_film.sh records next.
In a film without reverts level time is the game tick.
"""
import sys


# A drop in the tick counter to below this starts a level. A smaller drop
# is a checkpoint revert, or Theater stepping back at the film's end (it
# jumps back and holds there): a last stretch after a drop shorter than
# END_HOLD_TICKS is that, and isn't compared.
LEVEL_START_TICKS = 100
END_HOLD_TICKS = 600
SPAN_MARGIN = 10


def level(path):
    """{level time: hash} over the log's last level. Copies repeating the
    previous tick (held) don't advance level time; the first one counts."""
    rows = [line.split() for line in open(path) if line.strip()]
    ticks = [int(row[1]) for row in rows]
    start = 0
    for i in range(1, len(rows)):
        if ticks[i] < ticks[i - 1] and ticks[i] < LEVEL_START_TICKS:
            start = i
    out, time, last_drop = {}, 0, None
    for i in range(start, len(rows)):
        if i > start:
            if ticks[i] == ticks[i - 1]:
                continue
            time += 1
            if ticks[i] < ticks[i - 1]:
                last_drop = time
        out[time] = rows[i][2]
    if last_drop is not None and time - last_drop < END_HOLD_TICKS:
        out = {t: h for t, h in out.items() if t < last_drop}
    return out


def stretches(bad):
    """Runs of consecutive level times, as [first, last]."""
    out = []
    for t in bad:
        if out and t == out[-1][1] + 1:
            out[-1][1] = t
        else:
            out.append([t, t])
    return out


def spans(runs, budget):
    """The runs widened by SPAN_MARGIN, merged, cut at budget ticks."""
    out, left = [], budget
    for first, last in runs:
        first = max(0, first - SPAN_MARGIN)
        if out and first <= out[-1][1] + 1:
            first = out[-1][1] + 1
            if first > last:
                continue
        last = min(last, first + left - 1)
        if out and first == out[-1][1] + 1:
            out[-1][1] = last
        else:
            out.append([first, last])
        left -= last - first + 1
        if left <= 0:
            break
    return ','.join(f'{a}-{b}' for a, b in out)


def main():
    if len(sys.argv) == 5 and sys.argv[1] == '--spans':
        a, b = level(sys.argv[3]), level(sys.argv[4])
        bad = sorted(t for t in set(a) & set(b) if a[t] != b[t])
        print(spans(stretches(bad), int(sys.argv[2])))
        return
    if len(sys.argv) == 3 and sys.argv[1] == '--length':
        times = level(sys.argv[2])
        print(max(times) if times else 0)
        return
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, b = level(sys.argv[1]), level(sys.argv[2])
    common = sorted(set(a) & set(b))
    if not common:
        sys.exit('no ticks in common')
    bad = [t for t in common if a[t] != b[t]]
    print(f'{len(common)} common ticks ({common[0]}-{common[-1]}), {len(bad)} differ'
          + (f', first at tick {bad[0]}' if bad else ''))
    if len(a) != len(b):
        print(f'(lengths differ: {len(a)} and {len(b)} ticks)')
    runs = stretches(bad)
    if runs:
        print('differing ticks:', ', '.join(f'{s}-{e}' if s != e else f'{s}' for s, e in runs[:20])
              + (' ...' if len(runs) > 20 else ''))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
