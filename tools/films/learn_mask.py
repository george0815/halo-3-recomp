#!/usr/bin/env python3
"""Learns an H3_STATE_MASK for a film from two recorded replays of it.

    learn_mask.py learn <out mask> <record> <record>... [--known <mask>]
    learn_mask.py check <mask> <record> <record>...

The records come from run_film.sh --record (H3_STATE_RECORD): every game
tick's raw state as the words that changed since the previous tick. Both
replays are rebuilt tick by tick over the film's level (from the last tick
counter reset), each one against the first, and every word that differs at
any tick becomes a rule (format: debug/state_hash.cpp):

    array <name>             in a data array other than "object"
    object <tag> <offset>    in an object: that field of every object with
                             the same definition tag
    after <name>             in the memory between an array and the next one
    after <name> @ <offset>  the same in a gap over 128 KB: just that 64-byte
                             block (big gaps hold other subsystems' state)
    word <offset>            before the first array (the game globals)

"learn" writes the --known rules plus the new ones; "check" lists the words
a mask leaves differing (none means the replays hash alike). One pair of
replays doesn't show every cosmetic field: learn from five or six.

What differs between replays with H3_RANDOM_SEED and H3_LOCKSTEP is
cosmetic (effects, HUD, a few object fields that depend on when
asynchronous loads finish), but a learned rule can't tell: read the new
rules before trusting a mask (tools/films/README.md).
"""
import array
import bisect
import re
import struct
import sys

# Data array header (0x54 bytes before the elements).
ARRAY_SIGNATURE = 0x2C        # 'd@t@'
ARRAY_MAX_COUNT = 0x20
ARRAY_ELEMENTS = 0x44         # guest address of the elements
ARRAY_TOTAL_BYTES = 0x50
ARRAY_HEADER_BYTES = 0x54
OBJECT_HANDLE_BYTES = 16      # "object" array element: +12 the object's address
OBJECT_HANDLE_DATA = 12
MAX_OBJECT_BYTES = 0x4000
GAP_BLOCK, GAP_BLOCK_LIMIT = 64, 0x20000


def records(path):
    """Yields (tick, word indexes, words as stored) per game state copy."""
    with open(path, 'rb') as f:
        while True:
            header = f.read(8)
            if len(header) < 8:
                return
            tick, count = struct.unpack('<2I', header)
            pairs = array.array('I')
            pairs.frombytes(f.read(count * 8))
            yield tick, pairs[0::2], pairs[1::2]


def level_start(path):
    """Index of the copy where the tick counter last reset (the level)."""
    last, start = None, 0
    for i, (tick, _, _) in enumerate(records(path)):
        if last is not None and tick < last:
            start = i
        last = tick
    return start


def find_arrays(state):
    """(start, end, name) of every data array, by offset."""
    out = []
    for m in re.finditer(rb'd@t@', state):
        start = m.start() - ARRAY_SIGNATURE
        if start < 0 or start % 4:
            continue
        name = state[start:start + 32].split(b'\0')[0].decode('latin1')
        total = struct.unpack('>I', state[start + ARRAY_TOTAL_BYTES:start + ARRAY_TOTAL_BYTES + 4])[0]
        out.append((start, min(len(state), start + total), name))
    return sorted(out)


def find_objects(state, words, arrays):
    """(offset, definition tag) of every live object, by offset."""
    pool = [a for a in arrays if a[2] == 'object']
    if not pool:
        return []
    start = pool[0][0]
    count = struct.unpack('>i', state[start + ARRAY_MAX_COUNT:start + ARRAY_MAX_COUNT + 4])[0]
    elements = struct.unpack('>I', state[start + ARRAY_ELEMENTS:start + ARRAY_ELEMENTS + 4])[0]
    address = elements - (start + ARRAY_HEADER_BYTES)  # guest address of the state
    out = []
    for slot in range(count):
        handle = start + ARRAY_HEADER_BYTES + slot * OBJECT_HANDLE_BYTES + OBJECT_HANDLE_DATA
        pointer = words[handle // 4]
        if pointer and 0 < pointer - address < len(state):
            offset = pointer - address
            out.append((offset, words[offset // 4]))
    return sorted(out)


def classify(state, arrays, objects, offset):
    """The rule that masks the word at offset."""
    for start, end, name in arrays:
        if start <= offset < end and name != 'object':
            return ('array', name)
    pool = [a for a in arrays if a[2] == 'object']
    k = bisect.bisect_right([o[0] for o in objects], offset) - 1
    if pool and k >= 0 and offset >= pool[0][1] and offset - objects[k][0] < MAX_OBJECT_BYTES:
        return ('object', f'{objects[k][1]:08x}', f'{offset - objects[k][0]:x}')
    before = [a for a in arrays if a[1] <= offset]
    if before:
        prev = max(before, key=lambda a: a[1])
        following = min([a[0] for a in arrays if a[0] >= prev[1]] + [len(state)])
        if following - prev[1] > GAP_BLOCK_LIMIT:
            return ('after', prev[2], f'{(offset - prev[1]) & ~(GAP_BLOCK - 1):x}')
        return ('after', prev[2])
    return ('word', f'{offset:x}')


def covered(rule, mask):
    """Whether the mask already covers what rule describes."""
    return rule in mask or (rule[0] == 'after' and len(rule) == 3 and ('after', rule[1]) in mask)


def format_rule(rule):
    if rule[0] == 'after' and len(rule) == 3:
        return f'after {rule[1]} @ {rule[2]}'
    return ' '.join(rule)


def read_mask(path):
    rules = set()
    for line in open(path):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        kind, rest = line.split(None, 1)
        if kind == 'after' and ' @ ' in rest:
            rules.add((kind, *rest.split(' @ ')))
        elif kind in ('array', 'after'):
            rules.add((kind, rest))
        else:
            rules.add((kind, *rest.split()))
    return rules


def differences(paths, known):
    """{rule: first tick} for every difference the known rules don't cover."""
    runs = []
    for path in paths:
        it, start, words, tick = records(path), level_start(path), None, 0
        for i, (tick, indexes, values) in enumerate(it):
            if words is None:
                words = array.array('I', bytes(len(indexes) * 4))
            for k, v in zip(indexes, values):
                words[k] = v
            if i == start:
                break
        runs.append((it, words, tick))
    (it_a, a, tick), (it_b, b, tick_b) = runs
    if tick != tick_b:
        sys.exit(f'the records start their level at different ticks ({tick}, {tick_b})')
    differ = {i for i in range(len(a)) if a[i] != b[i]}
    found, arrays = {}, None
    while True:
        if differ:
            state = a.tobytes()  # the recorded words are the state's bytes as stored
            words = array.array('I', a)
            words.byteswap()
            if arrays is None:
                arrays = find_arrays(state)  # fixed within a level
            objects = find_objects(state, words, arrays)
            for i in differ:
                rule = classify(state, arrays, objects, i * 4)
                if not covered(rule, known):
                    found.setdefault(rule, tick)
        try:
            tick, ia, va = next(it_a)
            tick_b, ib, vb = next(it_b)
        except StopIteration:
            return found
        if tick != tick_b:
            sys.exit(f'records out of step at ticks {tick} and {tick_b}')
        for k, v in zip(ia, va):
            a[k] = v
        for k, v in zip(ib, vb):
            b[k] = v
        for k in set(ia) | set(ib):
            if a[k] != b[k]:
                differ.add(k)
            else:
                differ.discard(k)


def against_first(paths, known):
    """differences() of each record against the first, merged."""
    found = {}
    for path in paths[1:]:
        for rule, tick in differences([paths[0], path], known).items():
            found[rule] = min(tick, found.get(rule, tick))
    return found


def main():
    args = sys.argv[1:]
    known = set()
    if '--known' in args:
        k = args.index('--known')
        known = read_mask(args[k + 1])
        del args[k:k + 2]
    if len(args) >= 4 and args[0] == 'learn':
        out, paths = args[1], args[2:]
        new = against_first(paths, known)
        with open(out, 'w') as f:
            f.write(f'# H3_STATE_MASK learned by learn_mask.py from {" ".join(paths)}\n')
            for rule in sorted(known | set(new)):
                f.write(format_rule(rule) + '\n')
        print(f'{len(new)} new rules (first tick each):')
        for rule, tick in sorted(new.items(), key=lambda r: r[1]):
            print(f'  {tick:6d}  {format_rule(rule)}')
    elif len(args) >= 4 and args[0] == 'check':
        left = against_first(args[2:], read_mask(args[1]))
        print('the mask covers every difference' if not left else f'{len(left)} differences left:')
        for rule, tick in sorted(left.items(), key=lambda r: r[1]):
            print(f'  {tick:6d}  {format_rule(rule)}')
        sys.exit(1 if left else 0)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
