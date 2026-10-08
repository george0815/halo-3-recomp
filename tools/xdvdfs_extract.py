"""Minimal XDVDFS (XGD2) extractor. usage: xdvdfs_extract.py ISO OUTDIR [--list]"""
import os, struct, sys

SECTOR = 2048
PARTITIONS = (0xFD90000, 0x18300000, 0x2080000, 0)  # XGD2, XGD1, XGD3, plain xiso

def find_partition(f):
    for off in PARTITIONS:
        f.seek(off + 32 * SECTOR)
        if f.read(20) == b"MICROSOFT*XBOX*MEDIA":
            return off
    sys.exit("no XDVDFS volume found")

def walk(f, base, sector, size, path, out):
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack, seen = [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = data[off + 14:off + 14 + nlen].decode("latin-1")
        if left: stack.append(left * 4)
        if right: stack.append(right * 4)
        out.append((path + "/" + name, start, length, bool(attr & 0x10)))
        if attr & 0x10 and length:
            walk(f, base, start, length, path + "/" + name, out)

def main():
    iso, outdir = sys.argv[1], sys.argv[2]
    listonly = "--list" in sys.argv
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = []
        walk(f, base, root_sector, root_size, "", entries)
        for p, start, length, isdir in sorted(entries):
            if listonly:
                print(f"{'D' if isdir else 'F'} {length:>12} {p}")
                continue
            dst = os.path.join(outdir, p.lstrip("/"))
            if isdir:
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            f.seek(base + start * SECTOR)
            with open(dst, "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 1 << 24))
                    o.write(chunk); left -= len(chunk)
        print(f"{len(entries)} entries", file=sys.stderr)

main()
