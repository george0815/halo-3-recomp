// The game's build on the player's machine (build.h).

#include "build.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace h3setup
{
    namespace fs = std::filesystem;

    namespace
    {
        // A whole file as bytes (empty if it can't be read).
        std::string ReadText(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

        // Writes a generated file only when its text changed, so its timestamp
        // doesn't make the incremental build recompile what depends on it.
        bool WriteIfDifferent(const fs::path& path, const std::string& text)
        {
            if (fs::exists(path) && ReadText(path) == text)
                return true;
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << text;
            return bool(out);
        }

        // CMake's string(MAKE_C_IDENTIFIER): other characters become '_',
        // and a leading digit gets one in front.
        std::string CIdentifier(const std::string& name)
        {
            std::string id;
            for (unsigned char c : name)
                id += std::isalnum(c) ? char(c) : '_';
            if (!id.empty() && std::isdigit((unsigned char)id[0]))
                id = "_" + id;
            return id;
        }

        // The functions an import_stubs.cpp defines as weak fallbacks
        // (PPC_WEAK_FUNC(name)).
        std::vector<std::string> WeakNames(const std::string& stubs)
        {
            std::vector<std::string> names;
            std::regex weak("PPC_WEAK_FUNC\\(([A-Za-z0-9_]+)\\)");
            for (auto it = std::sregex_iterator(stubs.begin(), stubs.end(), weak); it != std::sregex_iterator(); ++it)
                names.push_back((*it)[1]);
            return names;
        }

        // The recompiled sources XenonRecomp wrote into dir, in a stable order.
        std::vector<fs::path> Sources(const fs::path& dir)
        {
            std::vector<fs::path> files;
            std::regex pattern("ppc_recomp\\.[0-9]+\\.cpp|ppc_func_mapping\\.cpp");
            for (const auto& entry : fs::directory_iterator(dir))
                if (std::regex_match(entry.path().filename().string(), pattern))
                    files.push_back(entry.path());
            std::sort(files.begin(), files.end());
            return files;
        }

        // One recompiled DLL: its name, a C identifier made from it, its image
        // base and size, and the folder its generated code is in.
        struct Module
        {
            std::string name, ident, base, size;
            fs::path dir;
        };

        // What H3RecompLib/CMakeLists.txt generates for the DLLs: each one's
        // stubs without imports something earlier already stubs (one weak
        // fallback per import; COFF allows no duplicates), and the table of
        // modules the runtime looks DLLs up in.
        bool Glue(const fs::path& gen, std::vector<Module>& modules)
        {
            std::set<std::string> stubbed;
            for (const std::string& name : WeakNames(ReadText(gen / "ppc" / "import_stubs.cpp")))
                stubbed.insert(name);
            std::vector<fs::path> dirs;
            if (fs::exists(gen / "modules"))
                for (const auto& entry : fs::directory_iterator(gen / "modules"))
                    if (fs::exists(entry.path() / "ppc_config.h"))
                        dirs.push_back(entry.path());
            std::sort(dirs.begin(), dirs.end());

            std::string decls, entries;
            for (const fs::path& dir : dirs)
            {
                Module module{ dir.filename().string(), CIdentifier(dir.filename().string()), "", "", dir };
                std::string config = ReadText(dir / "ppc_config.h");
                std::smatch match;
                if (std::regex_search(config, match, std::regex("PPC_IMAGE_BASE (0x[0-9A-F]+)")))
                    module.base = match[1];
                if (std::regex_search(config, match, std::regex("PPC_IMAGE_SIZE (0x[0-9A-F]+)")))
                    module.size = match[1];
                if (module.base.empty() || module.size.empty())
                {
                    fprintf(stderr, "error: %s has no image base or size\n", Utf8(dir / "ppc_config.h").c_str());
                    return false;
                }
                std::string stubs = ReadText(dir / "import_stubs.cpp");
                for (const std::string& name : WeakNames(stubs))
                {
                    if (stubbed.count(name))
                        stubs = std::regex_replace(stubs, std::regex("PPC_WEAK_FUNC\\(" + name + "\\)[^\\n]*\\n"), "");
                    else
                        stubbed.insert(name);
                }
                if (!WriteIfDifferent(gen / "module_stubs" / module.name / "import_stubs.cpp", stubs))
                    return false;
                decls += "extern PPCFuncMapping PPCFuncMappings_" + module.ident + "[];\n";
                entries += "        { \"" + module.name + "\", " + module.base + ", " + module.size + ", PPCFuncMappings_" + module.ident + " },\n";
                modules.push_back(module);
            }
            std::string registry = "// Generated by h3setup from the modules in build/modules/. Do not edit.\n\n"
                                   "#include <ppc_recomp_shared.h>\n\n#include <kernel/modules.h>\n\n" +
                                   decls + "\nnamespace h3::kernel\n{\n    const RecompiledModule kRecompiledModules[] = {\n" + entries +
                                   "        { nullptr, 0, 0, nullptr },\n    };\n}\n";
            return WriteIfDifferent(gen / "recompiled_modules.cpp", registry);
        }

        // runtime/build.txt: kind -> arguments with placeholders.
        std::map<std::string, std::vector<std::string>> ReadManifest(const fs::path& path)
        {
            std::map<std::string, std::vector<std::string>> manifest;
            std::istringstream text(ReadText(path));
            std::string line;
            while (std::getline(text, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::vector<std::string> fields;
                size_t start = 0, tab;
                while ((tab = line.find('\t', start)) != std::string::npos)
                {
                    fields.push_back(line.substr(start, tab - start));
                    start = tab + 1;
                }
                fields.push_back(line.substr(start));
                if (fields.size() > 1)
                    manifest[fields[0]] = std::vector<std::string>(fields.begin() + 1, fields.end());
            }
            return manifest;
        }

        // Replaces every {key} placeholder in a build.txt argument with its value.
        std::string Expand(std::string arg, const std::map<std::string, std::string>& values)
        {
            for (const auto& [key, value] : values)
            {
                std::string token = "{" + key + "}";
                for (size_t at = arg.find(token); at != std::string::npos; at = arg.find(token, at + value.size()))
                    arg.replace(at, token.size(), value);
            }
            return arg;
        }

        // One compile: the command line, the source and the object it writes.
        struct Job
        {
            std::vector<std::string> args;
            fs::path source, object;
            bool exe = false;  // the executable's own object (not the game's)
        };
    }

    // <install>/toolchain/sdk.
    fs::path SdkDirectory(const fs::path& install)
    {
        return fs::absolute(install) / "toolchain" / "sdk";
    }

    // `h3setup sdk`: runs the package's xwin to download and unpack
    // Microsoft's CRT and Windows SDK (only with accepted: the player agreed
    // to Microsoft's license). Skipped if they're already there.
    int FetchSdk(const fs::path& install, const fs::path& package, bool accepted)
    {
        fs::path sdk = SdkDirectory(install);
        // Written only after xwin succeeds: a failed run can leave crt/ and
        // sdk/ half filled, which must not count as installed.
        fs::path complete = sdk / ".complete";
        if (fs::exists(complete))
        {
            printf("Microsoft's C runtime and Windows SDK: already in %s\n", Utf8(sdk).c_str());
            return 0;
        }
        if (!accepted)
        {
            fprintf(stderr,
                "error: building the game on Windows needs Microsoft's C runtime and Windows SDK, which xwin downloads\n"
                "from Microsoft under the license terms of Microsoft's Visual Studio build tools.\n"
                "Run again with --accept-microsoft-license if you accept them.\n");
            return 1;
        }
        fs::path xwin = package / "toolchain" /
#ifdef _WIN32
            "xwin.exe";
#else
            "xwin";
#endif
        if (!fs::exists(xwin))
        {
            fprintf(stderr, "error: %s is missing\n", Utf8(xwin).c_str());
            return 1;
        }
        std::error_code ec;
        fs::create_directories(sdk.parent_path(), ec);
        printf("progress sdk 0\ndownloading Microsoft's C runtime and Windows SDK (about 1 GB, once)\n");
        fflush(stdout);
        fs::path cache = sdk.parent_path() / "xwin-cache", log = sdk.parent_path() / "xwin.log";
        // Leftovers of an earlier failed run (a half-unpacked cache, a
        // half-filled sdk/) make xwin fail again: start clean. The second
        // attempt copies out of the unpack directories instead of moving, in
        // case something (an antivirus scan) held or removed a file mid-move.
        int status = -1;
        for (int attempt = 0; attempt < 2 && status != 0; attempt++)
        {
            fs::remove_all(cache / "unpack", ec);
            fs::remove_all(sdk, ec);
            std::vector<std::string> args = { Utf8(xwin), "--accept-license", "--arch", "x86_64", "--cache-dir", Utf8(cache), "splat",
                "--output", Utf8(sdk) };
            if (attempt > 0)
            {
                printf("xwin failed (%d); retrying once from a clean cache\n", status);
                fflush(stdout);
                args.push_back("--copy");
            }
            status = RunProgram(args, log);
        }
        if (status != 0)
        {
            fprintf(stderr, "error: xwin failed (%d); see %s\n", status, Utf8(log).c_str());
            return 1;
        }
        std::ofstream(complete) << "xwin splat done\n";
        fs::remove_all(cache, ec);  // the downloads, no longer needed
        printf("progress sdk 100\n");
        return 0;
    }

    // Held for the rest of the process (the OS drops it if h3setup dies), so
    // a second install into the same directory (a second launcher window)
    // stops instead of sharing xwin's cache and the build directory.
    bool LockInstall(const fs::path& install)
    {
        std::error_code ec;
        fs::create_directories(install, ec);
        fs::path path = fs::absolute(install) / "h3setup.lock";
#ifdef _WIN32
        HANDLE lock = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        bool locked = lock != INVALID_HANDLE_VALUE;
#else
        int lock = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        bool locked = lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0;
#endif
        if (!locked)
            fprintf(stderr, "error: another install is already running in %s\n", Utf8(fs::absolute(install)).c_str());
        return locked;
    }

    // Starts a program directly (CreateProcessW on Windows, posix_spawnp
    // elsewhere: no shell, so paths with spaces need no quoting games),
    // with its output going to the log, and waits for it.
    int RunProgram(const std::vector<std::string>& args, const fs::path& log)
    {
#ifdef _WIN32
        auto wide = [](const std::string& text) {
            std::wstring result(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), int(result.size()));
            return result;
        };
        std::wstring line;
        for (const std::string& arg : args)
        {
            std::wstring warg = wide(arg);
            if (!line.empty())
                line += L' ';
            // CommandLineToArgvW's quoting: backslashes before a quote double.
            line += L'"';
            size_t slashes = 0;
            for (wchar_t c : warg)
            {
                if (c == L'\\')
                    slashes++;
                else
                {
                    if (c == L'"')
                        line.append(slashes + 1, L'\\');
                    slashes = 0;
                }
                line += c;
            }
            line.append(slashes, L'\\');
            line += L'"';
        }
        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        HANDLE out = CreateFileW(log.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{ sizeof(startup) };
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = out;
        startup.hStdError = out;
        PROCESS_INFORMATION process{};
        BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
        CloseHandle(out);
        if (!started)
            return -1;
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
        return int(code);
#else
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        std::vector<char*> argv;
        for (const std::string& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        pid_t pid;
        int error = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        if (error != 0)
            return -1;
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
    }

    // The compiler: $CXX if set, else the release's own clang-cl (Windows),
    // else clang++/clang-cl from the PATH.
    std::string FindCompiler(const fs::path& package)
    {
        if (const char* cxx = getenv("CXX"); cxx && *cxx)
            return cxx;
#ifdef _WIN32
        // The release's own (trimmed LLVM, toolchain/llvm).
        if (fs::path shipped = package / "toolchain" / "llvm" / "bin" / "clang-cl.exe"; fs::exists(shipped))
            return Utf8(shipped);
#endif
#ifdef _WIN32
        const char* names[] = { "clang-cl.exe" };
        const char separator = ';';
#else
        const char* names[] = { "clang++", "clang++-20", "clang++-19", "clang++-18" };
        const char separator = ':';
#endif
        const char* pathVariable = getenv("PATH");
        std::string paths = pathVariable ? pathVariable : "";
        for (const char* name : names)
        {
            size_t start = 0;
            while (start <= paths.size())
            {
                size_t end = paths.find(separator, start);
                std::string dir = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if (!dir.empty() && fs::exists(fs::path(dir) / name))
                    return Utf8(fs::path(dir) / name);
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
        }
        return "";
    }

    // `h3setup build`: generates the glue sources, compiles everything that's
    // missing or out of date in parallel (jobs threads), links H3Recomp and
    // copies the libraries it needs next to it.
    int Build(const fs::path& installArg, const fs::path& packageArg, int jobs)
    {
        fs::path install = fs::absolute(installArg), package = fs::absolute(packageArg);
        fs::path gen = install / "build", objects = gen / "obj", log = gen / "build.log";
        auto manifest = ReadManifest(package / "runtime" / "build.txt");
        for (const char* kind : { "game", "module", "module_stubs", "exe_source", "link" })
            if (!manifest.count(kind))
            {
                fprintf(stderr, "error: %s has no '%s' command\n", Utf8(package / "runtime" / "build.txt").c_str(), kind);
                return 1;
            }
        std::string compiler = FindCompiler(package);
        if (compiler.empty())
        {
#ifdef _WIN32
            fprintf(stderr, "error: the compiler that comes with this release is missing (toolchain\\llvm\\bin\\clang-cl.exe). "
                            "Download the release again.\n");
#else
            fprintf(stderr, "error: no C++ compiler found. Install clang 18 or newer: Ubuntu/Debian: sudo apt install clang; "
                            "Fedora: sudo dnf install clang; Arch: sudo pacman -S clang; openSUSE: sudo zypper install clang. "
                            "Or set CXX to a clang++.\n");
#endif
            return 1;
        }
        printf("compiler: %s\n", compiler.c_str());
        // Windows: lld-link beside clang-cl, and the CRT/SDK `sdk` fetched.
        fs::path linker = FromUtf8(compiler).parent_path() / "lld-link.exe";
        fs::path sdk = SdkDirectory(install);
#ifdef _WIN32
        if (!fs::exists(sdk / "crt" / "include"))
        {
            fprintf(stderr, "error: Microsoft's C runtime and Windows SDK aren't here yet (h3setup sdk)\n");
            return 1;
        }
#endif

        std::vector<Module> modules;
        if (!Glue(gen, modules))
            return 1;

        std::map<std::string, std::string> common{ { "cxx", compiler }, { "pkg", Utf8(package) }, { "gen", Utf8(gen) },
            { "ld", Utf8(linker) }, { "sdk", Utf8(sdk) } };
        std::vector<Job> work;
        auto add = [&](const std::string& kind, const fs::path& source, const fs::path& object, std::map<std::string, std::string> values) {
            values.insert(common.begin(), common.end());
            values["in"] = Utf8(source);
            values["out"] = Utf8(object);
            Job job{ {}, source, object, kind == "module_stubs" || kind == "exe_source" };
            for (const std::string& arg : manifest[kind])
                job.args.push_back(Expand(arg, values));
            work.push_back(job);
        };
        const char* objectSuffix =
#ifdef _WIN32
            ".obj";
#else
            ".o";
#endif
        for (const fs::path& source : Sources(gen / "ppc"))
            add("game", source, objects / "ppc" / (source.filename().string() + objectSuffix), {});
        for (const Module& module : modules)
        {
            std::map<std::string, std::string> values{ { "name", module.name }, { "ident", module.ident } };
            for (const fs::path& source : Sources(module.dir))
                add("module", source, objects / "modules" / module.name / (source.filename().string() + objectSuffix), values);
            add("module_stubs", gen / "module_stubs" / module.name / "import_stubs.cpp",
                objects / "module_stubs" / (module.name + objectSuffix), values);
        }
        add("exe_source", gen / "ppc" / "import_stubs.cpp", objects / ("import_stubs" + std::string(objectSuffix)), {});
        add("exe_source", gen / "recompiled_modules.cpp", objects / ("recompiled_modules" + std::string(objectSuffix)), {});

        // Compile what's missing or older than its source, in parallel.
        std::error_code ec;
        fs::remove(log, ec);
        std::vector<Job*> pending;
        for (Job& job : work)
        {
            fs::create_directories(job.object.parent_path(), ec);
            if (!fs::exists(job.object) || fs::last_write_time(job.object) < fs::last_write_time(job.source))
                pending.push_back(&job);
        }
        printf("compiling %zu of %zu files with %d jobs\n", pending.size(), work.size(), jobs);
        fflush(stdout);
        std::atomic<size_t> next{ 0 }, done{ 0 };
        std::atomic<bool> failed{ false };
        std::mutex outputMutex;
        int lastPercent = -1;
        auto worker = [&](int index) {
            fs::path jobLog = gen / ("compile_" + std::to_string(index) + ".log");
            for (size_t i; !failed && (i = next++) < pending.size();)
            {
                Job& job = *pending[i];
                fs::path temporary = job.object;
                temporary += ".part";
                std::vector<std::string> args = job.args;
                std::replace(args.begin(), args.end(), Utf8(job.object), Utf8(temporary));
                if (RunProgram(args, jobLog) != 0)
                {
                    std::lock_guard lock(outputMutex);
                    fprintf(stderr, "error: compiling %s failed (see %s)\n", Utf8(job.source).c_str(), Utf8(jobLog).c_str());
                    failed = true;
                    return;
                }
                fs::rename(temporary, job.object, ec);
                std::lock_guard lock(outputMutex);
                int percent = int(++done * 100 / pending.size());
                if (percent != lastPercent)
                {
                    printf("progress compile %d\n", percent);
                    fflush(stdout);
                    lastPercent = percent;
                }
            }
        };
        std::vector<std::thread> threads;
        for (int i = 0; i < std::max(1, jobs); i++)
            threads.emplace_back(worker, i);
        for (std::thread& thread : threads)
            thread.join();
        if (failed)
            return 1;

        // Link: the objects through a response file (too many for a
        // Windows command line).
        fs::path exe = install /
#ifdef _WIN32
            "H3Recomp.exe";
#else
            "H3Recomp";
#endif
        std::string exeList, gameList;
        for (const Job& job : work)
            (job.exe ? exeList : gameList) += "\"" + Utf8(job.object) + "\"\n";
        fs::path exeResponse = gen / "exe_objects.rsp", gameResponse = gen / "game_objects.rsp";
        WriteIfDifferent(exeResponse, exeList);
        WriteIfDifferent(gameResponse, gameList);
        std::vector<std::string> link;
        std::map<std::string, std::string> values = common;
        values["exe"] = Utf8(exe);
        for (const std::string& arg : manifest["link"])
        {
            if (arg == "{exe_objects}")
                link.push_back("@" + Utf8(exeResponse));
            else if (arg == "{game_objects}")
                link.push_back("@" + Utf8(gameResponse));
            else
                link.push_back(Expand(arg, values));
        }
        printf("linking %s\n", Utf8(exe).c_str());
        fflush(stdout);
        if (RunProgram(link, log) != 0)
        {
            fprintf(stderr, "error: linking failed (see %s)\n", Utf8(log).c_str());
            return 1;
        }

        // The shared libraries it loads: Linux in lib/ (rpath $ORIGIN/lib),
        // Windows next to it.
        fs::create_directories(install / "lib", ec);
        for (const auto& entry : fs::directory_iterator(package / "runtime" / "lib"))
        {
            std::string name = entry.path().filename().string();
            if (name.find(".so") != std::string::npos)
            {
                fs::remove(install / "lib" / name, ec);
                fs::copy(entry.path(), install / "lib" / name, fs::copy_options::copy_symlinks, ec);
            }
            else if (entry.path().extension() == ".dll" && entry.path().parent_path() != install)
                fs::copy_file(entry.path(), install / name, fs::copy_options::overwrite_existing, ec);
        }
        printf("built %s\n", Utf8(exe).c_str());
        return 0;
    }
}
