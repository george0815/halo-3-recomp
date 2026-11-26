#!/usr/bin/env python3
"""Native port progress (docs/ROADMAP.md 6.0): how many of the recompiled
game's functions, and how many bytes of its code, have a decompiled
replacement, overall and per subsystem.

Usage: python3 -I tools/re/progress.py [--markdown] [--list]
                                       [--subsystems <README.md>]

  --markdown     a Markdown table (for the README) instead of plain text
  --list         also list every replaced function
  --subsystems   a file with the subsystem map: Markdown table rows
                 "| 0x<start>-0x<end> | <name> | ..." (default
                 private/re/README.md when it exists; without one, only
                 the totals are printed)

Sources, all in the repository:
  - functions: H3RecompLib/ppc/ppc_func_mapping.cpp (the recompiler's
    function list); a function's size is the distance to the next one
    (alignment padding included), the last one ends at PPC_CODE_BASE +
    PPC_CODE_SIZE (ppc_config.h)
  - replacements: H3Native/**/*.cpp, H3_REPLACE_INT/FLOAT/STORES(address,
    name, ...), `Registration h3_registration_<address>(..., "name")` and
    strong PPC_FUNC(sub_<address>) definitions (decompiled in place).
    H3Recomp/hooks are fixes to the recompiled game, not decompilation,
    and don't count.
"""

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PPC = ROOT / "H3RecompLib" / "ppc"
NATIVE = ROOT / "H3Native"

REPLACE = re.compile(r"H3_REPLACE_(?:INT|FLOAT|STORES)\(\s*(?:0x)?([0-9A-Fa-f]{8})\s*,\s*([A-Za-z_]\w*)")
REGISTRATION = re.compile(r"Registration\s+h3_registration_\w*\(\s*0x([0-9A-Fa-f]{8})\s*,\s*\"([^\"]*)\"")
PPC_FUNC = re.compile(r"^\s*PPC_FUNC\(\s*sub_([0-9A-Fa-f]{8})\s*\)", re.M)
SUBSYSTEM_ROW = re.compile(r"^\|\s*0x([0-9A-Fa-f]{8})\s*-\s*(?:0x([0-9A-Fa-f]{8}))?\s*\|\s*([^|]+?)\s*\|", re.M)


def config_value(config, name):
    match = re.search(name + r"\s+(0x[0-9A-Fa-f]+)", config)
    if not match:
        sys.exit(f"{name} not found in ppc_config.h")
    return int(match.group(1), 16)


def functions():
    config = (PPC / "ppc_config.h").read_text()
    base = config_value(config, "PPC_CODE_BASE")
    size = config_value(config, "PPC_CODE_SIZE")
    starts = sorted({int(a, 16) for a in re.findall(r"\{\s*0x([0-9A-Fa-f]+)\s*,\s*sub_", (PPC / "ppc_func_mapping.cpp").read_text())})
    ends = starts[1:] + [base + size]
    return {s: e - s for s, e in zip(starts, ends)}


def replacements():
    found = {}
    for path in sorted(NATIVE.rglob("*.cpp")):
        if "generated" in path.parts:
            continue
        text = path.read_text(errors="replace")
        for pattern in (REPLACE, REGISTRATION):
            for address, name in pattern.findall(text):
                found.setdefault(int(address, 16), (name, path))
        for address in PPC_FUNC.findall(text):
            found.setdefault(int(address, 16), ("sub_" + address.upper(), path))
    return found


def subsystems(path, code_end):
    """[(start, end, name)] in file order; the first matching range wins
    (the coarse map's ranges overlap in places)."""
    rows = []
    for start, end, name in SUBSYSTEM_ROW.findall(path.read_text()):
        rows.append((int(start, 16), int(end, 16) if end else code_end, name))
    return rows


def subsystem_of(address, rows):
    for start, end, name in rows:
        if start <= address < end:
            return name
    return "other"


def percent(part, whole):
    return f"{100.0 * part / whole:.2f}%" if whole else "-"


def main():
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("--markdown", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--subsystems", type=Path)
    args = parser.parse_args()

    sizes = functions()
    code_end = max(sizes) + sizes[max(sizes)]
    replaced = replacements()
    unknown = sorted(a for a in replaced if a not in sizes)
    for address in unknown:
        name, path = replaced.pop(address)
        print(f"warning: {name} ({address:08X}, {path.relative_to(ROOT)}) is not a function start", file=sys.stderr)

    map_path = args.subsystems or ROOT / "private" / "re" / "README.md"
    rows = subsystems(map_path, code_end) if map_path.exists() else []

    groups = {}
    if rows:
        for address, size in sizes.items():
            group = groups.setdefault(subsystem_of(address, rows), [0, 0, 0, 0])
            group[0] += 1
            group[1] += size
            if address in replaced:
                group[2] += 1
                group[3] += size
    total = [len(sizes), sum(sizes.values()), len(replaced), sum(sizes[a] for a in replaced)]

    header = ["Subsystem", "Functions", "Replaced", "%", "Code bytes", "Replaced bytes", "%"]
    table = []
    for name, (count, size, done, done_size) in sorted(groups.items(), key=lambda item: (-item[1][2], -item[1][3], item[0])):
        table.append([name, f"{count:,}", f"{done:,}", percent(done, count), f"{size:,}", f"{done_size:,}", percent(done_size, size)])
    table.append(["Total", f"{total[0]:,}", f"{total[2]:,}", percent(total[2], total[0]),
                  f"{total[1]:,}", f"{total[3]:,}", percent(total[3], total[1])])

    if args.markdown:
        print("| " + " | ".join(header) + " |")
        print("|---|" + "---:|" * (len(header) - 1))
        for row in table:
            if row[0] == "Total":
                row = [f"**{cell}**" for cell in row]
            print("| " + " | ".join(row) + " |")
    else:
        widths = [max(len(str(row[i])) for row in [header] + table) for i in range(len(header))]
        for index, row in enumerate([header] + table):
            if index == len(table):
                print("  ".join("-" * w for w in widths))
            print("  ".join(cell.ljust(widths[0]) if i == 0 else cell.rjust(widths[i]) for i, cell in enumerate(row)))

    if args.list:
        print()
        for address in sorted(replaced):
            name, path = replaced[address]
            where = subsystem_of(address, rows) if rows else ""
            print(f"{address:08X}  {sizes[address]:6,}  {name:48} {path.relative_to(ROOT)}  {where}".rstrip())


if __name__ == "__main__":
    main()
