// Building the game on the player's machine (docs/ISO_TOOL.md, steps 4-6):
// the glue CMake generates for the recompiled DLLs, the compiles and the
// link, from the package's runtime/build.txt.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace h3setup
{
    // Runs a program with these arguments (no shell), its output appended
    // to log. Returns its exit status (-1 if it couldn't start).
    int RunProgram(const std::vector<std::string>& args, const std::filesystem::path& log);

    // The C++ compiler to use: $CXX, else clang++ (clang-cl on Windows) on
    // the PATH. Empty if none.
    std::string FindCompiler();

    // Builds <install>/H3Recomp from the code `recompile` generated in
    // <install>/build, with the package at <package>. 0 on success.
    int Build(const std::filesystem::path& install, const std::filesystem::path& package, int jobs);
}
