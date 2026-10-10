#!/usr/bin/env python3
"""Turns H3_PROFILE's profile.txt into a report: samples per thread, and the
hottest functions overall and per thread. Samples in shared libraries are
named by library and nearest exported symbol.

Usage: tools/h3profile.py <H3Recomp binary> <profile.txt> [top N]
"""
import bisect
import collections
import os
import subprocess
import sys


def load_symbols(path, dynamic):
    args = ["nm", "-C", "--defined-only", "-n"] + (["-D"] if dynamic else []) + [path]
    symbols = []
    for line in subprocess.run(args, capture_output=True, text=True).stdout.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "tTwWiI":
            symbols.append((int(parts[0], 16), parts[2]))
    symbols.sort()
    return [a for a, _ in symbols], symbols


def nearest(table, offset):
    addresses, symbols = table
    i = bisect.bisect_right(addresses, offset) - 1
    return symbols[i][1] if i >= 0 else "?"


def main():
    binary, profile = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
    executable = load_symbols(binary, False)
    base = 0
    maps = []  # (start, end, file offset, path)
    samples = []
    for line in open(profile):
        if line.startswith("# base "):
            base = int(line.split()[2], 16)
        elif line.startswith("# map "):
            fields = line.split()
            start, end = (int(x, 16) for x in fields[2].split("-"))
            path = fields[7] if len(fields) > 7 else "?"
            maps.append((start, end, int(fields[4], 16), path))
        else:
            thread, address = line.split()
            samples.append((int(thread), int(address, 16)))
    maps.sort()
    starts = [m[0] for m in maps]
    libraries = {}
    binary_name = os.path.basename(binary)

    def name(address):
        if not maps:  # old format: offsets into the executable
            return nearest(executable, address)
        i = bisect.bisect_right(starts, address) - 1
        if i < 0 or address >= maps[i][1]:
            return "?"
        start, _, file_offset, path = maps[i]
        if os.path.basename(path) == binary_name:
            return nearest(executable, address - base)
        if path not in libraries:
            libraries[path] = load_symbols(path, True) if os.path.exists(path) else ([], [])
        offset = address - start + file_offset
        return f"[{os.path.basename(path)}] {nearest(libraries[path], offset)}"

    names_cache = {}

    def cached(address):
        if address not in names_cache:
            names_cache[address] = name(address)
        return names_cache[address]

    total = len(samples)
    print(f"{total} samples")
    threads = collections.Counter(t for t, _ in samples)
    names = collections.Counter(cached(a) for _, a in samples)
    print("\nThreads:")
    for thread, count in threads.most_common():
        top_function = collections.Counter(cached(a) for t, a in samples if t == thread).most_common(4)
        print(f"  {thread}: {count} ({100 * count / total:.1f}%)  " + ", ".join(f"{n[:50]} {c}" for n, c in top_function))
    print("\nFunctions:")
    for function, count in names.most_common(top):
        print(f"  {100 * count / total:5.1f}%  {function[:110]}")


if __name__ == "__main__":
    main()
