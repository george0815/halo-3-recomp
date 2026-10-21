#!/usr/bin/env python3
"""Compares the live objects of two recorded replays at some ticks.

    compare_objects.py <record> <record> <tick>... [--tags <map>]

The records come from run_film.sh --record (windowed or not). For each tick
it lists, per object definition tag, how many objects each replay has, and
whether they're the same objects: matched by position (ObjectBase +0x54,
three big-endian floats), then compared by slot (datum index) and by where
in object memory they live.

Tells apart the two reasons learn_mask.py can see many fields of one
object type differ: the replays really diverged (different objects or
positions), or the same objects sit in other slots or other memory
(allocation order shifted by something cosmetic created earlier).
--tags names the tags (h3tags <map> tags output).
"""
import collections
import struct
import subprocess
import sys

import learn_mask as lm

POSITION = 0x54


def states_at(path, wanted):
    """{tick: state bytes} for the wanted ticks of a record."""
    out, words = {}, None
    for tick, indexes, values in lm.records(path):
        if words is None:
            words = bytearray(4 * len(indexes))
            view = memoryview(words).cast('I')
        for k, v in zip(indexes, values):
            view[k] = v
        if tick in wanted:
            out[tick] = bytes(words)
            if len(out) == len(wanted):
                break
    return out


def objects(state):
    """[(slot, tag, offset, position)] of the live objects."""
    arrays = lm.find_arrays(state)
    pool = [a for a in arrays if a[2] == 'object']
    if not pool:
        return []
    start = pool[0][0]
    count = struct.unpack('>i', state[start + lm.ARRAY_MAX_COUNT:start + lm.ARRAY_MAX_COUNT + 4])[0]
    elements = struct.unpack('>I', state[start + lm.ARRAY_ELEMENTS:start + lm.ARRAY_ELEMENTS + 4])[0]
    address = elements - (start + lm.ARRAY_HEADER_BYTES)
    out = []
    for slot in range(count):
        handle = start + lm.ARRAY_HEADER_BYTES + slot * lm.OBJECT_HANDLE_BYTES + lm.OBJECT_HANDLE_DATA
        pointer = struct.unpack('>I', state[handle:handle + 4])[0]
        if pointer and 0 < pointer - address < len(state):
            offset = pointer - address
            tag = struct.unpack('>I', state[offset:offset + 4])[0]
            position = struct.unpack('>3f', state[offset + POSITION:offset + POSITION + 12])
            out.append((slot, tag, offset, tuple(round(p, 3) for p in position)))
    return out


def tag_names(map_path):
    if not map_path:
        return {}
    text = subprocess.run(['build/release/tools/h3tags/h3tags', map_path, 'tags'],
                          capture_output=True, text=True).stdout
    names = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 4:
            names[int(parts[0], 16)] = parts[1] + ' ' + parts[3].rsplit('\\', 1)[-1]
    return names


def main():
    args = sys.argv[1:]
    names = {}
    if '--tags' in args:
        i = args.index('--tags')
        names = tag_names(args[i + 1])
        del args[i:i + 2]
    if len(args) < 3:
        sys.exit(__doc__)
    a_path, b_path, wanted = args[0], args[1], {int(t) for t in args[2:]}
    a_states, b_states = states_at(a_path, wanted), states_at(b_path, wanted)
    for tick in sorted(wanted):
        if tick not in a_states or tick not in b_states:
            print(f'tick {tick}: not in both records')
            continue
        a, b = objects(a_states[tick]), objects(b_states[tick])
        print(f'== tick {tick}: {len(a)} and {len(b)} objects')
        by_tag_a, by_tag_b = collections.defaultdict(list), collections.defaultdict(list)
        for o in a:
            by_tag_a[o[1]].append(o)
        for o in b:
            by_tag_b[o[1]].append(o)
        for tag in sorted(set(by_tag_a) | set(by_tag_b)):
            oa, ob = by_tag_a[tag], by_tag_b[tag]
            pa = sorted(o[3] for o in oa)
            pb = sorted(o[3] for o in ob)
            same_positions = pa == pb
            same_slots = sorted((o[0], o[3]) for o in oa) == sorted((o[0], o[3]) for o in ob)
            same_memory = sorted((o[2], o[3]) for o in oa) == sorted((o[2], o[3]) for o in ob)
            if same_positions and same_slots and same_memory:
                continue
            verdict = ('same objects, other slots/memory' if same_positions
                       else 'DIFFERENT objects/positions')
            print(f'  {tag:08x} {names.get(tag, "")}: {len(oa)} vs {len(ob)}: {verdict}'
                  f' (slots {"same" if same_slots else "differ"}, memory {"same" if same_memory else "differ"})')


if __name__ == '__main__':
    main()
