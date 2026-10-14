#!/usr/bin/env python3
"""Which groups have a field at an offset (whole tag: ancestors' parts first).

    layout_query.py <defs.json> <offset> [<element size>] [<group>...]

Prints, per group (all, or those listed), the field at that offset; with an
element size, only blocks whose elements are that big.
"""
import json
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from tag_readers import Layouts

defs = json.load(open(sys.argv[1]))
offset = int(sys.argv[2], 0)
size = int(sys.argv[3], 0) if len(sys.argv) > 3 and not sys.argv[3].isalpha() and sys.argv[3] != '-' else None
groups = [g for g in sys.argv[4 if size is not None else 3:] if g in defs['groups']] or list(defs['groups'])
layouts = Layouts(defs)
for g in groups:
    flat = layouts.group_flat(g)
    if offset not in flat:
        continue
    name, kind, fsize, field = flat[offset]
    if size is not None:
        if kind != 'block' or defs['structs'][field['struct']]['size'] != size:
            continue
    extra = f' -> {field["struct"]} ({defs["structs"][field["struct"]]["size"]:#x} bytes)' if 'struct' in field else ''
    print(f'{g}: {name} ({kind}){extra}')
