#!/usr/bin/env python3
"""Turns H3_PROFILE's profile.txt into a report: samples per thread, and the
hottest functions overall and per thread. Samples in shared libraries are
named by library and nearest exported symbol.

Usage: tools/h3profile.py <H3Recomp binary> <profile.txt> [top N] [thread id]
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
    only_thread = int(sys.argv[4]) if len(sys.argv) > 4 else None  # one thread's samples only
    executable = load_symbols(binary, False)
    base = 0
    maps = []  # (start, end, file offset, path)
    samples = []
    stacks = []  # return addresses per sample (H3_PROFILE_STACKS)
    for line in open(profile):
        if line.startswith("# base "):
            base = int(line.split()[2], 16)
        elif line.startswith("# map "):
            fields = line.split()
            start, end = (int(x, 16) for x in fields[2].split("-"))
            path = fields[7] if len(fields) > 7 else "?"
            maps.append((start, end, int(fields[4], 16), path))
        else:
            fields = line.split()
            if only_thread is not None and int(fields[0]) != only_thread:
                continue
            samples.append((int(fields[0]), int(fields[1], 16)))
            stacks.append([int(x, 16) for x in fields[2:]])
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

    def in_executable(address):
        if not maps:
            return True
        i = bisect.bisect_right(starts, address) - 1
        return i >= 0 and address < maps[i][1] and os.path.basename(maps[i][3]) == binary_name

    # Time in shared libraries, by the executable's function that called in.
    if any(stacks):
        callers = collections.Counter()
        pairs = collections.Counter()  # (calling function, library function)
        library_samples = 0
        for (thread, address), stack in zip(samples, stacks):
            if in_executable(address) or not stack:
                continue
            library_samples += 1
            # Frames up to and including the interrupted one belong to the
            # signal handler; the rest are return addresses.
            frames = stack[stack.index(address) + 1:] if address in stack else stack
            caller = next((f for f in frames if in_executable(f) and "h3::debug" not in cached(f - 1)), None)
            callers[cached(caller - 1) if caller else "?"] += 1
            pairs[(cached(caller - 1) if caller else "?", cached(address))] += 1
        print(f"\nLibrary time ({library_samples} samples) by calling function in the executable:")
        for function, count in callers.most_common(top):
            print(f"  {100 * count / total:5.1f}%  {function[:110]}")
        print("\nLibrary time by caller and library function:")
        for (caller, function), count in pairs.most_common(top):
            print(f"  {100 * count / total:5.1f}%  {caller[:60]} -> {function[:60]}")


if __name__ == "__main__":
    main()
