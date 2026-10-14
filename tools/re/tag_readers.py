#!/usr/bin/env python3
"""Annotates recompiled functions' tag reads (docs/ROADMAP.md 6.3 milestone A).

    tag_readers.py <defs.json> <function address>...
    tag_readers.py <defs.json> --list [--leaf-pure]

For a function in H3RecompLib/ppc, follows its registers through one pass
(branches ignored: good enough for the small accessors this is for) and
reports what each tag read is:

  - tag data pointers: from the tag table (0x827CFF0C, entry +4 by slot),
    the global scenario (0x827CFF08) and game globals (0x827CF604)
    pointers, block elements (a block's address + index * element size);
  - every load from them: offset and width, named from the 360 layouts
    when the struct is known ('scnr', 'matg', blocks of a known struct);
  - for pointers whose group isn't known: the groups whose layout has a
    field starting at every offset read, with matching widths.

--list prints the functions that read the tag table inline, smallest first,
with whether they call anything and store outside their stack frame.

defs.json: tools/tagdefs output (private/tagdefs/halo3_360.json).
"""
import collections
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PPC = os.path.join(ROOT, 'H3RecompLib', 'ppc')

TAG_TABLE = 0x827CFF0C
SCENARIO_TAG = 0x827CFF08
GLOBALS_TAG = 0x827CF604
WIDTH = {'lbz': 1, 'lhz': 2, 'lha': 2, 'lwz': 4, 'lfs': 4, 'lfd': 8, 'ld': 8}


def load_functions():
    functions = {}
    for path in glob.glob(os.path.join(PPC, 'ppc_recomp.*.cpp')):
        name = None
        for line in open(path):
            if line.startswith('PPC_FUNC_IMPL('):
                m = re.search(r'sub_([0-9A-F]{8})', line)
                name = m.group(1) if m else None
                if name:
                    functions[name] = []
            elif name and line.strip().startswith('// '):
                functions[name].append(line.strip()[3:])
            elif line.startswith('}'):
                name = None
    return functions


def signed16(text):
    return int(text)


class Layouts:
    """Field lookup in the 360 layouts: flattened (embedded structs and
    arrays expanded) field starts per struct."""

    def __init__(self, defs):
        self.defs = defs
        self.flat_cache = {}

    def flat(self, sid):
        if sid in self.flat_cache:
            return self.flat_cache[sid]
        out = {}
        structs = self.defs['structs']

        def walk(sid, at, prefix):
            for f in structs[sid]['fields']:
                o = at + f['offset']
                if f['type'] == 'struct':
                    walk(f['struct'], o, prefix + f['name'] + '.')
                elif f['type'] == 'array':
                    inner = structs[f['struct']]['size']
                    for i in range(f['count']):
                        walk(f['struct'], o + i * inner, f'{prefix}{f["name"]}[{i}].')
                elif f['size']:
                    out.setdefault(o, (prefix + f['name'], f['type'], f['size'], f))
        walk(sid, 0, '')
        self.flat_cache[sid] = out
        return out

    def group_chain(self, tag):
        """(struct id, start offset) of a group's ancestors' parts."""
        chain, t = [], tag
        while t:
            chain.insert(0, self.defs['groups'][t]['struct'])
            t = self.defs['groups'][t]['parent']
        out, at = [], 0
        for sid in chain:
            out.append((sid, at))
            at += self.defs['structs'][sid]['size']
        return out

    def group_flat(self, tag):
        out = {}
        for sid, at in self.group_chain(tag):
            for o, field in self.flat(sid).items():
                out[at + o] = field
        return out

    def describe(self, flat, offset, width):
        """'name (type)' for a read at offset, including reads inside a
        field (a block's address at +4, a vector's y at +4)."""
        if offset in flat:
            name, kind, size, _ = flat[offset]
            return f'{name} ({kind})'
        for o in sorted((o for o in flat if o < offset), reverse=True)[:1]:
            name, kind, size, _ = flat[o]
            if offset < o + size:
                part = {('block', 4): 'address', ('block', 0): 'count', ('tag reference', 12): 'datum',
                        ('tag reference', 0): 'group', ('data', 12): 'address', ('data', 0): 'size'}.get((kind, offset - o),
                        f'+{offset - o:#x}')
                return f'{name} ({kind}).{part}'
        return '?'

    def fits(self, flat, reads):
        """Whether every (offset, width) read lands on a field (or a block's
        count/address, a reference's datum)."""
        for offset, width in reads:
            if offset in flat:
                name, kind, size, _ = flat[offset]
                if width > size:
                    return False
                continue
            inside = [o for o in flat if o < offset < o + flat[o][2]]
            if not inside:
                return False
            o = inside[-1]
            kind = flat[o][1]
            if (kind, offset - o) not in (('block', 4), ('tag reference', 12), ('data', 12)):
                return False
        return True

    def candidates(self, reads):
        return [tag for tag in self.defs['groups'] if reads and self.fits(self.group_flat(tag), reads)]


def annotate(name, body, layouts, out=print):
    """One pass over body; prints the tag reads."""
    # Symbolic register values: ('const', n), ('tagtable',), ('slot', srcreg), ('tag', group or None, key),
    # ('elem', struct id, key), ('blockaddr', struct id, owner key)
    regs = {}
    tags = {}   # key -> {'group': tag or None, 'struct': sid or None, 'reads': [(offset, width, line)]}
    keyn = [0]

    def new_tag(group=None, struct=None, origin=''):
        keyn[0] += 1
        k = f'T{keyn[0]}'
        tags[k] = {'group': group, 'struct': struct, 'reads': [], 'origin': origin}
        return k

    address = int(name, 16)
    for i, line in enumerate(body):
        pc = address + 4 * i
        m = re.match(r'(\w+)\.? (.*)', line)
        if not m:
            continue
        op, args = m.group(1), [a.strip() for a in m.group(2).split(',')]
        try:
            if op == 'lis':
                regs[args[0]] = ('const', (int(args[1]) & 0xFFFF) << 16)
            elif op in ('addi', 'addic') and len(args) == 3:
                src = regs.get(args[1])
                imm = int(args[2])
                if src and src[0] == 'const':
                    regs[args[0]] = ('const', (src[1] + imm) & 0xFFFFFFFF)
                elif src and src[0] == 'tag':
                    regs[args[0]] = ('tagoff', src[1], imm)
                else:
                    regs[args[0]] = None
            elif op == 'li':
                regs[args[0]] = ('const', int(args[1]) & 0xFFFFFFFF)
            elif op == 'mr':
                regs[args[0]] = regs.get(args[1])
            elif op == 'rlwinm' and args[2:] == ['3', '13', '28']:
                src = regs.get(args[1])
                regs[args[0]] = ('slot', src[1] if src and src[0] == 'datum' else None)
            elif op == 'mulli':
                regs[args[0]] = ('scaled', int(args[2]))
            elif op == 'add':
                a, b = regs.get(args[1]), regs.get(args[2])
                pair = {x[0] if x else None: x for x in (a, b)}
                if 'slot' in pair and 'tagtable' in pair:
                    regs[args[0]] = ('entry', pair['slot'][1])
                elif 'scaled' in pair and 'blockaddr' in pair:
                    owner = pair['blockaddr']
                    k = new_tag(None, owner[1], f'element ({pair["scaled"][1]:#x} bytes) of {owner[2]}')
                    regs[args[0]] = ('tag', k)
                elif 'tag' in pair and ('scaled' in pair):
                    regs[args[0]] = None
                else:
                    regs[args[0]] = None
            elif op in WIDTH or op in ('lwzx', 'lhzx', 'lbzx', 'lfsx'):
                if op.endswith('x'):
                    regs[args[0]] = None
                    continue
                mm = re.match(r'(-?\d+)\((r\d+)\)', args[1])
                if not mm:
                    regs[args[0]] = None
                    continue
                off, basereg = int(mm.group(1)), mm.group(2)
                b = regs.get(basereg)
                value = None
                if b and b[0] == 'const':
                    target = (b[1] + off) & 0xFFFFFFFF
                    if target == TAG_TABLE:
                        value = ('tagtable',)
                    elif target == SCENARIO_TAG:
                        value = ('tag', new_tag('scnr', None, 'global scenario'))
                    elif target == GLOBALS_TAG:
                        value = ('tag', new_tag('matg', None, 'global game globals'))
                elif b and b[0] == 'entry' and off == 4:
                    group = b[1] if len(b) > 1 else None
                    value = ('tag', new_tag(group, None, 'tag table entry' + (' (datum of a reference to it)' if group else '')))
                elif b and b[0] == 'blockaddr':
                    # The block's address used directly: its first element.
                    k = new_tag(None, b[1], f'element 0 of {b[2]}')
                    regs[basereg] = ('tag', k)
                    b = regs[basereg]
                if b and b[0] in ('tag', 'tagoff'):
                    k = b[1]
                    o = off + (b[2] if b[0] == 'tagoff' else 0)
                    tags[k]['reads'].append((o, WIDTH.get(op, 4), f'{pc:08X} {line}'))
                    # A block's address: remember which block.
                    t = tags[k]
                    sid = t['struct']
                    flat = layouts.group_flat(t['group']) if t['group'] else layouts.flat(sid) if sid else None
                    if flat and (o - 4) in flat and flat[o - 4][1] == 'block':
                        value = ('blockaddr', flat[o - 4][3]['struct'], f'{k}.{flat[o - 4][0]}')
                    # A reference's datum: the group it may reference, if one.
                    if flat and (o - 12) in flat and flat[o - 12][1] == 'tag reference':
                        group = flat[o - 12][3].get('group')
                        value = ('datum', group if group in layouts.defs['groups'] else None)
                regs[args[0]] = value
            elif re.match(r'(st|cmp|b|c|mt|mf)', op):
                pass
            elif args:
                regs[args[0]] = None
        except (ValueError, IndexError):
            if args:
                regs[args[0]] = None
    for k, t in tags.items():
        if not t['reads']:
            continue
        what = t['group'] and f"'{t['group']}'" or t['struct'] or 'unknown group'
        out(f'  {k}: {what} ({t["origin"]})')
        flat = layouts.group_flat(t['group']) if t['group'] else layouts.flat(t['struct']) if t['struct'] else None
        for o, w, line in t['reads']:
            out(f'    {o:#06x} {w}  {layouts.describe(flat, o, w) if flat else ""}   [{line}]')
        if not flat:
            reads = sorted({(o, w) for o, w, _ in t['reads']})
            c = layouts.candidates(reads)
            out(f'    candidates ({len(c)}): {" ".join(c[:20])}{" ..." if len(c) > 20 else ""}')
            for g in c[:5]:
                gf = layouts.group_flat(g)
                out(f'      {g}: ' + '; '.join(f'{o:#x} {layouts.describe(gf, o, w)}' for o, w in reads))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    defs = json.load(open(sys.argv[1]))
    layouts = Layouts(defs)
    functions = load_functions()
    if sys.argv[2] == '--list':
        leaf_pure = '--leaf-pure' in sys.argv
        rows = []
        for name, body in functions.items():
            if not any(re.search(r'lwz r\d+,-244\(r\d+\)', x) for x in body):
                continue
            calls = any(re.match(r'bl 0x', x) and not re.match(r'bl 0x8258[de]', x) for x in body) or \
                any(x.startswith('bctrl') for x in body)
            stores = any(re.match(r'st[bhwdf]\w* ', x) and not x.endswith('(r1)') for x in body)
            if leaf_pure and (calls or stores):
                continue
            rows.append((len(body), name, calls, stores))
        for n, name, calls, stores in sorted(rows):
            print(f'{name} {n:5d} {"calls" if calls else "leaf"} {"stores" if stores else "pure"}')
        return
    for name in sys.argv[2:]:
        name = name.upper().replace('SUB_', '').replace('0X', '')
        print(f'sub_{name}:')
        annotate(name, functions[name], layouts)


if __name__ == '__main__':
    main()
