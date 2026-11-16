# Cross-compiling for Windows x64 from Linux with clang-cl and lld-link,
# against the MSVC CRT and Windows SDK laid out by xwin
# (`xwin --accept-license --arch x86_64 splat --output <dir>`; the
# directory is H3_XWIN_DIR, default ~/tools/xwin-sdk). On Windows itself,
# build with the "windows" presets instead (Visual Studio's clang-cl).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if (NOT H3_XWIN_DIR)
    if (DEFINED ENV{H3_XWIN_DIR})
        set(H3_XWIN_DIR "$ENV{H3_XWIN_DIR}")
    else()
        set(H3_XWIN_DIR "$ENV{HOME}/tools/xwin-sdk")
    endif()
endif()
set(H3_XWIN_DIR "${H3_XWIN_DIR}" CACHE PATH "xwin splat output (MSVC CRT + Windows SDK)")
# Try-compile projects inherit it.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES H3_XWIN_DIR)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)

set(H3_XWIN_INCLUDES
    "${H3_XWIN_DIR}/crt/include"
    "${H3_XWIN_DIR}/sdk/include/ucrt"
    "${H3_XWIN_DIR}/sdk/include/um"
    "${H3_XWIN_DIR}/sdk/include/shared"
    "${H3_XWIN_DIR}/sdk/include/winrt")
set(H3_XWIN_FLAGS "--target=x86_64-pc-windows-msvc -fms-compatibility-version=19.40")
foreach (dir ${H3_XWIN_INCLUDES})
    string(APPEND H3_XWIN_FLAGS " /imsvc \"${dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${H3_XWIN_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${H3_XWIN_FLAGS}")
set(CMAKE_RC_FLAGS_INIT "-I \"${H3_XWIN_DIR}/sdk/include/um\" -I \"${H3_XWIN_DIR}/sdk/include/shared\"")

set(H3_XWIN_LIBS
    "/libpath:\"${H3_XWIN_DIR}/crt/lib/x86_64\""
    "/libpath:\"${H3_XWIN_DIR}/sdk/lib/um/x86_64\""
    "/libpath:\"${H3_XWIN_DIR}/sdk/lib/ucrt/x86_64\"")
string(JOIN " " H3_XWIN_LINK ${H3_XWIN_LIBS})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${H3_XWIN_LINK}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${H3_XWIN_LINK}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${H3_XWIN_LINK}")

# Dependencies come from H3_WINDOWS_DEPS (prefix with SDL3, shaderc, Qt...),
# never from the Linux host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
