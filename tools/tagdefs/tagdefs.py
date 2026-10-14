#!/usr/bin/env python3
"""Halo 3 tag layouts from H3EK's own tag definitions (docs/ROADMAP.md 6.3).

    tagdefs.py extract <work dir> <map> <out.json>
        Reads the tag definitions out of tool.exe's initialized memory
        (dump_tool_memory.sh writes it to <work dir>) for every tag group in
        <map>, with each field's size in an Xbox 360 cache file.
    tagdefs.py validate <defs.json> <map> [<out.json>]
        Walks every tag in <map> with those layouts, checks block counts and
        addresses, tag references and string ids, and records per struct how
        many checks failed (written back into <out.json>, default <defs.json>).
    tagdefs.py compare <defs.json> <plugins dir> [<out.json>]
        Compares the layouts with Assembly's Halo 3 (360) plugins
        (src/Assembly/Plugins/Halo3/*.xml from github.com/XboxChaos/Assembly,
        community-made 360 layouts): pairs blocks by offset (or by order),
        compares element sizes, and records per struct "plugin": match or
        the 360 size. The plugins are reference data only; nothing from them
        ends up in the generated header except that verdict.
    tagdefs.py header <defs.json> <out.h>
        Writes C++ structs in the cache file's layout (big-endian be<T>
        fields; tag_block<T> etc. from H3Native/tags/tag_types.h).

H3EK is the MCC build. Its layouts are the 360 cache file's for nearly every
struct (sizes computed with 360 field sizes equal H3EK's stored sizes for
1,359 of 1,364 structs; the rest are filled in lazily and resolved through
their group's parent), but a few groups differ in cache files: sounds are a
32-byte cache-only struct, compiled shaders are platform specific, and some
resource structs changed for MCC. `validate` finds them; the header marks
them unverified.

How the definitions are laid out in tool.exe (x64):
  group      (name, flags | tag << 32, parent tag | version << 32, ?, block)
  block      (name, display name, maximum count, maximum count name, struct)
  struct     (name, display name, source file, line, GUID[16], fields,
              size, "sizeof(...)" text, ...)
  field      32 bytes: (type, name, definition, extra); a list ends with a
             terminator. Definitions: block -> block, struct -> struct,
             array -> (name, count, count name, struct), tag reference ->
             (flags, group tag, group tag list), enum and flags -> (name,
             count, strings), pad/skip -> byte count.
A group's tag data is its ancestors' structs, base first (biped = object +
unit + biped).
"""
import collections
import json
import re
import struct
import sys

# Field types, in H3EK's numbering (its field type name table).
TYPES = ['string', 'long string', 'string id', 'old string id', 'char integer', 'short integer', 'long integer',
    'int64 integer', 'angle', 'tag', 'char enum', 'short enum', 'long enum', 'long flags', 'word flags', 'byte flags',
    'point 2d', 'rectangle 2d', 'rgb color', 'argb color', 'real', 'real slider', 'real fraction', 'real point 2d',
    'real point 3d', 'real vector 2d', 'real vector 3d', 'real quaternion', 'real euler angles 2d',
    'real euler angles 3d', 'real plane 2d', 'real plane 3d', 'real rgb color', 'real argb color', 'real hsv color',
    'real ahsv color', 'short integer bounds', 'angle bounds', 'real bounds', 'fraction bounds', 'tag reference',
    'block', 'long block flags', 'word block flags', 'byte block flags', 'char block index',
    'custom char block index', 'short block index', 'custom short block index', 'long block index',
    'custom long block index', 'data', 'vertex buffer', 'pad', 'useless pad', 'skip', 'explanation', 'custom',
    'struct', 'array', 'pageable resource', 'api interop', 'terminator']
TERMINATOR = TYPES.index('terminator')

# Size in an Xbox 360 cache file. pad/skip carry their own; struct and array
# are their contents'; useless pad, explanation and custom take no space.
SIZES_360 = {
    'string': 32, 'long string': 256, 'string id': 4, 'old string id': 4, 'char integer': 1, 'short integer': 2,
    'long integer': 4, 'int64 integer': 8, 'angle': 4, 'tag': 4, 'char enum': 1, 'short enum': 2, 'long enum': 4,
    'long flags': 4, 'word flags': 2, 'byte flags': 1, 'point 2d': 4, 'rectangle 2d': 8, 'rgb color': 4,
    'argb color': 4, 'real': 4, 'real slider': 4, 'real fraction': 4, 'real point 2d': 8, 'real point 3d': 12,
    'real vector 2d': 8, 'real vector 3d': 12, 'real quaternion': 16, 'real euler angles 2d': 8,
    'real euler angles 3d': 12, 'real plane 2d': 12, 'real plane 3d': 16, 'real rgb color': 12,
    'real argb color': 16, 'real hsv color': 12, 'real ahsv color': 16, 'short integer bounds': 4,
    'angle bounds': 8, 'real bounds': 8, 'fraction bounds': 8, 'tag reference': 16, 'block': 12,
    'long block flags': 4, 'word block flags': 2, 'byte block flags': 1, 'char block index': 1,
    'custom char block index': 1, 'short block index': 2, 'custom short block index': 2, 'long block index': 4,
    'custom long block index': 4, 'data': 20, 'vertex buffer': 32, 'useless pad': 0, 'explanation': 0,
    'custom': 0, 'pageable resource': 8, 'api interop': 12,
}
NO_GROUP = 0xFFFFFFFF


def ancestors(defs, tag):
    """The group and its ancestors, base first."""
    out = []
    while tag:
        out.insert(0, tag)
        tag = defs['groups'][tag]['parent']
    return out


def tag_string(value):
    return value.to_bytes(4, 'big').decode('latin1')


# --- tool.exe image -------------------------------------------------------

class Image:
    """tool.exe as it is after static initialization: .data/.rdata from the
    gdb dumps, everything else from the file."""

    def __init__(self, work):
        d = open(f'{work}/tool.exe', 'rb').read()
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        count = struct.unpack_from('<H', d, pe + 6)[0]
        optional = struct.unpack_from('<H', d, pe + 20)[0]
        self.base = struct.unpack_from('<Q', d, pe + 24 + 24)[0]
        self.regions = []
        for i in range(count):
            o = pe + 24 + optional + i * 40
            name = d[o:o + 8].rstrip(b'\0').decode()
            size, address, raw_size, raw = struct.unpack_from('<4I', d, o + 8)
            if name == '.data':
                content = open(f'{work}/tool_data.bin', 'rb').read()
            elif name == '.rdata':
                content = open(f'{work}/tool_rdata.bin', 'rb').read()
            else:
                content = d[raw:raw + raw_size]
            self.regions.append((self.base + address, content))

    def locate(self, address):
        for start, content in self.regions:
            if start <= address < start + len(content):
                return content, address - start
        return None, None

    def q(self, address):
        content, o = self.locate(address) if address else (None, None)
        return struct.unpack_from('<Q', content, o)[0] if content is not None and o + 8 <= len(content) else None

    def u32(self, address):
        content, o = self.locate(address) if address else (None, None)
        return struct.unpack_from('<I', content, o)[0] if content is not None else None

    def string(self, address, limit=1024):
        content, o = self.locate(address) if address else (None, None)
        if content is None:
            return None
        end = content.find(b'\0', o, o + limit)
        if end < 0:
            return None
        try:
            return content[o:end].decode('ascii')
        except UnicodeDecodeError:
            return None


# --- definitions ----------------------------------------------------------

class Definitions:
    """Walks the definitions in an Image and produces the JSON layout."""

    def __init__(self, image):
        self.image = image
        self.lazy = {}  # struct definition -> the one its fields come from

    def fields(self, struct_def):
        out, entry = [], self.image.q(self.resolve(struct_def) + 0x30)
        while entry:
            kind = self.image.u32(entry)
            if kind is None or kind >= len(TYPES) or kind == TERMINATOR:
                break
            out.append((TYPES[kind], self.image.string(self.image.q(entry + 8)) or '', self.image.q(entry + 16),
                self.image.q(entry + 24)))
            entry += 32
        return out

    def resolve(self, struct_def):
        return self.lazy.get(struct_def, struct_def)

    def find_groups(self, tags):
        """{group tag: group definition} for the given tags."""
        found = {}
        for start, content in self.image.regions:
            for tag in tags:
                if tag in found:
                    continue
                pattern = struct.pack('<I', tag)
                i = content.find(pattern)
                while i != -1:
                    o = i - 12  # the tag is the high half of the second word
                    if o >= 0 and o % 8 == 0:
                        q = struct.unpack_from('<5Q', content, o)
                        block = q[4]
                        if q[1] >> 32 == tag and self.image.string(q[0]) and block and \
                                self.image.string(self.image.q(block) or 0):
                            struct_def = self.image.q(block + 0x20)
                            if struct_def and self.image.q(struct_def + 0x30):
                                found[tag] = start + o
                                break
                    i = content.find(pattern, i + 1)
        return found

    def group_struct(self, group_def):
        return self.image.q(self.image.q(group_def + 0x20) + 0x20)

    def resolve_lazy(self, groups):
        """Shader groups' structs get their fields later than static
        initialization: they're their parent's (shader_decal: render_method)."""
        structs = {tag: self.group_struct(g) for tag, g in groups.items()}
        for tag, g in groups.items():
            if not self.fields(structs[tag]):
                parent = self.image.q(g + 0x10) & 0xFFFFFFFF
                if parent in structs:
                    self.lazy[structs[tag]] = structs[parent]

    def string_list(self, definition):
        count, strings = self.image.q(definition + 8), self.image.q(definition + 16)
        if not count or count > 64 or not strings:
            return []
        return [self.image.string(self.image.q(strings + 8 * i)) or '' for i in range(count)]

    def export(self, groups):
        """The JSON layout: groups, and every struct they reach."""
        ids, structs, order = {}, {}, []

        def struct_id(struct_def):
            struct_def = self.resolve(struct_def)
            if struct_def in ids:
                return ids[struct_def]
            name = self.image.string(self.image.q(struct_def)) or f'struct_{struct_def:x}'
            unique, n = name, 2
            while unique in structs:
                unique, n = f'{name}_{n}', n + 1
            ids[struct_def] = unique
            structs[unique] = None  # reserve
            order.append((unique, struct_def))
            return unique

        for g in groups.values():
            struct_id(self.group_struct(g))
        i = 0
        while i < len(order):
            sid, struct_def = order[i]
            i += 1
            fields = []
            for kind, name, definition, extra in self.fields(struct_def):
                field = {'type': kind, 'name': name}  # offsets: layout()
                if kind in ('pad', 'skip'):
                    field['size'] = definition
                elif kind == 'struct':
                    field['struct'] = struct_id(definition)
                elif kind == 'array':
                    field['struct'] = struct_id(self.image.q(definition + 0x18))
                    field['count'] = self.image.q(definition + 8)
                elif kind == 'block':
                    field['struct'] = struct_id(self.image.q(definition + 0x20))
                    field['maximum'] = self.image.q(definition + 0x10)
                    field['size'] = SIZES_360[kind]
                elif kind == 'tag reference':
                    group = (self.image.q(definition) or 0) >> 32
                    field['group'] = tag_string(group) if group not in (0, NO_GROUP) else None
                    field['size'] = SIZES_360[kind]
                elif kind in ('char enum', 'short enum', 'long enum', 'long flags', 'word flags', 'byte flags'):
                    field['options'] = self.string_list(definition) if definition else []
                    field['size'] = SIZES_360[kind]
                else:
                    field['size'] = SIZES_360[kind]
                fields.append(field)
            structs[sid] = {'name': self.image.string(self.image.q(struct_def)),
                'h3ek_size': self.image.q(struct_def + 0x38), 'fields': fields}
        out = {'groups': {}, 'structs': structs}
        for tag, g in sorted(groups.items()):
            parent = self.image.q(g + 0x10) & 0xFFFFFFFF
            own = self.group_struct(g)
            if own in self.lazy:
                # A shader group adds no fields to its parent's (render_method):
                # in the group chain it's empty; embedded, it stands for them.
                sid = (self.image.string(self.image.q(own)) or 'lazy') + '_own'
                structs[sid] = {'name': sid, 'h3ek_size': 0, 'fields': []}
            else:
                sid = ids[own]
            out['groups'][tag_string(tag)] = {'name': self.image.string(self.image.q(g)),
                'parent': tag_string(parent) if parent != NO_GROUP else None, 'struct': sid}
        layout(out)
        return out


def layout(defs):
    """Fills in each field's offset and each struct's size (360 sizes)."""
    structs = defs['structs']
    done = {}

    def size(sid, depth=0):
        if sid in done:
            return done[sid]
        if depth > 64:
            raise ValueError(f'struct {sid} contains itself')
        offset = 0
        for f in structs[sid]['fields']:
            f['offset'] = offset
            if f['type'] == 'struct':
                f['size'] = size(f['struct'], depth + 1)
            elif f['type'] == 'array':
                f['size'] = f['count'] * size(f['struct'], depth + 1)
            offset += f['size']
        structs[sid]['size'] = done[sid] = offset
        return offset

    for sid in structs:
        size(sid)


# --- cache files ----------------------------------------------------------

class Map:
    """Just enough of a cache file for validation (H3Native/cache has the
    real reader)."""

    def __init__(self, path):
        f = open(path, 'rb')
        h = f.read(0x3000)
        masks = struct.unpack('>4I', h[0x46C:0x47C])
        address, size = struct.unpack('>2I', h[0x47C + 16:0x47C + 24])
        f.seek(address + masks[2] & 0xFFFFFFFF)
        self.data, self.base = f.read(size), struct.unpack('>I', h[0x2E8:0x2EC])[0]
        self.string_ids = struct.unpack('>I', h[0x158:0x15C])[0]
        index = struct.unpack('>I', h[0x10:0x14])[0]
        if not index:
            raise ValueError(f'{path} has no tag index (a shared map?)')
        group_count, group_table, tag_count, tag_table = struct.unpack('>4I', self.read(index, 16))
        self.groups = [struct.unpack('>4I', self.read(group_table + 16 * i, 16)) for i in range(group_count)]
        self.tags = [struct.unpack('>HHI', self.read(tag_table + 8 * i, 8)) for i in range(tag_count)]

    def contains(self, address, size=1):
        return self.base <= address and address - self.base + size <= len(self.data)

    def read(self, address, size):
        if not self.contains(address, size):
            raise ValueError(f'{address:08x} is outside the tag section')
        o = address - self.base
        return self.data[o:o + size]

    def datum_ok(self, datum, group):
        if datum == NO_GROUP:
            return True
        slot = datum & 0xFFFF
        if slot >= len(self.tags):
            return False
        group_index, salt, _ = self.tags[slot]
        if salt != datum >> 16 or group_index >= len(self.groups):
            return False
        return group == NO_GROUP or group in self.groups[group_index][:3]


class Validator:
    """Checks a map's tags against the layouts; counts failures per struct."""
    MAX_ELEMENTS = 256  # per block, to bound the walk

    def __init__(self, defs, cache):
        self.defs, self.cache = defs, cache
        self.failures = collections.Counter()
        self.checks = collections.Counter()
        self.strong = collections.Counter()  # passes that zeros or garbage rarely produce
        self.examples = {}

    def fail(self, sid, what):
        self.failures[sid] += 1
        self.examples.setdefault(sid, what)

    def walk(self, sid, address, depth=0):
        if depth > 16:
            return
        struct_def = self.defs['structs'][sid]
        if not self.cache.contains(address, struct_def['size']):
            self.fail(sid, f'struct at {address:08x} runs past the tag section')
            return
        for f in struct_def['fields']:
            a, kind = address + f['offset'], f['type']
            if kind == 'struct':
                self.walk(f['struct'], a, depth + 1)
            elif kind == 'array':
                inner = self.defs['structs'][f['struct']]['size']
                for i in range(f['count']):
                    self.walk(f['struct'], a + i * inner, depth + 1)
            elif kind == 'block':
                count, elements, _ = struct.unpack('>iII', self.cache.read(a, 12))
                self.checks[sid] += 1
                inner = self.defs['structs'][f['struct']]['size']
                if count < 0 or count > max(f['maximum'], 1) * 4 or (count and not self.cache.contains(elements, count * inner)):
                    self.fail(sid, f'{f["name"]}: {count} elements at {elements:08x}')
                    continue
                if count and count <= max(f['maximum'], 1):
                    self.strong[sid] += 1
                for i in range(min(count, self.MAX_ELEMENTS)):
                    self.walk(f['struct'], elements + i * inner, depth + 1)
            elif kind == 'tag reference':
                group, _, _, datum = struct.unpack('>4I', self.cache.read(a, 16))
                self.checks[sid] += 1
                declared = f.get('group')
                if not self.cache.datum_ok(datum, group):
                    self.fail(sid, f'{f["name"]}: group {group:08x} datum {datum:08x}')
                elif declared and group != NO_GROUP and tag_string(group) != declared and datum != NO_GROUP:
                    pass  # a group list allows others; not counted either way
                elif datum != NO_GROUP:
                    self.strong[sid] += 1
            elif kind in ('string id', 'old string id'):
                value = struct.unpack('>I', self.cache.read(a, 4))[0]
                self.checks[sid] += 1
                if value != NO_GROUP and (value & 0xFFFF) >= self.cache.string_ids:
                    self.fail(sid, f'{f["name"]}: string id {value:08x}')
                elif value and value != NO_GROUP and value >> 24 == 0:
                    self.strong[sid] += 1

    def run(self, only_groups=None):
        """Validates every tag (or only those whose group is in only_groups)."""
        structs = {tag: g['struct'] for tag, g in self.defs['groups'].items()}
        tags_by_group = collections.Counter()
        for group_index, salt, address in self.cache.tags:
            if group_index == 0xFFFF or not address:
                continue
            tag, parent, grandparent, _ = self.cache.groups[group_index]
            chain = [tag_string(t) for t in (grandparent, parent, tag) if t != NO_GROUP]
            if any(t not in structs for t in chain):
                continue
            if only_groups is not None and chain[-1] not in only_groups:
                continue
            tags_by_group[chain[-1]] += 1
            for t in chain:
                sid = structs[t]
                self.walk(sid, address)
                address += self.defs['structs'][sid]['size']
        return tags_by_group


# --- structure ----------------------------------------------------------

def reachable(defs):
    """{struct id: set of group tags whose tag data contains it}."""
    children = {sid: {f['struct'] for f in s['fields'] if 'struct' in f} for sid, s in defs['structs'].items()}
    out = collections.defaultdict(set)
    for tag, g in defs['groups'].items():
        chain, t = [], tag
        while t:  # a tag's data holds its ancestors' structs too
            chain.append(defs['groups'][t]['struct'] if t in defs['groups'] else None)
            t = defs['groups'][t]['parent'] if t in defs['groups'] else None
        todo, seen = [c for c in chain if c], set()
        while todo:
            sid = todo.pop()
            if sid in seen:
                continue
            seen.add(sid)
            out[sid].add(tag)
            todo.extend(children[sid])
    return out


# --- Assembly plugins -----------------------------------------------------

def plugin_tree(path):
    """(size, [(offset, element size, name, children)] of the root's blocks)."""
    import xml.etree.ElementTree as ET
    root = ET.parse(path).getroot()

    def blocks(node):
        out = []
        for child in node:
            if child.tag.lower() == 'tagblock' and child.get('offset') and child.get('elementSize'):
                out.append((int(child.get('offset'), 0), int(child.get('elementSize'), 0), child.get('name') or '',
                    blocks(child)))
        return out
    return int(root.get('baseSize', '0'), 0), blocks(root)


def flat_blocks(defs, sids, base=0):
    """Our blocks at absolute offsets in a struct chain (embedded structs and
    arrays flattened, as the plugins have them): [(offset, struct id, name)]."""
    out = []
    for sid in sids:
        def walk(sid, at):
            for f in defs['structs'][sid]['fields']:
                if f['type'] == 'block':
                    out.append((at + f['offset'], f['struct'], f['name']))
                elif f['type'] == 'struct':
                    walk(f['struct'], at + f['offset'])
                elif f['type'] == 'array':
                    inner = defs['structs'][f['struct']]['size']
                    for i in range(f['count']):
                        walk(f['struct'], at + f['offset'] + i * inner)
        walk(sid, base)
        base += defs['structs'][sid]['size']
    return out


def compare(defs, plugins):
    """Annotates structs with what the plugins say; returns a summary."""
    import os
    verdicts = {}  # struct id -> (360 size, plugin path)

    def match(sids, our_size, size, blocks, path):
        if len(sids) == 1:
            verdicts.setdefault(sids[0], (size, path))
        ours = flat_blocks(defs, sids)
        by_offset = {b[0]: b for b in blocks}
        paired = []
        if our_size == size:
            paired = [(o, by_offset[o[0]]) for o in ours if o[0] in by_offset]
        elif len(ours) == len(blocks):  # sizes differ: fall back on order
            paired = list(zip(ours, blocks))
        for (offset, sid, name), (_, element_size, plugin_name, children) in paired:
            match([sid], defs['structs'][sid]['size'], element_size, children, f'{path}/{plugin_name or name}')

    for tag, g in defs['groups'].items():
        path = os.path.join(plugins, tag.replace('*', '_').replace('<', '_').replace('>', '_').rstrip() + '.xml')
        if not os.path.exists(path):
            continue
        size, blocks = plugin_tree(path)
        chain, t = [], tag
        while t:
            chain.insert(0, defs['groups'][t]['struct'])
            t = defs['groups'][t]['parent']
        ours = sum(defs['structs'][sid]['size'] for sid in chain)
        g['plugin_size'] = size
        match(chain, ours, size, blocks, tag)
    for sid, (size, path) in verdicts.items():
        defs['structs'][sid]['plugin'] = {'size': size, 'path': path}
    same = sum(1 for sid, (size, _) in verdicts.items() if size == defs['structs'][sid]['size'])
    return same, len(verdicts), len(defs['structs'])


# --- C++ ------------------------------------------------------------------

CPP_TYPES = {
    'string': ('char', 32), 'long string': ('char', 256), 'string id': ('string_id', 1),
    'old string id': ('string_id', 1), 'char integer': ('int8_t', 1), 'short integer': ('be<int16_t>', 1),
    'long integer': ('be<int32_t>', 1), 'int64 integer': ('be<int64_t>', 1), 'angle': ('be<float>', 1),
    'tag': ('be<uint32_t>', 1), 'char enum': ('int8_t', 1), 'short enum': ('be<int16_t>', 1),
    'long enum': ('be<int32_t>', 1), 'long flags': ('be<uint32_t>', 1), 'word flags': ('be<uint16_t>', 1),
    'byte flags': ('uint8_t', 1), 'point 2d': ('be<int16_t>', 2), 'rectangle 2d': ('be<int16_t>', 4),
    'rgb color': ('be<uint32_t>', 1), 'argb color': ('be<uint32_t>', 1), 'real': ('be<float>', 1),
    'real slider': ('be<float>', 1), 'real fraction': ('be<float>', 1), 'real point 2d': ('be<float>', 2),
    'real point 3d': ('be<float>', 3), 'real vector 2d': ('be<float>', 2), 'real vector 3d': ('be<float>', 3),
    'real quaternion': ('be<float>', 4), 'real euler angles 2d': ('be<float>', 2),
    'real euler angles 3d': ('be<float>', 3), 'real plane 2d': ('be<float>', 3), 'real plane 3d': ('be<float>', 4),
    'real rgb color': ('be<float>', 3), 'real argb color': ('be<float>', 4), 'real hsv color': ('be<float>', 3),
    'real ahsv color': ('be<float>', 4), 'short integer bounds': ('be<int16_t>', 2), 'angle bounds': ('be<float>', 2),
    'real bounds': ('be<float>', 2), 'fraction bounds': ('be<float>', 2), 'tag reference': ('tag_reference', 1),
    'long block flags': ('be<uint32_t>', 1), 'word block flags': ('be<uint16_t>', 1),
    'byte block flags': ('uint8_t', 1), 'char block index': ('int8_t', 1), 'custom char block index': ('int8_t', 1),
    'short block index': ('be<int16_t>', 1), 'custom short block index': ('be<int16_t>', 1),
    'long block index': ('be<int32_t>', 1), 'custom long block index': ('be<int32_t>', 1), 'data': ('tag_data', 1),
    'vertex buffer': ('uint8_t', 32), 'pageable resource': ('pageable_resource', 1), 'api interop': ('uint8_t', 12),
}
CPP_KEYWORDS = {'default', 'class', 'struct', 'union', 'switch', 'case', 'new', 'delete', 'template', 'operator',
    'private', 'public', 'protected', 'char', 'int', 'float', 'double', 'long', 'short', 'signed', 'unsigned',
    'register', 'auto', 'const', 'static', 'volatile', 'goto', 'return', 'this', 'true', 'false', 'do', 'if',
    'else', 'for', 'while', 'break', 'continue', 'enum', 'typedef', 'export', 'explicit', 'friend', 'inline',
    'mutable', 'namespace', 'virtual', 'void', 'bool', 'catch', 'try', 'throw', 'using', 'and', 'or', 'not', 'xor'}


def identifier(text, fallback):
    """A C++ identifier from an H3EK field name: what precedes its markup
    (':' units, '#' description, trailing *!^ flags), snake_case."""
    text = re.split(r'[:#]', text)[0]
    text = re.sub(r'[^0-9A-Za-z]+', '_', text).strip('_').lower()
    if not text:
        return fallback
    if text[0].isdigit():
        text = '_' + text
    return text + '_' if text in CPP_KEYWORDS else text


def header(defs):
    structs = defs['structs']
    names = {sid: identifier(sid, 'struct') for sid in structs}
    # Embedded structs must be complete before use; blocks only need a name.
    order, state = [], {}

    def visit(sid):
        if state.get(sid) == 'done':
            return
        state[sid] = 'visiting'
        for f in structs[sid]['fields']:
            if f['type'] in ('struct', 'array') and state.get(f['struct']) != 'done':
                visit(f['struct'])
        state[sid] = 'done'
        order.append(sid)

    for sid in structs:
        visit(sid)

    out = ['// Generated by tools/tagdefs/tagdefs.py from H3EK tag definitions; do not edit.',
        '// Tag data in the Xbox 360 cache file\'s layout: big-endian fields, guest',
        '// addresses in blocks and data. See H3Native/tags/tag_types.h.', '',
        '#pragma once', '', '#include "tags/tag_types.h"', '',
        'namespace h3::native::tags::layout', '{',
        '#pragma pack(push, 1)']
    out += [f'    struct {names[sid]};' for sid in structs]
    for sid in order:
        s = structs[sid]
        out.append('')
        plugin, verdict = s.get('plugin'), s.get('validation')
        if plugin and plugin['size'] != s['size']:
            out.append(f'    // DIFFERS on 360: {plugin["size"]:#x} bytes there (Assembly: {plugin["path"]}); MCC changed it')
        elif verdict and verdict['failures']:
            out.append(f'    // UNVERIFIED: {verdict["failures"]} of {verdict["checks"]} checks fail in {verdict["map"]} '
                f'(e.g. {verdict["example"]})')
        elif plugin:
            out.append(f'    // 360 size confirmed (Assembly: {plugin["path"]})')
        out.append(f'    struct {names[sid]}  // {s["name"]}, {s["size"]:#x} bytes')
        out.append('    {')
        used = collections.Counter()
        for i, f in enumerate(s['fields']):
            kind = f['type']
            # Zero-size fields have no member (an empty C++ struct would
            # still take a byte).
            if kind in ('useless pad', 'explanation', 'custom') or f['size'] == 0:
                continue
            member = identifier(f['name'], f'field_{i}')
            used[member] += 1
            if used[member] > 1:
                member = f'{member}_{used[member]}'
            if kind in ('pad', 'skip'):
                line = f'uint8_t {member if f["name"] else f"pad_{i}"}[{f["size"]}];'
            elif kind == 'struct':
                line = f'{names[f["struct"]]} {member};'
            elif kind == 'array':
                line = f'{names[f["struct"]]} {member}[{f["count"]}];'
            elif kind == 'block':
                line = f'tag_block<{names[f["struct"]]}> {member};'
            else:
                cpp, count = CPP_TYPES[kind]
                line = f'{cpp} {member}{f"[{count}]" if count > 1 else ""};'
            out.append(f'        {line:<60} // {f["offset"]:#06x} {kind}' + (f': {f["name"]}' if f['name'] else ''))
        out.append('    };')
        if s['size']:  # an empty struct is 1 byte in C++, 0 in a cache file
            out.append(f'    static_assert(sizeof({names[sid]}) == {s["size"]:#x});')
    out += ['#pragma pack(pop)', '', '    // Group tag -> its own struct (a tag\'s data is its ancestors\' structs, base first).']
    for tag, g in sorted(defs['groups'].items()):
        parent = f'parent \'{g["parent"]}\'' if g['parent'] else 'no parent'
        out.append(f'    // \'{tag}\' {g["name"]}: {names[g["struct"]]} ({parent})')
    out += ['}', '']
    return '\n'.join(out)


# --- main -----------------------------------------------------------------

def main():
    if len(sys.argv) >= 5 and sys.argv[1] == 'extract':
        work, map_path, out = sys.argv[2:5]
        cache = Map(map_path)
        definitions = Definitions(Image(work))
        groups = definitions.find_groups([g[0] for g in cache.groups])
        missing = [tag_string(g[0]) for g in cache.groups if g[0] not in groups]
        definitions.resolve_lazy(groups)
        defs = definitions.export(groups)
        defs['missing_groups'] = missing
        mismatched = [(sid, s['size'], s['h3ek_size']) for sid, s in defs['structs'].items()
            if s['h3ek_size'] and s['size'] != s['h3ek_size']]
        json.dump(defs, open(out, 'w'), indent=1)
        print(f'{len(defs["groups"])} groups ({len(missing)} not found in tool.exe: {" ".join(missing)}), '
            f'{len(defs["structs"])} structs; {len(mismatched)} whose 360 size differs from H3EK\'s:')
        for sid, mine, theirs in mismatched:
            print(f'  {sid}: {mine:#x} vs {theirs:#x}')
    elif len(sys.argv) >= 4 and sys.argv[1] == 'validate':
        defs_path, map_path = sys.argv[2:4]
        out = sys.argv[4] if len(sys.argv) > 4 else defs_path
        defs = json.load(open(defs_path))
        validator = Validator(defs, Map(map_path))
        tags = validator.run()
        for sid, s in defs['structs'].items():
            if validator.checks[sid] or validator.failures[sid]:
                s['validation'] = {'map': map_path.split('/')[-1], 'checks': validator.checks[sid],
                    'failures': validator.failures[sid], 'example': validator.examples.get(sid, '')}
        json.dump(defs, open(out, 'w'), indent=1)
        total = sum(validator.checks.values())
        failed = sum(validator.failures.values())
        print(f'{sum(tags.values())} tags, {total} checks, {failed} failed, in {len(validator.failures)} structs:')
        for sid, n in validator.failures.most_common():
            print(f'  {n:6d}/{validator.checks[sid]:<6d} {sid}: {validator.examples[sid]}')
    elif len(sys.argv) >= 4 and sys.argv[1] == 'compare':
        defs = json.load(open(sys.argv[2]))
        same, compared, total = compare(defs, sys.argv[3])
        json.dump(defs, open(sys.argv[4] if len(sys.argv) > 4 else sys.argv[2], 'w'), indent=1)
        groups = [(t, g) for t, g in defs['groups'].items() if 'plugin_size' in g]
        chain_size = lambda t: sum(defs['structs'][defs['groups'][x]['struct']]['size'] for x in ancestors(defs, t))
        print(f'groups: {sum(1 for t, g in groups if chain_size(t) == g["plugin_size"])} of {len(groups)} '
            f'the same size as the plugin; structs: {same} of {compared} paired ones ({total} in all) the same size')
        for sid, st in sorted(defs['structs'].items()):
            if 'plugin' in st and st['plugin']['size'] != st['size']:
                print(f'  {sid}: {st["size"]:#x} here, {st["plugin"]["size"]:#x} in {st["plugin"]["path"]}')
    elif len(sys.argv) == 4 and sys.argv[1] == 'header':
        defs = json.load(open(sys.argv[2]))
        open(sys.argv[3], 'w').write(header(defs))
        print(f'wrote {sys.argv[3]}')
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
