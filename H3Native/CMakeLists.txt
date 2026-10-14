# Native game code (docs/ROADMAP.md phase 6): what replaces the recompiled
# game, piece by piece.
#
#   H3NativeCache  cache file reader (maps, tag index) in host layout; no
#                  dependency on the recompiled code, so tools use it too

add_library(H3NativeCache STATIC cache/cache_file.cpp)
target_include_directories(H3NativeCache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
