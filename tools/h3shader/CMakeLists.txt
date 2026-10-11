# h3shader: translates dumped shader microcode to GLSL (tests the translator).

add_executable(h3shader main.cpp)
target_link_libraries(h3shader PRIVATE H3Shader)
