#!/usr/bin/env python3
"""Builds the shipped shader manifest (shader_manifest.txt).

The game's shaders live in the map files as microcode (the vtsh / pixl /
glvs / glps tags' runtime shader objects; `h3tags <map> shaders <dir>` dumps
each one's region as mc_<group>_<address>.bin). The microcode the GPU runs is
that, patched by the game's D3D code when it binds a shader: texture fetch
constant indices (pixel shaders) and vertex fetch layouts (vertex shaders).

This matches every runtime shader recorded with H3_RECORD_SHADERS=<dir>
(<vs|ps>_<hash>.bin, big-endian dwords) to the region it came from and
writes, per shader, where its microcode is (map, region address, offset,
dword count) and its patches as XORs against the map's dwords: descriptor
bits only, no instructions. `H3Recomp --precompile-shaders` rebuilds each
shader from the user's own maps with it and compiles it into the shader
cache.

The last two dwords of a recorded shader are whatever followed it in memory
(not translated); they're taken from the map as they are.

Every entry is checked: the rebuilt microcode must translate to the same
GLSL as the recorded one (h3shader).

Usage: build_manifest.py <recorded dir> <h3tags dumps dir> <out manifest> [--h3shader <path>]
"""
import argparse
import collections
import glob
import os
import shutil
import subprocess
import sys
import tempfile

# Maps in the order the installer prefers them as a region's source (any map
# holding the region works; the first one listed wins).
MAP_ORDER = ['mainmenu', '005_intro', '010_jungle', '020_base', '030_outskirts', '040_voi', '050_floodvoi',
             '070_waste', '100_citadel', '110_hc', '120_halo', '130_epilogue', 'chill', 'construct', 'cyberdyne',
             'deadlock', 'guardian', 'isolation', 'riverworld', 'salvation', 'shrine', 'snowbound', 'zanzibar']
OFFSET_STEP = 64        # runtime microcode starts at a multiple of this into its region
MAX_OFFSET = 1024
JUNK_DWORDS = 2         # trailing dwords that aren't microcode


def fnv64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def load_regions(dumps):
    """content -> (map, address); one source per distinct region."""
    rank = {m: i for i, m in enumerate(MAP_ORDER)}
    regions = {}
    for path in glob.glob(os.path.join(dumps, '*', 'mc_*.bin')):
        map_name = os.path.basename(os.path.dirname(path))
        if map_name not in rank:
            continue
        address = int(os.path.basename(path).rsplit('_', 1)[1][:8], 16)
        data = open(path, 'rb').read()
        known = regions.get(data)
        if known is None or rank[map_name] < rank[known[0]]:
            regions[data] = (map_name, address)
    return regions


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('recorded')
    parser.add_argument('dumps')
    parser.add_argument('out')
    parser.add_argument('--h3shader', default='build/release/tools/h3shader/h3shader')
    args = parser.parse_args()

    regions = load_regions(args.dumps)
    index = collections.defaultdict(list)
    for data in regions:
        for offset in range(0, min(len(data), MAX_OFFSET), OFFSET_STEP):
            index[data[offset:offset + 12]].append((data, offset))
    print(f'{len(regions)} distinct microcode regions', file=sys.stderr)

    entries = []
    unmatched = 0
    work = tempfile.mkdtemp(prefix='shader_manifest_')
    try:
        original_dir = os.path.join(work, 'original')
        rebuilt_dir = os.path.join(work, 'rebuilt')
        os.makedirs(original_dir)
        os.makedirs(rebuilt_dir)
        for path in sorted(glob.glob(os.path.join(args.recorded, '*.bin'))):
            name = os.path.basename(path)
            stage = name[:2]
            code = open(path, 'rb').read()
            dwords = len(code) // 4
            if dwords <= JUNK_DWORDS:
                continue
            body = (dwords - JUNK_DWORDS) * 4
            best = None
            for data, offset in index.get(code[:12], []):
                if offset + len(code) > len(data):
                    continue
                patches = []
                for i in range(0, body, 4):
                    a = int.from_bytes(code[i:i + 4], 'big')
                    b = int.from_bytes(data[offset + i:offset + i + 4], 'big')
                    if a != b:
                        patches.append((i // 4, a ^ b))
                if best is None or len(patches) < len(best[2]):
                    best = (data, offset, patches)
            if best is None or len(best[2]) > dwords // 4:
                unmatched += 1
                continue
            data, offset, patches = best
            rebuilt = bytearray(data[offset:offset + len(code)])
            for i, xor in patches:
                value = int.from_bytes(rebuilt[i * 4:i * 4 + 4], 'big') ^ xor
                rebuilt[i * 4:i * 4 + 4] = value.to_bytes(4, 'big')
            open(os.path.join(original_dir, name), 'wb').write(code)
            open(os.path.join(rebuilt_dir, name), 'wb').write(bytes(rebuilt))
            map_name, address = regions[data]
            entries.append((name, stage, map_name, address, offset, dwords, fnv64(bytes(rebuilt[:body])), patches))

        # Same GLSL from the rebuilt microcode as from the recorded one.
        for directory in (original_dir, rebuilt_dir):
            files = sorted(glob.glob(os.path.join(directory, '*.bin')))
            for start in range(0, len(files), 500):
                subprocess.run([args.h3shader, directory] + files[start:start + 500], check=True,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        kept = []
        mismatched = 0
        for entry in entries:
            stem = entry[0][:-4]
            suffix = '.vert' if entry[1] == 'vs' else '.frag'
            a = os.path.join(original_dir, stem + suffix)
            b = os.path.join(rebuilt_dir, stem + suffix)
            if os.path.exists(a) and os.path.exists(b) and open(a).read() == open(b).read():
                kept.append(entry)
            else:
                mismatched += 1
    finally:
        shutil.rmtree(work, ignore_errors=True)

    kept.sort(key=lambda e: (e[1], e[2], e[3], e[4], e[5], e[6]))
    with open(args.out, 'w') as out:
        out.write('# Halo 3 recomp shader manifest (tools/shaders/build_manifest.py): per shader,\n')
        out.write('# stage map region-address offset dwords fnv64(rebuilt body) [dword:xor ...]\n')
        out.write('V 1\n')
        seen = set()
        for _, stage, map_name, address, offset, dwords, digest, patches in kept:
            line = f'{stage} {map_name} {address:08X} {offset} {dwords} {digest:016x}'
            if patches:
                line += ' ' + ' '.join(f'{i}:{x:08x}' for i, x in patches)
            if line not in seen:
                seen.add(line)
                out.write(line + '\n')
    print(f'{len(seen)} shaders in the manifest; {unmatched} not found in the maps, {mismatched} rebuilt differently',
          file=sys.stderr)


if __name__ == '__main__':
    main()
