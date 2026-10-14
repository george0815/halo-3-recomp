# h3tags: lists a cache file's header, tag groups and tags (tests the native
# cache file reader).

add_executable(h3tags main.cpp)
target_link_libraries(h3tags PRIVATE H3NativeCache)
