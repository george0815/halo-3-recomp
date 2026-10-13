// Guest shaders as host shaders: Xenos microcode is translated to GLSL
// (gpu/shader/translator.cpp), compiled to SPIR-V with shaderc, and turned
// into SDL_GPU shaders. Compiled SPIR-V is cached on disk because compiling
// a level's worth of shaders stalls the command processor for seconds.
//
// Also the renderer's small shared helpers (hashing, logging, fetch
// constants), declared in internal.h.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // FNV-1a's 64-bit prime.
        constexpr uint64_t kFnvPrime = 0x100000001B3ull;
        // Work that takes at least this long on the GPU thread is logged by
        // SlowTimer (H3_LOG_SLOW): long enough to drop a 60 Hz frame.
        constexpr double kSlowThresholdMs = 20.0;
        // Hash seeds, so a vertex and a pixel shader with the same microcode
        // (or GLSL) never share a cache entry.
        constexpr uint64_t kVertexSeed = 1, kPixelSeed = 2;

        // guest microcode hash -> shader
        std::unordered_map<uint64_t, std::unique_ptr<Shader>> g_shaders;

        // shaderc options for SPIR-V that SDL_GPU's Vulkan backend accepts.
        shaderc_compile_options_t CompileOptions(bool optimize)
        {
            shaderc_compile_options_t options = shaderc_compile_options_initialize();
            shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
            if (optimize)
                shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);
            return options;
        }

    }

    uint64_t Hash(const void* data, size_t size, uint64_t hash)
    {
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        size_t words = size / 8;
        for (size_t i = 0; i < words; i++)
        {
            uint64_t word;
            memcpy(&word, bytes + i * 8, 8);
            hash = (hash ^ word) * kFnvPrime;
        }
        for (size_t i = words * 8; i < size; i++)
            hash = (hash ^ bytes[i]) * kFnvPrime;
        return hash;
    }

    std::string ShaderCacheName(bool vertex, const std::string& glsl)
    {
        char name[32];
        snprintf(name, sizeof(name), "%016llx.spv", (unsigned long long)Hash(glsl.data(), glsl.size(), vertex ? kVertexSeed : kPixelSeed));
        return name;
    }

    std::vector<uint8_t> CompileSpirv(ShaderStage stage, const std::string& glsl, const std::string& cacheName)
    {
        // H3_DUMP_GLSL=1: write each shader's GLSL as <cache name>.glsl in
        // the working directory.
        static const bool dumpGlsl = EnvFlag("H3_DUMP_GLSL");
        if (dumpGlsl)
            std::ofstream(cacheName + ".glsl") << glsl;

        std::filesystem::path path;
        if (!g_shaderCacheDirectory.empty())
        {
            path = g_shaderCacheDirectory / cacheName;
            std::ifstream file(path, std::ios::binary);
            if (file)
            {
                std::vector<uint8_t> spirv((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                if (!spirv.empty() && spirv.size() % 4 == 0)  // SPIR-V is a stream of 32-bit words
                    return spirv;
            }
        }

        // Not cached: compile with the optimizer (shaderc's compiler object
        // may be used from several threads at once).
        shaderc_compile_options_t options = CompileOptions(true);
        shaderc_compilation_result_t result = shaderc_compile_into_spv(g_compiler, glsl.data(), glsl.size(),
            stage == ShaderStage::Vertex ? shaderc_vertex_shader
                : stage == ShaderStage::Fragment ? shaderc_fragment_shader
                                                 : shaderc_compute_shader,
            "xenos", "main", options);
        shaderc_compile_options_release(options);
        std::vector<uint8_t> spirv;
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
            LogOnce(std::string("shader compilation failed: ") + shaderc_result_get_error_message(result));
        else
        {
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(shaderc_result_get_bytes(result));
            spirv.assign(bytes, bytes + shaderc_result_get_length(result));
            if (!path.empty())
            {
                // Write then rename, so a killed run never leaves half a file.
                std::filesystem::path temporary = path;
                temporary += ".tmp";
                std::ofstream(temporary, std::ios::binary).write(reinterpret_cast<const char*>(spirv.data()), spirv.size());
                std::error_code error;
                std::filesystem::rename(temporary, path, error);
            }
        }
        shaderc_result_release(result);
        return spirv;
    }

    uint64_t ContentHash(const void* data, size_t size)
    {
        return XXH3_64bits(data, size);
    }

    void LogOnce(const std::string& message)
    {
        static std::mutex mutex;
        static std::vector<std::string> seen;
        std::lock_guard lock(mutex);
        if (std::find(seen.begin(), seen.end(), message) != seen.end())
            return;
        seen.push_back(message);
        fprintf(stderr, "[render] %s\n", message.c_str());
    }

    SlowTimer::~SlowTimer()
    {
        static const bool enabled = EnvFlag("H3_LOG_SLOW");
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (enabled && ms >= kSlowThresholdMs)
            fprintf(stderr, "[render] slow %s: %.0f ms (frame %u) %s\n", what, ms, g_swaps, detail.c_str());
    }

    VertexFetch ReadVertexFetch(uint32_t index)
    {
        // Vertex fetch constants share the fetch constant registers with
        // texture fetch constants: 2 dwords each instead of 6.
        uint32_t word0 = ReadRegister(kRegShaderConstantFetch + index * 2);
        uint32_t word1 = ReadRegister(kRegShaderConstantFetch + index * 2 + 1);
        VertexFetch fetch;
        fetch.valid = (word0 & 3) == 3;  // fetch constant type 3: vertex
        fetch.address = word0 & ~3u;
        fetch.dwords = (word1 >> 2) & 0xFFFFFF;
        fetch.endian = word1 & 3;
        return fetch;
    }

    float VertexFloat(const VertexFetch& fetch, uint32_t i)
    {
        uint32_t raw;
        memcpy(&raw, GuestPtr<uint8_t>(PhysicalToGuest(fetch.address)) + i * 4, 4);
        raw = SwapEndian32(raw, fetch.endian);
        float value;
        memcpy(&value, &raw, 4);
        return value;
    }

    texture::FetchConstant ReadTextureFetch(uint32_t index)
    {
        constexpr uint32_t kTextureFetchDwords = 6;
        uint32_t words[kTextureFetchDwords];
        for (uint32_t i = 0; i < kTextureFetchDwords; i++)
            words[i] = ReadRegister(kRegShaderConstantFetch + index * kTextureFetchDwords + i);
        return texture::ParseFetchConstant(words);
    }

    Shader* LoadShader(bool vertex, const uint32_t* dwords, uint32_t count)
    {
        uint64_t key = Hash(dwords, count * 4, vertex ? kVertexSeed : kPixelSeed);
        auto it = g_shaders.find(key);
        if (it != g_shaders.end())
            return it->second.get();

        auto entry = std::make_unique<Shader>();
        // The capture's name for it (frame_capture.cpp: FNV-1a over the
        // big-endian dwords, as in guest memory), for debug switches.
        entry->captureHash = 0xCBF29CE484222325ull;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t big = __builtin_bswap32(dwords[i]);
            const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&big);
            for (int b = 0; b < 4; b++)
                entry->captureHash = (entry->captureHash ^ bytes[b]) * 0x100000001B3ull;
        }
        shader::TranslationResult translated = shader::Translate(vertex, dwords, count);
        if (!translated.ok)
            LogOnce("shader translation failed: " + translated.error);
        else
        {
            // The program itself is shared by cache name (and may already be
            // there from warm-up, or still compiling on a worker).
            entry->cacheName = ShaderCacheName(vertex, translated.glsl);
            entry->host = RequestHostShader(vertex, translated.glsl, uint32_t(translated.info.textures.size()), entry->cacheName);
            entry->info = translated.info;
            // (Also kept for H3_DEBUG_VS_POSITIONS, which runs any vertex
            // shader's compute variant.)
            static const bool debugPositions = EnvString("H3_DEBUG_VS_POSITIONS") != nullptr;
            if (translated.info.usesMemExport || (vertex && debugPositions))
                entry->microcode.assign(dwords, dwords + count);
            entry->ok = true;
        }
        Shader* shader = entry.get();
        g_shaders.emplace(key, std::move(entry));
        return shader;
    }

    size_t ShaderCount()
    {
        return g_shaders.size();
    }

    SDL_GPUShader* CompileInternal(const char* glsl, bool vertex, uint32_t samplers, uint32_t uniformBuffers)
    {
        shaderc_compile_options_t options = CompileOptions(false);
        shaderc_compilation_result_t result = shaderc_compile_into_spv(g_compiler, glsl, strlen(glsl),
            vertex ? shaderc_vertex_shader : shaderc_fragment_shader, "internal", "main", options);
        shaderc_compile_options_release(options);
        SDL_GPUShader* shader = nullptr;
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
            LogOnce(std::string("internal shader compilation failed: ") + shaderc_result_get_error_message(result));
        else
        {
            SDL_GPUShaderCreateInfo info{};
            info.code_size = shaderc_result_get_length(result);
            info.code = reinterpret_cast<const Uint8*>(shaderc_result_get_bytes(result));
            info.entrypoint = "main";
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
            info.num_samplers = samplers;
            info.num_uniform_buffers = uniformBuffers;
            shader = SDL_CreateGPUShader(g_device, &info);
        }
        shaderc_result_release(result);
        return shader;
    }
}
