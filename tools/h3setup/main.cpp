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
//   h3setup install <iso> <dir> [--package <dir>] [--jobs <n>]
//                                  all of the above
//
// The package (the release: runtime/, recompiler/, tools/) defaults to the
// folder h3setup is in. docs/ISO_TOOL.md. Progress lines for the launcher
// start with "progress ".

#include "build.h"
#include "iso.h"
#include "sha1.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

    int Usage()
    {
        fprintf(stderr, "usage: h3setup check <iso>\n"
                        "       h3setup list <iso>\n"
                        "       h3setup extract <iso> <install directory>\n"
                        "       h3setup recompile <install directory> --config <dir> --tools <dir>\n"
                        "       h3setup build <install directory> [--package <dir>] [--jobs <n>]\n"
                        "       h3setup install <iso> <install directory> [--package <dir>] [--jobs <n>]\n");
        return 2;
    }

    bool OpenIso(h3setup::Iso& iso, const char* path)
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

    int Extract(h3setup::Iso& iso, const char* directory)
    {
        uint64_t total = 0, done = 0;
        for (const h3setup::IsoEntry& entry : iso.Entries())
            total += entry.directory ? 0 : entry.size;
        int lastPercent = -1;
        for (const h3setup::IsoEntry& entry : iso.Entries())
        {
            std::filesystem::path destination = std::filesystem::path(directory) / "game" / std::filesystem::path(entry.path.substr(1));
            uint64_t before = done;
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
        printf("extracted %zu entries (%.1f GB) to %s\n", iso.Entries().size(), double(total) / 1e9, directory);
        return 0;
    }

    // --- recompile ----------------------------------------------------------

    std::string Quote(const std::filesystem::path& path)
    {
        return "\"" + path.string() + "\"";
    }

    // Runs a command line; false (and says so) if it fails.
    bool Run(const std::string& command)
    {
#ifdef _WIN32
        // cmd.exe strips one pair of outer quotes.
        int status = std::system(("\"" + command + "\"").c_str());
#else
        int status = std::system(command.c_str());
#endif
        if (status != 0)
            fprintf(stderr, "error: command failed (%d): %s\n", status, command.c_str());
        return status == 0;
    }

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
        std::string relative = std::filesystem::relative(value, configDirectory).generic_string();
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
            fprintf(stderr, "error: missing %s, its jump tables or %s\n", source.string().c_str(), input.string().c_str());
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
        std::filesystem::path recomp = tools / "XenonRecomp", info = tools / "h3xexinfo";
        return Run(Quote(recomp) + " " + Quote(written) + " " + Quote(context) + " > " + Quote(build / "recompile.log") + " 2>&1") &&
            Run(Quote(info) + " " + Quote(input) + " --stubs " + Quote(output / "import_stubs.cpp") + " > " + Quote(build / "stubs.log") + " 2>&1");
    }

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
            fprintf(stderr, "error: no ppc_context.h in %s\n", configs.string().c_str());
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
                fprintf(stderr, "error: %s doesn't name its DLL's place on the disc\n", module.string().c_str());
                return 1;
            }
            if (!RecompileOne(module, game / std::filesystem::path(match[1].str()), build / "modules" / module.stem(), build,
                    tools, context, false))
                return 1;
            printf("progress recompile %d\n", 20 + 80 * ++done / int(modules.size()));
            fflush(stdout);
        }
        printf("recompiled into %s\n", build.string().c_str());
        return 0;
    }
}

int main(int argc, char** argv)
{
    if (argc < 3)
        return Usage();
    std::string command = argv[1];
    h3setup::Iso iso;

    // --package / --jobs after the positional arguments.
    std::filesystem::path package = std::filesystem::absolute(argv[0]).parent_path();
    int jobs = int(std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::string> positional;
    for (int i = 2; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "--package" && i + 1 < argc)
            package = argv[++i];
        else if (arg == "--jobs" && i + 1 < argc)
            jobs = std::max(1, atoi(argv[++i]));
        else
            positional.push_back(arg);
    }
    if (command == "build" && positional.size() == 1)
        return h3setup::Build(positional[0], package, jobs);
    if (command == "install" && positional.size() == 2)
    {
        if (!OpenIso(iso, positional[0].c_str()) || !Check(iso) || Extract(iso, positional[1].c_str()) != 0)
            return 1;
        if (Recompile(positional[1], package / "recompiler" / "config", package / "tools") != 0)
            return 1;
        return h3setup::Build(positional[1], package, jobs);
    }
    if (command == "check")
        return OpenIso(iso, argv[2]) && Check(iso) ? 0 : 1;
    if (command == "list")
    {
        if (!OpenIso(iso, argv[2]))
            return 1;
        for (const h3setup::IsoEntry& entry : iso.Entries())
            printf("%c %12llu %s\n", entry.directory ? 'D' : 'F', static_cast<unsigned long long>(entry.size), entry.path.c_str());
        return 0;
    }
    if (command == "extract" && argc == 4)
        return OpenIso(iso, argv[2]) && Check(iso) ? Extract(iso, argv[3]) : 1;
    if (command == "recompile" && argc == 7 && std::string(argv[3]) == "--config" && std::string(argv[5]) == "--tools")
        return Recompile(argv[2], argv[4], argv[6]);
    return Usage();
}
