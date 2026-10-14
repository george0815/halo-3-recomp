#!/usr/bin/env python3
"""Compares two films' per-tick state checksums (run_film.sh hash.txt).

    compare_hashes.py <reference hash.txt> <hash.txt>

Compares the last level of each log (from the last tick counter reset, so
the menus don't count) tick by tick. Prints the first differing tick and
the differing stretches; exits 1 if any tick differs. Both runs must use
the same mask (H3_STATE_MASK) for the hashes to mean the same thing.
"""
import sys


def level(path):
    """{tick: hash} over the log's last level."""
    rows = [line.split() for line in open(path) if line.strip()]
    start = 0
    for i in range(1, len(rows)):
        if int(rows[i][1]) < int(rows[i - 1][1]):
            start = i
    return {int(tick): digest for _, tick, digest, *_ in rows[start:]}


def main():
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
    stretches = []
    for t in bad:
        if stretches and t == stretches[-1][1] + 1:
            stretches[-1][1] = t
        else:
            stretches.append([t, t])
    if stretches:
        print('differing ticks:', ', '.join(f'{s}-{e}' if s != e else f'{s}' for s, e in stretches[:20])
              + (' ...' if len(stretches) > 20 else ''))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
