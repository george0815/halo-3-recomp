// Draws. Each guest draw packet becomes a PendingDraw: a host pipeline for
// the current shaders and render state, the shader constants, the vertex
// data it reads (copied out of guest memory), its indices (converted to
// triangle lists) and its textures. Draws into the same render targets
// collect until something forces a flush (a target change, a resolve, the
// swap), and are then recorded as one copy pass (vertex memory and indices)
// plus one render pass.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <map>
#include <string>
#include <cmath>
#include <cstdio>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // Float constants per shader stage: vertex shaders use the guest's
        // constants 0-255, pixel shaders 256-511 (4 floats each).
        constexpr uint32_t kStageConstants = 256 * 4;

        // An allocator whose resize leaves new elements uninitialized
        // (default- rather than value-initialized): staging buffers are
        // written right after they grow, and zero-filling them first showed
        // up in profiles.
        template <typename T>
        struct UninitializedAllocator : std::allocator<T>
        {
            template <typename U>
            struct rebind
            {
                using other = UninitializedAllocator<U>;
            };
            UninitializedAllocator() = default;
            template <typename U>
            UninitializedAllocator(const UninitializedAllocator<U>&) noexcept
            {
            }
            template <typename U>
            void construct(U* p) noexcept
            {
                ::new (static_cast<void*>(p)) U;
            }
            template <typename U, typename... Args>
            void construct(U* p, Args&&... args)
            {
                ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
            }
        };
        using StagingVector = std::vector<uint32_t, UninitializedAllocator<uint32_t>>;
        // Bool constants (8 words of 32 bits) and loop constants.
        constexpr uint32_t kBoolConstantWords = 8;
        constexpr uint32_t kLoopConstants = 32;
        // Upload buffers become reusable this many frames after use.
        constexpr uint32_t kTransferAge = 3;
        // Smallest upload buffer created (bytes); larger needs round up to a
        // multiple of kTransferGranularity. (Powers of two left up to half of
        // each frame's ~30 MB of upload buffers unused: 216 MB held.)
        constexpr uint32_t kMinTransferSize = 64 * 1024;
        constexpr uint32_t kTransferGranularity = 1024 * 1024;
        // Free upload buffers unused this many frames are released.
        constexpr uint32_t kTransferIdleFrames = 120;
        // Free upload buffers kept at most (a few frames' worth; the menu
        // flushes ~90 times a frame); past that the largest are released.
        constexpr size_t kMaxFreeTransfers = 512;
        // ...and at most this many bytes in them: each flush stages its
        // draws' vertex buffers again, so many flushes over big buffers
        // filled the pool with large buffers until the OOM killer stepped in.
        constexpr uint64_t kMaxFreeTransferBytes = 512ull * 1024 * 1024;
        // Smallest vertex memory / index buffer (bytes).
        constexpr uint32_t kMinDrawBufferSize = 1024 * 1024;
        // D3D clear rectangles: 7 floats per vertex (x, y, z, r, g, b, a).
        constexpr uint32_t kClearVertexFloats = 7;
        // PA_CL_VTE_CNTL: XY and Z already in screen space (no viewport
        // transform), as D3D's clears and blits use.
        constexpr uint32_t kVteScreenSpaceXyz = 0x300;
        // The value strips/fans use to restart (always recognized, plus
        // VGT_MULTI_PRIM_IB_RESET_INDX when enabled).
        constexpr uint32_t kResetIndex32 = 0xFFFFFFFFu, kResetIndex16 = 0xFFFFu;
        // Xenos indices are 24 bits.
        constexpr uint32_t kIndexMask = 0xFFFFFF;
        // Tessellation (VGT_OUTPUT_PATH_CNTL path select, VGT_HOS_CNTL modes).
        constexpr uint32_t kOutputPathTessellation = 1;
        constexpr uint32_t kTessellationAdaptive = 2;
        // Adaptive patches come with one tessellation factor per edge in
        // place of indices (3 per triangle patch).
        constexpr uint32_t kTriangleEdges = 3;
        // Host grid segments per patch edge are capped here (the console's
        // tessellator tops out at a factor of 15, plus 1).
        constexpr uint32_t kMaxTessSegments = 16;

        // A draw waiting to be recorded.
        struct PendingDraw
        {
            SDL_GPUGraphicsPipeline* pipeline = nullptr;
            // Its float constants' header in g_constantBuffer (vec4 index,
            // passed as the first instance; see QueueDraw).
            uint32_t constantHeader = 0;
            // H3_UNIFORM_CONSTANTS: each stage's snapshot in g_constantArena
            // (dwords) and its size in floats, pushed by Flush.
            uint32_t constantOffset[2] = {};
            uint32_t constantCount[2] = {};
            SystemConstants system;
            std::vector<SDL_GPUTextureSamplerBinding> vertexTextures, pixelTextures;
            SDL_GPUViewport viewport;
            SDL_Rect scissor;
            uint8_t stencilReference = 0;
            SDL_FColor blendConstants{};
            bool indexed = true;       // false for rectangle lists (the vertex shader expands them)
            uint32_t firstIndex = 0;   // in the frame's index buffer, in indices
            uint32_t count = 0;        // indices (or vertices) to draw
            // Triangles reused from an earlier flush this frame (see
            // g_frameIndices): what to convert again if the index buffer has
            // to grow before this draw's flush.
            bool reusedIndices = false;
            uint32_t initiator = 0, indexBase = 0, dmaSize = 0, resetIndex = 0;
            bool resetEnabled = false;
        };

        TargetSet g_targets;  // what the pending draws render into
        std::vector<std::unique_ptr<PendingDraw>> g_pending;
        // Recorded draws, kept for reuse (allocating one per draw showed up
        // in profiles).
        std::vector<std::unique_ptr<PendingDraw>> g_freeDraws;

        // A PendingDraw to fill (reused when possible). Every field is set by
        // Draw, so a reused one needn't be cleared.
        std::unique_ptr<PendingDraw> NewDraw()
        {
            if (g_freeDraws.empty())
                return std::make_unique<PendingDraw>();
            std::unique_ptr<PendingDraw> draw = std::move(g_freeDraws.back());
            g_freeDraws.pop_back();
            return draw;
        }

        // Uniform slot of the system constants (XeSystem) in both stages;
        // slot 0 is the float constants' block, used by compute shaders only.
        constexpr uint32_t kSystemUniformSlot = 1;
        // (guest address, dwords) -> offset in the memory buffer, so draws
        // sharing a vertex buffer stage it once per flush.
        std::unordered_map<uint64_t, uint32_t> g_stagedRanges;
        // Ranges staged by this frame's earlier flushes, still in the memory
        // buffer (it's only cycled at the frame's first flush): a later
        // flush reuses one whose guest memory hasn't changed (same content
        // hash) instead of copying and uploading it again. Halo's particle
        // buffer alone was staged 7 times a frame (4 MB); everything came to
        // ~20 MB a frame, copied twice on the CPU, at 60 Hz the GPU thread's
        // biggest cost after waiting.
        struct FrameRange
        {
            uint32_t offset;  // dwords into the memory buffer
            uint64_t hash;
            bool hashed;      // false: staged without hashing (see below)
        };
        std::unordered_map<uint64_t, FrameRange> g_frameRanges;
        // Hashing every range when first staged cost ~15 MB of hashing a
        // frame at the Sierra start, for ~30% of ranges ever reused. So a
        // range is hashed when first staged only if it was reused within
        // the last kReuseMemoryFrames frames (the same buffers recur frame
        // after frame), or when a later flush stages it again. Key -> the
        // frame it was last reused in.
        std::unordered_map<uint64_t, uint32_t> g_reuseHistory;
        constexpr uint32_t kReuseMemoryFrames = 60;
        // This flush's reused ranges (key, offset): if the flush has to grow
        // the buffer they're staged again (the new buffer holds only this
        // flush's data).
        std::vector<std::pair<uint64_t, uint32_t>> g_reusedRanges;
        // Smaller ranges aren't worth hashing.
        constexpr uint32_t kMinReuseDwords = 1024;
        // The same for converted indices: draws of one mesh in several
        // passes (depth, lighting, shadows) convert the same guest indices
        // into the same triangle list. Key: everything the conversion
        // depends on and the guest indices' content hash.
        struct FrameIndices
        {
            uint32_t firstIndex;  // in the index buffer
            uint32_t count;
        };
        std::unordered_map<uint64_t, FrameIndices> g_frameIndices;

        SDL_GPUBuffer* g_memoryBuffer = nullptr;
        uint32_t g_memoryBufferSize = 0;
        SDL_GPUBuffer* g_indexBuffer = nullptr;
        uint32_t g_indexBufferSize = 0;
        // Flushes within a frame append to the buffers above at these
        // offsets (in dwords) and only the frame's first flush cycles them.
        // Cycling on every flush made SDL keep one buffer instance per
        // in-flight flush, dozens per frame (gigabytes of host-visible
        // memory with a shared cycled transfer buffer).
        uint32_t g_memoryBase = 0;
        uint32_t g_indexBase = 0;

        // Upload buffers (UploadStream's, per frame), reused once they're
        // kTransferAge frames old so the GPU is done with them. Creating
        // buffers as needed instead cost mmap/ioctl calls that halved the
        // frame rate. (Flushes share a frame's buffer by appending: each
        // flush rewriting one buffer from the start, without cycling,
        // overwrote data still waiting to be uploaded.)
        struct TransferBuffer
        {
            SDL_GPUTransferBuffer* buffer;
            uint32_t size;
            uint32_t freedFrame = 0;  // g_swaps when it last became free
        };
        std::vector<TransferBuffer> g_freeTransfers;
        std::vector<TransferBuffer> g_frameTransfers[kTransferAge + 1];  // in use, by frame

        // An upload buffer of at least size bytes for this frame.
        SDL_GPUTransferBuffer* AcquireTransfer(uint32_t size)
        {
            std::vector<TransferBuffer>& used = g_frameTransfers[g_swaps % (kTransferAge + 1)];
            // Smallest free buffer that fits.
            size_t best = g_freeTransfers.size();
            for (size_t i = 0; i < g_freeTransfers.size(); i++)
                if (g_freeTransfers[i].size >= size && (best == g_freeTransfers.size() || g_freeTransfers[i].size < g_freeTransfers[best].size))
                    best = i;
            if (best < g_freeTransfers.size())
            {
                used.push_back(g_freeTransfers[best]);
                g_freeTransfers.erase(g_freeTransfers.begin() + best);
                return used.back().buffer;
            }
            uint32_t capacity = std::max<uint32_t>(kMinTransferSize, AlignUp(size, kTransferGranularity));
            SDL_GPUTransferBufferCreateInfo info{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, capacity, 0 };
            used.push_back({ SDL_CreateGPUTransferBuffer(g_device, &info), capacity, g_swaps });
            return used.back().buffer;
        }

        // Size of an upload buffer AcquireTransfer returned this frame.
        uint32_t TransferCapacity(SDL_GPUTransferBuffer* buffer)
        {
            for (const TransferBuffer& transfer : g_frameTransfers[g_swaps % (kTransferAge + 1)])
                if (transfer.buffer == buffer)
                    return transfer.size;
            return 0;
        }

        // At a swap: buffers from kTransferAge frames ago become free.
        void RetireTransfers()
        {
            std::vector<TransferBuffer>& oldest = g_frameTransfers[(g_swaps + 1) % (kTransferAge + 1)];
            for (TransferBuffer& transfer : oldest)
                transfer.freedFrame = g_swaps;
            g_freeTransfers.insert(g_freeTransfers.end(), oldest.begin(), oldest.end());
            oldest.clear();
            // Buffers no frame has wanted for a while (left over from a
            // loading screen or a heavier scene) go.
            std::erase_if(g_freeTransfers, [](const TransferBuffer& transfer) {
                if (g_swaps - transfer.freedFrame <= kTransferIdleFrames)
                    return false;
                SDL_ReleaseGPUTransferBuffer(g_device, transfer.buffer);
                return true;
            });
            auto freeBytes = [] {
                uint64_t bytes = 0;
                for (const TransferBuffer& transfer : g_freeTransfers)
                    bytes += transfer.size;
                return bytes;
            };
            while (g_freeTransfers.size() > kMaxFreeTransfers || (!g_freeTransfers.empty() && freeBytes() > kMaxFreeTransferBytes))
            {
                auto largest = std::max_element(g_freeTransfers.begin(), g_freeTransfers.end(),
                    [](const TransferBuffer& a, const TransferBuffer& b) { return a.size < b.size; });
                SDL_ReleaseGPUTransferBuffer(g_device, largest->buffer);
                g_freeTransfers.erase(largest);
            }
        }

        // A frame's upload buffer that staged data is written straight into
        // (vertex data or indices), so Flush only records an upload of its
        // part instead of copying everything a second time (that copy was
        // ~10% of the command processor thread at 60 Hz). Each flush appends
        // after the previous flush's data; the buffer is retired with the
        // frame's other upload buffers (kTransferAge).
        // Written only, never read back: the mapping may be write-combined
        // memory, where reads are very slow.
        class UploadStream
        {
        public:
            // Dwords staged by this flush so far.
            uint32_t size() const { return (m_cursor - m_flushStart) / 4; }

            // Space for up to dwords more dwords at the end of this flush's
            // data; Commit says how many were written.
            uint32_t* Reserve(uint32_t dwords)
            {
                uint64_t needed = uint64_t(m_cursor) + uint64_t(dwords) * 4;
                if (needed > m_capacity)
                    Grow(uint32_t(needed - m_flushStart));
                return reinterpret_cast<uint32_t*>(m_mapped + m_cursor);
            }
            void Commit(uint32_t dwords) { m_cursor += dwords * 4; }
            // Reserve + Commit: dwords to fill in.
            uint32_t* Append(uint32_t dwords)
            {
                uint32_t* out = Reserve(dwords);
                Commit(dwords);
                return out;
            }

            // At a flush: this flush's data as an upload source (at least one
            // dword, so the upload is never empty) and its size in bytes.
            // The next flush continues after it, 16-byte aligned.
            SDL_GPUTransferBufferLocation TakeFlush(uint32_t& bytes)
            {
                if (size() == 0)
                    *Append(1) = 0;
                SDL_UnmapGPUTransferBuffer(g_device, m_buffer);
                bytes = m_cursor - m_flushStart;
                SDL_GPUTransferBufferLocation location{ m_buffer, m_flushStart };
                m_frameBytes += bytes;
                m_cursor = AlignUp(m_cursor, kUploadAlignment);
                m_flushStart = m_cursor;
                // Mapped again for the next flush's data. No cycling: the
                // uploads recorded so far read only what lies before it.
                m_mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(g_device, m_buffer, false));
                return location;
            }

            // At a swap: the next frame starts a new buffer sized after this
            // frame's use (this one is retired with the frame).
            void EndFrame()
            {
                if (m_buffer)
                    SDL_UnmapGPUTransferBuffer(g_device, m_buffer);
                m_lastFrameBytes = m_frameBytes;
                m_buffer = nullptr;
                m_mapped = nullptr;
                m_capacity = m_flushStart = m_cursor = m_frameBytes = 0;
            }

        private:
            // Fresh buffer holding at least flushBytes (this flush's data so
            // far plus what's being reserved). This flush's data moves over;
            // earlier flushes' data stays in the old buffer, whose uploads
            // are already recorded. Rare: the first buffer of a frame is
            // sized from the previous frame.
            void Grow(uint32_t flushBytes)
            {
                uint32_t want = std::max(flushBytes, m_lastFrameBytes + m_lastFrameBytes / kHeadroomDivisor);
                if (m_buffer)
                    want = std::max(want, m_capacity * 2);
                want = std::max(kMinTransferSize, AlignUp(want, kTransferGranularity));
                SDL_GPUTransferBuffer* buffer = AcquireTransfer(want);
                uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(g_device, buffer, false));
                uint32_t carried = m_cursor - m_flushStart;
                if (carried)  // PERF: reads the old mapping; only on growth
                    memcpy(mapped, m_mapped + m_flushStart, carried);
                if (m_buffer)
                    SDL_UnmapGPUTransferBuffer(g_device, m_buffer);
                m_buffer = buffer;
                m_mapped = mapped;
                m_capacity = TransferCapacity(buffer);
                m_flushStart = 0;
                m_cursor = carried;
            }

            // Flushes start at this alignment (bytes) in the buffer.
            static constexpr uint32_t kUploadAlignment = 16;
            // A frame's first buffer gets the previous frame's use plus
            // 1/kHeadroomDivisor of it, so frames rarely grow mid-way.
            static constexpr uint32_t kHeadroomDivisor = 4;

            SDL_GPUTransferBuffer* m_buffer = nullptr;
            uint8_t* m_mapped = nullptr;
            uint32_t m_capacity = 0;    // bytes
            uint32_t m_flushStart = 0;  // bytes: where this flush's data starts
            uint32_t m_cursor = 0;      // bytes: end of the written data
            uint32_t m_frameBytes = 0;      // uploaded this frame
            uint32_t m_lastFrameBytes = 0;  // ...and the previous frame
        };
        // Vertex data the pending draws read, copied from guest memory (the
        // vertex shaders fetch from it as a storage buffer), and their
        // indices.
        UploadStream g_memoryStaging;
        UploadStream g_indexStaging;

        // --- Resident vertex data ------------------------------------------
        //
        // Vertex ranges that stay the same from frame to frame (8.4 of the
        // 9 MB staged a frame at the Sierra start) live in a buffer that
        // isn't cycled: a range is hashed once per flush that uses it and
        // uploaded only when new or changed, instead of being copied into
        // the frame's upload buffer and uploaded again every frame. Vertex
        // shaders read it when the fetch constant's w is set (translator.cpp,
        // XE_RESIDENT). Ranges memory export can write change on the GPU and
        // are never resident (MemExportMayWrite).

        // Smaller ranges are staged per frame (not worth a hash and an entry).
        constexpr uint32_t kMinResidentDwords = 1024;
        // The resident buffer's first size (bytes); it doubles when full.
        constexpr uint32_t kResidentInitialBytes = 32u * 1024 * 1024;
        // Ranges no draw used for this many frames give their space back.
        constexpr uint32_t kResidentIdleFrames = 600;
        // Range alignment in the buffer (bytes).
        constexpr uint32_t kResidentAlign = 16;

        struct ResidentRange
        {
            uint32_t offset = 0;  // bytes into g_residentBuffer
            uint32_t bytes = 0;
            uint64_t hash = 0;    // ContentHash of the guest data it holds
            uint32_t usedFrame = 0;
        };
        std::unordered_map<uint64_t, ResidentRange> g_resident;  // (guest address, dwords) -> range
        // Ranges this flush has checked: (guest address, dwords) -> dword
        // offset. Guest memory can change between flushes of a frame.
        std::unordered_map<uint64_t, uint32_t> g_residentThisFlush;
        SDL_GPUBuffer* g_residentBuffer = nullptr;
        uint32_t g_residentCapacity = 0;  // bytes
        // Free space: offset -> bytes, coalesced.
        std::map<uint32_t, uint32_t> g_residentFree;
        // Space given up, free once the GPU can no longer read it
        // (kTransferAge frames later, as for upload buffers).
        struct RetiredSpace
        {
            uint32_t offset, bytes, frame;
        };
        std::vector<RetiredSpace> g_residentRetired;
        // Data for new or changed ranges, and where each piece goes:
        // source in bytes from the start of this flush's part of the stream.
        UploadStream g_residentStaging;
        struct ResidentCopy
        {
            uint32_t source, destination, bytes;
        };
        std::vector<ResidentCopy> g_residentCopies;
        // A buffer outgrown this flush: its contents are copied into the new
        // one before the flush's uploads (offsets stay valid).
        SDL_GPUBuffer* g_residentGrowSource = nullptr;
        uint32_t g_residentGrowBytes = 0;

        // Gives space back to the free map, merging it with its neighbours.
        void FreeResident(uint32_t offset, uint32_t bytes)
        {
            auto next = g_residentFree.lower_bound(offset);
            if (next != g_residentFree.begin())
            {
                auto previous = std::prev(next);
                if (previous->first + previous->second == offset)
                {
                    offset = previous->first;
                    bytes += previous->second;
                    g_residentFree.erase(previous);
                }
            }
            if (next != g_residentFree.end() && offset + bytes == next->first)
            {
                bytes += next->second;
                g_residentFree.erase(next);
            }
            g_residentFree[offset] = bytes;
        }

        // Makes sure the resident buffer exists (shaders always bind it).
        bool EnsureResidentBuffer()
        {
            if (g_residentBuffer)
                return true;
            SDL_GPUBufferCreateInfo info{};
            info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
            info.size = kResidentInitialBytes;
            g_residentBuffer = SDL_CreateGPUBuffer(g_device, &info);
            if (!g_residentBuffer)
                return false;
            g_residentCapacity = kResidentInitialBytes;
            g_residentFree[0] = kResidentInitialBytes;
            return true;
        }

        // Space for bytes in the resident buffer (first fit), growing it
        // when nothing fits. False if that failed.
        bool AllocateResident(uint32_t bytes, uint32_t& offset)
        {
            for (int attempt = 0; attempt < 2; attempt++)
            {
                for (auto it = g_residentFree.begin(); it != g_residentFree.end(); ++it)
                    if (it->second >= bytes)
                    {
                        offset = it->first;
                        uint32_t rest = it->second - bytes;
                        g_residentFree.erase(it);
                        if (rest)
                            g_residentFree[offset + bytes] = rest;
                        return true;
                    }
                // Grow: twice the size, the old contents copied over at the
                // flush (from the oldest buffer, if it grows twice in one).
                uint32_t capacity = std::max(g_residentCapacity * 2, AlignUp(g_residentCapacity + bytes, kResidentAlign));
                SDL_GPUBufferCreateInfo info{};
                info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
                info.size = capacity;
                SDL_GPUBuffer* grown = SDL_CreateGPUBuffer(g_device, &info);
                if (!grown)
                    return false;
                if (!g_residentGrowSource)
                {
                    g_residentGrowSource = g_residentBuffer;
                    g_residentGrowBytes = g_residentCapacity;
                }
                else
                    SDL_ReleaseGPUBuffer(g_device, g_residentBuffer);  // never used: made this flush
                FreeResident(g_residentCapacity, capacity - g_residentCapacity);
                g_residentBuffer = grown;
                g_residentCapacity = capacity;
            }
            return false;
        }

        // Whether a vertex range is kept resident: large enough, and not
        // something memory export writes. H3_NO_RESIDENT=1: none are.
        bool ResidentEligible(uint32_t physical, uint32_t dwords)
        {
            static const bool disabled = EnvFlag("H3_NO_RESIDENT");
            return !disabled && dwords >= kMinResidentDwords && g_vsDataChecks.empty() &&
                !MemExportMayWrite(physical, physical + dwords * 4);
        }

        // The dword offset in the resident buffer of a vertex range's
        // current data: checked once per flush, uploaded when new or
        // changed. False if it can't be resident after all.
        bool ResidentOffset(uint64_t key, uint32_t address, uint32_t dwords, uint32_t& offset)
        {
            if (auto checked = g_residentThisFlush.find(key); checked != g_residentThisFlush.end())
            {
                offset = checked->second;
                return true;
            }
            if (!EnsureResidentBuffer())
                return false;
            const uint8_t* data = GuestPtr<uint8_t>(address);
            uint32_t bytes = dwords * 4;
            uint64_t hash = ContentHash(data, bytes);
            ResidentRange& range = g_resident[key];
            if (range.bytes == 0 || range.hash != hash)
            {
                // New or changed: new space (what earlier flushes read stays
                // as it was until the GPU is done with it).
                uint32_t space;
                if (!AllocateResident(AlignUp(bytes, kResidentAlign), space))
                {
                    if (range.bytes == 0)
                        g_resident.erase(key);
                    return false;
                }
                if (range.bytes)
                    g_residentRetired.push_back({ range.offset, AlignUp(range.bytes, kResidentAlign), g_swaps });
                g_residentCopies.push_back({ g_residentStaging.size() * 4, space, bytes });
                memcpy(g_residentStaging.Append(dwords), data, bytes);
                g_residentBytes += bytes;
                range = { space, bytes, hash, g_swaps };
            }
            range.usedFrame = g_swaps;
            offset = range.offset / 4;
            g_residentThisFlush.emplace(key, offset);
            return true;
        }

        // At a swap: retired space the GPU is done with becomes free, and
        // ranges unused for a while give theirs up.
        void EndFrameResident();
    }

    void ForgetResident(uint32_t begin, uint32_t end)
    {
        std::erase_if(g_resident, [&](const auto& item) {
            uint32_t physical = GuestToPhysical(uint32_t(item.first >> 32));
            uint32_t bytes = uint32_t(item.first) * 4;
            if (physical >= end || begin >= physical + bytes)
                return false;
            g_residentRetired.push_back({ item.second.offset, AlignUp(item.second.bytes, kResidentAlign), g_swaps });
            g_residentThisFlush.erase(item.first);
            return true;
        });
    }

    namespace
    {
        void EndFrameResident()
        {
            std::erase_if(g_residentRetired, [](const RetiredSpace& space) {
                if (g_swaps - space.frame <= kTransferAge)
                    return false;
                FreeResident(space.offset, space.bytes);
                return true;
            });
            if (g_swaps % kResidentIdleFrames == 0)
                std::erase_if(g_resident, [](const auto& item) {
                    if (g_swaps - item.second.usedFrame <= kResidentIdleFrames)
                        return false;
                    g_residentRetired.push_back({ item.second.offset, AlignUp(item.second.bytes, kResidentAlign), g_swaps });
                    return true;
                });
        }

        // The pending draws' float constants, byte-swapped from the register
        // file into the flush's constant buffer (shaders read them as a
        // storage buffer: see the XE_C declaration in shader/translator.cpp).
        // A draw shares the previous snapshot of its stage when the command
        // stream wrote no constants of that stage in between: Halo issues
        // thousands of draws per frame, and swapping up to 1024 floats per
        // stage for each showed up in profiles.
        UploadStream g_constantStaging;
        // H3_UNIFORM_CONSTANTS: the snapshots on the CPU instead (pushed
        // from here, so not in write-combined memory).
        StagingVector g_constantArena;
        SDL_GPUBuffer* g_constantBuffer = nullptr;
        uint32_t g_constantBufferSize = 0;
        // This flush's part of g_constantBuffer starts here (dwords; flushes
        // within a frame append, like g_memoryBase).
        uint32_t g_constantBase = 0;
        struct ConstantSnapshot
        {
            bool valid = false;
            uint32_t generation = 0;  // AluConstantGeneration when taken
            uint32_t offset = 0;      // dwords into g_constantBuffer
            uint32_t count = 0;       // floats
            const uint32_t* layout = nullptr;  // the shader's compactConstants (null: full layout)
        };
        ConstantSnapshot g_lastSnapshot[2];
        // The last snapshot's contents per stage (cached memory, unlike the
        // upload buffer): Halo rewrites constants without changing them, so
        // ~37% of snapshots at the Sierra start matched the previous one
        // byte for byte (~4 MB a frame not written).
        uint32_t g_lastSnapshotData[2][kStageConstants];
        // A compact layout's constants gathered for the snapshot.
        uint32_t g_gathered[2][kStageConstants];
        // At least one vec4 per snapshot (a shader may read no constants),
        // and snapshots are whole vec4s.
        constexpr uint32_t kMinSnapshotFloats = 4;
        // A draw's header: (vertex base, pixel base, unused, unused) in
        // vec4s, indexed by the draw's first instance. The last one written,
        // shared by following draws with the same snapshots.
        constexpr uint32_t kHeaderDwords = 4;
        uint32_t g_lastHeader = 0;  // vec4 index into g_constantBuffer
        uint32_t g_lastHeaderBases[2] = {};
        bool g_lastHeaderValid = false;

        // A snapshot of the float constants stage's (0 vertex, 1 pixel)
        // shader reads, as they are now: the first ones up to the highest
        // used, or the shader's compact list gathered. Returns its offset
        // (dwords into g_constantBuffer, or into g_constantArena with
        // H3_UNIFORM_CONSTANTS); count receives its size in floats.
        uint32_t SnapshotConstants(uint32_t stage, const shader::ShaderInfo& info, uint32_t& count)
        {
            // H3_NO_CONSTANT_REUSE=1: snapshot every draw (to compare).
            static const bool noReuse = EnvFlag("H3_NO_CONSTANT_REUSE");
            const std::vector<uint32_t>& compact = info.compactConstants;
            const uint32_t* layout = compact.empty() ? nullptr : compact.data();
            count = layout ? uint32_t(compact.size()) * 4 : std::min(info.floatConstantCount * 4, kStageConstants);
            count = AlignUp(std::max(count, kMinSnapshotFloats), kMinSnapshotFloats);
            ConstantSnapshot& last = g_lastSnapshot[stage];
            uint32_t generation = AluConstantGeneration(stage);
            bool sameLayout = last.valid && last.layout == layout && count <= last.count;
            if (!noReuse && sameLayout && last.generation == generation)
                return last.offset;
            const uint32_t* data = AluConstantsHost() + stage * kStageConstants;
            if (layout)
            {
                constexpr uint32_t kFloatsPerConstant = 4;
                for (size_t k = 0; k < compact.size(); k++)
                    memcpy(g_gathered[stage] + k * kFloatsPerConstant, data + compact[k] * kFloatsPerConstant, kFloatsPerConstant * 4);
                data = g_gathered[stage];
            }
            if (!noReuse && sameLayout && memcmp(g_lastSnapshotData[stage], data, size_t(count) * 4) == 0)
            {
                last.generation = generation;
                return last.offset;
            }
            uint32_t offset;
            uint32_t* out;
            if (g_uniformConstants)
            {
                offset = uint32_t(g_constantArena.size());
                g_constantArena.resize(offset + count);
                out = g_constantArena.data() + offset;
            }
            else
            {
                offset = g_constantBase + g_constantStaging.size();
                out = g_constantStaging.Append(count);
            }
            memcpy(out, data, size_t(count) * 4);
            memcpy(g_lastSnapshotData[stage], data, size_t(count) * 4);
            last = { true, generation, offset, count, layout };
            return offset;
        }

        // Queues a filled draw for the next Flush, with snapshots of the float
        // constants its shaders read. (Taken here, last: preparing the draw
        // can flush, which starts a new constant range.)
        void QueueDraw(std::unique_ptr<PendingDraw> draw)
        {
            const shader::ShaderInfo* infos[2] = { &g_vertexShader->info, &g_pixelShader->info };
            if (g_uniformConstants)
            {
                for (uint32_t stage = 0; stage < 2; stage++)
                    draw->constantOffset[stage] = SnapshotConstants(stage, *infos[stage], draw->constantCount[stage]);
                draw->constantHeader = 0;
                g_pending.push_back(std::move(draw));
                return;
            }
            uint32_t bases[2], count;
            bases[0] = SnapshotConstants(0, *infos[0], count) / 4;
            bases[1] = SnapshotConstants(1, *infos[1], count) / 4;
            if (!g_lastHeaderValid || bases[0] != g_lastHeaderBases[0] || bases[1] != g_lastHeaderBases[1])
            {
                g_lastHeader = (g_constantBase + g_constantStaging.size()) / 4;
                uint32_t* header = g_constantStaging.Append(kHeaderDwords);
                header[0] = bases[0], header[1] = bases[1], header[2] = header[3] = 0;
                g_lastHeaderBases[0] = bases[0], g_lastHeaderBases[1] = bases[1];
                g_lastHeaderValid = true;
            }
            draw->constantHeader = g_lastHeader;
            g_pending.push_back(std::move(draw));
        }


        // Makes sure a GPU buffer holds at least size bytes (growing it by
        // doubling). Returns true if the buffer was replaced (its old
        // contents are gone).
        bool EnsureBuffer(SDL_GPUBuffer*& buffer, uint32_t& capacity, uint32_t size, SDL_GPUBufferUsageFlags usage)
        {
            if (size <= capacity && buffer)
                return false;
            if (buffer)
                SDL_ReleaseGPUBuffer(g_device, buffer);
            capacity = std::max<uint32_t>(size, capacity * 2);
            capacity = std::max<uint32_t>(capacity, kMinDrawBufferSize);
            SDL_GPUBufferCreateInfo info{};
            info.usage = usage;
            info.size = capacity;
            buffer = SDL_CreateGPUBuffer(g_device, &info);
            return true;
        }

        // Host primitive type for a Xenos one (strips, fans, quads and
        // rectangles all become triangle lists). False if unsupported.
        bool HostTopology(uint32_t primitive, SDL_GPUPrimitiveType& topology)
        {
            switch (primitive)
            {
            case kPrimPointList: topology = SDL_GPU_PRIMITIVETYPE_POINTLIST; return true;
            case kPrimLineList: topology = SDL_GPU_PRIMITIVETYPE_LINELIST; return true;
            case kPrimLineStrip: topology = SDL_GPU_PRIMITIVETYPE_LINESTRIP; return true;
            case kPrimTriangleList:
            case kPrimTriangleFan:
            case kPrimTriangleStrip:
            case kPrimQuadList:
            case kPrimRectangleList:
            case kPrimTrianglePatch:
                topology = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                return true;
            default:
                return false;
            }
        }

        // --- Vertex data and indices -------------------------------------

        // Guest indices of the draw, made host-endian and offset, or 0..n-1
        // for auto-indexed draws. The VGT_DRAW_INITIATOR has the count in its
        // top 16 bits, the index source in bits 6-7 (0 = DMA) and the index
        // size in bit 11 (1 = 32-bit); dmaSize's top 2 bits are the endian.
        // Writes the initiator's count (top 16 bits) of them to indices.
        void ReadGuestIndices(uint32_t* indices, uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
        {
            uint32_t count = initiator >> 16;
            uint32_t offset = ReadRegister(kRegVgtIndxOffset);
            if (((initiator >> 6) & 3) != 0 || indexBase == 0)
            {
                for (uint32_t i = 0; i < count; i++)
                    indices[i] = i + offset;
                return;
            }
            bool wide = (initiator >> 11) & 1;
            uint32_t endian = dmaSize >> 30;
            for (uint32_t i = 0; i < count; i++)
            {
                if (wide)
                {
                    uint32_t raw;
                    memcpy(&raw, GuestPtr<uint8_t>(indexBase + i * 4), 4);
                    indices[i] = SwapEndian32(raw, endian);
                }
                else
                {
                    uint16_t raw;
                    memcpy(&raw, GuestPtr<uint8_t>(indexBase + i * 2), 2);
                    indices[i] = endian == 1 ? __builtin_bswap16(raw) : raw;  // 8in16 is the only swap for 16-bit
                }
            }
            // The reset index marks strip/fan restarts; keep it recognizable.
            for (uint32_t i = 0; i < count; i++)
                if (indices[i] != (wide ? kResetIndex32 : kResetIndex16))
                    indices[i] = (indices[i] + offset) & kIndexMask;
        }
        // The same as a vector.
        StagingVector GuestIndices(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
        {
            StagingVector indices(initiator >> 16);
            ReadGuestIndices(indices.data(), initiator, indexBase, dmaSize);
            return indices;
        }

        // Whether a primitive's indices are drawn as given (lists): no
        // conversion, so they're read straight into the upload buffer.
        bool IndicesAsGiven(uint32_t primitive)
        {
            return primitive != kPrimTriangleStrip && primitive != kPrimTriangleFan && primitive != kPrimQuadList;
        }

        // Appends the draw's indices to out as the host topology wants them
        // (strips and fans expanded into triangle lists, honoring restarts;
        // quads split in two). Returns how many were appended.
        uint32_t AppendTriangles(UploadStream& out, uint32_t initiator, uint32_t indexBase, uint32_t dmaSize,
            uint32_t resetIndex, bool resetEnabled)
        {
            uint32_t primitive = initiator & 0x3F;
            uint32_t count = initiator >> 16;
            if (IndicesAsGiven(primitive))
            {
                ReadGuestIndices(out.Append(count), initiator, indexBase, dmaSize);
                return count;
            }
            StagingVector in = GuestIndices(initiator, indexBase, dmaSize);
            // At most 3 indices per input index (strips and fans: one
            // triangle per index past the second; quads: 6 per 4).
            uint32_t* dst = out.Reserve(count * 3);
            uint32_t written = 0;
            auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
                dst[written] = a, dst[written + 1] = b, dst[written + 2] = c;
                written += 3;
            };
            auto isReset = [&](uint32_t index) { return resetEnabled && (index == resetIndex || index == kResetIndex32); };
            switch (primitive)
            {
            case kPrimTriangleStrip:
            {
                size_t start = 0;
                for (size_t i = 0; i <= in.size(); i++)
                {
                    if (i < in.size() && !isReset(in[i]))
                        continue;
                    // A run [start, i) between restarts. Odd triangles swap
                    // two vertices to keep the winding.
                    for (size_t j = start; j + 2 < i; j++)
                    {
                        bool odd = (j - start) & 1;
                        emit(in[j], in[odd ? j + 2 : j + 1], in[odd ? j + 1 : j + 2]);
                    }
                    start = i + 1;
                }
                break;
            }
            case kPrimTriangleFan:
                for (size_t j = 1; j + 1 < in.size(); j++)
                    emit(in[0], in[j], in[j + 1]);
                break;
            case kPrimQuadList:
                for (size_t j = 0; j + 3 < in.size(); j += 4)
                {
                    emit(in[j], in[j + 1], in[j + 2]);
                    emit(in[j], in[j + 2], in[j + 3]);
                }
                break;
            }
            out.Commit(written);
            return written;
        }

        // Vertex fetch constants the shader uses: copy their data into the
        // frame's memory staging and point the system constants at it.
        // Ranges an earlier flush this frame staged are reused while their
        // content hash matches (g_frameRanges). PERF: static vertex buffers
        // are still uploaded once per frame; they could stay resident.
        void StageVertexData(const Shader& vs, SystemConstants& system)
        {
            for (uint32_t index : vs.info.vertexFetchConstants)
            {
                VertexFetch fetch = ReadVertexFetch(index);
                uint32_t* entry = system.vfetch[index];
                entry[3] = 0;  // in the frame's memory buffer (1: resident)
                if (!fetch.valid)
                {
                    entry[0] = entry[1] = 0;
                    continue;
                }
                uint32_t address = PhysicalToGuest(fetch.address);
                entry[1] = fetch.dwords;
                entry[2] = fetch.endian;
                // Draws share large vertex buffers: stage each range once per
                // flush.
                uint64_t key = (uint64_t(address) << 32) | fetch.dwords;
                uint32_t resident;
                if (ResidentEligible(fetch.address, fetch.dwords) && ResidentOffset(key, address, fetch.dwords, resident))
                {
                    entry[0] = resident;
                    entry[3] = 1;
                    continue;
                }
                auto staged = g_stagedRanges.find(key);
                if (staged != g_stagedRanges.end())
                {
                    entry[0] = staged->second;
                    continue;
                }
                // Staged by an earlier flush this frame, unchanged since?
                static const bool noReuse = EnvFlag("H3_NO_STAGING_REUSE");
                bool reusable = !noReuse && fetch.dwords >= kMinReuseDwords;
                uint64_t hash = 0;
                bool hashed = false;
                if (reusable)
                {
                    auto earlier = g_frameRanges.find(key);
                    auto history = g_reuseHistory.find(key);
                    bool seen = earlier != g_frameRanges.end();
                    bool recentlyReused = history != g_reuseHistory.end() && g_swaps - history->second <= kReuseMemoryFrames;
                    if (seen || recentlyReused)
                    {
                        hash = ContentHash(GuestPtr<uint8_t>(address), size_t(fetch.dwords) * 4);
                        hashed = true;
                        if (seen && earlier->second.hashed && earlier->second.hash == hash)
                        {
                            entry[0] = earlier->second.offset;
                            g_stagedRanges.emplace(key, entry[0]);
                            g_reusedRanges.push_back({ key, entry[0] });
                            g_reuseHistory[key] = g_swaps;
                            continue;
                        }
                        // Staged twice this frame: from the next frame on,
                        // hash it when first staged so it can be reused.
                        if (seen)
                            g_reuseHistory[key] = g_swaps;
                    }
                }
                entry[0] = g_memoryBase + g_memoryStaging.size();
                g_stagedRanges.emplace(key, entry[0]);
                if (reusable)
                    g_frameRanges[key] = { entry[0], hash, hashed };
                memcpy(g_memoryStaging.Append(fetch.dwords), GuestPtr<uint8_t>(address), size_t(fetch.dwords) * 4);
            }
        }

        // D3D's fast clear: the bulk of a color buffer is cleared by drawing
        // depth-only rectangles through a 4x MSAA view whose depth buffer sits
        // at the color buffer's EDRAM base, so the depth/stencil writes land
        // in the color memory (four samples per pixel: 4x the fill rate).
        // Only the edges are cleared through the color path. Host targets are
        // kept per format, so do the color part here: fill the rectangles of
        // the color target at that base with the clear color, which D3D's
        // clear vertices carry (x, y, z, then r, g, b, a).
        void ClearColorUnderDepth(uint32_t initiator)
        {
            RenderTarget* target = ColorTargetAt(EdramBase(ReadRegister(kRegRbDepthInfo)));
            if (!target)
                return;
            VertexFetch fetch = ReadVertexFetch(0);
            uint32_t count = initiator >> 16;
            if (!fetch.valid || count < 3 || fetch.dwords < count * kClearVertexFloats)
                return;
            float color[4] = { VertexFloat(fetch, 3), VertexFloat(fetch, 4), VertexFloat(fetch, 5), VertexFloat(fetch, 6) };
            for (float c : color)
                if (!std::isfinite(c))
                    return;

            uint32_t surfaceMsaa = SurfaceMsaa();
            int32_t gridX = int32_t(GridX(surfaceMsaa)), gridY = int32_t(GridY(surfaceMsaa));
            Flush();
            for (uint32_t v = 0; v + 2 < count; v += 3)
            {
                float xs[3], ys[3];
                for (uint32_t i = 0; i < 3; i++)
                {
                    xs[i] = VertexFloat(fetch, (v + i) * kClearVertexFloats);
                    ys[i] = VertexFloat(fetch, (v + i) * kClearVertexFloats + 1);
                }
                // Corners sit on pixel edges (-0.5 offsets).
                int32_t x0 = int32_t(std::floor(std::min({ xs[0], xs[1], xs[2] }) + 0.5f)) * gridX;
                int32_t y0 = int32_t(std::floor(std::min({ ys[0], ys[1], ys[2] }) + 0.5f)) * gridY;
                int32_t x1 = int32_t(std::floor(std::max({ xs[0], xs[1], xs[2] }) + 0.5f)) * gridX;
                int32_t y1 = int32_t(std::floor(std::max({ ys[0], ys[1], ys[2] }) + 0.5f)) * gridY;
                x0 = std::clamp(x0, 0, int32_t(TargetWidth())), x1 = std::clamp(x1, x0, int32_t(TargetWidth()));
                y0 = std::clamp(y0, 0, int32_t(TargetHeight())), y1 = std::clamp(y1, y0, int32_t(TargetHeight()));
                FillRect(target->texture, target->format, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0), color);
                RecordColorWrite(EdramBase(ReadRegister(kRegRbDepthInfo)), SDL_Rect{ x0, y0, x1 - x0, y1 - y0 });
            }
        }

        // H3_LOG_CLEARS: screen-space rectangle lists (D3D's clears) with
        // their rectangles, target bases and surface layout.
        void LogClear(uint32_t initiator, uint32_t mode, uint32_t depthControl, uint32_t colorMask)
        {
            VertexFetch fetch = ReadVertexFetch(0);
            uint32_t count = initiator >> 16;
            auto value = [&](uint32_t i) { return VertexFloat(fetch, i); };
            fprintf(stderr, "[clear] f%u d%u mode %u color %08X/%08X depth %08X surface %08X dc %08X mask %08X rects:", g_swaps + 1,
                g_frameDraws, mode, ReadRegister(kRegColorInfo[0]), ReadRegister(kRegColorInfo[1]), ReadRegister(kRegRbDepthInfo),
                ReadRegister(kRegRbSurfaceInfo), depthControl, colorMask);
            constexpr uint32_t kMaxLoggedVertices = 12;
            for (uint32_t v = 0; v + 2 < count && v < kMaxLoggedVertices; v += 3)
                fprintf(stderr, " (%g,%g %g,%g %g,%g)", value(v * kClearVertexFloats), value(v * kClearVertexFloats + 1),
                    value((v + 1) * kClearVertexFloats), value((v + 1) * kClearVertexFloats + 1), value((v + 2) * kClearVertexFloats),
                    value((v + 2) * kClearVertexFloats + 1));
            fprintf(stderr, " z %g color %g %g %g %g clip %08X\n", value(2), value(3), value(4), value(5), value(6),
                ReadRegister(kRegPaClClipCntl));
        }

        // PA_SU_SC_MODE_CNTL polygon offset enables, per facing (bit 13,
        // points and lines, isn't used by Halo 3).
        constexpr uint32_t kPolyOffsetFrontEnable = 1u << 11;
        constexpr uint32_t kPolyOffsetBackEnable = 1u << 12;
        // PA_SU_POLY_OFFSET_*_SCALE is in units of 1/16 of the depth slope
        // per pixel (Xenia: xenos::kPolygonOffsetScaleSubpixelUnit).
        constexpr float kPolyOffsetScaleUnit = 1.0f / 16.0f;

        // Fills the viewport, the NDC transform and the scissor of a draw.
        // The viewport transform maps into host clip space over the whole
        // render target: Xenos screen position = ndc * scale + offset (each
        // part only when enabled in PA_CL_VTE_CNTL), plus the window offset.
        // Screen Y grows down from row 0; SDL_GPU's NDC has +Y up.
        void SetViewportAndScissor(PendingDraw& draw, uint32_t vte, uint32_t modeControl)
        {
            SystemConstants& system = draw.system;
            float xScale = (vte & 1) ? RegisterFloat(kRegPaClVportXScale) : 1.0f;
            float xOffset = (vte & 2) ? RegisterFloat(kRegPaClVportXOffset) : 0.0f;
            float yScale = (vte & 4) ? RegisterFloat(kRegPaClVportYScale) : 1.0f;
            float yOffset = (vte & 8) ? RegisterFloat(kRegPaClVportYOffset) : 0.0f;
            float zScale = (vte & 16) ? RegisterFloat(kRegPaClVportZScale) : 1.0f;
            float zOffset = (vte & 32) ? RegisterFloat(kRegPaClVportZOffset) : 0.0f;
            int32_t offsetX, offsetY;
            WindowOffset(offsetX, offsetY);
            bool windowOffsetEnabled = (modeControl >> 16) & 1;
            float windowX = windowOffsetEnabled ? float(offsetX) : 0.0f, windowY = windowOffsetEnabled ? float(offsetY) : 0.0f;

            // Render targets are in sample space: scale by this surface's
            // samples per pixel (D3D clears 1x surfaces through a 4x view; the
            // menu draws its G-buffer at 4x and lights it at 1x), times the
            // render scale.
            uint32_t surfaceMsaa = SurfaceMsaa();
            float gridX = float(GridX(surfaceMsaa)), gridY = float(GridY(surfaceMsaa));
            float width = float(TargetWidth()), height = float(TargetHeight());
            system.pixelScale[0] = gridX, system.pixelScale[1] = gridY;
            system.renderScale = float(g_renderScale);
            // D3D-style pixel centers sit on integers; the host's are at +0.5.
            if (!(ReadRegister(kRegPaSuVtxCntl) & 1))
                windowX += 0.5f, windowY += 0.5f;
            system.ndcScale[0] = xScale * gridX * 2.0f / width;
            system.ndcOffset[0] = (xOffset + windowX) * gridX * 2.0f / width - 1.0f;
            system.ndcScale[1] = -yScale * gridY * 2.0f / height;
            system.ndcOffset[1] = 1.0f - (yOffset + windowY) * gridY * 2.0f / height;
            system.targetSize[0] = width, system.targetSize[1] = height;
            system.ndcScale[2] = zScale;
            // The polygon offset's constant part moves NDC depth (which the
            // 0..1 viewport maps 1:1 onto depth); the pixel shader adds its
            // slope part. gridX/gridY are host texels per guest pixel.
            PolygonOffset polygonOffset = CurrentPolygonOffset(modeControl, std::max(gridX, gridY));
            system.ndcOffset[2] = zOffset + polygonOffset.constant;
            system.depthBiasSlope = polygonOffset.slope;
            // Point lists: the register's half-width (Halo 3 leaves it at
            // 0.5, one guest pixel), scaled so a point still covers its whole
            // guest pixel at a render scale. Host points are square, so the
            // height is ignored; no Halo 3 shader exports its own point size.
            constexpr float kPointHalfSizeUnit = 1.0f / 16.0f;
            float pointDiameter = float(ReadRegister(kRegPaSuPointSize) >> 16) * kPointHalfSizeUnit * 2.0f;
            system.pointSize = std::max(pointDiameter * gridX, 1.0f);
            draw.viewport = { 0.0f, 0.0f, width, height, 0.0f, 1.0f };

            // Scissor: the window scissor (15-bit coordinates), offset by the
            // window offset unless disabled (bit 31 of the top-left).
            constexpr float kMaxCoordinate = 32767.0f;
            uint32_t tl = ReadRegister(kRegPaScWindowScissorTl), br = ReadRegister(kRegPaScWindowScissorBr);
            int32_t x0 = tl & 0x7FFF, y0 = (tl >> 16) & 0x7FFF, x1 = br & 0x7FFF, y1 = (br >> 16) & 0x7FFF;
            if (!(tl >> 31))
                x0 += offsetX, x1 += offsetX, y0 += offsetY, y1 += offsetY;
            x0 = int32_t(float(x0) * gridX), x1 = int32_t(std::min(float(x1) * gridX, kMaxCoordinate));
            y0 = int32_t(float(y0) * gridY), y1 = int32_t(std::min(float(y1) * gridY, kMaxCoordinate));
            x0 = std::clamp(x0, 0, int32_t(TargetWidth()));
            x1 = std::clamp(x1, x0, int32_t(TargetWidth()));
            y0 = std::clamp(y0, 0, int32_t(TargetHeight()));
            y1 = std::clamp(y1, y0, int32_t(TargetHeight()));
            draw.scissor = { x0, y0, x1 - x0, y1 - y0 };

            static const bool logScissor = EnvFlag("H3_LOG_SCISSOR");
            if (logScissor && (offsetX || offsetY))
            {
                static int logged = 0;
                if (logged++ < 8)
                    fprintf(stderr, "[render] window offset %d,%d tl %08X br %08X -> scissor %d,%d %dx%d ndc y %g %g\n", offsetX,
                        offsetY, tl, br, x0, y0, x1 - x0, y1 - y0, system.ndcScale[1], system.ndcOffset[1]);
            }
        }

        // Fills the system constants that come straight from registers:
        // bool/loop constants, pixel parameter register, position formats,
        // render target exponent biases and clamping, alpha test, depth
        // rounding.
        void SetRegisterConstants(SystemConstants& system, uint32_t vte, const TargetSet& targets)
        {
            SetControlConstants(system);
            // Pixel parameters (position, face) go to the register
            // SQ_CONTEXT_MISC names when SQ_PROGRAM_CNTL enables them.
            constexpr uint32_t kNoParamGen = 0xFF;
            uint32_t programControl = ReadRegister(kRegSqProgramCntl);
            uint32_t contextMisc = ReadRegister(kRegSqContextMisc);
            system.psParamGen = ((programControl >> 18) & 1) ? ((contextMisc >> 8) & 0xFF) : kNoParamGen;

            // Position formats: VTX_XY_FMT, VTX_Z_FMT, VTX_W0_FMT (bits 8-10).
            system.flags |= ((vte >> 8) & 7) << kSystemFlagVteShift;
            for (uint32_t i = 0; i < 4; i++)
            {
                uint32_t info = ReadRegister(kRegColorInfo[i]);
                // Render target exponent bias: color outputs scale by 2^bias
                // (Halo writes its final image to 7e3 with a bias of 5).
                system.colorExpBias[i] = std::ldexp(1.0f, ColorExpBias(info));
                // 8_8_8_8_GAMMA targets are host floats; the console clamps.
                if (ColorFormat(info) == kColorFormat8888Gamma)
                    system.flags |= 1u << (kSystemFlagClampColorShift + i);
            }
            // Alpha test: RB_COLORCONTROL function (bits 0-2) and enable (bit 3).
            uint32_t colorControl = ReadRegister(kRegRbColorControl);
            if (colorControl & 8)
                system.flags |= ((colorControl & 7) << kSystemFlagAlphaFuncShift) | kSystemFlagAlphaTest;
            system.alphaRef = RegisterFloat(kRegRbAlphaRef);
            // D24FS8 depth: round to the 24-bit float format.
            if (targets.depth && DepthIsFloat(ReadRegister(kRegRbDepthInfo)))
                system.flags |= kSystemFlagRoundDepth24;
            if (ApproxGamma())
                system.flags |= kSystemFlagApproxGamma;
        }
    }

    void SetControlConstants(SystemConstants& system)
    {
        for (uint32_t i = 0; i < kBoolConstantWords; i++)
            system.bools[i] = ReadRegister(kRegShaderConstantBool + i);
        for (uint32_t i = 0; i < kLoopConstants; i++)
            system.loops[i] = ReadRegister(kRegShaderConstantLoop + i);
    }

    std::vector<uint32_t> GuestDrawIndices(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
    {
        StagingVector indices = GuestIndices(initiator, indexBase, dmaSize);
        return std::vector<uint32_t>(indices.begin(), indices.end());
    }

    // H3_GPU_TIMING=<first frame> (diagnostic, slow): from that swap on,
    // each flush runs on an otherwise idle GPU and is waited for, so the
    // wait is its GPU time; logged per flush position in the frame every
    // kGpuTimingSwaps swaps, slowest first, with the labelled work between
    // flushes (GpuTimedScope).
    namespace
    {
        bool GpuTimingActive()
        {
            static const char* setting = EnvString("H3_GPU_TIMING");
            static const uint32_t first = setting ? uint32_t(strtoul(setting, nullptr, 10)) : 0;
            return setting && g_swaps >= first;
        }

        constexpr uint32_t kGpuTimingSwaps = 60;
        struct FlushTiming
        {
            double seconds = 0;
            double gapSeconds = 0;  // the untracked work just before it
            uint32_t count = 0, draws = 0;
            uint32_t colorInfo = 0, depthInfo = 0;
        };
        std::vector<FlushTiming> g_flushTimings;
        uint32_t g_flushIndex = 0, g_flushTimingFrame = ~0u, g_flushTimingStart = 0;
        double g_otherSeconds = 0;  // waits for untracked work between flushes
        uint32_t g_otherWaits = 0;  // ...and how many (each costs an empty submit's time)
        double g_gapSeconds = 0;    // the wait before the current flush
        std::vector<const char*> g_timedLabels;  // open GpuTimedScopes, innermost last
        std::map<std::string, double> g_labelSeconds;

        // Attributes a wait to the innermost open scope, or to untracked work.
        void AttributeWait(double seconds)
        {
            if (g_timedLabels.empty())
                g_otherSeconds += seconds, g_otherWaits += seconds > 0;
            else
                g_labelSeconds[g_timedLabels.back()] += seconds;
        }

        // Submits what's recorded so far and waits for it; returns the wait.
        double SubmitAndWait()
        {
            if (!g_commands)
                return 0;  // nothing recorded since the last submit
            g_submits++;
            auto start = std::chrono::steady_clock::now();
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(Commands());
            g_commands = nullptr;
            if (fence)
            {
                SDL_WaitForGPUFences(g_device, true, &fence, 1);
                SDL_ReleaseGPUFence(g_device, fence);
            }
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }

        void ReportFlushTimings()
        {
            uint32_t frames = g_swaps - g_flushTimingStart;
            std::vector<uint32_t> order(g_flushTimings.size());
            for (uint32_t i = 0; i < order.size(); i++)
                order[i] = i;
            std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_flushTimings[a].seconds > g_flushTimings[b].seconds; });
            double total = 0;
            uint32_t flushes = 0;
            for (const FlushTiming& timing : g_flushTimings)
                total += timing.seconds, flushes += timing.count;
            // The cost of a submit and wait itself: an empty one, timed.
            static const double floor = [] {
                constexpr int kSamples = 20;
                double sum = 0;
                for (int i = 0; i < kSamples; i++)
                {
                    Commands();  // an empty command buffer
                    sum += SubmitAndWait();
                }
                return sum / kSamples;
            }();
            fprintf(stderr, "[gpu timing] frame %u: draw flushes %.2f ms/frame (%.1f flushes, empty submit %.3f ms), other work %.2f ms/frame"
                            " (without the waits' own cost: draws %.2f, other %.2f); slowest:",
                g_swaps, total * 1000 / frames, double(flushes) / frames, floor * 1000, g_otherSeconds * 1000 / frames,
                (total - flushes * floor) * 1000 / frames, (g_otherSeconds - g_otherWaits * floor) * 1000 / frames);
            g_otherSeconds = 0;
            g_otherWaits = 0;
            for (const auto& [label, seconds] : g_labelSeconds)
                fprintf(stderr, " [%s %.2f ms]", label.c_str(), seconds * 1000 / frames);
            g_labelSeconds.clear();
            for (uint32_t i = 0; i < std::min<size_t>(order.size(), 12); i++)
            {
                const FlushTiming& t = g_flushTimings[order[i]];
                fprintf(stderr, " #%u %.2fms/%ud (c%08X d%08X)", order[i], t.seconds * 1000 / frames, t.count ? t.draws / t.count : 0,
                    t.colorInfo, t.depthInfo);
            }
            // The longest waits for untracked work, by the flush they precede.
            std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_flushTimings[a].gapSeconds > g_flushTimings[b].gapSeconds; });
            fprintf(stderr, "; longest gaps before:");
            for (uint32_t i = 0; i < std::min<size_t>(order.size(), 6); i++)
                fprintf(stderr, " #%u %.2fms", order[i], g_flushTimings[order[i]].gapSeconds * 1000 / frames);
            fprintf(stderr, "\n");
            g_flushTimings.clear();
            g_flushTimingStart = g_swaps;
        }
    }

    GpuTimedScope::GpuTimedScope(const char* label)
    {
        // H3_GPU_TIMING_COARSE=1: no waits around labelled work (waiting
        // after each of hundreds of small dispatches inflates them): all
        // work between two flushes is timed together, as other work.
        // H3_GPU_TIMING_LABELS=<label>[,...]: only those labels get waits.
        static const bool coarse = EnvFlag("H3_GPU_TIMING_COARSE");
        static const char* only = EnvString("H3_GPU_TIMING_LABELS");
        if (!GpuTimingActive() || coarse)
            return;
        if (only && !strstr((std::string(",") + only + ",").c_str(), (std::string(",") + label + ",").c_str()))
            return;
        active = true;
        AttributeWait(SubmitAndWait());
        g_timedLabels.push_back(label);
    }

    GpuTimedScope::~GpuTimedScope()
    {
        if (!active)
            return;
        AttributeWait(SubmitAndWait());
        g_timedLabels.pop_back();
    }

    namespace
    {
        // Records the resident buffer's pending work: the old contents of a
        // buffer outgrown since the last flush, then the uploads of new or
        // changed ranges. Also when a flush has no draws (a draw that staged
        // a range can still be dropped): left for a later frame, the uploads
        // would read from where the next frame's staging data is.
        void UploadResidentRanges(SDL_GPUCommandBuffer* commands)
        {
            EnsureResidentBuffer();
            if (g_residentGrowSource)
            {
                // In a pass of their own, so the uploads below land after them.
                SDL_GPUCopyPass* grow = SDL_BeginGPUCopyPass(commands);
                SDL_GPUBufferLocation from{ g_residentGrowSource, 0 }, to{ g_residentBuffer, 0 };
                SDL_CopyGPUBufferToBuffer(grow, &from, &to, g_residentGrowBytes, false);
                SDL_EndGPUCopyPass(grow);
                SDL_ReleaseGPUBuffer(g_device, g_residentGrowSource);
                g_residentGrowSource = nullptr;
            }
            if (g_residentCopies.empty())
                return;
            uint32_t bytes = 0;
            SDL_GPUTransferBufferLocation source = g_residentStaging.TakeFlush(bytes);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
            for (const ResidentCopy& piece : g_residentCopies)
            {
                SDL_GPUTransferBufferLocation from{ source.transfer_buffer, source.offset + piece.source };
                SDL_GPUBufferRegion to{ g_residentBuffer, piece.destination, piece.bytes };
                SDL_UploadToGPUBuffer(copy, &from, &to, false);
            }
            SDL_EndGPUCopyPass(copy);
            g_residentCopies.clear();
        }
    }

    void Flush()
    {
        // Queued memory exports first: the draws may read their results
        // (ScatterFrameExports below), and they must see what the draws'
        // inputs were before this.
        RecordPendingMemExports();
        if (g_pending.empty())
        {
            if (!g_residentCopies.empty() || g_residentGrowSource)
                UploadResidentRanges(Commands());
            return;
        }
        bool timing = GpuTimingActive();
        uint32_t timedDraws = uint32_t(g_pending.size());
        if (timing)
        {
            if (g_flushTimingFrame != g_swaps)
            {
                g_flushTimingFrame = g_swaps;
                g_flushIndex = 0;
                if (g_swaps - g_flushTimingStart >= kGpuTimingSwaps)
                    ReportFlushTimings();
            }
            g_gapSeconds = SubmitAndWait();  // everything before this flush
            AttributeWait(g_gapSeconds);
        }
        SDL_GPUCommandBuffer* commands = Commands();

        // Staged offsets already include the bases, so this flush's data goes
        // to [base, base + size) of each buffer. A replaced buffer only needs
        // to hold this flush's range: earlier passes keep the old one until
        // they finish.
        // Growing the memory buffer replaces it, and the new one holds only
        // this flush's data: stage the reused ranges again (their guest
        // memory is unchanged) and point the draws at the copies; earlier
        // flushes' ranges are gone.
        if (g_memoryBase != 0 && (g_memoryBase * 4 + std::max<size_t>(g_memoryStaging.size(), 1) * 4 > g_memoryBufferSize))
        {
            std::unordered_map<uint32_t, uint32_t> moved;  // old offset -> new
            for (const auto& [key, offset] : g_reusedRanges)
            {
                uint32_t dwords = uint32_t(key);
                uint32_t newOffset = g_memoryBase + g_memoryStaging.size();
                memcpy(g_memoryStaging.Append(dwords), GuestPtr<uint8_t>(uint32_t(key >> 32)), size_t(dwords) * 4);
                moved[offset] = newOffset;
                g_stagedRanges[key] = newOffset;
            }
            if (!moved.empty())
                for (std::unique_ptr<PendingDraw>& draw : g_pending)
                    for (auto& entry : draw->system.vfetch)
                        if (auto it = moved.find(entry[0]); entry[1] != 0 && it != moved.end())
                            entry[0] = it->second;
            std::erase_if(g_frameRanges, [](const auto& item) { return item.second.offset < g_memoryBase; });
            for (const auto& [key, offset] : g_stagedRanges)
                if (auto it = g_frameRanges.find(key); it != g_frameRanges.end())
                    it->second.offset = offset;
        }
        g_reusedRanges.clear();
        // The same for reused triangle lists: convert them again into this
        // flush's part of the (new) index buffer.
        if (g_indexBase != 0 && (g_indexBase * 4 + std::max<size_t>(g_indexStaging.size(), 1) * 4 > g_indexBufferSize))
        {
            for (std::unique_ptr<PendingDraw>& draw : g_pending)
            {
                if (!draw->reusedIndices)
                    continue;
                uint32_t first = g_indexStaging.size();
                AppendTriangles(g_indexStaging, draw->initiator, draw->indexBase, draw->dmaSize, draw->resetIndex, draw->resetEnabled);
                draw->firstIndex = g_indexBase + first;
                draw->reusedIndices = false;
            }
            std::erase_if(g_frameIndices, [](const auto& item) { return item.second.firstIndex < g_indexBase; });
        }
        uint32_t memoryBytes = 0, indexBytes = 0;
        SDL_GPUTransferBufferLocation memorySource = g_memoryStaging.TakeFlush(memoryBytes);
        SDL_GPUTransferBufferLocation indexSource = g_indexStaging.TakeFlush(indexBytes);
        uint32_t constantBytes = 0;
        SDL_GPUTransferBufferLocation constantSource = g_constantStaging.TakeFlush(constantBytes);
        // New or changed resident vertex ranges (before the draws' uploads).
        UploadResidentRanges(commands);
        bool constantCycle = g_constantBase == 0;
        if (EnsureBuffer(g_constantBuffer, g_constantBufferSize, g_constantBase * 4 + constantBytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ))
            constantCycle = false;
        bool memoryCycle = g_memoryBase == 0;
        bool indexCycle = g_indexBase == 0;
        // Compute-writable too: this frame's memory exports are scattered
        // into it (ScatterFrameExports).
        if (EnsureBuffer(g_memoryBuffer, g_memoryBufferSize, g_memoryBase * 4 + memoryBytes,
                SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE))
            memoryCycle = false;
        if (EnsureBuffer(g_indexBuffer, g_indexBufferSize, g_indexBase * 4 + indexBytes, SDL_GPU_BUFFERUSAGE_INDEX))
            indexCycle = false;
        g_stagedBytes += memoryBytes;
        g_stagedIndexBytes += indexBytes;
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUBufferRegion destination{ g_memoryBuffer, g_memoryBase * 4, memoryBytes };
        SDL_UploadToGPUBuffer(copy, &memorySource, &destination, memoryCycle);
        destination = { g_indexBuffer, g_indexBase * 4, indexBytes };
        SDL_UploadToGPUBuffer(copy, &indexSource, &destination, indexCycle);
        destination = { g_constantBuffer, g_constantBase * 4, constantBytes };
        SDL_UploadToGPUBuffer(copy, &constantSource, &destination, constantCycle);
        SDL_EndGPUCopyPass(copy);
        // Particle state the frame's exports have written since: GPU
        // particles are drawn right after their update.
        {
            std::vector<std::pair<uint64_t, uint32_t>> ranges;
            ranges.reserve(g_stagedRanges.size());
            for (const auto& [key, offset] : g_stagedRanges)
                ranges.push_back({ (uint64_t(GuestToPhysical(uint32_t(key >> 32))) << 32) | uint32_t(key), offset });
            ScatterFrameExports(g_memoryBuffer, ranges);
            commands = Commands();  // (H3_GPU_TIMING may have submitted it)
        }
        if (!g_vsDataChecks.empty())
        {
            VerifyVsData(g_memoryBuffer, g_stagedRanges);
            commands = Commands();
        }
        // Next flush's offsets, 16-byte aligned.
        g_memoryBase = AlignUp(g_memoryBase * 4 + memoryBytes, 16) / 4;
        g_indexBase = AlignUp(g_indexBase * 4 + indexBytes, 16) / 4;
        g_constantBase = AlignUp(g_constantBase * 4 + constantBytes, 16) / 4;

        SDL_GPUColorTargetInfo colors[4]{};
        for (uint32_t i = 0; i < g_targets.colorCount; i++)
        {
            colors[i].texture = g_targets.color[i];
            colors[i].load_op = SDL_GPU_LOADOP_LOAD;
            colors[i].store_op = SDL_GPU_STOREOP_STORE;
        }
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = g_targets.depth;
        depth.load_op = SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_LOAD;
        depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
        CountPass(PassKind::Draw, 0);
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, colors, g_targets.colorCount, g_targets.depth ? &depth : nullptr);

        SDL_GPUBufferBinding indexBinding{ g_indexBuffer, 0 };
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        // Vertex storage buffers: vertex memory, float constants (unless
        // pushed as uniforms), resident vertex memory (the order of the
        // translator's bindings); fragment: float constants.
        if (g_uniformConstants)
        {
            SDL_GPUBuffer* vertexBuffers[] = { g_memoryBuffer, g_residentBuffer };
            SDL_BindGPUVertexStorageBuffers(pass, 0, vertexBuffers, 2);
        }
        else
        {
            SDL_GPUBuffer* vertexBuffers[] = { g_memoryBuffer, g_constantBuffer, g_residentBuffer };
            SDL_BindGPUVertexStorageBuffers(pass, 0, vertexBuffers, 3);
            SDL_BindGPUFragmentStorageBuffers(pass, 0, &g_constantBuffer, 1);
        }
        // State is set only when it differs from the previous draw's: draws
        // in a row mostly share pipelines, viewports and system constants.
        const PendingDraw* previous = nullptr;
        for (const auto& draw : g_pending)
        {
            if (!previous || draw->pipeline != previous->pipeline)
                SDL_BindGPUGraphicsPipeline(pass, draw->pipeline);
            if (!previous || memcmp(&draw->viewport, &previous->viewport, sizeof(draw->viewport)) != 0)
                SDL_SetGPUViewport(pass, &draw->viewport);
            if (!previous || memcmp(&draw->scissor, &previous->scissor, sizeof(draw->scissor)) != 0)
                SDL_SetGPUScissor(pass, &draw->scissor);
            if (!previous || draw->stencilReference != previous->stencilReference)
                SDL_SetGPUStencilReference(pass, draw->stencilReference);
            if (!previous || memcmp(&draw->blendConstants, &previous->blendConstants, sizeof(draw->blendConstants)) != 0)
                SDL_SetGPUBlendConstants(pass, draw->blendConstants);
            if (!draw->vertexTextures.empty())
                SDL_BindGPUVertexSamplers(pass, 0, draw->vertexTextures.data(), uint32_t(draw->vertexTextures.size()));
            if (!draw->pixelTextures.empty())
                SDL_BindGPUFragmentSamplers(pass, 0, draw->pixelTextures.data(), uint32_t(draw->pixelTextures.size()));
            // H3_UNIFORM_CONSTANTS: float constants pushed when the snapshot
            // differs from the previous draw's.
            if (g_uniformConstants)
                for (uint32_t stage = 0; stage < 2; stage++)
                    if (!previous || draw->constantOffset[stage] != previous->constantOffset[stage] ||
                        draw->constantCount[stage] > previous->constantCount[stage])
                    {
                        const uint32_t* data = g_constantArena.data() + draw->constantOffset[stage];
                        if (stage == 0)
                            SDL_PushGPUVertexUniformData(commands, 0, data, draw->constantCount[stage] * 4);
                        else
                            SDL_PushGPUFragmentUniformData(commands, 0, data, draw->constantCount[stage] * 4);
                    }
            // System constants, pushed to both stages when they changed (the
            // float constants are in g_constantBuffer, found through the
            // draw's header: its first instance).
            if (!previous || memcmp(&draw->system, &previous->system, sizeof(draw->system)) != 0)
            {
                SDL_PushGPUVertexUniformData(commands, kSystemUniformSlot, &draw->system, sizeof(draw->system));
                SDL_PushGPUFragmentUniformData(commands, kSystemUniformSlot, &draw->system, sizeof(draw->system));
            }
            previous = draw.get();
            if (draw->indexed)
                SDL_DrawGPUIndexedPrimitives(pass, draw->count, 1, draw->firstIndex, 0, draw->constantHeader);
            else
                SDL_DrawGPUPrimitives(pass, draw->count, 1, 0, draw->constantHeader);
        }
        SDL_EndGPURenderPass(pass);
        if (timing)
        {
            double seconds = SubmitAndWait();
            if (g_flushTimings.size() <= g_flushIndex)
                g_flushTimings.resize(g_flushIndex + 1);
            FlushTiming& entry = g_flushTimings[g_flushIndex++];
            entry.seconds += seconds;
            entry.count++;
            entry.draws += timedDraws;
            entry.colorInfo = ReadRegister(kRegColorInfo[0]);
            entry.depthInfo = ReadRegister(kRegRbDepthInfo);
            entry.gapSeconds += g_gapSeconds;
        }

        for (std::unique_ptr<PendingDraw>& draw : g_pending)
            g_freeDraws.push_back(std::move(draw));
        g_pending.clear();
        g_lastSnapshot[0].valid = g_lastSnapshot[1].valid = false;
        g_lastHeaderValid = false;
        g_stagedRanges.clear();
        g_residentThisFlush.clear();
        g_constantArena.clear();
    }

    void EndFrameBuffers()
    {
        g_memoryBase = g_indexBase = g_constantBase = 0;
        // OnSwap flushed, but a flush with no draws left (all dropped after
        // staging) returns early: forget what points into this frame's
        // buffers.
        g_stagedRanges.clear();
        g_reusedRanges.clear();
        g_lastSnapshot[0].valid = g_lastSnapshot[1].valid = false;
        g_lastHeaderValid = false;
        g_frameRanges.clear();  // the next flush cycles the memory buffer
        g_frameIndices.clear();
        if (g_swaps % kReuseMemoryFrames == 0)
            std::erase_if(g_reuseHistory, [](const auto& item) { return g_swaps - item.second > kReuseMemoryFrames; });
        g_memoryStaging.EndFrame();
        g_indexStaging.EndFrame();
        g_constantStaging.EndFrame();
        g_residentStaging.EndFrame();
        g_residentThisFlush.clear();
        EndFrameResident();
        RetireTransfers();
    }

    PolygonOffset CurrentPolygonOffset(uint32_t modeControl, float pixelScale)
    {
        // One offset per draw: the host can't pick per facing, so the front
        // one wins when both are on (Halo sets them equal).
        PolygonOffset offset;
        // H3_NO_POLY_OFFSET=1: ignore polygon offset (to compare).
        static const bool disabled = EnvFlag("H3_NO_POLY_OFFSET");
        if (disabled)
            return offset;
        uint32_t scaleRegister, offsetRegister;
        if (modeControl & kPolyOffsetFrontEnable)
            scaleRegister = kRegPaSuPolyOffsetFrontScale, offsetRegister = kRegPaSuPolyOffsetFrontOffset;
        else if (modeControl & kPolyOffsetBackEnable)
            scaleRegister = kRegPaSuPolyOffsetBackScale, offsetRegister = kRegPaSuPolyOffsetBackOffset;
        else
            return offset;
        // The host measures the slope per host texel, which is pixelScale
        // times smaller than per guest pixel.
        offset.slope = RegisterFloat(scaleRegister) * kPolyOffsetScaleUnit * pixelScale;
        offset.constant = RegisterFloat(offsetRegister);
        return offset;
    }

    VideoMemoryUse DrawBufferMemory()
    {
        VideoMemoryUse use;
        use.count = (g_memoryBuffer ? 1 : 0) + (g_indexBuffer ? 1 : 0) + (g_constantBuffer ? 1 : 0) + (g_residentBuffer ? 1 : 0);
        use.bytes = uint64_t(g_memoryBufferSize) + g_indexBufferSize + g_constantBufferSize + g_residentCapacity;
        return use;
    }

    VideoMemoryUse UploadBufferMemory()
    {
        VideoMemoryUse use;
        auto add = [&](const std::vector<TransferBuffer>& buffers) {
            for (const TransferBuffer& transfer : buffers)
            {
                use.count++;
                use.bytes += transfer.size;
            }
        };
        add(g_freeTransfers);
        for (const std::vector<TransferBuffer>& frame : g_frameTransfers)
            add(frame);
        return use;
    }

    const TargetSet& CurrentTargetSet()
    {
        return g_targets;
    }

    void Draw(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
    {
        uint32_t primitive = initiator & 0x3F;
        // 0x10-0x12 are tessellation patches here. Triangle patches (Halo's
        // water) are drawn as a uniform grid per patch (DrawTessellated
        // below); line and quad patches are skipped as unknown types.
        uint32_t mode = ReadRegister(kRegRbModeControl) & 7;
        if (mode != kEdramModeColorDepth && mode != kEdramModeDepthOnly)
            return;
        uint32_t vte = ReadRegister(kRegPaClVteCntl);
        bool screenSpaceRectangles = primitive == kPrimRectangleList && (vte & kVteScreenSpaceXyz) == kVteScreenSpaceXyz;
        if (mode == kEdramModeDepthOnly && screenSpaceRectangles)
            ClearColorUnderDepth(initiator);

        // Both shaders must exist: with async compilation, a shader still
        // compiling skips the draw; otherwise this waits for it (warm-up may
        // be making it).
        if (!g_vertexShader || !g_pixelShader || !ShaderReady(*g_vertexShader, !g_asyncShaders) ||
            !ShaderReady(*g_pixelShader, !g_asyncShaders))
            return;

        // Vertex data that exports in flight write (GPU particles drawn
        // from the state their update just exported): wait for it. And
        // after a run of export dispatches, submit them with a fence right
        // away: fenced only at the frame's end, next frame's wait above
        // waited for the whole frame (20 fps).
        if (!g_vertexShader->info.usesMemExport)
        {
            SyncMemExportsFor(*g_vertexShader);
            FenceMemExports();
        }

        // Memory export (occlusion tests, Halo's water): run the shader's
        // compute variant for the writes (memexport.cpp). Shaders that only
        // write memory have nothing to rasterize; drawing them would put
        // points at the default position.
        if (g_vertexShader->info.usesMemExport)
        {
            RunMemExport(*g_vertexShader, initiator, indexBase, dmaSize);
            if (!g_vertexShader->info.writesPosition)
                return;
        }

        uint32_t colorMask = ReadRegister(kRegRbColorMask);
        uint32_t depthControl = ReadRegister(kRegRbDepthControl);
        // With the depth test off, depth isn't written either (as Xenia's
        // GetNormalizedDepthControl has it; a host pipeline can't write
        // depth without testing anyway). Halo's GPU particle updates (memory
        // export point draws, color mask 0) set Z_WRITE_ENABLE alone: bound
        // to the depth buffer for that, they rasterized ~1,500 points a
        // frame that wrote nothing, ~10 ms of GPU time at the Sierra start.
        if (!(depthControl & kDepthControlZEnable))
            depthControl &= ~kDepthControlZWriteEnable;
        TargetSet targets = CurrentTargets(*g_pixelShader, colorMask, depthControl, mode);
        if (targets.colorCount == 0 && !targets.depth)
            return;
        // Occlusion query draws (depth-only, between EVENT_WRITE_ZPD
        // reports) go into the query id texture to count their samples.
        uint32_t queryId = targets.depth ? OcclusionQueryDraw(mode) : 0;
        if (queryId)
        {
            targets.color[0] = OcclusionQueryTexture();
            targets.colorFormat[0] = SDL_GPU_TEXTUREFORMAT_R32_UINT;
            targets.colorCount = 1;
        }

        SDL_GPUPrimitiveType topology;
        if (!HostTopology(primitive, topology))
        {
            LogOnce("unsupported primitive type " + std::to_string(primitive));
            return;
        }

        if (!(targets == g_targets))
        {
            Flush();
            g_targets = targets;
        }
        // The pipeline: shaders plus the render state baked into it.
        uint32_t stencilRefMask = ReadRegister(kRegRbStencilRefMask);
        PipelineDesc desc{};
        desc.vs = g_vertexShader->cacheId;
        desc.ps = g_pixelShader->cacheId;
        desc.topology = topology;
        desc.colorCount = targets.colorCount;
        desc.depthFormat = targets.depth ? uint32_t(g_depthFormat) : 0;
        for (uint32_t i = 0; i < 4; i++)
        {
            desc.colorFormats[i] = i < targets.colorCount ? uint32_t(targets.colorFormat[i]) : 0;
            desc.blend[i] = i < targets.colorCount ? ReadRegister(kRegRbBlendControl[i]) : 0;
        }
        desc.colorMask = colorMask;
        desc.depthControl = targets.depth ? depthControl : 0;
        desc.stencilMasks = targets.depth ? stencilRefMask & 0xFFFF00 : 0;  // not the reference (dynamic state)
        desc.psColorTargetMask = g_pixelShader->info.colorTargetMask;
        LogVertexData(*g_vertexShader, initiator);
        if (!queryId && targets.colorCount && HighlightDraw(*g_vertexShader))
        {
            // Show what the shader reads: each vertex fetch range and its
            // first dwords as floats (big-endian in memory).
            static uint32_t logged = 0;
            if (logged++ < 6)
                for (uint32_t index : g_vertexShader->info.vertexFetchConstants)
                {
                    VertexFetch fetch = ReadVertexFetch(index);
                    fprintf(stderr, "[render]   vfetch %u: valid %d address %08X dwords %u endian %u:", index, fetch.valid, fetch.address,
                        fetch.dwords, fetch.endian);
                    for (uint32_t i = 0; fetch.valid && i < std::min(fetch.dwords, 24u); i++)
                    {
                        uint32_t bits = __builtin_bswap32(GuestRef<uint32_t>(PhysicalToGuest(fetch.address) + i * 4).value);
                        float f;
                        memcpy(&f, &bits, 4);
                        fprintf(stderr, " %g", f);
                    }
                    fprintf(stderr, " | draw count %u\n", initiator >> 16);
                }
            desc.ps = kHighlightPixelShader;
            desc.blend[0] = 0x00010001;
            desc.colorMask = (desc.colorMask & ~0xFu) | 0xF;
            desc.psColorTargetMask = 1;
            desc.depthControl = 0;
            desc.rasterizer &= ~kRasterizerMask;
        }
        if (queryId)
        {
            constexpr uint32_t kAllChannels = 0xF;
            constexpr uint32_t kBlendOneZero = 0x00010001;  // RB_BLENDCONTROL: no blending
            desc.ps = kOcclusionQueryPixelShader;
            desc.blend[0] = kBlendOneZero;
            desc.colorMask = kAllChannels;
            desc.psColorTargetMask = 1;
            // No depth writes: the query only tests.
            desc.depthControl &= ~kDepthControlZWriteEnable;
        }
        // Culling, for real triangles only: rectangle lists are expanded in
        // the vertex shader and tessellated patches drawn as host grids,
        // neither in the guest's winding. Culling both faces draws nothing.
        // H3_NO_CULL=1 turns culling off (to compare).
        static const bool noCull = EnvFlag("H3_NO_CULL");
        uint32_t cull = ReadRegister(kRegPaSuScModeCntl) & kRasterizerMask;
        bool triangles = primitive == kPrimTriangleList || primitive == kPrimTriangleStrip || primitive == kPrimTriangleFan ||
            primitive == kPrimQuadList;
        if ((cull & (kRasterizerCullFront | kRasterizerCullBack)) == (kRasterizerCullFront | kRasterizerCullBack) && triangles && !noCull)
            return;
        if (triangles && !noCull)
            desc.rasterizer = cull;
        if (ReadRegister(kRegPaClClipCntl) & kClipCntlClipDisable)
            desc.rasterizer |= kRasterizerNoDepthClip;
        SDL_GPUGraphicsPipeline* pipeline = GetPipeline(desc);
        if (!pipeline)
            return;

        std::unique_ptr<PendingDraw> draw = NewDraw();
        draw->pipeline = pipeline;
        draw->stencilReference = uint8_t(stencilRefMask);
        draw->blendConstants = { RegisterFloat(kRegRbBlendRed), RegisterFloat(kRegRbBlendRed + 1), RegisterFloat(kRegRbBlendRed + 2),
            RegisterFloat(kRegRbBlendRed + 3) };


        SystemConstants& system = draw->system;
        memset(&system, 0, sizeof(system));
        uint32_t modeControl = ReadRegister(kRegPaSuScModeCntl);
        SetViewportAndScissor(*draw, vte, modeControl);
        SetRegisterConstants(system, vte, targets);

        static const bool logDepth = EnvFlag("H3_LOG_DEPTH");
        if (logDepth)
        {
            static std::unordered_map<uint64_t, int> seen;
            uint64_t key = (uint64_t(depthControl) << 32) | (uint64_t(mode) << 8) | primitive;
            if (seen[key]++ == 0)
                fprintf(stderr, "[render] depth: mode %u prim %u depthcontrol %08X z scale %g offset %g vte %03X depth info %08X\n", mode,
                    primitive, depthControl, system.ndcScale[2], system.ndcOffset[2], vte, ReadRegister(kRegRbDepthInfo));
        }
        static const bool logClears = EnvFlag("H3_LOG_CLEARS");
        if (logClears && screenSpaceRectangles)
            LogClear(initiator, mode, depthControl, colorMask);

        StageVertexData(*g_vertexShader, system);
        draw->vertexTextures = BindTextures(*g_vertexShader, system);
        draw->pixelTextures = BindTextures(*g_pixelShader, system);
        if (desc.ps == kHighlightPixelShader)
            draw->pixelTextures.clear();
        if (queryId)
        {
            draw->pixelTextures.clear();  // the query shader samples nothing
            system.queryId = queryId;
        }

        // At a render scale, screen-space draws (rectangle lists, or no
        // viewport scale: D3D's full-screen passes and the UI) have their
        // corners snapped to whole guest pixels. Their edges can sit on pixel
        // centers, which covers the whole pixel on the console but only part
        // of the scaled pixel here, leaving stale host texels along the edge.
        // 3D geometry keeps its sub-pixel edges.
        if (g_renderScale > 1 && (primitive == kPrimRectangleList || !(vte & 1)))
            system.flags |= kSystemFlagSnapToPixels;

        if (primitive == kPrimTrianglePatch)
        {
            if ((ReadRegister(kRegVgtOutputPathCntl) & 3) != kOutputPathTessellation)
            {
                LogOnce("triangle patches without tessellation enabled");
                g_freeDraws.push_back(std::move(draw));
                return;
            }
            // No hardware tessellator through SDL_GPU: each patch becomes a
            // uniform grid (the vertex shader's tessellated path), at the
            // maximum tessellation level. Adaptive per-edge factors (the
            // "indices") only pick a coarser level per edge, so they're
            // ignored: the surface is the same, just finer everywhere,
            // and a uniform level can't crack between patches.
            uint32_t count = initiator >> 16;
            bool adaptive = (ReadRegister(kRegVgtHosCntl) & 3) == kTessellationAdaptive;
            uint32_t patches = adaptive ? count / kTriangleEdges : count;
            float level = RegisterFloat(kRegVgtHosMaxTessLevel) + 1.0f;
            uint32_t segments = std::isfinite(level) ? uint32_t(std::clamp(std::ceil(level), 1.0f, float(kMaxTessSegments))) : 1;
            LogOnce("tessellated triangle patches: " + std::to_string(segments) + " segments per edge" +
                (adaptive ? " (adaptive factors ignored)" : ""));
            system.flags |= kSystemFlagTessellated;
            system.tessSegments = segments;
            system.tessPatchBase = ReadRegister(kRegVgtIndxOffset);
            draw->indexed = false;
            draw->count = patches * kTriangleEdges * segments * segments;
            if (draw->count == 0)
            {
                g_freeDraws.push_back(std::move(draw));
                return;
            }
            QueueDraw(std::move(draw));
            return;
        }

        if (primitive == kPrimRectangleList)
        {
            StagingVector indices = GuestIndices(initiator, indexBase, dmaSize);
            // Rectangles: the vertex shader reads the three given vertices'
            // indices from the memory buffer and makes two triangles.
            system.flags |= kSystemFlagRectangleList;
            system.rectIndices = g_memoryBase + g_memoryStaging.size();
            uint32_t rects = uint32_t(indices.size() / 3);
            memcpy(g_memoryStaging.Append(rects * 3), indices.data(), size_t(rects) * 3 * 4);
            draw->indexed = false;
            draw->count = rects * 6;
        }
        else
        {
            uint32_t resetIndex = ReadRegister(kRegVgtMultiPrimIbResetIndx);
            bool resetEnabled = (modeControl >> 21) & 1;
            draw->indexed = true;
            draw->initiator = initiator, draw->indexBase = indexBase, draw->dmaSize = dmaSize;
            draw->resetIndex = resetIndex, draw->resetEnabled = resetEnabled;
            draw->reusedIndices = false;
            // Converted by an earlier draw this frame?
            static const bool noReuse = EnvFlag("H3_NO_STAGING_REUSE");
            uint64_t key = 0;
            if (!noReuse)
            {
                bool autoIndexed = ((initiator >> 6) & 3) != 0 || indexBase == 0;
                uint32_t count = initiator >> 16;
                uint32_t bytes = autoIndexed ? 0 : count * (((initiator >> 11) & 1) ? 4 : 2);
                uint64_t parameters[] = { initiator, autoIndexed ? 0 : indexBase, dmaSize >> 30, ReadRegister(kRegVgtIndxOffset),
                    resetEnabled ? resetIndex : 0, uint64_t(resetEnabled),
                    bytes ? ContentHash(GuestPtr<uint8_t>(indexBase), bytes) : 0 };
                key = ContentHash(parameters, sizeof(parameters));
                auto earlier = g_frameIndices.find(key);
                if (earlier != g_frameIndices.end())
                {
                    draw->firstIndex = earlier->second.firstIndex;
                    draw->count = earlier->second.count;
                    draw->reusedIndices = true;
                }
            }
            if (!draw->reusedIndices)
            {
                uint32_t first = g_indexStaging.size();
                draw->count = AppendTriangles(g_indexStaging, initiator, indexBase, dmaSize, resetIndex, resetEnabled);
                draw->firstIndex = g_indexBase + first;
                if (key)
                    g_frameIndices[key] = { draw->firstIndex, draw->count };
            }
        }
        if (draw->count == 0)
        {
            g_freeDraws.push_back(std::move(draw));
            return;
        }
        SDL_Rect scissor = draw->scissor;
        QueueDraw(std::move(draw));
        // The color targets this draw writes (for EDRAM format switches,
        // targets.cpp), as CurrentTargets picked them.
        if (mode == kEdramModeColorDepth)
            for (uint32_t i = 0; i < 4; i++)
                if (((colorMask >> (4 * i)) & 0xF) && (g_pixelShader->info.colorTargetMask & (1u << i)))
                    RecordColorWrite(EdramBase(ReadRegister(kRegColorInfo[i])), scissor);

        // EDRAM aliasing of color onto depth. After predicated tiling Halo
        // restores the whole frame's depth buffer by drawing the resolved
        // depth's bytes as 8_8_8_8 color into a target at the depth buffer's
        // own EDRAM base; later draws (particles, the sky, transparents)
        // depth-test against it. Host color and depth targets are separate
        // textures, so turn what was drawn back into depth (or stencil).
        // H3_NO_DEPTH_RESTORE=1 turns this off (to compare);
        // H3_NO_STENCIL_RESTORE=1 only the stencil part.
        static const bool noDepthRestore = EnvFlag("H3_NO_DEPTH_RESTORE");
        if (mode == kEdramModeColorDepth && !noDepthRestore)
        {
            uint32_t depthInfo = ReadRegister(kRegRbDepthInfo);
            uint32_t depthBase = EdramBase(depthInfo);
            for (uint32_t i = 0; i < targets.colorCount; i++)
            {
                uint32_t colorInfo = ReadRegister(kRegColorInfo[i]);
                // Writing all four bytes restores depth. Writing red alone
                // writes the stencil byte (the low byte of the D24S8 word):
                // each predicated tile starts by drawing an 8x8 dither
                // pattern there, which also clears the upper bits; objects
                // fading with distance then stencil-test their fade level
                // against it (RB_STENCILREFMASK ref vs. mask 0x1F).
                constexpr uint32_t kAllChannels = 0xF, kRedOnly = 0x1;
                uint32_t channels = (colorMask >> (4 * i)) & 0xF;
                bool written = (channels == kAllChannels || channels == kRedOnly) && (g_pixelShader->info.colorTargetMask & (1u << i));
                if (!written || EdramBase(colorInfo) != depthBase || ColorFormat(colorInfo) != kColorFormat8888)
                    continue;
                RenderTarget* depth = GetRenderTarget(depthBase, DepthIsFloat(depthInfo), true);
                if (!depth)
                    break;
                Flush();
                if (channels == kRedOnly)
                {
                    static const bool noStencilRestore = EnvFlag("H3_NO_STENCIL_RESTORE");
                    if (!noStencilRestore)
                        RestoreStencilFromBytes(targets.color[i], depth->texture, TargetWidth(), TargetHeight(), scissor);
                    DumpDepthAfterRestore(targets.color[i], targets.colorFormat[i], depth->texture);
                    break;
                }
                RestoreDepthFromBytes(targets.color[i], depth->texture, TargetWidth(), TargetHeight(), scissor, DepthIsFloat(depthInfo));
                DumpDepthAfterRestore(targets.color[i], targets.colorFormat[i], depth->texture);
                break;
            }
        }
    }
}
