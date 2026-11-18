# h3setup: the player's own Halo 3 disc image -> a playable install
# (docs/ISO_TOOL.md). Standalone: no recompiled code, no SDL/Qt.

add_executable(h3setup main.cpp iso.cpp build.cpp)
