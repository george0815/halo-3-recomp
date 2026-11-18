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
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace h3setup
{
    namespace fs = std::filesystem;

    namespace
    {
        std::string ReadText(const fs::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            return text.str();
        }

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

        std::vector<std::string> WeakNames(const std::string& stubs)
        {
            std::vector<std::string> names;
            std::regex weak("PPC_WEAK_FUNC\\(([A-Za-z0-9_]+)\\)");
            for (auto it = std::sregex_iterator(stubs.begin(), stubs.end(), weak); it != std::sregex_iterator(); ++it)
                names.push_back((*it)[1]);
            return names;
        }

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
                    fprintf(stderr, "error: %s has no image base or size\n", (dir / "ppc_config.h").string().c_str());
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

        struct Job
        {
            std::vector<std::string> args;
            fs::path source, object;
        };
    }

    int RunProgram(const std::vector<std::string>& args, const fs::path& log)
    {
#ifdef _WIN32
        std::wstring line;
        for (const std::string& arg : args)
        {
            std::wstring warg = fs::path(arg).wstring();
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

    std::string FindCompiler()
    {
        if (const char* cxx = getenv("CXX"); cxx && *cxx)
            return cxx;
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
                    return (fs::path(dir) / name).string();
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
        }
        return "";
    }

    int Build(const fs::path& installArg, const fs::path& packageArg, int jobs)
    {
        fs::path install = fs::absolute(installArg), package = fs::absolute(packageArg);
        fs::path gen = install / "build", objects = gen / "obj", log = gen / "build.log";
        auto manifest = ReadManifest(package / "runtime" / "build.txt");
        for (const char* kind : { "game", "module", "module_stubs", "exe_source", "link" })
            if (!manifest.count(kind))
            {
                fprintf(stderr, "error: %s has no '%s' command\n", (package / "runtime" / "build.txt").string().c_str(), kind);
                return 1;
            }
        std::string compiler = FindCompiler();
        if (compiler.empty())
        {
            fprintf(stderr, "error: no C++ compiler found. Install clang (18 or newer), or set CXX.\n");
            return 1;
        }
        printf("compiler: %s\n", compiler.c_str());

        std::vector<Module> modules;
        if (!Glue(gen, modules))
            return 1;

        std::map<std::string, std::string> common{ { "cxx", compiler }, { "pkg", package.generic_string() }, { "gen", gen.generic_string() } };
        std::vector<Job> work;
        auto add = [&](const std::string& kind, const fs::path& source, const fs::path& object, std::map<std::string, std::string> values) {
            values.insert(common.begin(), common.end());
            values["in"] = source.generic_string();
            values["out"] = object.generic_string();
            Job job{ {}, source, object };
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
                std::replace(args.begin(), args.end(), job.object.generic_string(), temporary.generic_string());
                if (RunProgram(args, jobLog) != 0)
                {
                    std::lock_guard lock(outputMutex);
                    fprintf(stderr, "error: compiling %s failed (see %s)\n", job.source.string().c_str(), jobLog.string().c_str());
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
        std::string list;
        for (const Job& job : work)
            list += "\"" + job.object.generic_string() + "\"\n";
        fs::path response = gen / "objects.rsp";
        WriteIfDifferent(response, list);
        std::vector<std::string> link;
        std::map<std::string, std::string> values = common;
        values["exe"] = exe.generic_string();
        for (const std::string& arg : manifest["link"])
            link.push_back(arg == "{objects}" ? "@" + response.generic_string() : Expand(arg, values));
        printf("linking %s\n", exe.string().c_str());
        fflush(stdout);
        if (RunProgram(link, log) != 0)
        {
            fprintf(stderr, "error: linking failed (see %s)\n", log.string().c_str());
            return 1;
        }

        // The shared libraries it loads, next to it in lib/ (rpath $ORIGIN/lib).
        fs::create_directories(install / "lib", ec);
        for (const auto& entry : fs::directory_iterator(package / "runtime" / "lib"))
        {
            std::string name = entry.path().filename().string();
            if (name.find(".so") != std::string::npos || entry.path().extension() == ".dll")
                fs::copy(entry.path(), install / "lib" / name, fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, ec);
        }
        printf("built %s\n", exe.string().c_str());
        return 0;
    }
}
