// Occlusion queries (EVENT_WRITE_ZPD). The game asks how many samples of a
// draw pass the depth test: D3D writes a sample count report before and
// after the draw (EVENT_WRITE_ZPD to RB_SAMPLE_COUNT_ADDR) and the game
// subtracts them. Halo 3 fades tac lights and other light glows by it: a
// depth-tested quad at the light, color writes off, between two reports.
//
// SDL_GPU has no occlusion queries, so the samples are counted by hand:
//
//   - Each report closes the interval since the previous report and opens
//     the next. Depth-only draws inside an interval are drawn a second time
//     into an R32_UINT "query id" texture (the interval's number per frame;
//     same depth test, no depth writes) instead of nowhere.
//   - At the end of the frame a compute pass counts the texels of each id.
//     The counts come back a frame or two later and are kept per report
//     address (D3D gives each query object its own report memory).
//   - Reports are written right away, as the game may read them as soon as
//     the command processor passes them: each interval's count is the last
//     one measured for the same end address (0 before the first). Lights
//     fade in a frame late, which is invisible next to their own fading.
//
// Intervals whose draws aren't all depth-only (D3D's own uses, if any) keep
// the old fake count so they never read as hidden. H3_FAKE_ZPD=1 reports
// every interval as visible, as before.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <cstdio>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // Query ids per frame (more are counted as id 0, i.e. not at all).
        constexpr uint32_t kMaxQueries = 255;
        // What an interval without measurement or with other draws reports
        // (the old fake behaviour: plenty of samples).
        constexpr uint32_t kFakeSamples = 1000;
        // A sample count report: Total, ZFail, ZPass, StencilFail, each as A
        // and B lanes the game adds up (little-endian dwords, as in Xenia's
        // xe_gpu_depth_sample_counts).
        constexpr uint32_t kReportDwords = 8;
        constexpr uint32_t kReportTotalA = 0, kReportZPassA = 4;
        // Compute workgroup size of the counting pass (8x8 texels).
        constexpr uint32_t kCountGroupSize = 8;

        // Query pixel shader: writes the draw's query id (system constants,
        // queryId) to the id texture. %u: the field's byte offset.
        const char* kQueryFragmentShader = R"(#version 450
layout(std140, set = 3, binding = 1) uniform XeSystem
{
    layout(offset = %u) uint query_id;
} xe_sys;
layout(location = 0) out uint xe_query;
void main()
{
    xe_query = xe_sys.query_id;
}
)";

        // Counting pass: one invocation per texel, atomically adding to its
        // id's counter.
        const char* kCountComputeShader = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform usampler2D xe_ids;
layout(std430, set = 1, binding = 0) buffer XeCounts { uint counts[]; };
void main()
{
    ivec2 size = textureSize(xe_ids, 0);
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= size.x || p.y >= size.y)
        return;
    uint id = texelFetch(xe_ids, p, 0).r;
    if (id != 0u)
        atomicAdd(counts[id], 1u);
}
)";

        // One interval between two reports.
        struct Interval
        {
            uint32_t endAddress = 0;
            bool queryDraws = false;  // depth-only draws drawn into the id texture
            bool otherDraws = false;  // anything else
        };

        // This frame's intervals (id = index + 1); the open one is the last.
        std::vector<Interval> g_intervals;
        bool g_open = false;
        // Running total the reports count up from.
        uint32_t g_total = 0;
        // Last measured samples per end report address.
        std::unordered_map<uint32_t, uint32_t> g_measured;

        SDL_GPUTexture* g_ids = nullptr;            // query id per texel
        bool g_idsCleared = false;                  // this frame
        SDL_GPUComputePipeline* g_countPipeline = nullptr;
        SDL_GPUBuffer* g_counts = nullptr;          // (kMaxQueries + 1) counters
        SDL_GPUBuffer* g_zeros = nullptr;           // as many zeros

        // A counting pass whose results are on their way back.
        struct Readback
        {
            SDL_GPUFence* fence = nullptr;
            SDL_GPUTransferBuffer* download = nullptr;
            std::vector<uint32_t> endAddresses;  // by id - 1
        };
        std::vector<Readback> g_readbacks;  // oldest first

        constexpr uint32_t kCountBytes = (kMaxQueries + 1) * 4;

        bool Disabled()
        {
            static const bool fake = EnvFlag("H3_FAKE_ZPD");
            return fake;
        }

        // The id texture, counter buffers and counting pipeline (first use).
        bool EnsureResources()
        {
            if (g_ids && g_counts && g_zeros && g_countPipeline)
                return true;
            if (!g_ids)
            {
                SDL_GPUTextureCreateInfo info{};
                info.type = SDL_GPU_TEXTURETYPE_2D;
                info.format = SDL_GPU_TEXTUREFORMAT_R32_UINT;
                info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                info.width = TargetWidth();
                info.height = TargetHeight();
                info.layer_count_or_depth = 1;
                info.num_levels = 1;
                g_ids = SDL_CreateGPUTexture(g_device, &info);
            }
            if (!g_counts || !g_zeros)
            {
                SDL_GPUBufferCreateInfo info{};
                info.size = kCountBytes;
                info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
                g_counts = SDL_CreateGPUBuffer(g_device, &info);
                info.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
                g_zeros = SDL_CreateGPUBuffer(g_device, &info);
                // Fill the zero buffer once.
                SDL_GPUTransferBufferCreateInfo transfer{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kCountBytes, 0 };
                SDL_GPUTransferBuffer* upload = SDL_CreateGPUTransferBuffer(g_device, &transfer);
                if (!g_zeros || !upload)
                    return false;
                memset(SDL_MapGPUTransferBuffer(g_device, upload, false), 0, kCountBytes);
                SDL_UnmapGPUTransferBuffer(g_device, upload);
                SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
                SDL_GPUTransferBufferLocation source{ upload, 0 };
                SDL_GPUBufferRegion destination{ g_zeros, 0, kCountBytes };
                SDL_UploadToGPUBuffer(copy, &source, &destination, false);
                SDL_EndGPUCopyPass(copy);
                SDL_ReleaseGPUTransferBuffer(g_device, upload);
            }
            if (!g_countPipeline)
            {
                std::vector<uint8_t> spirv = CompileSpirv(ShaderStage::Compute, kCountComputeShader, "query_count.spv");
                if (spirv.empty())
                    return false;
                SDL_GPUComputePipelineCreateInfo info{};
                info.code_size = spirv.size();
                info.code = spirv.data();
                info.entrypoint = "main";
                info.format = SDL_GPU_SHADERFORMAT_SPIRV;
                info.num_samplers = 1;
                info.num_readwrite_storage_buffers = 1;
                info.threadcount_x = info.threadcount_y = kCountGroupSize;
                info.threadcount_z = 1;
                g_countPipeline = SDL_CreateGPUComputePipeline(g_device, &info);
            }
            return g_ids && g_counts && g_zeros && g_countPipeline;
        }

        // Writes a report: the running total as both the total and the
        // samples passed, split over the A and B lanes.
        void WriteReport(uint32_t address)
        {
            uint32_t counts[kReportDwords] = {};
            counts[kReportTotalA] = counts[kReportZPassA] = g_total - (g_total >> 1);
            counts[kReportTotalA + 1] = counts[kReportZPassA + 1] = g_total >> 1;
            memcpy(GuestPtr<uint8_t>(PhysicalToGuest(address & ~3u)), counts, sizeof(counts));
        }
    }

    void OnOcclusionReport(uint32_t address)
    {
        if (Disabled())
        {
            g_total += kFakeSamples;
            WriteReport(address);
            return;
        }
        // Close the open interval: its samples are the last measurement for
        // this end address (or the fake count if it held other draws).
        if (g_open)
        {
            Interval& interval = g_intervals.back();
            interval.endAddress = address;
            if (interval.otherDraws)
                g_total += kFakeSamples;
            else if (interval.queryDraws)
            {
                auto found = g_measured.find(address);
                g_total += found != g_measured.end() ? found->second : 0;
            }
        }
        WriteReport(address);
        // Open the next one.
        if (g_intervals.size() < kMaxQueries)
        {
            g_intervals.push_back({});
            g_open = true;
        }
        else
            g_open = false;
    }

    uint32_t OcclusionQueryDraw(uint32_t mode)
    {
        if (!g_open || Disabled())
            return 0;
        Interval& interval = g_intervals.back();
        if (mode != kEdramModeDepthOnly || !EnsureResources())
        {
            interval.otherDraws = true;
            return 0;
        }
        interval.queryDraws = true;
        if (!g_idsCleared)
        {
            // Clear the id texture before this frame's first query draw.
            Flush();
            SDL_GPUColorTargetInfo color{};
            color.texture = g_ids;
            color.load_op = SDL_GPU_LOADOP_CLEAR;
            color.store_op = SDL_GPU_STOREOP_STORE;
            SDL_EndGPURenderPass(SDL_BeginGPURenderPass(Commands(), &color, 1, nullptr));
            g_idsCleared = true;
        }
        return uint32_t(g_intervals.size());
    }

    SDL_GPUTexture* OcclusionQueryTexture()
    {
        return g_ids;
    }

    SDL_GPUShader* OcclusionQueryFragmentShader()
    {
        static SDL_GPUShader* shader = [] {
            char glsl[512];
            snprintf(glsl, sizeof(glsl), kQueryFragmentShader, uint32_t(offsetof(SystemConstants, queryId)));
            return CompileInternal(glsl, false, 0, 2);
        }();
        return shader;
    }

    void EndFrameQueries()
    {
        bool any = false;
        for (const Interval& interval : g_intervals)
            any = any || (interval.queryDraws && interval.endAddress);
        if (any && g_idsCleared && EnsureResources())
        {
            Flush();
            SDL_GPUCommandBuffer* commands = Commands();
            // Zero the counters, count, then download.
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUBufferLocation zeros{ g_zeros, 0 };
            SDL_GPUBufferLocation counts{ g_counts, 0 };
            SDL_CopyGPUBufferToBuffer(copy, &zeros, &counts, kCountBytes, false);
            SDL_EndGPUCopyPass(copy);
            SDL_GPUStorageBufferReadWriteBinding writable{ g_counts, false };
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &writable, 1);
            SDL_BindGPUComputePipeline(pass, g_countPipeline);
            SDL_GPUTextureSamplerBinding ids{ g_ids, PointSampler() };
            SDL_BindGPUComputeSamplers(pass, 0, &ids, 1);
            SDL_DispatchGPUCompute(pass, (TargetWidth() + kCountGroupSize - 1) / kCountGroupSize,
                (TargetHeight() + kCountGroupSize - 1) / kCountGroupSize, 1);
            SDL_EndGPUComputePass(pass);
            Readback readback;
            SDL_GPUTransferBufferCreateInfo transfer{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, kCountBytes, 0 };
            readback.download = SDL_CreateGPUTransferBuffer(g_device, &transfer);
            if (readback.download)
            {
                copy = SDL_BeginGPUCopyPass(commands);
                SDL_GPUBufferRegion source{ g_counts, 0, kCountBytes };
                SDL_GPUTransferBufferLocation destination{ readback.download, 0 };
                SDL_DownloadFromGPUBuffer(copy, &source, &destination);
                SDL_EndGPUCopyPass(copy);
                FenceMemExports();  // submits g_commands if memexport results wait in it
                readback.fence = SDL_SubmitGPUCommandBufferAndAcquireFence(Commands());
                g_commands = nullptr;
                for (const Interval& interval : g_intervals)
                    readback.endAddresses.push_back(interval.queryDraws ? interval.endAddress : 0);
                g_readbacks.push_back(std::move(readback));
            }
        }
        g_intervals.clear();
        g_open = false;
        g_idsCleared = false;
    }

    void PollOcclusionQueries()
    {
        size_t done = 0;
        for (; done < g_readbacks.size(); done++)
        {
            Readback& readback = g_readbacks[done];
            if (readback.fence && !SDL_QueryGPUFence(g_device, readback.fence))
                break;
            const uint32_t* counts = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(g_device, readback.download, false));
            // Host texels are render-scale squared per guest sample.
            uint32_t perSample = g_renderScale * g_renderScale;
            static const bool log = EnvFlag("H3_LOG_ZPD");
            for (size_t i = 0; i < readback.endAddresses.size(); i++)
                if (uint32_t address = readback.endAddresses[i])
                {
                    g_measured[address] = counts[i + 1] / perSample;
                    if (log)
                        fprintf(stderr, "[render] occlusion query %08X: %u samples\n", address, counts[i + 1] / perSample);
                }
            SDL_UnmapGPUTransferBuffer(g_device, readback.download);
            SDL_ReleaseGPUTransferBuffer(g_device, readback.download);
            if (readback.fence)
                SDL_ReleaseGPUFence(g_device, readback.fence);
        }
        g_readbacks.erase(g_readbacks.begin(), g_readbacks.begin() + done);
    }
}
