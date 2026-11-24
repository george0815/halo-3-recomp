// h3setup: turns the player's own Halo 3 disc image into a playable install.
//
//   h3setup check <iso>            is this a supported Halo 3 disc?
//   h3setup list <iso>             the files on the disc
//   h3setup extract <iso> <dir>    copy the disc's files into <dir>/game
//   h3setup recompile <dir> --config <config dir> --tools <bin dir>
//                                  translate <dir>/game's executable and
//                                  DLLs to C++ in <dir>/build
//   h3setup build <dir> [--package <dir>] [--jobs <n>]
//                                  compile and link <dir>/H3Recomp
//   h3setup sdk <dir> --accept-microsoft-license
//                                  Windows: fetch Microsoft's C runtime and
//                                  Windows SDK (xwin) into <dir>/toolchain/sdk
//   h3setup shaders <dir>          fill <dir>/game/shadercache ahead of the
//                                  first start (H3Recomp --precompile-shaders
//                                  with the package's shaders/ manifests)
//   h3setup install <iso> <dir> [--package <dir>] [--jobs <n>]
//                   [--accept-microsoft-license]   all of the above
//
// The package (the release: runtime/, recompiler/, tools/) defaults to the
// folder h3setup is in. docs/ISO_TOOL.md. Progress lines for the launcher
// start with "progress ".

#include "build.h"
#include "iso.h"
#include "sha1.h"

#include <cstdio>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using h3setup::FromUtf8;
using h3setup::Utf8;

namespace
{
    // The executable the recompiler config (H3RecompLib/config/halo3.toml)
    // is made for: the disc release, no title update (docs/DUMPING.md).
    struct SupportedBuild
    {
        const char* sha1;
        const char* name;
    };
    constexpr SupportedBuild kSupported[] = {
        { "927a8c49062a55179d84116e33f8a5a0b3db13c1", "Halo 3 (disc, version 0.0.0.42)" },
    };

    // The command summary (docs/ISO_TOOL.md describes each).
    int Usage()
    {
        fprintf(stderr, "usage: h3setup check <iso>\n"
                        "       h3setup list <iso>\n"
                        "       h3setup extract <iso> <install directory>\n"
                        "       h3setup recompile <install directory> --config <dir> --tools <dir>\n"
                        "       h3setup build <install directory> [--package <dir>] [--jobs <n>]\n"
                        "       h3setup sdk <install directory> --accept-microsoft-license\n"
                        "       h3setup install <iso> <install directory> [--package <dir>] [--jobs <n>] [--accept-microsoft-license]\n");
        return 2;
    }

    // Opens and indexes the disc image, printing why when it can't.
    bool OpenIso(h3setup::Iso& iso, const std::filesystem::path& path)
    {
        if (iso.Open(path))
            return true;
        fprintf(stderr, "error: %s\n", iso.Error().c_str());
        return false;
    }

    // The disc's default.xex against the supported builds. Prints what it
    // found; true if supported.
    bool Check(h3setup::Iso& iso)
    {
        const h3setup::IsoEntry* xex = iso.Find("/default.xex");
        if (!xex || xex->directory)
        {
            fprintf(stderr, "error: no default.xex on this disc (is it an Xbox 360 game disc?)\n");
            return false;
        }
        std::vector<uint8_t> data;
        if (!iso.Read(*xex, data))
        {
            fprintf(stderr, "error: can't read default.xex from the image\n");
            return false;
        }
        h3setup::Sha1 sha1;
        sha1.Update(data.data(), data.size());
        std::string hash = sha1.Hex();
        for (const SupportedBuild& build : kSupported)
            if (hash == build.sha1)
            {
                printf("supported: %s (default.xex sha1 %s)\n", build.name, hash.c_str());
                return true;
            }
        fprintf(stderr,
            "error: this disc's default.xex (sha1 %s) isn't a supported Halo 3 build.\n"
            "Supported: the original disc release (sha1 %s). Other pressings and title updates aren't yet.\n",
            hash.c_str(), kSupported[0].sha1);
        return false;
    }

    // `extract`: copies every file of the disc into <install>/game, skipping
    // files an earlier run already copied (same size), with progress lines.
    int Extract(h3setup::Iso& iso, const std::filesystem::path& directory)
    {
        uint64_t total = 0, done = 0;
        for (const h3setup::IsoEntry& entry : iso.Entries())
            total += entry.directory ? 0 : entry.size;

        // Room for the disc's files and the build (~1.5 GB of objects).
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        std::filesystem::space_info space = std::filesystem::space(directory, ec);
        constexpr uint64_t kBuildBytes = 1536ull << 20;
        uint64_t have = 0;
        if (!ec)
        {
            have = space.available;
            for (const h3setup::IsoEntry& entry : iso.Entries())  // already extracted (a retry)
                if (!entry.directory && std::filesystem::file_size(directory / "game" / FromUtf8(entry.path.substr(1)), ec) == entry.size && !ec)
                    have += entry.size;
            if (have < total + kBuildBytes)
            {
                fprintf(stderr, "error: not enough free space in %s: the install needs %.1f GB, %.1f GB are free\n", Utf8(directory).c_str(),
                    double(total + kBuildBytes) / 1e9, double(space.available) / 1e9);
                return 1;
            }
        }
        int lastPercent = -1;
        for (const h3setup::IsoEntry& entry : iso.Entries())
        {
            std::filesystem::path destination = directory / "game" / FromUtf8(entry.path.substr(1));
            uint64_t before = done;
            // Already there from an earlier run that stopped: keep it.
            std::error_code sizeError;
            if (!entry.directory && std::filesystem::file_size(destination, sizeError) == entry.size && !sizeError)
            {
                done += entry.size;
                continue;
            }
            bool ok = iso.Extract(entry, destination, [&](uint64_t bytes) {
                int percent = total ? int((before + bytes) * 100 / total) : 100;
                if (percent != lastPercent)
                {
                    printf("progress extract %d\n", percent);
                    fflush(stdout);
                    lastPercent = percent;
                }
            });
            if (!ok)
            {
                fprintf(stderr, "error: %s\n", iso.Error().c_str());
                return 1;
            }
            if (!entry.directory)
                done += entry.size;
        }
        printf("extracted %zu entries (%.1f GB) to %s\n", iso.Entries().size(), double(total) / 1e9, Utf8(directory).c_str());
        return 0;
    }

    // --- recompile ----------------------------------------------------------

    // Runs a program (no shell), its output into log; false (and says so)
    // if it fails.
    bool Run(const std::vector<std::string>& args, const std::filesystem::path& log)
    {
        std::error_code ec;
        std::filesystem::remove(log, ec);
        int status = h3setup::RunProgram(args, log);
        if (status != 0)
            fprintf(stderr, "error: %s failed (%d); see %s\n", FromUtf8(args[0]).filename().string().c_str(), status, Utf8(log).c_str());
        return status == 0;
    }

    // A whole file as bytes (empty if it can't be read).
    std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }

    // A config's  key = "path"  line pointed at another file. XenonRecomp
    // joins the config's directory and the value as strings, so the value
    // must be relative to the config.
    void SetPath(std::string& config, const std::string& key, const std::filesystem::path& value,
        const std::filesystem::path& configDirectory)
    {
        std::string relative = Utf8(std::filesystem::relative(value, configDirectory));
        std::string replacement = key + " = \"" + relative + "\"";
        config = std::regex_replace(config, std::regex("(^|\n)" + key + " = \"[^\"]*\""), "$1" + replacement);
    }

    // One XenonRecomp config: copied into <build>/config with its input
    // (relative to the disc's root), output directory and jump tables
    // pointed at this install. The jump tables are shipped (addresses
    // XenonAnalyse found; it doesn't run here).
    bool RecompileOne(const std::filesystem::path& source, const std::filesystem::path& input,
        const std::filesystem::path& output, const std::filesystem::path& build, const std::filesystem::path& tools,
        const std::filesystem::path& context, bool executable)
    {
        std::string config = ReadText(source);
        std::filesystem::path tables = source.parent_path() / (source.stem().string() + "_switch_tables.toml");
        if (config.empty() || !std::filesystem::exists(tables) || !std::filesystem::exists(input))
        {
            fprintf(stderr, "error: missing %s, its jump tables or %s\n", Utf8(source).c_str(), Utf8(input).c_str());
            return false;
        }
        std::error_code ec;
        std::filesystem::path configDirectory = build / "config";
        std::filesystem::create_directories(configDirectory, ec);
        std::filesystem::remove_all(output, ec);
        std::filesystem::create_directories(output, ec);
        std::filesystem::copy_file(tables, configDirectory / tables.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        SetPath(config, "file_path", input, configDirectory);
        SetPath(config, "out_directory_path", output, configDirectory);
        SetPath(config, "switch_table_file_path", configDirectory / tables.filename(), configDirectory);
        std::filesystem::path written = configDirectory / source.filename();
        std::ofstream(written, std::ios::binary) << config;

        printf("recompiling %s\n", input.filename().string().c_str());
        fflush(stdout);
#ifdef _WIN32
        const char* suffix = ".exe";
#else
        const char* suffix = "";
#endif
        std::filesystem::path recomp = tools / ("XenonRecomp" + std::string(suffix)), info = tools / ("h3xexinfo" + std::string(suffix));
        return Run({ Utf8(recomp), Utf8(written), Utf8(context) }, build / "recompile.log") &&
            Run({ Utf8(info), Utf8(input), "--stubs", Utf8(output / "import_stubs.cpp") }, build / "stubs.log");
    }

    // `recompile`: runs XenonRecomp over default.xex and each DLL the
    // package has a config for, into <install>/build (ppc/ and modules/),
    // plus each one's import stubs.
    // Prints "progress recompile <percent>" lines for the launcher.
    int Recompile(std::filesystem::path install, std::filesystem::path configs, std::filesystem::path tools)
    {
        // XenonRecomp reads a config's paths relative to the config itself.
        install = std::filesystem::absolute(install);
        configs = std::filesystem::absolute(configs);
        tools = std::filesystem::absolute(tools);
        std::filesystem::path game = install / "game", build = install / "build";
        std::filesystem::path context = configs / "ppc_context.h";
        if (!std::filesystem::exists(context))
        {
            fprintf(stderr, "error: no ppc_context.h in %s\n", Utf8(configs).c_str());
            return 1;
        }
        if (!RecompileOne(configs / "halo3.toml", game / "default.xex", build / "ppc", build, tools, context, true))
            return 1;
        printf("progress recompile 20\n");
        int done = 0;
        std::vector<std::filesystem::path> modules;
        for (const auto& entry : std::filesystem::directory_iterator(configs / "modules"))
            if (entry.path().extension() == ".toml" && entry.path().stem().string().find("_switch_tables") == std::string::npos)
                modules.push_back(entry.path());
        std::sort(modules.begin(), modules.end());
        for (const std::filesystem::path& module : modules)
        {
            // The DLL's place on the disc, from the config's own path
            // (".../private/game/<path on the disc>").
            std::smatch match;
            std::string config = ReadText(module);
            if (!std::regex_search(config, match, std::regex("file_path = \"[^\"]*/game/([^\"]+)\"")))
            {
                fprintf(stderr, "error: %s doesn't name its DLL's place on the disc\n", Utf8(module).c_str());
                return 1;
            }
            if (!RecompileOne(module, game / FromUtf8(match[1].str()), build / "modules" / module.stem(), build,
                    tools, context, false))
                return 1;
            printf("progress recompile %d\n", 20 + 80 * ++done / int(modules.size()));
            fflush(stdout);
        }
        printf("recompiled into %s\n", Utf8(build).c_str());
        return 0;
    }
}

namespace
{
    // The built game compiles every shader of the maps (and the shipped
    // manifest's patched variants) into its shader cache, so the first
    // start doesn't stutter on each new shader. Optional: on failure the
    // game still compiles them as it meets them. Relays the game's
    // "progress shaders <percent>" lines from its log.
    int PrecompileShaders(const std::filesystem::path& installArg, const std::filesystem::path& packageArg)
    {
        // Absolute, as the other steps use them: Windows' CreateProcess
        // doesn't find a program by a relative path with forward slashes.
        std::filesystem::path install = std::filesystem::absolute(installArg), package = std::filesystem::absolute(packageArg);
        std::filesystem::path exe = install /
#ifdef _WIN32
            "H3Recomp.exe";
#else
            "H3Recomp";
#endif
        std::filesystem::path log = install / "build" / "shaders.log";
        std::error_code ec;
        std::filesystem::create_directories(log.parent_path(), ec);
        std::filesystem::remove(log, ec);
        printf("progress shaders 0\n");
        fflush(stdout);
        int status = -1;
        std::atomic<bool> finished{ false };
        std::thread worker([&] {
            status = h3setup::RunProgram({ Utf8(exe), Utf8(install / "game" / "default.xex"), "--precompile-shaders",
                Utf8(package / "shaders") }, log);
            finished = true;
        });
        std::string last;
        while (!finished)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::ifstream in(log);
            std::string line, progress;
            while (std::getline(in, line))
                if (line.rfind("progress shaders ", 0) == 0)
                    progress = line;
            if (!progress.empty() && progress != last)
            {
                last = progress;
                printf("%s\n", progress.c_str());
                fflush(stdout);
            }
        }
        worker.join();
        if (status != 0)
            fprintf(stderr, "warning: shader precompile failed (%d); see %s\n", status, Utf8(log).c_str());
        else
            printf("progress shaders 100\n");
        return 0;
    }
}

// The commands (Usage). Arguments are UTF-8 on every platform (Windows
// reads its wide command line).
int main(int argcNarrow, char** argvNarrow)
{
    // The arguments as UTF-8 (Windows: from the UTF-16 command line; argv
    // there is in the ANSI code page and loses other characters).
    std::vector<std::string> argv(argvNarrow, argvNarrow + argcNarrow);
#ifdef _WIN32
    int count = 0;
    if (wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &count))
    {
        argv.clear();
        for (int i = 0; i < count; i++)
        {
            int length = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string text(size_t(std::max(length, 1) - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, text.data(), length, nullptr, nullptr);
            argv.push_back(text);
        }
        LocalFree(wide);
    }
#endif
    int argc = int(argv.size());
    if (argc < 3)
        return Usage();
    std::string command = argv[1];
    h3setup::Iso iso;

    // --package / --jobs after the positional arguments.
    std::filesystem::path package = std::filesystem::absolute(FromUtf8(argv[0])).parent_path();
    int jobs = int(std::max(1u, std::thread::hardware_concurrency()));
    bool acceptMicrosoft = false;
    std::vector<std::filesystem::path> positional;
    for (int i = 2; i < argc; i++)
    {
        const std::string& arg = argv[i];
        if (arg == "--package" && i + 1 < argc)
            package = FromUtf8(argv[++i]);
        else if (arg == "--jobs" && i + 1 < argc)
            jobs = std::max(1, atoi(argv[++i].c_str()));
        else if (arg == "--accept-microsoft-license")
            acceptMicrosoft = true;
        else
            positional.push_back(FromUtf8(arg));
    }
    if (command == "build" && positional.size() == 1)
        return h3setup::Build(positional[0], package, jobs);
    if (command == "sdk" && positional.size() == 1)
        return h3setup::FetchSdk(positional[0], package, acceptMicrosoft);
    if (command == "install" && positional.size() == 2)
    {
        if (!OpenIso(iso, positional[0]) || !Check(iso))
            return 1;
#ifdef _WIN32
        // First, so a missing license acceptance stops before the 6 GB copy.
        if (h3setup::FetchSdk(positional[1], package, acceptMicrosoft) != 0)
            return 1;
#endif
        if (Extract(iso, positional[1]) != 0)
            return 1;
        if (Recompile(positional[1], package / "recompiler" / "config", package / "tools") != 0)
            return 1;
        if (h3setup::Build(positional[1], package, jobs) != 0)
            return 1;
        return PrecompileShaders(positional[1], package);
    }
    if (command == "shaders" && positional.size() == 1)
        return PrecompileShaders(positional[0], package);
    if (command == "check" && positional.size() == 1)
        return OpenIso(iso, positional[0]) && Check(iso) ? 0 : 1;
    if (command == "list" && positional.size() == 1)
    {
        if (!OpenIso(iso, positional[0]))
            return 1;
        for (const h3setup::IsoEntry& entry : iso.Entries())
            printf("%c %12llu %s\n", entry.directory ? 'D' : 'F', static_cast<unsigned long long>(entry.size), entry.path.c_str());
        return 0;
    }
    if (command == "extract" && positional.size() == 2)
        return OpenIso(iso, positional[0]) && Check(iso) ? Extract(iso, positional[1]) : 1;
    if (command == "recompile" && argc == 7 && argv[3] == "--config" && argv[5] == "--tools")
        return Recompile(FromUtf8(argv[2]), FromUtf8(argv[4]), FromUtf8(argv[6]));
    return Usage();
}
