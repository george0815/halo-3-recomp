# Native game code (docs/ROADMAP.md phase 6): what replaces the recompiled
# game, piece by piece.
#
#   H3NativeCache  cache file reader (maps, tag index) in host layout; no
#                  dependency on the recompiled code, so tools use it too

add_library(H3NativeCache STATIC cache/cache_file.cpp)
target_include_directories(H3NativeCache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# H3NativeTags: tag layouts and host conversion, generated locally from the
# user's H3EK (tools/tagdefs) into tags/generated/. Without them the native
# tag code that needs them is left out (H3_HAVE_TAG_LAYOUTS undefined) and
# the recompiled originals run.
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tags/generated/tag_convert.cpp)
    add_library(H3NativeTags STATIC tags/generated/tag_convert.cpp)
    target_include_directories(H3NativeTags PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/thirdparty/XenonRecomp/XenonUtils)
    target_compile_definitions(H3NativeTags PUBLIC H3_HAVE_TAG_LAYOUTS=1)
    # ~20k lines of straight-line code: optimizing it takes long and buys little.
    target_compile_options(H3NativeTags PRIVATE -O1)
else()
    add_library(H3NativeTags INTERFACE)
    message(STATUS "No generated tag layouts (tools/tagdefs): native tag readers use the recompiled originals")
endif()
