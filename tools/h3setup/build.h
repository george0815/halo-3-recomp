// Building the game on the player's machine (docs/ISO_TOOL.md, steps 4-6):
// the glue CMake generates for the recompiled DLLs, the compiles and the
// link, from the package's runtime/build.txt.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace h3setup
{
    // Paths as UTF-8 text (forward slashes) and back: every path h3setup
    // prints, passes to a program or reads from its command line goes
    // through these, so non-ASCII folder names work on Windows too (where
    // a plain std::string path would mean the ANSI code page).
    inline std::string Utf8(const std::filesystem::path& path)
    {
        std::u8string text = path.generic_u8string();
        return std::string(text.begin(), text.end());
    }
    // And back: a UTF-8 string as a path.
    inline std::filesystem::path FromUtf8(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    // Runs a program with these arguments (UTF-8; no shell), its output
    // appended to log. Returns its exit status (-1 if it couldn't start).
    int RunProgram(const std::vector<std::string>& args, const std::filesystem::path& log);

    // Locks <install> against a second h3setup working in it at the same
    // time; false (with the error printed) if one already is.
    bool LockInstall(const std::filesystem::path& install);

    // The C++ compiler to use: $CXX, else (Windows) the package's
    // toolchain/llvm, else clang++ (clang-cl on Windows) on the PATH.
    // Empty if none.
    std::string FindCompiler(const std::filesystem::path& package);

    // Windows: Microsoft's C runtime and Windows SDK, which the package's
    // xwin downloads (Microsoft's license; accepted = the player agreed)
    // into <install>/toolchain/sdk. 0 when they're there.
    std::filesystem::path SdkDirectory(const std::filesystem::path& install);
    int FetchSdk(const std::filesystem::path& install, const std::filesystem::path& package, bool accepted);

    // Builds <install>/H3Recomp from the code `recompile` generated in
    // <install>/build, with the package at <package>. 0 on success.
    int Build(const std::filesystem::path& install, const std::filesystem::path& package, int jobs);
}
