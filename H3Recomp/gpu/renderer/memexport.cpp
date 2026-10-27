// Memory export (memexport): vertex shaders that write guest memory through
// the eA/eM# export registers instead of (or as well as) producing
// vertices. Halo 3 uses it for its GPU particles (state updated by point
// draws, ~19 dwords per particle; the menu has them too), for visibility
// tests sampling the last frame's depth (lens flares, lights) and for its
// water (simulation, tessellation factors).
//
// SDL_GPU's vertex shaders can't write buffers, so such a draw also runs the
// shader's compute variant (gpu/shader/translator.cpp, kMemExportFunctions):
// one invocation per guest vertex. Where the shader writes comes from its
// stream constants (the float constants feeding eA), recognized on the CPU
// by their signature, as Xenia does. Per draw:
//
//   1. the compute shader runs with the draw's constants, vertex data and
//      textures, and appends every dword it exports inside the streams to
//      a record buffer as (address, value). Vertex data goes through a
//      per-frame input arena that uploads each guest range once a frame
//      (Halo runs ~100 of these draws a frame over the same buffers),
//   2. the records are downloaded, and once the GPU is done (a frame or two
//      later; nothing waits) they're applied to guest memory in order.
//
// Dispatches are queued and recorded in batches of independent ones (see
// "Batching" below): one at a time, Halo's ~120 tiny dispatches a frame
// cost ~9 ms of GPU time in pipeline drains.
//
// Records rather than a copy of the streams: one frame's occlusion draws
// each fill a few elements of the same (half-megabyte) stream, so copying
// and comparing whole streams per draw cost more than the frame.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // Streams a dispatch can write (the translator's xe_export_streams).
        constexpr uint32_t kMaxStreams = 4;
        // Largest stream accepted (bytes); larger ones are probably not a
        // stream constant after all.
        constexpr uint32_t kMaxStreamBytes = 16u * 1024 * 1024;
        // Compute workgroup size (kComputeMain's local_size_x).
        constexpr uint32_t kThreadsPerGroup = 64;
        // Float constants per stage (4 floats each), as in draw.cpp.
        constexpr uint32_t kStageFloats = 256 * 4;
        // Hash seed for compute variants' shader cache names (vertex and
        // pixel shaders use 1 and 2).
        constexpr uint64_t kComputeSeed = 3;
        // Smallest buffer created for a dispatch (bytes).
        constexpr uint32_t kMinSlotBytes = 16 * 1024;
        // Records a vertex may export at most: each flush stores up to 5
        // elements of up to 4 dwords, and a shader may flush more than once.
        constexpr uint32_t kRecordsPerVertex = 2 * 5 * 4;
        // Record buffer layout (dwords): count, unused, then (address,
        // value) pairs.
        constexpr uint32_t kRecordHeaderDwords = 2;
        constexpr uint32_t kRecordDwords = 2;
        constexpr uint32_t kRecordHeaderBytes = kRecordHeaderDwords * 4;
        // Records per dispatch at most (bytes stay well under the slot cap).
        constexpr uint32_t kMaxRecords = 1u << 20;
        // Free dispatch slots kept for reuse at most: a few frames' worth of
        // dispatches are in flight at once (Halo issues dozens a frame), and
        // releasing and recreating buffers showed up as ioctl/mmap time.
        constexpr size_t kMaxFreeSlots = 1024;
        // ...and at most this many bytes of buffers in them (the OOM killer
        // took the game when slots weren't bounded).
        constexpr uint64_t kMaxFreeBytes = 256ull * 1024 * 1024;

        // memexport stream constant fields (Xenia's xe_gpu_memexport_stream_t).
        constexpr uint32_t kStreamSignature0 = 1;           // dword 0 bits 30-31
        constexpr uint32_t kStreamSignature1 = 0x4B000000;  // dword 1
        constexpr uint32_t kStreamSignature2 = 0x4B0;       // dword 2 bits 20-31
        constexpr uint32_t kStreamSignature3 = 0x96;        // dword 3 bits 23-31

        // Bytes per element of a memexport format (ColorFormat), 0 if the
        // compute variant doesn't store it.
        uint32_t ElementBytes(uint32_t format)
        {
            switch (format)
            {
            case 6:   // k_8_8_8_8
            case 7:   // k_2_10_10_10
            case 25:  // k_16_16
            case 31:  // k_16_16_FLOAT
            case 36:  // k_32_FLOAT
                return 4;
            case 26:  // k_16_16_16_16
            case 32:  // k_16_16_16_16_FLOAT
            case 37:  // k_32_32_FLOAT
                return 8;
            case 38:  // k_32_32_32_32_FLOAT
                return 16;
            default:
                return 0;
            }
        }

        // A guest memory range one dispatch may write.
        struct Stream
        {
            uint32_t base = 0;   // physical byte address
            uint32_t bytes = 0;
        };

        // The translator's XeExportInfo uniform block (std140).
        struct ExportInfo
        {
            uint32_t streams[kMaxStreams][4];  // base, bytes, unused, unused
            uint32_t vertexCount;
            uint32_t indexed;
            uint32_t indexOffset;  // dword in the vertex memory buffer
            uint32_t indexBase;    // added to auto-generated indices
            uint32_t capacity;     // records that fit
            uint32_t debug;        // DebugVertexPositions: positions instead of records
            uint32_t recordBase;   // dword where this dispatch's records start
        };

        // One dispatch's part of a slot's record buffer.
        struct Region
        {
            uint32_t base = 0;     // dword where its header is (count, unused, then records)
            uint32_t records = 0;  // record capacity
            std::vector<Stream> streams;  // physical ranges it writes
            uint64_t shader = 0;          // the vertex shader's capture hash (debugging)
        };
        // GPU and transfer buffers of one batch of dispatches (see
        // RecordPendingMemExports), reused once its results are back.
        struct Slot
        {
            uint32_t recordCapacity = 0;              // bytes: exports and download
            SDL_GPUBuffer* exports = nullptr;         // records, read-write
            SDL_GPUTransferBuffer* download = nullptr;
            std::vector<Region> regions;              // its dispatches, in order
        };
        std::vector<Slot> g_freeSlots;

        // Dispatches recorded but not yet submitted, and submitted ones
        // waiting for their fence (oldest first).
        std::vector<Slot> g_unfenced;
        struct Batch
        {
            SDL_GPUFence* fence;
            std::vector<Slot> slots;
            uint32_t frame = 0;     // g_swaps when submitted
            uint64_t sequence = 0;  // submission order (g_batchSequence)
        };
        std::vector<Batch> g_batches;
        // Batches submitted so far, and the last one whose results are in
        // guest memory (batches complete in order).
        uint64_t g_batchSequence = 0, g_completedSequence = 0;

        // --- Deferred fence writes ---------------------------------------
        //
        // On the console a fence the GPU writes (EVENT_WRITE_SHD) lands
        // after everything before it, memory exports included, so a CPU
        // that saw the fence may reuse that memory. Here exports reach guest
        // memory a frame or two after their packets: a fence written at
        // once let Halo move resources over a particle buffer whose late
        // exports then landed on what moved in (The Covenant's ground
        // lightmap: a 62916-byte particle stream at 13354BB8 written over
        // layer 1 of the lightmap compacted down onto it, rainbow patches on
        // the ground). So while exports are in flight, fence writes wait for
        // the batches submitted before them (DeferFenceWrite), in order.
        struct DeferredWrite
        {
            uint32_t address;   // physical
            uint32_t swapped;   // the value as stored (endian applied)
            uint64_t sequence;  // the batch it waits for
        };
        std::vector<DeferredWrite> g_deferredWrites;

        // Does the deferred fence writes whose batches are complete.
        void ApplyDeferredWrites()
        {
            size_t done = 0;
            for (; done < g_deferredWrites.size() && g_deferredWrites[done].sequence <= g_completedSequence; done++)
            {
                const DeferredWrite& write = g_deferredWrites[done];
                memcpy(GuestPtr<uint8_t>(PhysicalToGuest(write.address & ~3u)), &write.swapped, 4);
            }
            g_deferredWrites.erase(g_deferredWrites.begin(), g_deferredWrites.begin() + done);
        }

        // Compute variants by guest shader (null pipeline: failed).
        std::unordered_map<const Shader*, SDL_GPUComputePipeline*> g_pipelines;

        // Per-frame input arenas: vertex data (and indices) of every
        // dispatch in a frame, each guest range uploaded once. An arena is
        // reused kArenaFrames frames later, when the GPU is done with it
        // (the same assumption draw.cpp's upload buffers make).
        constexpr uint32_t kArenaFrames = 4;
        constexpr uint32_t kMinArenaBytes = 4u * 1024 * 1024;
        struct InputArena
        {
            SDL_GPUBuffer* buffer = nullptr;
            SDL_GPUTransferBuffer* upload = nullptr;
            uint32_t capacity = 0;  // bytes
            uint32_t used = 0;      // bytes staged this frame
            uint32_t uploaded = 0;  // ...of which uploads are recorded (RecordArenaUpload)
            uint32_t frame = ~0u;   // g_swaps it was last reset for
            std::unordered_map<uint64_t, uint32_t> ranges;  // (guest address, dwords) -> dword offset
        };
        InputArena g_arenas[kArenaFrames];

        // 16 zero bytes on the GPU: copied over a record buffer's header to
        // reset its count.
        SDL_GPUBuffer* g_zeroBuffer = nullptr;
        constexpr uint32_t kZeroBytes = 16;

        // Creates g_zeroBuffer on first use.
        bool EnsureZeroBuffer()
        {
            if (g_zeroBuffer)
                return true;
            SDL_GPUBufferCreateInfo buffer{};
            buffer.size = kZeroBytes;
            buffer.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
            g_zeroBuffer = SDL_CreateGPUBuffer(g_device, &buffer);
            SDL_GPUTransferBufferCreateInfo transfer{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, kZeroBytes, 0 };
            SDL_GPUTransferBuffer* upload = SDL_CreateGPUTransferBuffer(g_device, &transfer);
            if (!g_zeroBuffer || !upload)
                return false;
            memset(SDL_MapGPUTransferBuffer(g_device, upload, false), 0, kZeroBytes);
            SDL_UnmapGPUTransferBuffer(g_device, upload);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            SDL_GPUTransferBufferLocation source{ upload, 0 };
            SDL_GPUBufferRegion destination{ g_zeroBuffer, 0, kZeroBytes };
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
            SDL_EndGPUCopyPass(copy);
            SDL_ReleaseGPUTransferBuffer(g_device, upload);
            return true;
        }

        // Exports landing in this frame's input arena. Within a frame Halo
        // spawns particles with one dispatch and updates them with the next,
        // which reads the spawn's output; staged once a frame from guest
        // memory (where exports arrive only a frame or two later), the
        // update read empty slots and wrote dead particles back: no muzzle
        // flashes, no splashes. So after each dispatch, a scatter pass writes
        // its records into the arena's copies of the ranges they fall in,
        // and later dispatches in the same command buffer see them.
        // One scatter dispatch covers up to kMaxScatterRegions dispatches'
        // records (one buffer, a region each; workgroup y picks the region)
        // into up to kMaxScatterRanges copied guest ranges: one dispatch per
        // export cost ~5% of the command processor thread in driver calls.
        constexpr uint32_t kMaxScatterRanges = 16;
        constexpr uint32_t kMaxScatterRegions = 128;
        struct ScatterInfo  // std140
        {
            uint32_t rangeCount, regionCount, maxCapacity, unused;
            uint32_t ranges[kMaxScatterRanges][4];    // physical base, bytes, target dword offset, unused
            uint32_t regions[kMaxScatterRegions][4];  // header dword, record capacity, unused, unused
        };
        const char* kScatterShader = R"(#version 450
layout(local_size_x = 64) in;
layout(std430, set = 0, binding = 0) readonly buffer XeRecords { uint records[]; };
layout(std430, set = 1, binding = 0) buffer XeArena { uint arena[]; };
layout(std140, set = 2, binding = 0) uniform XeScatter
{
    uvec4 counts;  // x: ranges, y: regions
    uvec4 ranges[16];
    uvec4 regions[128];  // x: dword of the region's header (record count), y: record capacity
};
void main()
{
    uvec4 region = regions[gl_WorkGroupID.y];
    uint i = gl_GlobalInvocationID.x;
    uint base = region.x;
    if (i >= min(records[base], region.y))
        return;
    uint address = records[base + 2u + i * 2u] & ~3u, value = records[base + 3u + i * 2u];
    for (uint r = 0u; r < counts.x; r++)
    {
        uvec4 range = ranges[r];
        if (address >= range.x && address - range.x < range.y)
            arena[range.z + (address - range.x) / 4u] = value;
    }
}
)";
        SDL_GPUComputePipeline* g_scatterPipeline = nullptr;

        // This frame's dispatches so far (their record buffers stay alive
        // until applied, a frame or more later), to replay into ranges
        // staged after them: the spawn dispatch runs before anything has
        // staged the particle buffer the update then reads.
        struct FrameExport
        {
            SDL_GPUBuffer* records;
            uint32_t recordBase;  // dword of its header in records
            uint32_t capacity;
            std::vector<Stream> streams;
            uint64_t shader = 0;  // capture hash (debugging)
            // For ordering (DisjointRun): its shader and, auto-indexed, the
            // vertex indices it ran.
            const Shader* vs = nullptr;
            uint32_t firstIndex = 0, indexCount = 0;
        };
        std::vector<FrameExport> g_frameExports;
        uint32_t g_frameExportsFrame = ~0u;  // g_swaps g_frameExports is for
        // The distinct streams of g_frameExports (usually one or two: the
        // particle buffer), to find the staged ranges exports land in fast.
        std::vector<Stream> g_frameStreams;
        // Every distinct stream any dispatch has had (MemExportMayWrite).
        std::vector<Stream> g_everStreams;

        // Adds a dispatch to this frame's exports.
        void AddFrameExport(FrameExport&& frameExport)
        {
            if (g_frameExportsFrame != g_swaps)
            {
                g_frameExports.clear();
                g_frameStreams.clear();
                g_frameExportsFrame = g_swaps;
            }
            for (const Stream& stream : frameExport.streams)
                if (std::none_of(g_frameStreams.begin(), g_frameStreams.end(),
                        [&](const Stream& known) { return known.base == stream.base && known.bytes == stream.bytes; }))
                    g_frameStreams.push_back(stream);
            g_frameExports.push_back(std::move(frameExport));
        }

        // Whether two dispatches are runs of one shader over disjoint
        // vertex index ranges: they touch disjoint elements even when their
        // stream constants name the same buffer (see DependsOnPending).
        bool DisjointRun(const Shader* vsA, uint32_t firstA, uint32_t countA, const Shader* vsB, uint32_t firstB, uint32_t countB)
        {
            return vsA == vsB && countA && countB && (firstA + countA <= firstB || firstB + countB <= firstA);
        }

        // Starts info for scattering records into a buffer holding copies
        // of guest ranges ((physical address << 32 | dwords), dword offset):
        // the ranges that overlap streams (where the records can land).
        // False if none do. Regions go in with AddScatterRegion.
        bool ScatterSetup(ScatterInfo& info, const std::vector<Stream>& streams, const std::vector<std::pair<uint64_t, uint32_t>>& ranges)
        {
            info.rangeCount = info.regionCount = info.maxCapacity = 0;
            for (const auto& [key, offset] : ranges)
            {
                uint32_t base = uint32_t(key >> 32), bytes = uint32_t(key) * 4;
                bool overlaps = false;
                for (const Stream& stream : streams)
                    overlaps = overlaps || (base < stream.base + stream.bytes && stream.base < base + bytes);
                if (!overlaps || info.rangeCount == kMaxScatterRanges)
                    continue;
                uint32_t* range = info.ranges[info.rangeCount++];
                range[0] = base, range[1] = bytes, range[2] = offset;
            }
            static const bool log = EnvFlag("H3_LOG_MEMEXPORT");
            static uint32_t logged = 0;
            if (log && logged++ < 40)
            {
                fprintf(stderr, "[render] memexport scatter f%u: %u ranges of %zu staged; streams", g_swaps, info.rangeCount, ranges.size());
                for (const Stream& stream : streams)
                    fprintf(stderr, " %08X+%u", stream.base, stream.bytes);
                for (uint32_t r = 0; r < info.rangeCount; r++)
                    fprintf(stderr, " | range %08X+%u @%u", info.ranges[r][0], info.ranges[r][1], info.ranges[r][2]);
                fprintf(stderr, "\n");
            }
            return info.rangeCount != 0;
        }

        // Adds a dispatch's records (header at dword base of the records
        // buffer, at most capacity records) to a scatter. False if it's full.
        bool AddScatterRegion(ScatterInfo& info, uint32_t base, uint32_t capacity)
        {
            if (info.regionCount == kMaxScatterRegions)
                return false;
            uint32_t* region = info.regions[info.regionCount++];
            region[0] = base, region[1] = capacity;
            info.maxCapacity = std::max(info.maxCapacity, capacity);
            return true;
        }

        // The scatter shader's pipeline, made on first use. False if that
        // failed.
        bool EnsureScatterPipeline()
        {
            if (g_scatterPipeline)
                return true;
            std::vector<uint8_t> spirv = CompileSpirv(ShaderStage::Compute, kScatterShader, InternalCacheName("memexport_scatter", kScatterShader));
            if (spirv.empty())
                return false;
            SDL_GPUComputePipelineCreateInfo create{};
            create.code_size = spirv.size();
            create.code = spirv.data();
            create.entrypoint = "main";
            create.format = SDL_GPU_SHADERFORMAT_SPIRV;
            create.num_readonly_storage_buffers = 1;
            create.num_readwrite_storage_buffers = 1;
            create.num_uniform_buffers = 1;
            create.threadcount_x = kThreadsPerGroup;
            create.threadcount_y = create.threadcount_z = 1;
            g_scatterPipeline = SDL_CreateGPUComputePipeline(g_device, &create);
            return g_scatterPipeline != nullptr;
        }

        // Records a scatter (ScatterSetup's info with its regions) into an
        // open compute pass whose read-write buffer is the target, and
        // empties the region list for the next one.
        void DispatchScatter(SDL_GPUCommandBuffer* commands, SDL_GPUComputePass* pass, SDL_GPUBuffer* records, ScatterInfo& info)
        {
            if (!info.regionCount)
                return;
            SDL_BindGPUComputePipeline(pass, g_scatterPipeline);
            SDL_BindGPUComputeStorageBuffers(pass, 0, &records, 1);
            SDL_PushGPUComputeUniformData(commands, 0, &info, sizeof(info));
            SDL_DispatchGPUCompute(pass, (info.maxCapacity + kThreadsPerGroup - 1) / kThreadsPerGroup, info.regionCount, 1);
            info.regionCount = info.maxCapacity = 0;
        }

        // Records a scatter of a dispatch's records into target's copies of
        // guest ranges, in a compute pass of its own.
        void ScatterRecords(SDL_GPUCommandBuffer* commands, SDL_GPUBuffer* records, uint32_t recordBase, uint32_t capacity,
            const std::vector<Stream>& streams, SDL_GPUBuffer* target, const std::vector<std::pair<uint64_t, uint32_t>>& ranges)
        {
            GpuTimedScope timed{ "export scatter" };  // H3_GPU_TIMING
            commands = Commands();  // (the timing scope may have submitted it)
            static ScatterInfo info;
            if (!ScatterSetup(info, streams, ranges) || !EnsureScatterPipeline())
                return;
            AddScatterRegion(info, recordBase, capacity);
            SDL_GPUStorageBufferReadWriteBinding writable{ target, false };
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &writable, 1);
            DispatchScatter(commands, pass, records, info);
            SDL_EndGPUComputePass(pass);
        }

        // The same into this frame's input arena: all its staged ranges, or
        // only `only` ((key, dword offset) pairs just staged).
        void ScatterIntoArena(SDL_GPUCommandBuffer* commands, SDL_GPUBuffer* records, uint32_t recordBase, uint32_t capacity,
            const std::vector<Stream>& streams, const std::vector<std::pair<uint64_t, uint32_t>>* only = nullptr)
        {
            static const bool disabled = EnvFlag("H3_NO_EXPORT_SCATTER");
            InputArena& arena = g_arenas[g_swaps % kArenaFrames];
            if (disabled || !arena.buffer || arena.frame != g_swaps)
                return;
            std::vector<std::pair<uint64_t, uint32_t>> all;
            if (!only)
                all.assign(arena.ranges.begin(), arena.ranges.end());
            ScatterRecords(commands, records, recordBase, capacity, streams, arena.buffer, only ? *only : all);
        }

        // One piece of input to stage: guest data (key: address and dwords,
        // or 0 for data that isn't shared, like indices).
        struct InputPiece
        {
            const void* data;
            uint32_t dwords;
            uint64_t key;
            uint32_t* offset;  // receives the dword offset in the arena
        };

        void RecordPendingDispatches();

        // Records the upload of what's been staged into this frame's arena
        // since the last one, in an open copy pass.
        void RecordArenaUpload(SDL_GPUCopyPass* copy)
        {
            InputArena& arena = g_arenas[g_swaps % kArenaFrames];
            if (arena.frame != g_swaps || !arena.buffer || arena.used <= arena.uploaded)
                return;
            SDL_GPUTransferBufferLocation source{ arena.upload, arena.uploaded };
            SDL_GPUBufferRegion destination{ arena.buffer, arena.uploaded, arena.used - arena.uploaded };
            SDL_UploadToGPUBuffer(copy, &source, &destination, false);
            arena.uploaded = arena.used;
        }

        // Stages pieces into this frame's arena (only ranges not already
        // there), to be uploaded by the next RecordArenaUpload; staged (if
        // given) receives the (key, dword offset) of ranges new this time.
        // Returns the arena buffer, or null on failure.
        SDL_GPUBuffer* StageInput(std::vector<InputPiece>& pieces, std::vector<std::pair<uint64_t, uint32_t>>* staged = nullptr)
        {
            InputArena& arena = g_arenas[g_swaps % kArenaFrames];
            if (arena.frame != g_swaps)
            {
                arena.frame = g_swaps;
                arena.used = arena.uploaded = 0;
                arena.ranges.clear();
            }
            // Bytes the new pieces need (16-byte aligned each).
            auto newBytes = [&] {
                uint32_t bytes = 0;
                for (const InputPiece& piece : pieces)
                    if (!piece.key || !arena.ranges.contains(piece.key))
                        bytes += AlignUp(piece.dwords * 4, 16);
                return bytes;
            };
            uint32_t needed = newBytes();
            if (arena.used + needed > arena.capacity)
            {
                // Grow. What this frame staged so far stays in the old
                // buffer (dispatches queued so far are recorded now, with
                // its upload); start over.
                RecordPendingDispatches();
                if (arena.buffer)
                    SDL_ReleaseGPUBuffer(g_device, arena.buffer);
                if (arena.upload)
                    SDL_ReleaseGPUTransferBuffer(g_device, arena.upload);
                arena.capacity = std::max({ kMinArenaBytes, std::bit_ceil(arena.used + needed), arena.capacity * 2 });
                static const bool log = EnvFlag("H3_LOG_MEMEXPORT");
                if (log)
                    fprintf(stderr, "[render] memexport arena %u grows to %u bytes (used %u, needed %u, %zu pieces)\n", g_swaps % kArenaFrames,
                        arena.capacity, arena.used, needed, pieces.size());
                SDL_GPUBufferCreateInfo buffer{};
                buffer.size = arena.capacity;
                // Written too: dispatches' exports are scattered into it
                // for the next dispatches (ScatterIntoArena).
                buffer.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
                arena.buffer = SDL_CreateGPUBuffer(g_device, &buffer);
                SDL_GPUTransferBufferCreateInfo transfer{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, arena.capacity, 0 };
                arena.upload = SDL_CreateGPUTransferBuffer(g_device, &transfer);
                arena.used = arena.uploaded = 0;
                arena.ranges.clear();
                needed = newBytes();
                if (!arena.buffer || !arena.upload)
                {
                    arena.capacity = 0;
                    return nullptr;
                }
            }
            uint8_t* mapped = needed ? static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(g_device, arena.upload, false)) : nullptr;
            for (InputPiece& piece : pieces)
            {
                if (piece.key)
                    if (auto found = arena.ranges.find(piece.key); found != arena.ranges.end())
                    {
                        *piece.offset = found->second;
                        continue;
                    }
                *piece.offset = arena.used / 4;
                memcpy(mapped + arena.used, piece.data, size_t(piece.dwords) * 4);
                if (piece.key)
                {
                    arena.ranges.emplace(piece.key, arena.used / 4);
                    if (staged)
                        staged->push_back({ piece.key, arena.used / 4 });
                }
                arena.used += AlignUp(piece.dwords * 4, 16);
            }
            if (mapped)
                SDL_UnmapGPUTransferBuffer(g_device, arena.upload);
            return arena.buffer;
        }

        // Bytes a slot's buffers take (for the free list's limit).
        uint64_t SlotBytes(const Slot& slot)
        {
            return 2 * uint64_t(slot.recordCapacity);
        }

        // A slot holding at least recordBytes of records: the smallest free
        // one that fits, or a new one.
        Slot AcquireSlot(uint32_t recordBytes)
        {
            size_t best = g_freeSlots.size();
            for (size_t i = 0; i < g_freeSlots.size(); i++)
            {
                const Slot& slot = g_freeSlots[i];
                if (slot.recordCapacity >= recordBytes && (best == g_freeSlots.size() || slot.recordCapacity < g_freeSlots[best].recordCapacity))
                    best = i;
            }
            if (best < g_freeSlots.size())
            {
                Slot slot = std::move(g_freeSlots[best]);
                g_freeSlots[best] = std::move(g_freeSlots.back());
                g_freeSlots.pop_back();
                return slot;
            }
            Slot slot;
            slot.recordCapacity = std::max(kMinSlotBytes, std::bit_ceil(recordBytes));
            SDL_GPUBufferCreateInfo buffer{};
            buffer.size = slot.recordCapacity;
            buffer.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
            slot.exports = SDL_CreateGPUBuffer(g_device, &buffer);
            SDL_GPUTransferBufferCreateInfo transfer{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, slot.recordCapacity, 0 };
            slot.download = SDL_CreateGPUTransferBuffer(g_device, &transfer);
            return slot;
        }

        void ReleaseSlot(Slot& slot)
        {
            SDL_ReleaseGPUBuffer(g_device, slot.exports);
            SDL_ReleaseGPUTransferBuffer(g_device, slot.download);
        }

        // Back into the free list (the largest go when it's full).
        void RecycleSlot(Slot&& slot)
        {
            g_freeSlots.push_back(std::move(slot));
            auto freeBytes = [] {
                uint64_t bytes = 0;
                for (const Slot& free : g_freeSlots)
                    bytes += SlotBytes(free);
                return bytes;
            };
            while (g_freeSlots.size() > kMaxFreeSlots || (g_freeSlots.size() > 1 && freeBytes() > kMaxFreeBytes))
            {
                auto largest = std::max_element(g_freeSlots.begin(), g_freeSlots.end(),
                    [](const Slot& a, const Slot& b) { return SlotBytes(a) < SlotBytes(b); });
                ReleaseSlot(*largest);
                g_freeSlots.erase(largest);
            }
        }

        // Applies one finished dispatch's records (results: its region's
        // header in the downloaded buffer) to guest memory.
        void CompleteRegion(const Region& region, const uint32_t* results)
        {
            uint32_t count = std::min(results[0], region.records);
            // Pages written (one dispatch can write far-apart streams; the
            // span between them may hold live resolves).
            constexpr uint32_t kPageShift = 12;
            std::vector<uint32_t> pages;
            uint32_t lastPage = ~0u;
            for (uint32_t i = 0; i < count; i++)
            {
                const uint32_t* record = results + kRecordHeaderDwords + i * kRecordDwords;
                uint32_t address = record[0] & ~3u;
                memcpy(GuestPtr<uint8_t>(PhysicalToGuest(address)), &record[1], 4);  // already in memory byte order
                // H3_TRACE_EXPORTS=<hex address>-<hex end>: log every record
                // applied in that physical range (frame, address, float).
                static const char* trace = EnvString("H3_TRACE_EXPORTS");
                static uint32_t traceBegin = trace ? uint32_t(strtoul(trace, nullptr, 16)) : 0;
                static uint32_t traceEnd = trace && strchr(trace, '-') ? uint32_t(strtoul(strchr(trace, '-') + 1, nullptr, 16)) : 0;
                // H3_TRACE_EXPORTS_VS=<8 hex digits>[,...]: only those
                // shaders' records.
                static const char* traceShader = EnvString("H3_TRACE_EXPORTS_VS");
                char shaderText[9];
                if (traceShader)
                    snprintf(shaderText, sizeof(shaderText), "%08llx", (unsigned long long)(region.shader >> 32));
                // H3_TRACE_EXPORTS_AT=<modulus>:<remainder>: only addresses
                // at that offset (one field of a record array).
                static const char* traceAt = EnvString("H3_TRACE_EXPORTS_AT");
                static const uint32_t traceModulus = traceAt ? uint32_t(strtoul(traceAt, nullptr, 0)) : 0;
                static const uint32_t traceRemainder = traceAt && strchr(traceAt, ':') ? uint32_t(strtoul(strchr(traceAt, ':') + 1, nullptr, 0)) : 0;
                if (address >= traceBegin && address < traceEnd && (!traceShader || strstr(traceShader, shaderText)) &&
                    (!traceModulus || address % traceModulus == traceRemainder))
                {
                    uint32_t bits = __builtin_bswap32(record[1]);
                    float value;
                    memcpy(&value, &bits, 4);
                    fprintf(stderr, "[trace] f%u %08X %g (%08X) vs_%016llx record %u\n", g_swaps, address, value, bits,
                        (unsigned long long)region.shader, i);
                }
                uint32_t page = address >> kPageShift;
                if (page != lastPage)
                    pages.push_back(lastPage = page);
            }
            // Memory the game reads back as a texture may still have a
            // resolved texture standing in for it (resolve.cpp), which would
            // hide what was just written. Halo's flare visibility samples
            // exported occlusion results at 0x04443000, where the water
            // ripples resolve too: the stale ripple texture kept tac lights
            // shining through rocks.
            std::sort(pages.begin(), pages.end());
            pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
            for (uint32_t page : pages)
                InvalidateResolved(page << kPageShift, (page + 1) << kPageShift);
            // H3_LOG_MEMEXPORT: records per completed dispatch (the first
            // few hundred).
            static const bool log = EnvFlag("H3_LOG_MEMEXPORT");
            static uint32_t logged = 0;
            constexpr uint32_t kMaxLogged = 300;
            if (log && logged++ < kMaxLogged)
            {
                fprintf(stderr, "[render] memexport f%u: %u records%s:", g_swaps, count, results[0] > region.records ? " (overflowed)" : "");
                // The first few, with the value read as a big-endian float.
                constexpr uint32_t kShown = 6;
                for (uint32_t i = 0; i < std::min(count, kShown); i++)
                {
                    const uint32_t* record = results + kRecordHeaderDwords + i * kRecordDwords;
                    uint32_t bits = __builtin_bswap32(record[1]);
                    float value;
                    memcpy(&value, &bits, 4);
                    fprintf(stderr, " %08X=%g", record[0], value);
                }
                fprintf(stderr, "\n");
            }
            // H3_CHECK_EXPORT_DUPES: count addresses one dispatch writes
            // more than once with different values (their order is the
            // export shader's atomic order, so the result is arbitrary).
            static const bool dupes = EnvFlag("H3_CHECK_EXPORT_DUPES");
            if (dupes)
            {
                std::unordered_map<uint32_t, uint32_t> seen;
                uint32_t conflicting = 0, first = 0;
                for (uint32_t i = 0; i < count; i++)
                {
                    const uint32_t* record = results + kRecordHeaderDwords + i * kRecordDwords;
                    auto [it, inserted] = seen.emplace(record[0] & ~3u, record[1]);
                    if (!inserted && it->second != record[1] && conflicting++ == 0)
                        first = record[0];
                }
                static uint32_t logged = 0;
                if (conflicting && logged++ < 200)
                    fprintf(stderr, "[dupes] f%u: %u of %u records conflict (first %08X), streams %08X+%u\n", g_swaps, conflicting, count, first,
                        region.streams.empty() ? 0 : region.streams[0].base, region.streams.empty() ? 0 : region.streams[0].bytes);
            }
        }

        // Applies a finished batch's records to guest memory, dispatch by
        // dispatch in the order they ran.
        void CompleteSlot(Slot& slot)
        {
            const uint32_t* mapped = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(g_device, slot.download, false));
            for (const Region& region : slot.regions)
                CompleteRegion(region, mapped + region.base);
            SDL_UnmapGPUTransferBuffer(g_device, slot.download);
            slot.regions.clear();
        }

        // The compute variant of a guest vertex shader (made on first use;
        // null if it fails).
        SDL_GPUComputePipeline* ComputePipeline(const Shader& vs)
        {
            auto it = g_pipelines.find(&vs);
            if (it != g_pipelines.end())
                return it->second;
            SDL_GPUComputePipeline*& pipeline = g_pipelines[&vs];
            SlowTimer timer{ "memexport compute pipeline" };
            shader::TranslationResult translated =
                shader::Translate(true, vs.microcode.data(), uint32_t(vs.microcode.size()), true);
            if (!translated.ok)
            {
                LogOnce("memexport compute translation failed: " + translated.error);
                return nullptr;
            }
            char name[32];
            snprintf(name, sizeof(name), "%016llx.spv",
                (unsigned long long)Hash(translated.glsl.data(), translated.glsl.size(), kComputeSeed));
            std::vector<uint8_t> spirv = CompileSpirv(ShaderStage::Compute, translated.glsl, name);
            if (spirv.empty())
                return nullptr;
            SDL_GPUComputePipelineCreateInfo info{};
            info.code_size = spirv.size();
            info.code = spirv.data();
            info.entrypoint = "main";
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
            info.num_samplers = uint32_t(translated.info.textures.size());
            info.num_readonly_storage_buffers = 1;   // xe_memory
            info.num_readwrite_storage_buffers = 1;  // xe_export
            info.num_uniform_buffers = 3;            // constants, system, export info
            info.threadcount_x = kThreadsPerGroup;
            info.threadcount_y = info.threadcount_z = 1;
            pipeline = SDL_CreateGPUComputePipeline(g_device, &info);
            if (!pipeline)
                LogOnce(std::string("memexport compute pipeline creation failed: ") + SDL_GetError());
            return pipeline;
        }

        // The streams the shader's stream constant candidates describe.
        std::vector<Stream> FindStreams(const Shader& vs)
        {
            std::vector<Stream> streams;
            const uint32_t* alu = AluConstantsHost();
            for (uint32_t constant : vs.info.memExportStreamConstants)
            {
                uint32_t d[4];
                for (uint32_t i = 0; i < 4; i++)
                    d[i] = alu[constant * 4 + i];  // vertex constants come first
                // H3_LOG_STREAMS=<8 hex digits of a capture hash>: that
                // shader's stream constants (base, index, format word, count).
                static const char* logStreams = EnvString("H3_LOG_STREAMS");
                static uint32_t loggedStreams = 0;
                if (logStreams && uint32_t(vs.captureHash >> 32) == uint32_t(strtoul(logStreams, nullptr, 16)) && loggedStreams++ < 64)
                    fprintf(stderr, "[streams] f%u c%u: %08X %08X %08X %08X (format %u, endian %u, number %u)\n", g_swaps, constant, d[0], d[1], d[2],
                        d[3], (d[2] >> 8) & 63, d[2] & 7, (d[2] >> 16) & 7);
                if ((d[0] >> 30) != kStreamSignature0 || d[1] != kStreamSignature1 || (d[2] >> 20) != kStreamSignature2 ||
                    (d[3] >> 23) != kStreamSignature3)
                    continue;
                uint32_t elementBytes = ElementBytes((d[2] >> 8) & 63);
                if (!elementBytes)
                {
                    LogOnce("memexport format " + std::to_string((d[2] >> 8) & 63) + " isn't supported");
                    continue;
                }
                Stream stream;
                stream.base = (d[0] & 0x3FFFFFFF) << 2;
                stream.bytes = (d[3] & 0x7FFFFF) * elementBytes;
                if (stream.bytes == 0 || stream.bytes > kMaxStreamBytes || streams.size() == kMaxStreams)
                    continue;
                // H3_LOG_STREAMS_AT=<hex physical address>: each distinct
                // stream holding that address (which shader, from when):
                // finds the export overwriting a piece of memory found with
                // H3_WATCH_WRITES (CompleteSlot as the writer). "all": every
                // distinct stream.
                static const char* logAt = EnvString("H3_LOG_STREAMS_AT");
                if (logAt)
                {
                    static const bool all = strcmp(logAt, "all") == 0;
                    static const uint32_t at = uint32_t(strtoul(logAt, nullptr, 16));
                    static std::unordered_map<uint64_t, uint32_t> logged;  // (shader, base, bytes) -> first frame
                    uint64_t key = vs.captureHash ^ (uint64_t(stream.base) << 32) ^ stream.bytes;
                    if ((all || (at >= stream.base && at < stream.base + stream.bytes)) && logged.emplace(key, g_swaps).second)
                        fprintf(stderr, "[streams-at] f%u vs_%016llx c%u: stream %08X+%u (format %u) holds %08X\n", g_swaps,
                            (unsigned long long)vs.captureHash, constant, stream.base, stream.bytes, (d[2] >> 8) & 63, at);
                }
                streams.push_back(stream);
            }
            return streams;
        }

        // --- Batching ----------------------------------------------------
        //
        // Dispatches are queued and recorded together: per dispatch, a copy
        // pass (zero the record count), the compute pass, a scatter pass and
        // a download each cost a pipeline drain, and the GPU ran Halo's ~120
        // tiny dispatches a frame (particle updates of 16 points, visibility
        // tests of one) back to back: ~9 ms of GPU time a frame at the
        // Sierra start. A batch is five passes: inputs and zeroed headers;
        // replays of earlier exports into newly staged inputs; every
        // dispatch (one record buffer, a region each); every scatter into
        // the arena; one download.
        //
        // A batch holds only independent dispatches: one that reads or
        // writes guest memory a queued dispatch writes is queued after the
        // batch is recorded (it must see those exports). Queued dispatches
        // are recorded before anything else records GPU work that could
        // depend on them or change what they read: draws (Flush), resolves,
        // fences, swaps (RecordPendingMemExports).

        // Region alignment in a batch's record buffer (dwords).
        constexpr uint32_t kRegionAlignDwords = 4;
        // Dispatches queued at most before a batch is recorded anyway.
        constexpr size_t kMaxPendingDispatches = 256;

        // A dispatch RunMemExport queued.
        struct PendingDispatch
        {
            SDL_GPUComputePipeline* pipeline = nullptr;
            SDL_GPUBuffer* input = nullptr;  // this frame's input arena
            std::vector<SDL_GPUTextureSamplerBinding> textures;
            std::vector<uint32_t> constants;  // float constants as the shader reads them
            SystemConstants system;
            ExportInfo info{};
            std::vector<Stream> streams;  // guest ranges it writes
            std::vector<Stream> inputs;   // guest ranges its vertex data comes from
            std::vector<std::pair<uint64_t, uint32_t>> staged;  // arena ranges first staged for it
            uint64_t shader = 0;  // capture hash (debugging)
            const Shader* vs = nullptr;
            // Auto-indexed draws: the vertex indices it runs (first, count),
            // else count 0.
            uint32_t firstIndex = 0, indexCount = 0;
        };
        std::vector<PendingDispatch> g_pendingDispatches;

        // Whether two sets of guest ranges overlap.
        bool Overlap(const std::vector<Stream>& a, const std::vector<Stream>& b)
        {
            for (const Stream& x : a)
                for (const Stream& y : b)
                    if (x.base < y.base + y.bytes && y.base < x.base + x.bytes)
                        return true;
            return false;
        }

        // Whether a dispatch depends on (or would race with) a queued one:
        // it reads or writes guest memory the queued one writes. Except
        // runs of one shader over disjoint vertex index ranges, which
        // touch disjoint elements: Halo updates its GPU particles with ~90
        // draws of one shader a frame, 16 particles each, all reading and
        // writing the same particle buffer in place (their stream constant
        // names the whole buffer). Its spawn dispatch, another shader,
        // stays ordered before the updates that read what it wrote.
        bool DependsOnPending(const PendingDispatch& dispatch)
        {
            for (const PendingDispatch& pending : g_pendingDispatches)
            {
                if (!Overlap(dispatch.inputs, pending.streams) && !Overlap(dispatch.streams, pending.streams))
                    continue;
                if (!DisjointRun(dispatch.vs, dispatch.firstIndex, dispatch.indexCount, pending.vs, pending.firstIndex, pending.indexCount))
                    return true;
            }
            return false;
        }

        void RecordPendingDispatches()
        {
            InputArena& arena = g_arenas[g_swaps % kArenaFrames];
            if (g_pendingDispatches.empty())
            {
                // Staged input without a dispatch (DebugVertexPositions).
                if (arena.frame == g_swaps && arena.buffer && arena.used > arena.uploaded)
                {
                    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
                    RecordArenaUpload(copy);
                    SDL_EndGPUCopyPass(copy);
                }
                return;
            }
            GpuTimedScope timed{ "memexport" };  // H3_GPU_TIMING
            std::vector<uint32_t> bases;
            uint32_t dwords = 0;
            for (const PendingDispatch& pending : g_pendingDispatches)
            {
                bases.push_back(dwords);
                dwords += AlignUp(kRecordHeaderDwords + pending.info.capacity * kRecordDwords, kRegionAlignDwords);
            }
            Slot slot = AcquireSlot(dwords * 4);
            if (!slot.exports || !slot.download || !EnsureZeroBuffer())
            {
                ReleaseSlot(slot);
                g_pendingDispatches.clear();
                return;
            }

            // 1. Inputs, and every region's record count zeroed.
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(Commands());
            RecordArenaUpload(copy);
            for (uint32_t base : bases)
            {
                SDL_GPUBufferLocation zero{ g_zeroBuffer, 0 }, header{ slot.exports, base * 4 };
                SDL_CopyGPUBufferToBuffer(copy, &zero, &header, kRecordHeaderBytes, false);
            }
            SDL_EndGPUCopyPass(copy);

            // 2. Guest memory lacks this frame's earlier exports: replay them
            // into the ranges first staged for these dispatches, in order.
            if (g_frameExportsFrame == g_swaps)
                for (const PendingDispatch& pending : g_pendingDispatches)
                    if (!pending.staged.empty())
                        for (const FrameExport& earlier : g_frameExports)
                            ScatterIntoArena(Commands(), earlier.records, earlier.recordBase, earlier.capacity, earlier.streams, &pending.staged);

            // 3. The dispatches, each into its region.
            SDL_GPUCommandBuffer* commands = Commands();
            SDL_GPUStorageBufferReadWriteBinding writable{ slot.exports, false };
            SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &writable, 1);
            for (size_t i = 0; i < g_pendingDispatches.size(); i++)
            {
                PendingDispatch& pending = g_pendingDispatches[i];
                pending.info.recordBase = bases[i];
                SDL_BindGPUComputePipeline(pass, pending.pipeline);
                if (!pending.textures.empty())
                    SDL_BindGPUComputeSamplers(pass, 0, pending.textures.data(), uint32_t(pending.textures.size()));
                SDL_BindGPUComputeStorageBuffers(pass, 0, &pending.input, 1);
                SDL_PushGPUComputeUniformData(commands, 0, pending.constants.data(), uint32_t(pending.constants.size() * 4));
                SDL_PushGPUComputeUniformData(commands, 1, &pending.system, sizeof(pending.system));
                SDL_PushGPUComputeUniformData(commands, 2, &pending.info, sizeof(pending.info));
                SDL_DispatchGPUCompute(pass, (pending.info.vertexCount + kThreadsPerGroup - 1) / kThreadsPerGroup, 1, 1);
            }
            SDL_EndGPUComputePass(pass);

            // 4. Their records into the arena's copies of what they wrote,
            // for later dispatches this frame.
            static const bool noScatter = EnvFlag("H3_NO_EXPORT_SCATTER");
            if (!noScatter && arena.buffer && arena.frame == g_swaps && EnsureScatterPipeline())
            {
                std::vector<std::pair<uint64_t, uint32_t>> ranges(arena.ranges.begin(), arena.ranges.end());
                std::vector<Stream> written;  // the batch's distinct streams (usually one buffer for all)
                for (const PendingDispatch& pending : g_pendingDispatches)
                    for (const Stream& stream : pending.streams)
                        if (std::none_of(written.begin(), written.end(),
                                [&](const Stream& known) { return known.base == stream.base && known.bytes == stream.bytes; }))
                            written.push_back(stream);
                static ScatterInfo info;
                if (ScatterSetup(info, written, ranges))
                {
                    SDL_GPUStorageBufferReadWriteBinding target{ arena.buffer, false };
                    SDL_GPUComputePass* scatter = SDL_BeginGPUComputePass(commands, nullptr, 0, &target, 1);
                    for (size_t i = 0; i < g_pendingDispatches.size(); i++)
                        if (!AddScatterRegion(info, bases[i], g_pendingDispatches[i].info.capacity))
                        {
                            DispatchScatter(commands, scatter, slot.exports, info);
                            AddScatterRegion(info, bases[i], g_pendingDispatches[i].info.capacity);
                        }
                    DispatchScatter(commands, scatter, slot.exports, info);
                    SDL_EndGPUComputePass(scatter);
                }
            }
            for (size_t i = 0; i < g_pendingDispatches.size(); i++)
            {
                const PendingDispatch& pending = g_pendingDispatches[i];
                AddFrameExport({ slot.exports, bases[i], pending.info.capacity, pending.streams, pending.shader, pending.vs,
                    pending.firstIndex, pending.indexCount });
                slot.regions.push_back({ bases[i], pending.info.capacity, pending.streams, pending.shader });
            }

            // 5. The records back to the CPU (applied once the GPU is done).
            copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUBufferRegion results{ slot.exports, 0, dwords * 4 };
            SDL_GPUTransferBufferLocation download{ slot.download, 0 };
            SDL_DownloadFromGPUBuffer(copy, &results, &download);
            SDL_EndGPUCopyPass(copy);
            g_unfenced.push_back(std::move(slot));
            g_pendingDispatches.clear();
        }
    }

    void RunMemExport(const Shader& vs, uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
    {
        static const bool disabled = EnvFlag("H3_NO_MEMEXPORT");
        if (disabled || vs.microcode.empty())
            return;
        std::vector<Stream> streams = FindStreams(vs);
        if (streams.empty())
        {
            LogOnce("memexport draw without a recognizable stream constant");
            return;
        }
        for (const Stream& stream : streams)
            if (std::none_of(g_everStreams.begin(), g_everStreams.end(),
                    [&](const Stream& known) { return known.base == stream.base && known.bytes == stream.bytes; }))
            {
                g_everStreams.push_back(stream);
                // Resident copies of it would miss this frame's exports.
                ForgetResident(stream.base, stream.base + stream.bytes);
            }
        SDL_GPUComputePipeline* pipeline = ComputePipeline(vs);
        if (!pipeline)
            return;

        // Vertex data written by exports still in flight (a particle
        // update reading what this frame's spawns or last frame's update
        // wrote): wait for them first, as the console would see them.
        SyncMemExportsFor(vs);

        PendingDispatch dispatch;
        dispatch.pipeline = pipeline;
        dispatch.streams = std::move(streams);
        dispatch.shader = vs.captureHash;
        dispatch.vs = &vs;
        // Constants, as a draw would have them.
        const uint32_t* alu = AluConstantsHost();
        uint32_t constantFloats = std::max(std::min(vs.info.floatConstantCount * 4, kStageFloats), 4u);
        dispatch.constants.assign(alu, alu + constantFloats);
        SystemConstants& system = dispatch.system;
        memset(&system, 0, sizeof(system));
        SetControlConstants(system);

        // Vertex data the shader fetches, then the indices if any (staged
        // in the frame's input arena).
        std::vector<InputPiece> pieces;
        for (uint32_t index : vs.info.vertexFetchConstants)
        {
            VertexFetch fetch = ReadVertexFetch(index);
            uint32_t* entry = system.vfetch[index];
            if (!fetch.valid || fetch.dwords == 0)
                continue;
            entry[1] = fetch.dwords;
            entry[2] = fetch.endian;
            pieces.push_back({ GuestPtr<uint8_t>(PhysicalToGuest(fetch.address)), fetch.dwords,
                (uint64_t(fetch.address) << 32) | fetch.dwords, &entry[0] });
            dispatch.inputs.push_back({ fetch.address, fetch.dwords * 4 });
        }
        ExportInfo& info = dispatch.info;
        for (size_t i = 0; i < dispatch.streams.size(); i++)
        {
            info.streams[i][0] = dispatch.streams[i].base;
            info.streams[i][1] = dispatch.streams[i].bytes;
        }
        info.vertexCount = initiator >> 16;
        if (info.vertexCount == 0)
            return;
        std::vector<uint32_t> indices;
        bool autoIndexed = ((initiator >> 6) & 3) != 0 || indexBase == 0;
        if (autoIndexed)
        {
            info.indexBase = ReadRegister(kRegVgtIndxOffset);
            dispatch.firstIndex = info.indexBase;
            dispatch.indexCount = info.vertexCount;
        }
        else
        {
            indices = GuestDrawIndices(initiator, indexBase, dmaSize);
            info.indexed = 1;
            pieces.push_back({ indices.data(), uint32_t(indices.size()), 0, &info.indexOffset });
        }
        static const uint32_t kPadding[4] = {};
        uint32_t paddingOffset = 0;
        if (pieces.empty())  // the storage buffer binding needs something
            pieces.push_back({ kPadding, 4, 0, &paddingOffset });

        info.capacity = std::min(info.vertexCount * kRecordsPerVertex, kMaxRecords);
        static const bool logSizes = EnvFlag("H3_LOG_MEMEXPORT");
        static uint32_t loggedSizes = 0;
        constexpr uint32_t kMaxLoggedSizes = 100;
        if (logSizes && loggedSizes++ < kMaxLoggedSizes)
            fprintf(stderr, "[render] memexport dispatch f%u: %u vertices, records %u bytes\n", g_swaps, info.vertexCount,
                (kRecordHeaderDwords + info.capacity * kRecordDwords) * 4);

        // Textures (filling the system constants' texture part).
        dispatch.textures = BindTextures(vs, system);

        // No Flush(): the dispatch reads only its own buffers and textures
        // (resolved ones were flushed by their resolve), so it may run
        // ahead of the pending draws. Flushing here (~100 times a frame in
        // Sierra 117) made draw.cpp stage the big vertex buffers once per
        // flush and ran the machine out of memory.
        // H3_NO_EXPORT_BATCHING=1: a batch per dispatch (to compare).
        static const bool noBatching = EnvFlag("H3_NO_EXPORT_BATCHING");
        if (noBatching || DependsOnPending(dispatch) || g_pendingDispatches.size() >= kMaxPendingDispatches)
            RecordPendingDispatches();
        dispatch.input = StageInput(pieces, &dispatch.staged);
        if (!dispatch.input)
            return;
        g_pendingDispatches.push_back(std::move(dispatch));
    }

    void DebugVertexPositions(const Shader& vs, uint32_t initiator)
    {
        if (vs.microcode.empty())
            return;
        SDL_GPUComputePipeline* pipeline = ComputePipeline(vs);
        if (!pipeline)
            return;
        static std::vector<uint32_t> constants(kStageFloats);
        const uint32_t* alu = AluConstantsHost();
        uint32_t constantFloats = std::max(std::min(vs.info.floatConstantCount * 4, kStageFloats), 4u);
        for (uint32_t i = 0; i < constantFloats; i++)
            constants[i] = alu[i];
        static SystemConstants system;
        memset(&system, 0, sizeof(system));
        SetControlConstants(system);
        std::vector<InputPiece> pieces;
        for (uint32_t index : vs.info.vertexFetchConstants)
        {
            VertexFetch fetch = ReadVertexFetch(index);
            uint32_t* entry = system.vfetch[index];
            if (!fetch.valid || fetch.dwords == 0)
                continue;
            entry[1] = fetch.dwords;
            entry[2] = fetch.endian;
            pieces.push_back({ GuestPtr<uint8_t>(PhysicalToGuest(fetch.address)), fetch.dwords,
                (uint64_t(fetch.address) << 32) | fetch.dwords, &entry[0] });
        }
        static const uint32_t kPadding[4] = {};
        uint32_t paddingOffset = 0;
        if (pieces.empty())
            pieces.push_back({ kPadding, 4, 0, &paddingOffset });
        ExportInfo info{};
        info.vertexCount = std::min(initiator >> 16, 4096u);
        if (info.vertexCount == 0 || ((initiator >> 6) & 3) != 2)  // auto-indexed draws only
            return;
        info.indexBase = ReadRegister(kRegVgtIndxOffset);
        info.capacity = info.vertexCount * 4;  // records of 2 dwords: 8 dwords per vertex
        info.debug = 1;
        std::vector<SDL_GPUTextureSamplerBinding> textures = BindTextures(vs, system);
        if (!EnsureZeroBuffer())
            return;
        RecordPendingDispatches();  // queued dispatches first
        SDL_GPUBuffer* input = StageInput(pieces);
        if (!input)
            return;
        RecordPendingDispatches();  // (nothing queued: just uploads the input)
        uint32_t bytes = (kRecordHeaderDwords + info.capacity * kRecordDwords) * 4;
        SDL_GPUBufferCreateInfo bufferInfo{ SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, bytes, 0 };
        SDL_GPUBuffer* output = SDL_CreateGPUBuffer(g_device, &bufferInfo);
        SDL_GPUTransferBufferCreateInfo transferInfo{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes, 0 };
        SDL_GPUTransferBuffer* download = SDL_CreateGPUTransferBuffer(g_device, &transferInfo);
        SDL_GPUCommandBuffer* commands = Commands();
        SDL_GPUStorageBufferReadWriteBinding writable{ output, false };
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &writable, 1);
        SDL_BindGPUComputePipeline(pass, pipeline);
        if (!textures.empty())
            SDL_BindGPUComputeSamplers(pass, 0, textures.data(), uint32_t(textures.size()));
        SDL_BindGPUComputeStorageBuffers(pass, 0, &input, 1);
        SDL_PushGPUComputeUniformData(commands, 0, constants.data(), constantFloats * 4);
        SDL_PushGPUComputeUniformData(commands, 1, &system, sizeof(system));
        SDL_PushGPUComputeUniformData(commands, 2, &info, sizeof(info));
        SDL_DispatchGPUCompute(pass, (info.vertexCount + kThreadsPerGroup - 1) / kThreadsPerGroup, 1, 1);
        SDL_EndGPUComputePass(pass);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUBufferRegion region{ output, 0, bytes };
        SDL_GPUTransferBufferLocation location{ download, 0 };
        SDL_DownloadFromGPUBuffer(copy, &region, &location);
        SDL_EndGPUCopyPass(copy);
        g_submits++;
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_commands);
        g_commands = nullptr;
        SDL_WaitForGPUFences(g_device, true, &fence, 1);
        SDL_ReleaseGPUFence(g_device, fence);
        const float* data = static_cast<const float*>(SDL_MapGPUTransferBuffer(g_device, download, false));
        for (uint32_t v = 0; v < info.vertexCount; v++)
        {
            const float* p = data + kRecordHeaderDwords + v * 8;
            bool odd = !(p[3] > 0) || std::fabs(p[0]) > 1.5f * std::fabs(p[3]) || std::fabs(p[1]) > 1.5f * std::fabs(p[3]) || p[0] != p[0];
            fprintf(stderr, "[vspos] f%u draw %u v%u: pos %g %g %g %g ndc %g %g | o0 %g %g %g %g%s\n", g_swaps, g_frameDraws, v, p[0], p[1], p[2],
                p[3], p[0] / p[3], p[1] / p[3], p[4], p[5], p[6], p[7], odd ? " ODD" : "");
        }
        SDL_UnmapGPUTransferBuffer(g_device, download);
        SDL_ReleaseGPUTransferBuffer(g_device, download);
        SDL_ReleaseGPUBuffer(g_device, output);
    }

    void ScatterFrameExports(SDL_GPUBuffer* target, const std::vector<std::pair<uint64_t, uint32_t>>& ranges)
    {
        static const bool disabled = EnvFlag("H3_NO_EXPORT_SCATTER");
        if (disabled || g_frameExportsFrame != g_swaps || ranges.empty())
            return;
        // The staged ranges any export lands in (usually the particle
        // buffer, or none): matching every export against every staged
        // range at every flush cost ~2% of this thread.
        std::vector<std::pair<uint64_t, uint32_t>> hit;
        for (const auto& range : ranges)
        {
            uint32_t base = uint32_t(range.first >> 32), bytes = uint32_t(range.first) * 4;
            for (const Stream& stream : g_frameStreams)
                if (base < stream.base + stream.bytes && stream.base < base + bytes)
                {
                    hit.push_back(range);
                    break;
                }
        }
        if (hit.empty() || !EnsureScatterPipeline())
            return;
        // One compute pass for as many as can go together: a scatter
        // writing what one in the pass writes (other than a disjoint run)
        // starts a new pass, since passes keep them in order. One pass
        // each cost ~5% of this thread in driver calls.
        GpuTimedScope timed{ "export scatter" };  // H3_GPU_TIMING
        static ScatterInfo info;
        ScatterSetup(info, g_frameStreams, hit);
        // Whether an export can land in the hit ranges at all.
        auto lands = [&](const FrameExport& dispatch) {
            for (const Stream& stream : dispatch.streams)
                for (uint32_t r = 0; r < info.rangeCount; r++)
                    if (info.ranges[r][0] < stream.base + stream.bytes && stream.base < info.ranges[r][0] + info.ranges[r][1])
                        return true;
            return false;
        };
        SDL_GPUCommandBuffer* commands = Commands();
        SDL_GPUStorageBufferReadWriteBinding writable{ target, false };
        SDL_GPUComputePass* pass = nullptr;
        SDL_GPUBuffer* records = nullptr;  // the record buffer of info's regions
        std::vector<const FrameExport*> inPass;
        for (const FrameExport& dispatch : g_frameExports)
        {
            if (!lands(dispatch))
                continue;
            bool ordered = std::any_of(inPass.begin(), inPass.end(), [&](const FrameExport* other) {
                return Overlap(dispatch.streams, other->streams) &&
                    !DisjointRun(dispatch.vs, dispatch.firstIndex, dispatch.indexCount, other->vs, other->firstIndex, other->indexCount);
            });
            if (pass && ordered)
            {
                DispatchScatter(commands, pass, records, info);
                SDL_EndGPUComputePass(pass);
                pass = nullptr;
                inPass.clear();
            }
            if (!pass)
                pass = SDL_BeginGPUComputePass(commands, nullptr, 0, &writable, 1);
            // Regions of one record buffer go in one dispatch.
            if (dispatch.records != records || info.regionCount == kMaxScatterRegions)
            {
                DispatchScatter(commands, pass, records, info);
                records = dispatch.records;
            }
            AddScatterRegion(info, dispatch.recordBase, dispatch.capacity);
            inPass.push_back(&dispatch);
        }
        if (pass)
        {
            DispatchScatter(commands, pass, records, info);
            SDL_EndGPUComputePass(pass);
        }
    }

    bool MemExportMayWrite(uint32_t begin, uint32_t end)
    {
        for (const Stream& stream : g_everStreams)
            if (begin < stream.base + stream.bytes && stream.base < end)
                return true;
        return false;
    }

    std::string FrameExportsOverlapping(uint32_t begin, uint32_t end)
    {
        std::string text;
        if (g_frameExportsFrame != g_swaps)
            return text;
        char item[64];
        for (size_t i = 0; i < g_frameExports.size(); i++)
            for (const Stream& stream : g_frameExports[i].streams)
                if (begin < stream.base + stream.bytes && end > stream.base)
                {
                    snprintf(item, sizeof(item), " #%zu:%08X+%u:vs_%08llx", i, stream.base, stream.bytes,
                        (unsigned long long)(g_frameExports[i].shader >> 32));
                    text += item;
                }
        return text;
    }

    bool MemExportPending(uint32_t begin, uint32_t end)
    {
        // Exports from earlier frames only: this frame's dispatches all
        // write the same particle streams, so waiting on them would stall
        // every dispatch (96 waits a frame in Sierra, 10 fps).
        for (const Batch& batch : g_batches)
        {
            if (batch.frame >= g_swaps)
                continue;
            for (const Slot& slot : batch.slots)
                for (const Region& region : slot.regions)
                    for (const Stream& stream : region.streams)
                        if (begin < stream.base + stream.bytes && end > stream.base)
                            return true;
        }
        return false;
    }

    namespace
    {
        // Applies a finished batch's results to guest memory, frees it, and
        // does the fence writes that waited for it.
        void CompleteBatch(Batch& batch)
        {
            for (Slot& slot : batch.slots)
            {
                CompleteSlot(slot);
                RecycleSlot(std::move(slot));
            }
            SDL_ReleaseGPUFence(g_device, batch.fence);
            g_completedSequence = batch.sequence;
            ApplyDeferredWrites();
        }
    }

    void SyncMemExportsFor(const Shader& shader)
    {
        // H3_MEMEXPORT_ASYNC=1: never wait (results land a frame or two
        // late, as before; GPU particles then simulate on stale state and
        // short effects - muzzle flashes, splashes - go missing).
        static const bool async = EnvFlag("H3_MEMEXPORT_ASYNC");
        if (async || g_batches.empty() || g_batches.front().frame >= g_swaps)
            return;
        bool pending = false;
        for (uint32_t index : shader.info.vertexFetchConstants)
        {
            VertexFetch fetch = ReadVertexFetch(index);
            if (fetch.valid && fetch.dwords && MemExportPending(fetch.address, fetch.address + fetch.dwords * 4))
            {
                pending = true;
                break;
            }
        }
        if (!pending)
            return;
        // Wait for the earlier frames' batches (usually done by now) and
        // apply them, oldest first.
        SlowTimer timer{ "memexport sync" };
        size_t done = 0;
        for (; done < g_batches.size() && g_batches[done].frame < g_swaps; done++)
        {
            Batch& batch = g_batches[done];
            SDL_WaitForGPUFences(g_device, true, &batch.fence, 1);
            CompleteBatch(batch);
        }
        g_batches.erase(g_batches.begin(), g_batches.begin() + done);
        // Guest memory changed under this frame's input arena: upload
        // ranges again from now on (what's already recorded keeps its
        // copies). Draws stage per flush and see the new memory as is.
        g_arenas[g_swaps % kArenaFrames].ranges.clear();
        g_memExportSyncs++;
    }

    void RecordPendingMemExports()
    {
        RecordPendingDispatches();
    }

    void FenceMemExports()
    {
        RecordPendingDispatches();
        if (g_unfenced.empty() || !g_commands)
            return;
        g_submits++;
        SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(g_commands);
        g_commands = nullptr;
        g_batches.push_back({ fence, std::move(g_unfenced), g_swaps, ++g_batchSequence });
        g_unfenced.clear();
    }

    bool DeferFenceWrite(uint32_t address, uint32_t swapped)
    {
        // H3_MEMEXPORT_FENCE_ASYNC=1: fences land at once (the old
        // behaviour: exports can land after the CPU reused their memory).
        static const bool async = EnvFlag("H3_MEMEXPORT_FENCE_ASYNC");
        if (async)
            return false;
        // Submit what's queued so the fence can wait for it.
        FenceMemExports();
        if (g_batches.empty() && g_deferredWrites.empty())
            return false;
        // Behind the last batch in flight; if none (earlier fence writes
        // still waiting), in order behind those.
        uint64_t sequence = g_batches.empty() ? g_deferredWrites.back().sequence : g_batches.back().sequence;
        g_deferredWrites.push_back({ address, swapped, sequence });
        return true;
    }

    namespace
    {
        // Completes the batches whose GPU work is done (oldest first; they
        // finish in order), without waiting.
        void CompleteFinishedBatches()
        {
            size_t done = 0;
            for (; done < g_batches.size(); done++)
            {
                Batch& batch = g_batches[done];
                if (!SDL_QueryGPUFence(g_device, batch.fence))
                    break;
                CompleteBatch(batch);
            }
            g_batches.erase(g_batches.begin(), g_batches.begin() + done);
        }
    }

    void PollDeferredFenceWrites()
    {
        // Doesn't wait: the command processor calls this while idle or
        // waiting, and blocking it on the GPU there delayed the next
        // frame's packets.
        if (!g_deferredWrites.empty())
            CompleteFinishedBatches();
    }

    void PollMemExports()
    {
        CompleteFinishedBatches();

        // H3_LOG_MEMEXPORT: slot counts every few seconds (free, waiting
        // for a fence, unsubmitted) and the bytes their buffers hold.
        static const bool log = EnvFlag("H3_LOG_MEMEXPORT");
        constexpr uint32_t kLogInterval = 150;
        if (log && g_swaps % kLogInterval == 0)
        {
            size_t waiting = 0;
            uint64_t bytes = 0;
            for (const Slot& slot : g_freeSlots)
                bytes += SlotBytes(slot);
            for (const Batch& batch : g_batches)
                for (const Slot& slot : batch.slots)
                    waiting++, bytes += SlotBytes(slot);
            for (const Slot& slot : g_unfenced)
                bytes += SlotBytes(slot);
            fprintf(stderr, "[render] memexport slots: %zu free, %zu waiting in %zu batches, %zu unsubmitted; %llu MB\n",
                g_freeSlots.size(), waiting, g_batches.size(), g_unfenced.size(), (unsigned long long)(bytes >> 20));
        }
    }
}
