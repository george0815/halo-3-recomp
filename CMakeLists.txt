# Top-level build. Pieces:
#   thirdparty/XenonRecomp  the recompiler (built as a tool)
#   H3RecompLib             the recompiled game code (generated; see tools/recompile.sh)
#   H3Recomp                the runtime executable
#   H3Launcher              the Qt launcher (settings, profile, controls); optional
#   H3Native                native game code (cache file reader, decompiled functions)
#   tools/                  h3xexinfo (XEX inspector), h3shader (shader translator test),
#                           h3tags (cache file / tag index lister)

cmake_minimum_required(VERSION 3.20)

project(H3Recomp LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "XenonRecomp output is only tested with Clang 18+. Use the CMake presets.")
endif()

# Recompiler toolchain: XenonAnalyse, XenonRecomp, XenonUtils.
add_subdirectory(thirdparty/XenonRecomp)

# Xenos shader microcode -> GLSL translator, used by the runtime and by
# tools/h3shader (which needs no game build).
add_library(H3Shader STATIC H3Recomp/gpu/shader/translator.cpp)
target_include_directories(H3Shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/H3Recomp)

# User settings (settings.ini): shared by the runtime and the launcher, so
# it has no SDL or Qt dependency.
add_library(H3Config STATIC H3Recomp/config/settings.cpp)
target_include_directories(H3Config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/H3Recomp)

# Native game code that builds without the recompiled code.
add_subdirectory(H3Native)

# Our own tooling.
add_subdirectory(tools/h3xexinfo)
add_subdirectory(tools/h3shader)
add_subdirectory(tools/h3tags)

# The launcher, when Qt 6 is available.
find_package(Qt6 COMPONENTS Widgets QUIET)
if (Qt6_FOUND)
    add_subdirectory(H3Launcher)
else()
    message(STATUS "Qt 6 Widgets not found: skipping H3Launcher")
endif()

# The game library and runtime only exist once the recompiler has produced
# code from the user's own XEX (see tools/recompile.sh).
set(H3_PPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/H3RecompLib/ppc)

if (EXISTS ${H3_PPC_DIR}/ppc_config.h)
    add_subdirectory(H3RecompLib)
    add_subdirectory(H3Recomp)
else()
    message(STATUS "No recompiled code in H3RecompLib/ppc yet: building tools only. Run tools/recompile.sh.")
endif()
