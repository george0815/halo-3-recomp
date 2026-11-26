#!/usr/bin/env python3
"""The game state's TLS slots: which hold data arrays, and their names.

    data_arrays.py <snapshot.mem> [--names] [--all]

<snapshot.mem> is a call recording's memory snapshot (tools/films/
record_calls.sh: <dir>/tick_<t>.mem, a sparse file whose byte offset is
the guest address); the thread block (r13) comes from the .calls file next
to it (the most common r13 among its calls: the main thread).

Game state is reached through r13: the word at r13 + 0 points at the TLS
array, one word per slot (slot = byte offset, as the code uses it). Data
arrays are recognized by their header:

    +0x00 char[32] name     +0x2C 'd@t@' signature
    +0x20 s32 maximum count +0x34 s32 next index
    +0x24 s32 element size  +0x38 s32 first unallocated
    +0x28 u8 alignment bits +0x3C s32 actual count
    +0x29 u8 valid          +0x40 u16 next salt
    +0x2A u16 flags         +0x44 elements (guest address)

--names also scans the recompiled code (H3RecompLib/ppc) for game state
allocations stored into TLS slots (the allocation's name string is passed
in r3/r4 before the call), to name the slots that aren't data arrays
(globals structures, indexes). --all lists every non-null slot.
"""
import collections
import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SIGNATURE = 0x64407440  # 'd@t@'
TLS_SLOTS = 1024        # bytes of the TLS array scanned
RECORD = struct.Struct('<II14Q14Q')  # call_record.cpp CallRecord (232 bytes)


class Memory:
    def __init__(self, path):
        self.file = open(path, 'rb')
        self.size = os.fstat(self.file.fileno()).st_size

    def read(self, address, size):
        if address < 0x10000 or address + size > self.size:
            return None
        self.file.seek(address)
        return self.file.read(size)

    def u32(self, address):
        data = self.read(address, 4)
        return struct.unpack('>I', data)[0] if data else None

    def string(self, address, size=64):
        data = self.read(address, size)
        if not data:
            return None
        text = data.split(b'\0')[0]
        return text.decode('latin1') if text and all(32 <= c < 127 for c in text) else None


def main_thread_block(memory_path):
    calls = memory_path[:-len('.mem')] + '.calls'
    data = open(calls, 'rb').read()
    if data[:8] != b'H3CALLS1':
        sys.exit(f'{calls}: not a call recording')
    counts = collections.Counter()
    for offset in range(8, len(data) - RECORD.size + 1, RECORD.size):
        fields = RECORD.unpack_from(data, offset)
        counts[fields[2 + 13] & 0xFFFFFFFF] += 1
    if not counts:
        sys.exit(f'{calls}: no calls')
    return counts.most_common(1)[0][0]


def data_array(memory, address):
    header = memory.read(address, 0x48)
    if not header or struct.unpack_from('>I', header, 0x2C)[0] != SIGNATURE:
        return None
    maximum, size = struct.unpack_from('>ii', header, 0x20)
    next_index, first_unallocated, count = struct.unpack_from('>iii', header, 0x34)
    elements = struct.unpack_from('>I', header, 0x44)[0]
    name = header[:32].split(b'\0')[0].decode('latin1')
    return name, maximum, size, count, first_unallocated, elements


def allocation_names(memory):
    """TLS slot -> names of the game state allocations the code stores there."""
    names = collections.defaultdict(set)
    function_line = re.compile(r'PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)')
    instruction = re.compile(r'\s*// (\w+)\.? (.*)')
    for path in sorted(glob.glob(os.path.join(ROOT, 'H3RecompLib/ppc/ppc_recomp.*.cpp'))):
        tls, high, immediate, strings, call_names = set(), {}, {}, {}, []
        for line in open(path):
            if function_line.match(line):
                tls, high, immediate, strings, call_names = set(), {}, {}, {}, []
                continue
            m = instruction.match(line)
            if not m:
                continue
            op, args = m.group(1), [a.strip() for a in m.group(2).split(',')]
            if op == 'lwz' and len(args) == 2 and args[1] == '0(r13)':
                tls.add(args[0])
            elif op == 'lis' and len(args) == 2:
                high[args[0]] = (int(args[1], 0) & 0xFFFF) << 16
            elif op == 'addi' and len(args) == 3 and args[1] in high:
                text = memory.string((high[args[1]] + int(args[2], 0)) & 0xFFFFFFFF)
                if text:
                    strings[args[0]] = text
            elif op == 'li' and len(args) == 2:
                immediate[args[0]] = int(args[1], 0)
            elif op == 'bl':
                # The allocation's name: r4 (game state malloc) or r3 (data array new).
                call_names = [strings[r] for r in ('r4', 'r3') if r in strings]
                strings = {}
            elif op == 'stwx' and len(args) == 3 and args[0] == 'r3' and call_names:
                for slot_register, base in ((args[1], args[2]), (args[2], args[1])):
                    if slot_register in immediate and base in tls:
                        names[immediate[slot_register]].update(call_names[:1])
    return names


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1 or not args[0].endswith('.mem'):
        sys.exit(__doc__)
    memory = Memory(args[0])
    r13 = main_thread_block(args[0])
    tls = memory.u32(r13)
    if tls is None:
        sys.exit(f'thread block {r13:#x} not in the snapshot')
    names = allocation_names(memory) if '--names' in sys.argv else {}
    print(f'thread block {r13:#010x}, TLS array {tls:#010x}')
    print(f'{"slot":>5}  {"address":>10}  {"name":32}  {"max":>6}  {"size":>6}  {"count":>6}  elements')
    for slot in range(0, TLS_SLOTS, 4):
        value = memory.u32(tls + slot)
        if not value:
            continue
        array = data_array(memory, value)
        if array:
            name, maximum, size, count, _, elements = array
            print(f'{slot:>5}  {value:#010x}  {name:32}  {maximum:>6}  {size:>6}  {count:>6}  {elements:#010x}')
        elif slot in names or '--all' in sys.argv:
            label = ', '.join(sorted(names.get(slot, ()))) or '?'
            print(f'{slot:>5}  {value:#010x}  ({label})')


if __name__ == '__main__':
    main()
