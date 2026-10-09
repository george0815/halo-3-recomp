#!/usr/bin/env python3
"""Fix up XenonAnalyse output for Halo 3 and the DLLs it loads.

Needs the flat image and its layout from
`h3xexinfo <xex> --dump <image.bin>` (which also writes <image.bin>.json).

  h3analysis.py tables <image.bin> <switch_tables.toml>
      Rewrite the jump table file. Halo 3's absolute jump tables all follow one
      inline pattern:

          lis   r12, table@ha
          addi  r12, r12, table@l
          slwi  r0, rIndex, 2
          lwzx  r0, r12, r0
          mtctr r0
          bctr
          table: .long case0, case1, ...

      XenonAnalyse misreads some of them (wrong index register, wrong bound,
      code read as labels) and misses the ones without a bounds check
      entirely. Missed ones compile to an indirect call to a case label and
      crash at runtime. Every table that matches the pattern is re-read from
      the image; other kinds (offset/computed) are kept as XenonAnalyse wrote
      them.

  h3analysis.py functions <image.bin> <switch_tables.toml> <ppc_func_mapping.cpp> <recompile.log>
      Print `functions` entries for halo3.toml. Leaf functions without .pdata
      are sized by XenonRecomp's analyser, which stops at the first bctr; the
      case blocks then become fake functions and the recompiler reports
      "Switch case ... is trying to jump outside function". This follows the
      control flow (including the jump tables) to find the real extent.
"""
import bisect
import json
import re
import struct
import sys

def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def is_bctr(i):
    return i >> 26 == 19 and (i >> 1) & 0x3FF == 528 and not i & 1 and (i >> 21) & 0x14 == 0x14


class Image:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        layout = json.load(open(path + ".json"))
        self.base = layout["base"]
        sections = {s["name"]: (s["base"], s["base"] + s["size"]) for s in layout["sections"]}
        code = [(s["base"], s["base"] + s["size"]) for s in layout["sections"] if s["code"]]
        # All code sections as one range (Halo 3 has .text and BINK).
        self.text = (min(lo for lo, _ in code), max(hi for _, hi in code))
        self.pdata = {}
        for a in range(*sections[".pdata"], 8):
            begin, info = self.u32(a), self.u32(a + 4)
            if begin:
                self.pdata[begin] = ((info >> 8) & 0x3FFFFF) * 4
        self.bl_targets = set()
        for a in range(*self.text, 4):
            i = self.u32(a)
            if i >> 26 == 18 and i & 1:
                self.bl_targets.add((sext(i & 0x3FFFFFC, 26) + (0 if i & 2 else a)) & 0xFFFFFFFF)
        # Known entry points: .pdata functions and bl targets.
        self.entry_set = set(self.pdata) | self.bl_targets
        self.entries = sorted(self.entry_set)

    def u32(self, a):
        return struct.unpack_from(">I", self.data, a - self.base)[0]

    def enclosing(self, a):
        """Known entry points around a: (start, next start)."""
        i = bisect.bisect_right(self.entries, a)
        return self.entries[i - 1], self.entries[i]

    def read_inline_table(self, bctr):
        """(index register, labels) for an inline absolute table, or None."""
        j = self.u32(bctr - 4)  # mtctr rX
        if not (j >> 26 == 31 and (j >> 1) & 0x3FF == 467 and (j >> 11) & 0x3FF == 9 << 5):
            return None
        rx = (j >> 21) & 31
        j = self.u32(bctr - 8)  # lwzx rX, rA, rB
        if not (j >> 26 == 31 and (j >> 1) & 0x3FF == 23 and (j >> 21) & 31 == rx):
            return None
        operands = {(j >> 16) & 31, (j >> 11) & 31}

        table = index = None
        for b in range(bctr - 12, bctr - 64, -4):
            k = self.u32(b)
            op, rs, ra = k >> 26, (k >> 21) & 31, (k >> 16) & 31
            # slwi rA, rS, 2 == rlwinm rA, rS, 2, 0, 29
            if op == 21 and (k >> 11) & 31 == 2 and (k >> 6) & 31 == 0 and (k >> 1) & 31 == 29:
                if ra in operands and index is None:
                    index = rs
            # addi rD, rA, lo preceded by lis rA, hi
            elif op == 14 and rs in operands and table is None:
                k2 = self.u32(b - 4)
                if k2 >> 26 == 15 and (k2 >> 21) & 31 == ra and (k2 >> 16) & 31 == 0:
                    table = (((k2 & 0xFFFF) << 16) + sext(k & 0xFFFF, 16)) & 0xFFFFFFFF
        if table != bctr + 4 or index is None:
            return None
        table = bctr + 4

        # The table runs until a label or anything outside the function. An
        # entry may also be another function's start: a tail call (the
        # recompiler calls it and returns).
        lo, hi = self.enclosing(bctr)
        labels, p = [], table
        while p < hi:
            w = self.u32(p)
            if not (lo <= w < hi or w in self.entry_set) or table <= w <= p or p in labels:
                break
            labels.append(w)
            p += 4
        return (index, labels) if labels else None


def parse_tables(path):
    """[(kind, base, r, default, labels)] in file order."""
    tables, kind = [], None
    for line in open(path):
        if m := re.match(r"# ---- (.*) ----", line):
            kind = m.group(1)
        elif m := re.match(r"base = 0x([0-9A-F]+)", line):
            tables.append([kind, int(m.group(1), 16), None, 0, []])
        elif m := re.match(r"r = (\d+)", line):
            tables[-1][2] = int(m.group(1))
        elif m := re.match(r"default = 0x([0-9A-F]+)", line):
            tables[-1][3] = int(m.group(1), 16)
        elif m := re.match(r"\s+0x([0-9A-F]+),", line):
            tables[-1][4].append(int(m.group(1), 16))
    return tables


def bctr_after(img, a):
    while not is_bctr(img.u32(a)):
        a += 4
    return a


def cmd_tables(img_path, toml_path):
    img = Image(img_path)
    tables = parse_tables(toml_path)
    out, fixed, covered = [], 0, set()
    for kind, base, r, default, labels in tables:
        bctr = bctr_after(img, base)
        covered.add(bctr)
        inline = img.read_inline_table(bctr)
        if inline and inline != (r, labels):
            r, labels = inline
            default, fixed = 0, fixed + 1
        out.append((kind, base, r, default, labels))

    added = 0
    for a in range(*img.text, 4):
        if a not in covered and is_bctr(img.u32(a)):
            inline = img.read_inline_table(a)
            if inline:
                out.append(("ABSOLUTE JUMPTABLE (added by h3analysis)", a, inline[0], 0, inline[1]))
                added += 1

    with open(toml_path, "w") as f:
        f.write("# Generated by XenonAnalyse, corrected by tools/h3analysis.py\n")
        kind = None
        for k, base, r, default, labels in out:
            if k != kind:
                f.write(f"# ---- {k} ----\n")
                kind = k
            f.write(f"[[switch]]\nbase = 0x{base:X}\nr = {r}\ndefault = 0x{default:X}\nlabels = [\n")
            f.writelines(f"    0x{l:X},\n" for l in labels)
            f.write("]\n\n")
    print(f"{len(tables)} tables read, {fixed} corrected, {added} added", file=sys.stderr)


def extent(img, start, switches):
    """End of the function at start, following branches and jump tables."""
    seen, work = set(), [start]
    while work:
        a = work.pop()
        if a in seen or (a != start and a in img.pdata) or not img.text[0] <= a < img.text[1]:
            continue
        i = img.u32(a)
        if i == 0:
            continue
        seen.add(a)
        op = i >> 26
        if op == 18:  # b / bl
            target = (sext(i & 0x3FFFFFC, 26) + (0 if i & 2 else a)) & 0xFFFFFFFF
            if i & 1:
                work.append(a + 4)
            elif target >= start and (target == start or target not in img.entries):
                work.append(target)  # otherwise a tail call
        elif op == 16:  # bc
            target = (sext(i & 0xFFFC, 16) + (0 if i & 2 else a)) & 0xFFFFFFFF
            work.append(a + 4)
            if not i & 1 and (target == start or target not in img.entries):
                work.append(target)
        elif op == 19 and (i >> 1) & 0x3FF in (16, 528):  # bclr / bcctr
            if (i >> 1) & 0x3FF == 528 and a in switches:
                work.extend(switches[a])
            elif i & 1 or (i >> 21) & 0x14 != 0x14:
                work.append(a + 4)
        else:
            work.append(a + 4)
    return max(seen) + 4


def cmd_functions(img_path, toml_path, mapping_path, log_path):
    img = Image(img_path)
    switches = {bctr_after(img, base): labels for _, base, _, _, labels in parse_tables(toml_path)}
    starts = sorted(int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]+), sub_", open(mapping_path).read()))
    log = open(log_path).read()

    broken = set()
    for a in re.findall(r"Switch case at ([0-9A-F]+) is trying", log):
        i = bisect.bisect_right(starts, int(a, 16)) - 1
        fn = starts[i]
        if fn not in img.pdata:
            broken.add(fn)
        else:
            # A .pdata function can still break when the stub before it has
            # no .pdata and falls (or branches) into it: the analyser runs the
            # stub through the whole function, so the sweep skips the real
            # start. Blame the closest non-.pdata start before it.
            while i > 0 and starts[i] in img.pdata:
                i -= 1
            if starts[i] not in img.pdata:
                broken.add(starts[i])

    entries = sorted((fn, extent(img, fn, switches)) for fn in broken)
    # Fragments of a larger broken function are covered by the outer entry.
    result, end = [], 0
    for fn, fn_end in entries:
        if fn < end and fn not in img.entries:
            continue
        result.append((fn, fn_end - fn))
        end = max(end, fn_end)
    for fn, size in result:
        print(f"    {{ address = 0x{fn:08X}, size = 0x{size:X} }},")
    print(f"{len(result)} functions ({len(broken)} broken, rest were fragments)", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "tables":
        cmd_tables(*sys.argv[2:])
    elif len(sys.argv) == 6 and sys.argv[1] == "functions":
        cmd_functions(*sys.argv[2:])
    else:
        sys.exit(__doc__)
