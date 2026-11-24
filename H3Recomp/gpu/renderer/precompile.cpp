// Shader precompile on a cold start (H3Recomp --precompile-shaders).
//
// The shader cache (SPIR-V per translated shader, and pipelines.txt, the
// pipelines the game used) normally fills as the game draws, which stutters
// the first time each shader shows up. This fills it ahead of time from the
// user's own map files and two shipped manifests:
//
//   Every shader in the maps, as is: the vtsh / pixl / glvs / glps tags'
//     compiled shader blocks point (+0x4C) at the game's runtime shader
//     object, whose +0x10 is its microcode's region: the shader's literal
//     constants (64-byte blocks), then the microcode. The microcode starts
//     at the first 64-byte step whose control flow parses (MicrocodeStart).
//     Most pixel shaders run exactly like that.
//   shader_manifest.txt (tools/shaders/build_manifest.py): per shader the
//     game is known to use, where its microcode is in a map (map, region
//     address, offset, dword count) and the patches the game's D3D code
//     applies when it binds it (texture fetch slots, vertex fetch layouts),
//     as XORs against the map's dwords, plus a hash of the result. Each is
//     rebuilt, translated and compiled exactly as the game would (same
//     translator, same cache name, same compile options).
//   pipelines.txt: the shipped pipeline manifest, copied into the cache
//     when the cache has none, so the first start's background warm-up
//     builds those pipelines (renderer/pipelines.cpp StartShaderWarmup).
//
// Nothing from the game is shipped: the microcode comes from the user's maps.

#include "gpu/renderer/internal.h"

#include "gpu/shader/translator.h"

#include "cache/cache_file.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <unordered_set>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace h3::gpu::renderer
{
    using namespace internal;

    namespace
    {
        // One shader_manifest.txt line: which map region the microcode comes
        // from, where it starts, how long it is, the hash to check, and the
        // XOR patches that turn it into the bound variant.
        struct Entry
        {
            bool vertex = false;
            std::string map;
            uint32_t address = 0, offset = 0, dwords = 0;
            uint64_t digest = 0;
            std::vector<std::pair<uint32_t, uint32_t>> patches;  // dword index, XOR
        };

        constexpr uint32_t kJunkDwords = 2;  // trailing dwords that aren't microcode (build_manifest.py)

        // FNV-1a, 64-bit: the manifest's check that a rebuilt shader is the one
        // recorded (build_manifest.py computes the same over the same bytes).
        uint64_t Fnv64(const uint8_t* bytes, size_t size)
        {
            uint64_t hash = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < size; i++)
                hash = (hash ^ bytes[i]) * 0x100000001B3ull;
            return hash;
        }

        // Parses shader_manifest.txt (format: tools/shaders/build_manifest.py):
        // "<vs|ps> <map> <region address> <offset> <dwords> <fnv64> [index:xor ...]"
        // per line; '#' comments and the "V" version line are skipped, malformed
        // lines are dropped. False if the file can't be opened.
        bool ReadManifest(const std::filesystem::path& path, std::vector<Entry>& entries)
        {
            std::ifstream file(path);
            if (!file)
                return false;
            std::string line;
            while (std::getline(file, line))
            {
                if (line.empty() || line[0] == '#' || line[0] == 'V')
                    continue;
                std::istringstream in(line);
                Entry entry;
                std::string stage, address, digest;
                in >> stage >> entry.map >> address >> entry.offset >> entry.dwords >> digest;
                if (!in || (stage != "vs" && stage != "ps") || entry.dwords <= kJunkDwords || entry.dwords > (1u << 16))
                    continue;
                entry.vertex = stage == "vs";
                entry.address = uint32_t(std::stoul(address, nullptr, 16));
                entry.digest = std::stoull(digest, nullptr, 16);
                std::string patch;
                bool ok = true;
                while (in >> patch)
                {
                    size_t colon = patch.find(':');
                    if (colon == std::string::npos)
                    {
                        ok = false;
                        break;
                    }
                    uint32_t index = uint32_t(std::stoul(patch.substr(0, colon)));
                    if (index >= entry.dwords)
                    {
                        ok = false;
                        break;
                    }
                    entry.patches.emplace_back(index, uint32_t(std::stoul(patch.substr(colon + 1), nullptr, 16)));
                }
                if (ok)
                    entries.push_back(std::move(entry));
            }
            return true;
        }
    }

    namespace
    {
        // A big-endian dword of map data.
        uint32_t Big32(const uint8_t* p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        // Where a shader region's microcode starts (after its literal
        // constants), or -1: the first 64-byte step whose control flow parses
        // (pairs of 48-bit instructions in 3 dwords; known opcodes; the first
        // exec's slot address within the data; an end instruction where the
        // first exec's slots begin). Right for every region matched against
        // the game's own loaded shaders (5029 of 5034; tools/shaders).
        int MicrocodeStart(const std::vector<uint8_t>& region)
        {
            constexpr size_t kStep = 64, kMaxOffset = 1024;
            for (size_t offset = 0; offset < std::min(region.size(), kMaxOffset); offset += kStep)
            {
                size_t dwords = (region.size() - offset) / 4;
                const uint8_t* data = region.data() + offset;
                int64_t firstExec = -1;
                bool valid = false, done = false;
                for (size_t pair = 0; !done && pair + 3 <= dwords && pair < 3 * 256; pair += 3)
                {
                    uint32_t a0 = Big32(data + pair * 4), a1 = Big32(data + pair * 4 + 4), b1 = Big32(data + pair * 4 + 8);
                    const uint32_t lows[2] = { a0, ((a1 >> 16) & 0xFFFF) | ((b1 & 0xFFFF) << 16) };
                    const uint32_t highs[2] = { a1 & 0xFFFF, (b1 >> 16) & 0xFFFF };
                    for (int k = 0; k < 2 && !done; k++)
                    {
                        uint32_t op = (highs[k] >> 12) & 0xF;
                        bool exec = op == 1 || op == 2 || op == 3 || op == 4 || op == 5 || op == 6 || op == 13 || op == 14;
                        if (exec)
                        {
                            uint32_t address = lows[k] & 0xFFF;
                            if (firstExec < 0)
                            {
                                firstExec = address;
                                if (address == 0 || size_t(address) * 3 > dwords)
                                {
                                    done = true;
                                    break;
                                }
                            }
                        }
                        else if (!(op == 0 || (op >= 7 && op <= 12) || op == 15))
                        {
                            done = true;
                            break;
                        }
                        if (op == 2 || op == 4 || op == 6 || op == 14)
                        {
                            size_t pairs = pair / 3 + 1;
                            valid = firstExec >= 0 && int64_t(pairs) <= firstExec && firstExec <= int64_t(pairs) + 1;
                            done = true;
                        }
                    }
                    if (firstExec >= 0 && int64_t(pair / 3 + 1) > firstExec + 1)
                        done = true;
                }
                if (valid)
                    return int(offset);
            }
            return -1;
        }

        // Every shader microcode in a map: (vertex, region bytes from its
        // microcode start to the next region).
        void MapShaders(native::cache::CacheFile& file, std::vector<std::pair<bool, std::vector<uint8_t>>>& out, size_t& noStart)
        {
            using native::cache::GroupTag;
            const uint32_t vtsh = GroupTag("vtsh"), pixl = GroupTag("pixl"), glvs = GroupTag("glvs"), glps = GroupTag("glps");
            std::map<uint32_t, bool> regions;  // microcode region address -> vertex
            const auto& index = file.Index();
            for (const auto& tag : index.tags)
            {
                if (tag.Empty() || !tag.address)
                    continue;
                uint32_t group = index.groups[tag.groupIndex].tag;
                bool shader = group == vtsh || group == pixl, global = group == glvs || group == glps;
                if (!shader && !global)
                    continue;
                // compiled shaders: a tag block (count, address) at +0x14
                // (vtsh, pixl) or +0x10 (glvs, glps); 0x50-byte elements.
                uint8_t block[8];
                if (!file.ReadTagData(tag.address + (shader ? 0x14 : 0x10), block, sizeof(block)))
                    continue;
                int32_t count = int32_t(Big32(block));
                uint32_t elements = Big32(block + 4);
                for (int32_t i = 0; i < count && i < 4096; i++)
                {
                    uint8_t runtime[4], object[20];
                    if (!file.ReadTagData(elements + uint32_t(i) * 0x50 + 0x4C, runtime, 4))
                        break;
                    uint32_t objectAddress = Big32(runtime);
                    if (!objectAddress || !file.ReadTagData(objectAddress, object, sizeof(object)))
                        continue;
                    if (uint32_t code = Big32(object + 16))
                        regions.emplace(code, group == vtsh || group == glvs);
                }
            }
            for (auto it = regions.begin(); it != regions.end(); ++it)
            {
                auto next = std::next(it);
                uint32_t size = next == regions.end() ? 16384 : std::min<uint32_t>(next->first - it->first, 16384);
                std::vector<uint8_t> region(size);
                while (size >= 4 && !file.ReadTagData(it->first, region.data(), size))
                    size -= 4;
                region.resize(size);
                int start = MicrocodeStart(region);
                if (start < 0)
                {
                    noStart++;
                    continue;
                }
                out.emplace_back(it->second, std::vector<uint8_t>(region.begin() + start, region.end()));
            }
        }
    }

    // `H3Recomp <xex> --precompile-shaders`: collects every shader from the
    // maps and the manifest's variants, translates and compiles them on all
    // cores into the shader cache, and installs the shipped pipeline list.
    int PrecompileShaders(const std::filesystem::path& manifestDirectory, const std::filesystem::path& mapsDirectory)
    {
        if (g_shaderCacheDirectory.empty())
        {
            fprintf(stderr, "[precompile] no shader cache directory\n");
            return 1;
        }
        std::error_code error;
        std::filesystem::create_directories(g_shaderCacheDirectory, error);
        if (!g_compiler)
            g_compiler = shaderc_compiler_initialize();

        // The manifest's patched variants (optional: without it, the maps'
        // shaders as they are).
        std::vector<Entry> entries;
        if (!ReadManifest(manifestDirectory / "shader_manifest.txt", entries))
            fprintf(stderr, "[precompile] no %s: the maps' shaders only\n", (manifestDirectory / "shader_manifest.txt").string().c_str());

        // The microcode, rebuilt (one map open at a time).
        struct Job
        {
            bool vertex;
            std::vector<uint32_t> dwords;  // host order
            bool fromMap = false;          // as in the map (not from the manifest)
        };
        std::vector<Job> jobs;
        size_t missing = 0, mismatched = 0;
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.map < b.map; });
        std::unique_ptr<native::cache::CacheFile> file;
        std::string openMap;
        for (const Entry& entry : entries)
        {
            if (entry.map != openMap)
            {
                openMap = entry.map;
                std::string message;
                file = native::cache::CacheFile::Open((mapsDirectory / (entry.map + ".map")).string(), &message);
                if (!file)
                    fprintf(stderr, "[precompile] %s: %s\n", entry.map.c_str(), message.c_str());
            }
            std::vector<uint8_t> bytes(size_t(entry.dwords) * 4);
            if (!file || !file->ReadTagData(entry.address + entry.offset, bytes.data(), bytes.size()))
            {
                missing++;
                continue;
            }
            for (const auto& [index, xor_] : entry.patches)
            {
                uint32_t big = (uint32_t(bytes[index * 4]) << 24) | (uint32_t(bytes[index * 4 + 1]) << 16) |
                               (uint32_t(bytes[index * 4 + 2]) << 8) | bytes[index * 4 + 3];
                big ^= xor_;
                for (int b = 0; b < 4; b++)
                    bytes[index * 4 + b] = uint8_t(big >> (24 - 8 * b));
            }
            if (Fnv64(bytes.data(), size_t(entry.dwords - kJunkDwords) * 4) != entry.digest)
            {
                mismatched++;  // another version of the map
                continue;
            }
            Job job{ entry.vertex, std::vector<uint32_t>(entry.dwords), false };
            for (uint32_t i = 0; i < entry.dwords; i++)
                job.dwords[i] = (uint32_t(bytes[i * 4]) << 24) | (uint32_t(bytes[i * 4 + 1]) << 16) |
                                (uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
            jobs.push_back(std::move(job));
        }
        file.reset();

        // Every shader in every map, as is.
        size_t mapShaders = 0, noStart = 0;
        std::error_code listError;
        std::vector<std::filesystem::path> maps;
        for (const auto& item : std::filesystem::directory_iterator(mapsDirectory, listError))
            if (item.path().extension() == ".map")
                maps.push_back(item.path());
        std::sort(maps.begin(), maps.end());
        for (const std::filesystem::path& path : maps)
        {
            std::unique_ptr<native::cache::CacheFile> map = native::cache::CacheFile::Open(path.string());
            if (!map)
                continue;
            std::vector<std::pair<bool, std::vector<uint8_t>>> shaders;
            MapShaders(*map, shaders, noStart);
            for (auto& [vertex, bytes] : shaders)
            {
                Job job{ vertex, std::vector<uint32_t>(bytes.size() / 4), true };
                for (size_t i = 0; i < job.dwords.size(); i++)
                    job.dwords[i] = Big32(bytes.data() + i * 4);
                jobs.push_back(std::move(job));
                mapShaders++;
            }
        }
        printf("[precompile] %zu shaders from the manifest, %zu from %zu maps (%zu without a microcode start)\n",
            jobs.size() - mapShaders, mapShaders, maps.size(), noStart);

        // Translate and compile on every core (the compiler may be used from
        // several threads; each cache file is written then renamed).
        std::atomic<size_t> next{ 0 }, done{ 0 }, failed{ 0 }, cached{ 0 }, duplicates{ 0 }, unbound{ 0 };
        std::mutex namesLock;
        std::unordered_set<std::string> names, errors;
        unsigned workers = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        for (unsigned w = 0; w < workers; w++)
            threads.emplace_back([&] {
                for (size_t i; (i = next.fetch_add(1)) < jobs.size();)
                {
                    const Job& job = jobs[i];
                    shader::TranslationResult translated = shader::Translate(job.vertex, job.dwords.data(), uint32_t(job.dwords.size()));
                    if (!translated.ok && job.fromMap && job.vertex)
                        unbound++;  // its vertex formats come when the game binds it (the manifest's variants)
                    else if (!translated.ok)
                    {
                        failed++;
                        // H3_LOG_PRECOMPILE=1: each distinct translation error.
                        static const bool log = EnvFlag("H3_LOG_PRECOMPILE");
                        if (log)
                        {
                            std::lock_guard<std::mutex> lock(namesLock);
                            if (errors.insert(translated.error).second)
                                fprintf(stderr, "[precompile] %s %zu dwords: %s\n", job.vertex ? "vs" : "ps", job.dwords.size(),
                                    translated.error.c_str());
                        }
                    }
                    else
                    {
                        std::string name = ShaderCacheName(job.vertex, translated.glsl);
                        bool first;
                        {
                            std::lock_guard<std::mutex> lock(namesLock);
                            first = names.insert(name).second;
                        }
                        std::error_code exists;
                        if (!first)
                            duplicates++;
                        else if (std::filesystem::exists(g_shaderCacheDirectory / name, exists))
                            cached++;
                        else if (CompileSpirv(job.vertex, translated.glsl, name).empty())
                            failed++;
                    }
                    // Progress for h3setup (and the launcher): "progress shaders <percent>".
                    size_t count = ++done;
                    if (count % 500 == 0 || count == jobs.size())
                    {
                        printf("progress shaders %zu\n", count * 100 / std::max<size_t>(jobs.size(), 1));
                        fflush(stdout);
                    }
                }
            });
        for (std::thread& thread : threads)
            thread.join();

        // The shipped pipeline manifest, when the cache has none yet.
        std::filesystem::path pipelines = g_shaderCacheDirectory / "pipelines.txt";
        std::filesystem::path shipped = manifestDirectory / "pipelines.txt";
        if (!std::filesystem::exists(pipelines, error) && std::filesystem::exists(shipped, error))
            std::filesystem::copy_file(shipped, pipelines, error);

        printf("[precompile] done: %zu jobs, %zu distinct shaders (%zu were cached), %zu failed, %zu map vertex shaders "
               "without vertex formats (unbound); manifest: %zu not in the maps, %zu from other map versions\n",
            jobs.size(), names.size(), cached.load(), failed.load(), unbound.load(), missing, mismatched);
        return 0;
    }
}
