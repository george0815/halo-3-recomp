# Tag definitions

Typed C++ layouts for Halo 3's tags (docs/ROADMAP.md 6.3), generated from
the tag definitions in your own H3EK (the official MCC editing kit). Nothing
derived from H3EK is committed: the JSON goes to `private/tagdefs/`, the
header to `H3Native/tags/generated/` (git-ignored).

```sh
W=/tmp/h3ek-work            # scratch: copies of tool.exe and its DLLs, a Wine prefix, ~450 MB of dumps
A=<Assembly checkout>/src/Assembly/Plugins/Halo3   # github.com/XboxChaos/Assembly (360 layouts)
D=private/tagdefs
tools/tagdefs/dump_tool_memory.sh "<H3EK folder>" $W                     # needs wine and gdb
tools/tagdefs/tagdefs.py extract $W private/game/maps/010_jungle.map $D/halo3.json
tools/tagdefs/tagdefs.py merge $D/halo3.json $A $D/halo3_360.json        # MCC -> 360 layouts
tools/tagdefs/tagdefs.py validate $D/halo3_360.json private/game/maps/010_jungle.map
tools/tagdefs/tagdefs.py compare $D/halo3_360.json $A
tools/tagdefs/tagdefs.py header $D/halo3_360.json H3Native/tags/generated/tag_layouts.h
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
- `merge` rebuilds, from Assembly's community 360 plugins, every struct
  whose size or layout (where its tag references and blocks sit) differs,
  keeping H3EK's names and enum options everywhere else. Ancestor structs
  are split out of a group's flat plugin at its section comments (UNIT,
  BIPED, MACHINE...); nested blocks are paired by name, then offset, then
  order.
- `compare` checks struct sizes against the plugins.
- `header` writes packed big-endian structs (`H3Native/tags/tag_types.h`
  has `tag_block<T>`, `tag_reference`, ...), each with a size
  `static_assert` (and one per member offset), and a comment per struct:
  360 size confirmed, DIFFERS on 360, or UNVERIFIED (validation failures).
  Next to it, `tag_host.h` (the same structs in host layout, native
  endian, `Block<T>` with host pointers; a `<name>_tag` struct per group)
  and `tag_convert.cpp` (conversion both ways, by group tag). `h3tags <map>
  convert` converts every tag and checks the round trip gives back the
  file's bytes.

## What's known (2026-10-14)

- 175 groups (all but `itmc`, `vehc`: not found in tool.exe; 010_jungle has no tags of them), 1,588 structs.
  H3EK alone is MCC's layout: 106 of 127 groups had the 360 size, and 12%
  of the checks on retail maps failed (sounds, compiled shaders, physics
  shapes, the resource gestalt, AI pathfinding, objects/units...).
- After `merge` (314 structs rebuilt from the plugins): all 127 groups and
  all 1,020 paired structs have the plugins' sizes; validation on 12 maps
  (campaign, multiplayer, main menu, ~0.3-0.65 M checks each) fails exactly
  one check per map: the zone manifest's attachment hierarchy, a block with
  a count and no address (filled in at runtime).
- `h3tags <map> bsps` reads the scenario's BSP list through the generated
  structs (a first consumer).
- Field names in rebuilt structs are the plugins' (and their guesses for
  unknown fields: "Unknown", unnamed padding).
