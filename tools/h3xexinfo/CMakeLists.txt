# h3xexinfo: prints XEX layout/imports, dumps the image, writes import stubs.

add_executable(h3xexinfo main.cpp)
target_link_libraries(h3xexinfo PRIVATE XenonUtils)
