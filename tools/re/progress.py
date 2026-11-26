#!/usr/bin/env python3
"""Native port progress (docs/ROADMAP.md 6.0): how many of the recompiled
game's functions, and how many bytes of its code, have a decompiled
replacement, overall and per subsystem.

Usage: python3 -I tools/re/progress.py [--markdown] [--list]
                                       [--subsystems <map.md>]
                                       [--json <file> [--verified <replay output>...]
                                        [--date YYYY-MM-DD] [--native-dir <dir>]]

  --markdown     a Markdown table (for the README) instead of plain text
  --list         also list every replaced function
  --subsystems   a file with the subsystem map: Markdown table rows
                 "| 0x<start>-0x<end> | <name> | ..." (default
                 tools/re/subsystems.md)
  --json         write the progress page's data (docs/_data/progress.json):
                 totals and subsystems, plus a history with one entry per
                 date (an existing file's history is kept; the same date is
                 replaced)
  --verified     tools/films/replay_calls.sh outputs: a replacement counts
                 as verified when some output compared at least one of its
                 calls and none reported it DIFFERS, HUNG or CRASHED
  --date         the history entry's date (default today)
  --native-dir   count the replacements in this copy of H3Native (an older
                 commit's, to fill in history) instead of the working tree's

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
import datetime
import json
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


def replacements(native=NATIVE):
    found = {}
    for path in sorted(native.rglob("*.cpp")):
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


REPLAY_ROW = re.compile(r"^\s+([A-Za-z_]\w*)\s+(\d+) calls, (\d+) differed\s+(.*)$", re.M)


def verified_names(paths):
    """Names compared on at least one call, never failing."""
    compared, failed = set(), set()
    for path in paths:
        for name, calls, differed, verdict in REPLAY_ROW.findall(Path(path).read_text(errors="replace")):
            if int(differed) or any(word in verdict for word in ("DIFFERS", "HUNG", "CRASHED")):
                failed.add(name)
            elif int(calls):
                compared.add(name)
    return compared - failed


def percent(part, whole):
    return f"{100.0 * part / whole:.2f}%" if whole else "-"


def main():
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("--markdown", action="store_true")
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--subsystems", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--verified", type=Path, nargs="+", default=[])
    parser.add_argument("--date", default=datetime.date.today().isoformat())
    parser.add_argument("--native-dir", type=Path)
    args = parser.parse_args()

    sizes = functions()
    code_end = max(sizes) + sizes[max(sizes)]
    native = args.native_dir.resolve() if args.native_dir else NATIVE
    replaced = replacements(native)
    unknown = sorted(a for a in replaced if a not in sizes)
    for address in unknown:
        name, path = replaced.pop(address)
        print(f"warning: {name} ({address:08X}, {path.relative_to(native.parent)}) is not a function start", file=sys.stderr)

    map_path = args.subsystems or ROOT / "tools" / "re" / "subsystems.md"
    rows = subsystems(map_path, code_end) if map_path.exists() else []

    names = verified_names(args.verified)
    verified = {a for a, (name, _) in replaced.items() if name in names}

    groups = {}
    verified_groups = {}
    if rows:
        for address, size in sizes.items():
            name = subsystem_of(address, rows)
            group = groups.setdefault(name, [0, 0, 0, 0])
            group[0] += 1
            group[1] += size
            if address in replaced:
                group[2] += 1
                group[3] += size
            if address in verified:
                verified_groups[name] = verified_groups.get(name, 0) + 1
    total = [len(sizes), sum(sizes.values()), len(replaced), sum(sizes[a] for a in replaced)]

    if args.json:
        write_json(args, groups, verified_groups, total, len(verified))
        return

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


def write_json(args, groups, verified_groups, total, verified_total):
    """The progress page's data; history entries keep their own numbers.
    verified is null where it wasn't measured (no --verified)."""
    measured = bool(args.verified)
    old = json.loads(args.json.read_text()) if args.json.exists() else {}
    history = [h for h in old.get("history", []) if h["date"] != args.date]
    history.append({
        "date": args.date,
        "replaced": total[2],
        "verified": verified_total if measured else None,
        "subsystems": {name: g[2] for name, g in groups.items()},
    })
    history.sort(key=lambda h: h["date"])
    latest = max(h["date"] for h in history)
    data = old if args.date != latest else {
        "updated": args.date,
        "total": {"functions": total[0], "bytes": total[1], "replaced": total[2],
                  "replaced_bytes": total[3], "verified": verified_total if measured else None},
        "subsystems": [
            {"name": name, "functions": g[0], "bytes": g[1], "replaced": g[2], "replaced_bytes": g[3],
             "verified": verified_groups.get(name, 0) if measured else None}
            for name, g in sorted(groups.items(), key=lambda item: (-item[1][2], -item[1][0], item[0]))
        ],
    }
    data["history"] = history
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(data, indent=1) + "\n")
    print(f"wrote {args.json}: {total[2]} replaced, {verified_total if measured else '-'} verified ({args.date})")


if __name__ == "__main__":
    main()
