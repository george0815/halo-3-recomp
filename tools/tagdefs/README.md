# Tag definitions

Typed C++ layouts for Halo 3's tags (docs/ROADMAP.md 6.3), generated from
the tag definitions in your own H3EK (the official MCC editing kit). Nothing
derived from H3EK is committed: the JSON goes to `private/tagdefs/`, the
header to `H3Native/tags/generated/` (git-ignored).

```sh
W=/tmp/h3ek-work            # scratch: copies of tool.exe and its DLLs, a Wine prefix, ~450 MB of dumps
tools/tagdefs/dump_tool_memory.sh "<H3EK folder>" $W                         # needs wine and gdb
tools/tagdefs/tagdefs.py extract $W private/game/maps/010_jungle.map private/tagdefs/halo3.json
tools/tagdefs/tagdefs.py validate private/tagdefs/halo3.json private/game/maps/010_jungle.map
tools/tagdefs/tagdefs.py compare private/tagdefs/halo3.json <Assembly>/src/Assembly/Plugins/Halo3   # optional
tools/tagdefs/tagdefs.py header private/tagdefs/halo3.json H3Native/tags/generated/tag_layouts.h
```

- `dump_tool_memory.sh` runs a copy of `tool.exe` under Wine (it prints its
  usage and exits) and dumps its data sections at exit: many struct
  definitions only get their fields and sizes from static constructors.
- `extract` finds every tag group of the map in that memory and writes
  groups, structs and fields (type, name, offset, 360 size, nested struct,
  block maximum, reference group, enum options). A tag's data is its
  ancestors' structs, base first (biped = object + unit + biped).
- `validate` walks every tag of a retail map with the layouts and checks
  block counts and addresses, tag references and string ids.
- `compare` checks struct sizes against Assembly's community-made 360
  plugins (reference only: just the verdict reaches the header).
- `header` writes packed big-endian structs (`H3Native/tags/tag_types.h`
  has `tag_block<T>`, `tag_reference`, ...), each with a size
  `static_assert`, and a comment per struct: 360 size confirmed, DIFFERS on
  360, or UNVERIFIED (validation failures).

## What's known (2026-10-14, 010_jungle)

- 175 groups (all but `itmc`, `vehc`), 1,364 structs. H3EK's stored sizes
  equal the sizes computed with 360 field sizes for every struct, so H3EK
  is laid out like the 360 cache file except where MCC changed the content.
- Against the plugins: 106 of 127 groups have the same total size; 409 of
  the 480 structs that could be paired are the same size (71 differ).
- Where they differ, MCC both added fields (scenario: `future_more_flags`)
  and removed some (biped is 0x4E0 on 360, 0x490 in H3EK). Known
  cache-only or platform layouts: `snd!` is a 32-byte cache struct pointing
  into `ugh!`; compiled shaders (`glps`, `glvs`, rasterizer constant
  tables); physics shapes (`phmo`); the resource gestalt (`zone`, `play`).
- Validation: 383,186 checks, 12% fail, nearly all in those structs.

Reconciling the differing structs (by hand, from the plugins and the 360
code that reads them) is the next step before decompiled code relies on
them.
