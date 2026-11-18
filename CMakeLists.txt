# Top-level build. Pieces:
#   thirdparty/XenonRecomp  the recompiler (built as a tool)
#   H3RecompLib             the recompiled game code (generated; see tools/recompile.sh)
#   H3Recomp                the runtime executable
#   H3Launcher              the Qt launcher (settings, profile, controls); optional
#   H3Native                native game code (cache file reader, decompiled functions)
#   tools/                  h3xexinfo (XEX inspector), h3shader (shader translator test), h3setup (ISO -> install),
#                           h3tags (cache file / tag index lister)

cmake_minimum_required(VERSION 3.20)

project(H3Recomp VERSION 0.75.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(WARNING "XenonRecomp output is only tested with Clang 18+. Use the CMake presets.")
endif()

# Recompiler toolchain: XenonAnalyse, XenonRecomp, XenonUtils.
add_subdirectory(thirdparty/XenonRecomp)
if (MSVC)
    # XenonRecomp asks for the static C runtime; everything else (SDL, Qt,
    # shaderc) uses the DLL one, and the two can't be linked together.
    function(h3_dll_runtime dir)
        get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
        foreach (target ${targets})
            get_target_property(type ${target} TYPE)
            if (NOT type STREQUAL "INTERFACE_LIBRARY")
                set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
            endif()
        endforeach()
        get_property(subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
        foreach (sub ${subdirs})
            h3_dll_runtime(${sub})
        endforeach()
    endfunction()
    h3_dll_runtime(${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenonRecomp)
endif()
if (WIN32)
    # windows.h in our code: no min/max macros (they break std::min and
    # std::max), the lean set of headers, and no deprecation warnings for
    # standard C functions (getenv, fopen...). After XenonRecomp, whose
    # third-party code expects the full set.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    set(H3_WINDOWS_DEPS "${PROJECT_SOURCE_DIR}/build/windows-deps" CACHE PATH "Windows dependencies (tools/build_windows_deps.sh)")
    # The Visual C++ runtime DLLs, shipped next to each executable so players
    # needn't install the redistributable (tools/build_windows_deps.sh vcredist).
    file(GLOB H3_VCREDIST_DLLS "${H3_WINDOWS_DEPS}/vcredist/*.dll")
    if (NOT H3_VCREDIST_DLLS)
        message(WARNING "No Visual C++ runtime DLLs in ${H3_WINDOWS_DEPS}/vcredist: run tools/build_windows_deps.sh vcredist")
    endif()
endif()

# Xenos shader microcode -> GLSL translator, used by the runtime and by
# tools/h3shader (which needs no game build).
add_library(H3Shader STATIC H3Recomp/gpu/shader/translator.cpp)
target_include_directories(H3Shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/H3Recomp)

# User settings (settings.ini): shared by the runtime and the launcher, so
# it has no SDL or Qt dependency.
add_library(H3Config STATIC H3Recomp/config/settings.cpp)
target_include_directories(H3Config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/H3Recomp)

# Windows: our command line tools run with UTF-8 as their code page
# (cmake/utf8.manifest), for paths with any characters; the recompiler too.
if (WIN32)
    target_sources(XenonRecomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8.manifest)
    target_sources(XenonAnalyse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8.manifest)
endif()

# Native game code that builds without the recompiled code.
add_subdirectory(H3Native)

# Our own tooling.
add_subdirectory(tools/h3xexinfo)
add_subdirectory(tools/h3shader)
add_subdirectory(tools/h3tags)
add_subdirectory(tools/h3setup)
if (WIN32)
    target_sources(h3xexinfo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/utf8.manifest)
endif()

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

# Warnings for our own code (not the recompiler or the generated game code).
# Left off: unused parameters (hook and callback signatures are fixed) and
# missing field initializers (tables and GPU structs leave trailing fields
# and padding at zero on purpose).
set(H3_OWN_TARGETS H3Shader H3Config H3NativeCache h3xexinfo h3shader h3tags h3setup H3Launcher H3Runtime H3Recomp)
foreach (target ${H3_OWN_TARGETS})
    if (TARGET ${target})
        get_target_property(type ${target} TYPE)
        if (NOT type STREQUAL "INTERFACE_LIBRARY")
            if (MSVC)
                target_compile_options(${target} PRIVATE /W4 /wd4100)
                if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")  # clang-cl
                    target_compile_options(${target} PRIVATE -Wno-unused-parameter -Wno-missing-field-initializers)
                endif()
            else()
                target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
            endif()
        endif()
    endif()
endforeach()
