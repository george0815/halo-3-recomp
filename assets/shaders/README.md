# Shader manifests

`h3setup install` ends by running `H3Recomp <xex> --precompile-shaders <this folder>`
(H3Recomp/gpu/renderer/precompile.cpp), which fills the shader cache before the first
start, so the game doesn't stutter the first time it meets each shader.

Nothing here is game code. The shaders come from the player's own map files:

- Every shader in the maps, as it is. Pixel shaders mostly run unchanged; vertex shaders
  can't be used until the game binds them (their vertex formats are filled in then).
- `shader_manifest.txt`: the variants the game makes when it binds a shader. The game's
  D3D code patches texture fetch slots (pixel shaders) and vertex fetch layouts (vertex
  shaders). Per variant: the map and address of the shader, the offset of its microcode,
  its length, the patches as XORs against the map's dwords (the changed bits only), and
  a hash to check the result. Built by `tools/shaders/build_manifest.py` from shaders
  recorded in play (`H3_RECORD_SHADERS=<dir>`) and the map dumps (`h3tags <map> shaders`).
- `pipelines.txt`: the pipelines the game used, as the game's own shader
  cache manifest (renderer/pipelines.cpp). It is copied into a new cache so the first
  start's background warm-up builds them.

## Regenerating (each release: the translator's output changes the cache names)

1. Record: run the film library (and the menus, and multiplayer maps) with
   `H3_RECORD_SHADERS=<dir>` and a fresh shader cache directory.
2. `for m in game/maps/*.map: h3tags $m shaders build/shx/<map>`
3. `tools/shaders/build_manifest.py <dir> build/shx assets/shaders/shader_manifest.txt`
4. Copy the fresh cache's `pipelines.txt` here without its repeated lines:
   `awk '!seen[$0]++' <cache>/pipelines.txt > assets/shaders/pipelines.txt`
5. Check: `H3Recomp <xex> --precompile-shaders assets/shaders` into an empty cache, then
   start the game; its log says how many shaders and pipelines it warms up.
