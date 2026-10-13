// Host shaders and pipelines, and keeping their creation off the frame.
//
// Creating a pipeline makes the driver compile the shaders for that render
// state, which takes 50-200 ms the first time (and compiling a shader's
// SPIR-V with shaderc, 30-100 ms). Done on the GPU thread, a level's worth of
// them stalled the game for seconds. Three things help:
//
//  - Every shader and pipeline the game uses is recorded in a manifest,
//    pipelines.txt in the shader cache folder. Pipelines are identified by
//    plain data (PipelineDesc: shader cache names and render state), so the
//    record stays valid between runs.
//  - Warm-up (on by default; settings.ini precompile_shaders): at start,
//    worker threads recreate everything in the manifest from the cached
//    SPIR-V, in the background, before the game asks for it. With the
//    driver's own disk cache that's quick; after a driver update it
//    recompiles everything, still off the GPU thread.
//  - Asynchronous compilation (off by default; settings.ini async_shaders):
//    shaders and pipelines the game needs that aren't ready yet are created
//    on the workers, and draws that need them are skipped meanwhile. No
//    stalls, but objects can be missing for a few frames the first time.
//
// The manifest only ever holds what this machine has seen; nothing derived
// from the game is shipped.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        constexpr const char* kManifestFile = "pipelines.txt";
        // The manifest's first line is "V <version>". Bump the version when
        // the translator's output changes for every shader (new resource
        // layout, new declarations): all shader ids change, and warming up
        // the old manifest would build thousands of pipelines nothing uses
        // at every start. A manifest of another version is set aside as
        // pipelines.txt.old and a new one started.
        // 2: float constants in a storage buffer (draw.cpp QueueDraw).
        // 3: compact constant layouts (ShaderInfo::compactConstants).
        // 4: resident vertex memory binding (draw.cpp ResidentOffset).
        constexpr uint32_t kManifestVersion = 4;
        // Worker threads for shader and pipeline creation. The game itself
        // keeps about six host threads busy; drivers compile pipelines
        // single-threaded, so a few workers go a long way.
        constexpr unsigned kMinWorkers = 2, kMaxWorkers = 4;

        // Each translated shader has two uniform slots: the float
        // constants (used by compute shaders only; draws read them from a
        // storage buffer) and the system constants (translator.h).
        constexpr uint32_t kShaderUniformBuffers = 2;

        // --- Shared state (g_mutex) ----------------------------------------

        std::mutex g_mutex;
        std::condition_variable g_finished;  // a shader or pipeline finished (or failed)

        std::unordered_map<uint64_t, std::unique_ptr<HostShader>> g_hostShaders;  // SPIR-V cache id -> shader

        struct PipelineEntry
        {
            SDL_GPUGraphicsPipeline* pipeline = nullptr;
            bool done = false;  // created (or failed); false while a worker is on it
        };
        std::unordered_map<uint64_t, PipelineEntry> g_pipelines;  // PipelineDesc hash -> pipeline

        // --- Worker pool -----------------------------------------------------

        std::mutex g_jobMutex;
        std::condition_variable g_jobAdded;
        std::deque<std::function<void()>> g_jobs;
        std::vector<std::thread> g_workers;

        // Runs f on a worker thread (starting the workers on first use).
        void Enqueue(std::function<void()> f)
        {
            std::lock_guard lock(g_jobMutex);
            if (g_workers.empty())
            {
                unsigned count = std::clamp(std::thread::hardware_concurrency() / 4, kMinWorkers, kMaxWorkers);
                for (unsigned i = 0; i < count; i++)
                    g_workers.emplace_back([]
                    {
                        for (;;)
                        {
                            std::function<void()> job;
                            {
                                std::unique_lock lock(g_jobMutex);
                                g_jobAdded.wait(lock, [] { return !g_jobs.empty(); });
                                job = std::move(g_jobs.front());
                                g_jobs.pop_front();
                            }
                            job();
                        }
                    });
                for (std::thread& worker : g_workers)
                    worker.detach();  // they live as long as the process
            }
            g_jobs.push_back(std::move(f));
            g_jobAdded.notify_one();
        }

        // --- Manifest ----------------------------------------------------------
        //
        // One record per line, appended as new shaders and pipelines appear:
        //   S <id> <vertex 0|1> <samplers>
        //   P <vs id> <ps id> <topology> <color count> <depth format>
        //     <4 color formats> <4 blend controls> <color mask> <depth control>
        //     <stencil masks> <pixel shader color target mask>
        // Ids are the 16 hex digits of the shaders' SPIR-V cache names.

        std::mutex g_manifestMutex;
        std::unordered_set<uint64_t> g_recorded;  // shader ids and pipeline hashes already in the manifest

        std::filesystem::path ManifestPath() { return g_shaderCacheDirectory / kManifestFile; }

        // Makes sure the manifest is of this version (kManifestVersion):
        // one of another version is set aside and a new one started. Once
        // per run, before the first read or write (warm-up may be off).
        // Returns whether an existing manifest of this version is there.
        bool CheckManifestVersion()
        {
            static const bool current = [] {
                std::ifstream file(ManifestPath());
                std::string line;
                if (file && std::getline(file, line) && line == "V " + std::to_string(kManifestVersion))
                    return true;
                file.close();
                std::error_code error;
                if (std::filesystem::exists(ManifestPath(), error))
                {
                    std::filesystem::path old = ManifestPath();
                    old += ".old";
                    std::filesystem::rename(ManifestPath(), old, error);
                    fprintf(stderr, "[render] shader manifest from another version set aside as %s\n", old.string().c_str());
                }
                std::ofstream(ManifestPath()) << "V " << kManifestVersion << '\n';
                return false;
            }();
            return current;
        }

        // Appends a record (once per key).
        void Record(uint64_t key, const std::string& line)
        {
            if (g_shaderCacheDirectory.empty())
                return;
            CheckManifestVersion();
            std::lock_guard lock(g_manifestMutex);
            if (!g_recorded.insert(key).second)
                return;
            std::ofstream(ManifestPath(), std::ios::app) << line << '\n';
        }

        std::string Hex(uint64_t value)
        {
            char text[17];
            snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);
            return text;
        }

        std::string PipelineRecord(const PipelineDesc& desc)
        {
            std::ostringstream line;
            line << "P " << Hex(desc.vs) << ' ' << Hex(desc.ps) << ' ' << desc.topology << ' ' << desc.colorCount << ' ' << desc.depthFormat;
            for (uint32_t format : desc.colorFormats)
                line << ' ' << format;
            for (uint32_t blend : desc.blend)
                line << ' ' << blend;
            line << ' ' << desc.colorMask << ' ' << desc.depthControl << ' ' << desc.stencilMasks << ' ' << desc.psColorTargetMask;
            // Added later: records without it parse as no culling.
            line << ' ' << desc.rasterizer;
            return line.str();
        }

        // --- Creation ----------------------------------------------------------

        // Host blend factor for a Xenos BlendFactor (RB_BLENDCONTROL fields).
        SDL_GPUBlendFactor BlendFactor(uint32_t xenos)
        {
            switch (xenos)
            {
            case 0: return SDL_GPU_BLENDFACTOR_ZERO;
            case 1: return SDL_GPU_BLENDFACTOR_ONE;
            case 4: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
            case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
            case 6: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            case 7: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            case 8: return SDL_GPU_BLENDFACTOR_DST_COLOR;
            case 9: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
            case 10: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
            case 11: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
            case 12:  // constant color
            case 14:  // constant alpha (approximated as constant color)
                return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
            case 13:
            case 15:
                return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
            case 16: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
            default: return SDL_GPU_BLENDFACTOR_ONE;
            }
        }

        // Host blend operation for a Xenos BlendOp.
        SDL_GPUBlendOp BlendOp(uint32_t xenos)
        {
            switch (xenos)
            {
            case 1: return SDL_GPU_BLENDOP_SUBTRACT;
            case 2: return SDL_GPU_BLENDOP_MIN;
            case 3: return SDL_GPU_BLENDOP_MAX;
            case 4: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            default: return SDL_GPU_BLENDOP_ADD;
            }
        }

        // Blend state for one render target from its RB_BLENDCONTROL value:
        // color src : 5, op : 3 @5, dst : 5 @8; alpha the same @16.
        SDL_GPUColorTargetBlendState BlendState(uint32_t blend)
        {
            SDL_GPUColorTargetBlendState state{};
            state.src_color_blendfactor = BlendFactor(blend & 0x1F);
            state.color_blend_op = BlendOp((blend >> 5) & 7);
            state.dst_color_blendfactor = BlendFactor((blend >> 8) & 0x1F);
            state.src_alpha_blendfactor = BlendFactor((blend >> 16) & 0x1F);
            state.alpha_blend_op = BlendOp((blend >> 21) & 7);
            state.dst_alpha_blendfactor = BlendFactor((blend >> 24) & 0x1F);
            // 0x0001 is src one, add, dst zero: no blending.
            constexpr uint32_t kNoBlend = 0x0001;
            state.enable_blend = !((blend & 0x1FFF) == kNoBlend && ((blend >> 16) & 0x1FFF) == kNoBlend);
            return state;
        }

        // Depth/stencil state from RB_DEPTHCONTROL and RB_STENCILREFMASK.
        // Xenos compare functions and stencil ops are SDL's minus one.
        SDL_GPUDepthStencilState DepthStencilState(uint32_t depthControl, uint32_t stencilRefMask)
        {
            SDL_GPUDepthStencilState ds{};
            ds.enable_depth_test = (depthControl >> 1) & 1;
            ds.enable_depth_write = ds.enable_depth_test && ((depthControl >> 2) & 1);
            ds.compare_op = SDL_GPUCompareOp(((depthControl >> 4) & 7) + 1);
            ds.enable_stencil_test = depthControl & 1;
            auto stencil = [](uint32_t func, uint32_t fail, uint32_t pass, uint32_t depthFail) {
                SDL_GPUStencilOpState state;
                state.compare_op = SDL_GPUCompareOp(func + 1);
                state.fail_op = SDL_GPUStencilOp(fail + 1);
                state.pass_op = SDL_GPUStencilOp(pass + 1);
                state.depth_fail_op = SDL_GPUStencilOp(depthFail + 1);
                return state;
            };
            ds.front_stencil_state =
                stencil((depthControl >> 8) & 7, (depthControl >> 11) & 7, (depthControl >> 14) & 7, (depthControl >> 17) & 7);
            ds.back_stencil_state = ((depthControl >> 7) & 1)  // separate back-face stencil
                ? stencil((depthControl >> 20) & 7, (depthControl >> 23) & 7, (depthControl >> 26) & 7, (depthControl >> 29) & 7)
                : ds.front_stencil_state;
            ds.compare_mask = uint8_t(stencilRefMask >> 8);
            ds.write_mask = uint8_t(stencilRefMask >> 16);

            // Debugging switches.
            static const bool noDepth = EnvFlag("H3_NO_DEPTH"), noStencil = EnvFlag("H3_NO_STENCIL"),
                              depthFlip = EnvFlag("H3_DEPTH_FLIP");
            if (noDepth)  // no depth or stencil tests
                ds.enable_depth_test = ds.enable_stencil_test = false;
            if (noStencil)  // no stencil test
                ds.enable_stencil_test = false;
            if (depthFlip)  // swap less/greater
            {
                static const SDL_GPUCompareOp kFlip[] = { SDL_GPU_COMPAREOP_INVALID, SDL_GPU_COMPAREOP_NEVER,
                    SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_LESS,
                    SDL_GPU_COMPAREOP_NOT_EQUAL, SDL_GPU_COMPAREOP_LESS_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS };
                ds.compare_op = kFlip[ds.compare_op];
            }
            return ds;
        }

        // An SDL shader from SPIR-V (any thread).
        SDL_GPUShader* CreateShader(bool vertex, uint32_t samplers, const std::vector<uint8_t>& spirv)
        {
            if (spirv.empty())
                return nullptr;
            SDL_GPUShaderCreateInfo info{};
            info.code_size = spirv.size();
            info.code = spirv.data();
            info.entrypoint = "main";
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.stage = vertex ? SDL_GPU_SHADERSTAGE_VERTEX : SDL_GPU_SHADERSTAGE_FRAGMENT;
            info.num_samplers = samplers;
            // Vertex shaders: vertex memory, float constants (unless pushed
            // as uniforms), resident vertex memory; pixel shaders: float
            // constants (unless pushed). Draw.cpp's Flush binds them in
            // this order.
            info.num_storage_buffers = (vertex ? 2 : 0) + (g_uniformConstants ? 0 : 1);
            info.num_uniform_buffers = kShaderUniformBuffers;
            SDL_GPUShader* shader = SDL_CreateGPUShader(g_device, &info);
            if (!shader)
                LogOnce(std::string("SDL_CreateGPUShader failed: ") + SDL_GetError());
            return shader;
        }

        // Publishes a finished shader (null: failed) to waiting threads.
        void FinishShader(HostShader& host, SDL_GPUShader* shader)
        {
            {
                std::lock_guard lock(g_mutex);
                host.shader.store(shader);
                host.failed.store(shader == nullptr);
            }
            g_finished.notify_all();
        }

        // Blocks until a shader is created or has failed; its SDL shader.
        SDL_GPUShader* WaitForShader(HostShader& host)
        {
            std::unique_lock lock(g_mutex);
            g_finished.wait(lock, [&] { return host.shader.load() || host.failed.load(); });
            return host.shader.load();
        }

        HostShader* FindHostShader(uint64_t id)
        {
            std::lock_guard lock(g_mutex);
            auto found = g_hostShaders.find(id);
            return found != g_hostShaders.end() ? found->second.get() : nullptr;
        }

        // Creates the pipeline a description stands for (any thread; waits
        // for its shaders if they're still being created).
        SDL_GPUGraphicsPipeline* CreatePipeline(const PipelineDesc& desc)
        {
            HostShader* vs = FindHostShader(desc.vs);
            SDL_GPUShader* vertexShader = vs ? WaitForShader(*vs) : nullptr;
            // Occlusion query draws write their query id (queries.cpp).
            SDL_GPUShader* pixelShader = nullptr;
            if (desc.ps == kOcclusionQueryPixelShader)
                pixelShader = OcclusionQueryFragmentShader();
            else if (desc.ps == kHighlightPixelShader)
                pixelShader = HighlightFragmentShader();
            else if (HostShader* ps = FindHostShader(desc.ps))
                pixelShader = WaitForShader(*ps);
            if (!vertexShader || !pixelShader)
                return nullptr;

            SlowTimer timer{ "pipeline", Hex(desc.vs) + ".spv " + Hex(desc.ps) + ".spv" };
            SDL_GPUColorTargetDescription descriptions[4]{};
            for (uint32_t i = 0; i < desc.colorCount; i++)
            {
                descriptions[i].format = SDL_GPUTextureFormat(desc.colorFormats[i]);
                descriptions[i].blend_state = BlendState(desc.blend[i]);
                // Targets the pixel shader doesn't write keep their contents.
                uint32_t mask = (desc.colorMask >> (4 * i)) & 0xF;
                if (!(desc.psColorTargetMask & (1u << i)))
                    mask = 0;
                descriptions[i].blend_state.color_write_mask = SDL_GPUColorComponentFlags(mask);
                descriptions[i].blend_state.enable_color_write_mask = true;
            }

            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vertexShader;
            info.fragment_shader = pixelShader;
            info.primitive_type = SDL_GPUPrimitiveType(desc.topology);
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            // Culling (PA_SU_SC_MODE_CNTL). Faces are judged on screen in
            // both APIs (the NDC transform keeps the guest's on-screen
            // picture), so the guest's winding carries over as is.
            info.rasterizer_state.cull_mode = (desc.rasterizer & kRasterizerCullBack)    ? SDL_GPU_CULLMODE_BACK
                : (desc.rasterizer & kRasterizerCullFront) ? SDL_GPU_CULLMODE_FRONT
                                                           : SDL_GPU_CULLMODE_NONE;
            info.rasterizer_state.front_face =
                (desc.rasterizer & kRasterizerFrontClockwise) ? SDL_GPU_FRONTFACE_CLOCKWISE : SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            // Depth clipping unless the guest turned clipping off
            // (PA_CL_CLIP_CNTL): D3D's clear rectangles carry a z just below
            // 0 (-3.7e-9) and rely on that; clipped, the letterbox edges of
            // Halo's cutscenes were never cleared and old bloom glowed
            // through them as a yellow haze.
            info.rasterizer_state.enable_depth_clip = !(desc.rasterizer & kRasterizerNoDepthClip);
            info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            bool depth = desc.depthFormat != 0;
            if (depth)
                info.depth_stencil_state = DepthStencilState(desc.depthControl, desc.stencilMasks);
            info.target_info.color_target_descriptions = descriptions;
            info.target_info.num_color_targets = desc.colorCount;
            info.target_info.has_depth_stencil_target = depth;
            info.target_info.depth_stencil_format = depth ? SDL_GPUTextureFormat(desc.depthFormat) : g_depthFormat;

            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
            if (!pipeline)
                LogOnce(std::string("pipeline creation failed: ") + SDL_GetError());
            return pipeline;
        }

        // Creates the pipeline for an entry already in g_pipelines and
        // publishes it.
        void BuildPipeline(uint64_t key, const PipelineDesc& desc)
        {
            SDL_GPUGraphicsPipeline* pipeline = CreatePipeline(desc);
            {
                std::lock_guard lock(g_mutex);
                PipelineEntry& entry = g_pipelines[key];
                entry.pipeline = pipeline;
                entry.done = true;
            }
            g_finished.notify_all();
        }

        // Reads one P record (after the "P"). False if malformed.
        bool ParsePipeline(std::istringstream& in, PipelineDesc& desc)
        {
            std::string vs, ps;
            in >> vs >> ps >> desc.topology >> desc.colorCount >> desc.depthFormat;
            for (uint32_t& format : desc.colorFormats)
                in >> format;
            for (uint32_t& blend : desc.blend)
                in >> blend;
            in >> desc.colorMask >> desc.depthControl >> desc.stencilMasks >> desc.psColorTargetMask;
            if (!in || desc.colorCount > 4)
                return false;
            // Optional (older manifests don't have it).
            if (!(in >> desc.rasterizer))
                desc.rasterizer = 0;
            desc.vs = std::stoull(vs, nullptr, 16);
            desc.ps = std::stoull(ps, nullptr, 16);
            return true;
        }
    }

    uint64_t ShaderCacheId(const std::string& cacheName)
    {
        return std::stoull(cacheName.substr(0, 16), nullptr, 16);
    }

    HostShader* RequestHostShader(bool vertex, const std::string& glsl, uint32_t samplers, const std::string& cacheName)
    {
        uint64_t id = ShaderCacheId(cacheName);
        HostShader* host;
        {
            std::lock_guard lock(g_mutex);
            std::unique_ptr<HostShader>& slot = g_hostShaders[id];
            if (slot)
                return slot.get();  // already made (or being made, by warm-up)
            slot = std::make_unique<HostShader>();
            host = slot.get();
        }
        Record(id, "S " + Hex(id) + ' ' + (vertex ? '1' : '0') + ' ' + std::to_string(samplers));
        auto compile = [host, vertex, samplers, glsl, cacheName]
        {
            SlowTimer timer{ "shader", cacheName };
            FinishShader(*host, CreateShader(vertex, samplers, CompileSpirv(vertex, glsl, cacheName)));
        };
        if (g_asyncShaders)
            Enqueue(compile);
        else
            compile();
        return host;
    }

    bool ShaderReady(const Shader& shader, bool wait)
    {
        if (!shader.ok || !shader.host)
            return false;
        if (shader.host->shader.load())
            return true;
        return wait && WaitForShader(*shader.host) != nullptr;
    }

    SDL_GPUGraphicsPipeline* GetPipeline(const PipelineDesc& desc)
    {
        uint64_t key = Hash(&desc, sizeof(desc));
        {
            std::unique_lock lock(g_mutex);
            auto found = g_pipelines.find(key);
            if (found != g_pipelines.end())
            {
                if (found->second.done)
                    return found->second.pipeline;
                if (g_asyncShaders)
                    return nullptr;  // a worker is on it
                // Being created by warm-up: wait rather than make a second one.
                g_finished.wait(lock, [&] { return g_pipelines[key].done; });
                return g_pipelines[key].pipeline;
            }
            g_pipelines.emplace(key, PipelineEntry{});
        }
        Record(key, PipelineRecord(desc));
        if (g_asyncShaders)
        {
            Enqueue([key, desc] { BuildPipeline(key, desc); });
            return nullptr;
        }
        BuildPipeline(key, desc);
        std::lock_guard lock(g_mutex);
        return g_pipelines[key].pipeline;
    }

    void StartShaderWarmup()
    {
        if (g_shaderCacheDirectory.empty())
            return;
        if (!CheckManifestVersion())
            return;  // new: nothing to warm up yet
        std::ifstream file(ManifestPath());
        std::string line;
        std::getline(file, line);  // the version
        // Shaders first: the workers take jobs in order, so every shader is
        // started before any pipeline that waits on it.
        std::vector<std::pair<uint64_t, PipelineDesc>> pipelines;
        size_t shaders = 0;
        while (std::getline(file, line))
        {
            std::istringstream in(line);
            std::string type;
            in >> type;
            if (type == "S")
            {
                std::string id;
                int vertex = 0;
                uint32_t samplers = 0;
                in >> id >> vertex >> samplers;
                if (!in)
                    continue;
                std::string cacheName = id + ".spv";
                uint64_t key = ShaderCacheId(cacheName);
                {
                    std::lock_guard lock(g_manifestMutex);
                    g_recorded.insert(key);
                }
                std::ifstream spirvFile(g_shaderCacheDirectory / cacheName, std::ios::binary);
                if (!spirvFile)
                    continue;  // deleted from the cache: made again when the game uses it
                std::vector<uint8_t> spirv((std::istreambuf_iterator<char>(spirvFile)), std::istreambuf_iterator<char>());
                HostShader* host;
                {
                    std::lock_guard lock(g_mutex);
                    std::unique_ptr<HostShader>& slot = g_hostShaders[key];
                    if (slot)
                        continue;
                    slot = std::make_unique<HostShader>();
                    host = slot.get();
                }
                shaders++;
                Enqueue([host, vertex, samplers, spirv = std::move(spirv)] { FinishShader(*host, CreateShader(vertex != 0, samplers, spirv)); });
            }
            else if (type == "P")
            {
                PipelineDesc desc{};
                if (!ParsePipeline(in, desc))
                    continue;
                uint64_t key = Hash(&desc, sizeof(desc));
                {
                    std::lock_guard lock(g_manifestMutex);
                    g_recorded.insert(key);
                }
                pipelines.emplace_back(key, desc);
            }
        }
        size_t queued = 0;
        for (const auto& [key, desc] : pipelines)
        {
            {
                std::lock_guard lock(g_mutex);
                if (!g_hostShaders.count(desc.vs) || !g_hostShaders.count(desc.ps) || !g_pipelines.emplace(key, PipelineEntry{}).second)
                    continue;  // a shader is gone from the cache, or it's a duplicate
            }
            queued++;
            Enqueue([key = key, desc = desc] { BuildPipeline(key, desc); });
        }
        fprintf(stderr, "[render] warming up %zu shaders and %zu pipelines in the background\n", shaders, queued);
    }

    size_t PipelineCount()
    {
        std::lock_guard lock(g_mutex);
        return g_pipelines.size();
    }
}
