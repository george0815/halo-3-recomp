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

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // FNV-1a's 64-bit prime.
        constexpr uint64_t kFnvPrime = 0x100000001B3ull;
        // Work that takes at least this long on the GPU thread is logged by
        // SlowTimer (H3_LOG_SLOW): long enough to drop a 60 Hz frame.
        constexpr double kSlowThresholdMs = 20.0;
        // Each translated shader has two uniform buffers: its float
        // constants and the system constants (translator.h).
        constexpr uint32_t kShaderUniformBuffers = 2;
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

        // SPIR-V for translated GLSL: from the disk cache if it's there,
        // otherwise compiled (with the optimizer) and stored. cacheName
        // receives the cache file name, which identifies the shader in logs.
        std::vector<uint8_t> CompileCached(bool vertex, const std::string& glsl, std::string& cacheName)
        {
            char name[32];
            snprintf(name, sizeof(name), "%016llx.spv",
                (unsigned long long)Hash(glsl.data(), glsl.size(), vertex ? kVertexSeed : kPixelSeed));
            cacheName = name;

            // H3_DUMP_GLSL=1: write each shader's GLSL as <cache name>.glsl
            // in the working directory.
            static const bool dumpGlsl = EnvFlag("H3_DUMP_GLSL");
            if (dumpGlsl)
                std::ofstream(std::string(name) + ".glsl") << glsl;

            std::filesystem::path path;
            if (!g_shaderCacheDirectory.empty())
            {
                path = g_shaderCacheDirectory / name;
                std::ifstream file(path, std::ios::binary);
                if (file)
                {
                    std::vector<uint8_t> spirv((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    if (!spirv.empty() && spirv.size() % 4 == 0)  // SPIR-V is a stream of 32-bit words
                        return spirv;
                }
            }

            shaderc_compile_options_t options = CompileOptions(true);
            shaderc_compilation_result_t result = shaderc_compile_into_spv(g_compiler, glsl.data(), glsl.size(),
                vertex ? shaderc_vertex_shader : shaderc_fragment_shader, "xenos", "main", options);
            shaderc_compile_options_release(options);
            std::vector<uint8_t> spirv;
            if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success)
            {
                LogOnce(std::string("shader compilation failed: ") + shaderc_result_get_error_message(result));
            }
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

        SlowTimer timer{ "shader" };
        auto entry = std::make_unique<Shader>();
        shader::TranslationResult translated = shader::Translate(vertex, dwords, count);
        if (!translated.ok)
        {
            LogOnce("shader translation failed: " + translated.error);
        }
        else
        {
            std::vector<uint8_t> spirv = CompileCached(vertex, translated.glsl, entry->cacheName);
            if (!spirv.empty())
            {
                SDL_GPUShaderCreateInfo info{};
                info.code_size = spirv.size();
                info.code = spirv.data();
                info.entrypoint = "main";
                info.format = SDL_GPU_SHADERFORMAT_SPIRV;
                info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
                info.num_samplers = uint32_t(translated.info.textures.size());
                info.num_storage_buffers = vertex ? 1 : 0;  // vertex shaders fetch from the vertex memory buffer
                info.num_uniform_buffers = kShaderUniformBuffers;
                entry->shader = SDL_CreateGPUShader(g_device, &info);
                if (!entry->shader)
                    LogOnce(std::string("SDL_CreateGPUShader failed: ") + SDL_GetError());
                entry->info = translated.info;
                entry->ok = entry->shader != nullptr;
            }
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
