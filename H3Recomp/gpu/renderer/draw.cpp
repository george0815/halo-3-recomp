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
#include <cmath>
#include <cstdio>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        // Float constants per shader stage: vertex shaders use the guest's
        // constants 0-255, pixel shaders 256-511 (4 floats each).
        constexpr uint32_t kStageConstants = 256 * 4;
        // Bool constants (8 words of 32 bits) and loop constants.
        constexpr uint32_t kBoolConstantWords = 8;
        constexpr uint32_t kLoopConstants = 32;
        // Upload buffers become reusable this many frames after use.
        constexpr uint32_t kTransferAge = 3;
        // Smallest upload buffer created (bytes); larger needs round up to a
        // power of two.
        constexpr uint32_t kMinTransferSize = 64 * 1024;
        // Free upload buffers kept at most (a few frames' worth; the menu
        // flushes ~90 times a frame); past that the largest are released.
        constexpr size_t kMaxFreeTransfers = 512;
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

        // A draw waiting to be recorded.
        struct PendingDraw
        {
            SDL_GPUGraphicsPipeline* pipeline = nullptr;
            float vertexConstants[kStageConstants];
            float pixelConstants[kStageConstants];
            SystemConstants system;
            std::vector<SDL_GPUTextureSamplerBinding> vertexTextures, pixelTextures;
            SDL_GPUViewport viewport;
            SDL_Rect scissor;
            uint8_t stencilReference = 0;
            SDL_FColor blendConstants{};
            bool indexed = true;       // false for rectangle lists (the vertex shader expands them)
            uint32_t firstIndex = 0;   // in the frame's index buffer, in indices
            uint32_t count = 0;        // indices (or vertices) to draw
        };

        TargetSet g_targets;  // what the pending draws render into
        std::vector<std::unique_ptr<PendingDraw>> g_pending;
        // Vertex data the pending draws read, copied from guest memory (the
        // vertex shaders fetch from it as a storage buffer), and their
        // indices.
        std::vector<uint32_t> g_memoryStaging;
        std::vector<uint32_t> g_indexStaging;
        // (guest address, dwords) -> offset in the memory buffer, so draws
        // sharing a vertex buffer stage it once per flush.
        std::unordered_map<uint64_t, uint32_t> g_stagedRanges;

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

        // Upload buffers for Flush: one per flush (writing several flushes
        // into one shared buffer without cycling lost data), reused once
        // they're kTransferAge frames old so the GPU is done with them.
        // Creating a buffer per flush instead cost mmap/ioctl calls that
        // halved the frame rate.
        struct TransferBuffer
        {
            SDL_GPUTransferBuffer* buffer;
            uint32_t size;
        };
        std::vector<TransferBuffer> g_freeTransfers;
        std::vector<TransferBuffer> g_frameTransfers[kTransferAge + 1];  // in use, by frame

        std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline*> g_pipelines;  // render state hash -> pipeline

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
            uint32_t capacity = std::max<uint32_t>(kMinTransferSize, std::bit_ceil(size));
            SDL_GPUTransferBufferCreateInfo info{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, capacity, 0 };
            used.push_back({ SDL_CreateGPUTransferBuffer(g_device, &info), capacity });
            return used.back().buffer;
        }

        // At a swap: buffers from kTransferAge frames ago become free.
        void RetireTransfers()
        {
            std::vector<TransferBuffer>& oldest = g_frameTransfers[(g_swaps + 1) % (kTransferAge + 1)];
            g_freeTransfers.insert(g_freeTransfers.end(), oldest.begin(), oldest.end());
            oldest.clear();
            while (g_freeTransfers.size() > kMaxFreeTransfers)
            {
                auto largest = std::max_element(g_freeTransfers.begin(), g_freeTransfers.end(),
                    [](const TransferBuffer& a, const TransferBuffer& b) { return a.size < b.size; });
                SDL_ReleaseGPUTransferBuffer(g_device, largest->buffer);
                g_freeTransfers.erase(largest);
            }
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

        // --- Pipelines ---------------------------------------------------

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

        // The host pipeline for shaders + render state (created on first use;
        // creating one can take the driver hundreds of milliseconds).
        SDL_GPUGraphicsPipeline* GetPipeline(Shader* vs, Shader* ps, SDL_GPUPrimitiveType topology, const TargetSet& targets,
            const uint32_t blend[4], uint32_t colorMask, uint32_t depthControl, uint32_t stencilRefMask)
        {
            // Everything that goes into the pipeline, hashed as the cache key.
            struct Key
            {
                SDL_GPUShader* vs;
                SDL_GPUShader* ps;
                uint32_t topology, colorCount, depthFormat;
                uint32_t colorFormats[4];
                uint32_t blend[4];
                uint32_t colorMask, depthControl, stencilMasks;
            } key{};
            key.vs = vs->shader;
            key.ps = ps->shader;
            key.topology = topology;
            key.colorCount = targets.colorCount;
            key.depthFormat = targets.depth ? g_depthFormat : 0;
            for (uint32_t i = 0; i < 4; i++)
            {
                key.colorFormats[i] = targets.colorFormat[i];
                key.blend[i] = blend[i];
            }
            key.colorMask = colorMask;
            key.depthControl = depthControl;
            key.stencilMasks = stencilRefMask & 0xFFFF00;  // not the reference (dynamic state)
            uint64_t hash = Hash(&key, sizeof(key));
            auto it = g_pipelines.find(hash);
            if (it != g_pipelines.end())
                return it->second;

            SlowTimer timer{ "pipeline", vs->cacheName + " " + ps->cacheName };
            SDL_GPUColorTargetDescription descriptions[4]{};
            for (uint32_t i = 0; i < targets.colorCount; i++)
            {
                descriptions[i].format = targets.colorFormat[i];
                descriptions[i].blend_state = BlendState(blend[i]);
                // Targets the pixel shader doesn't write keep their contents.
                uint32_t mask = (colorMask >> (4 * i)) & 0xF;
                if (!(ps->info.colorTargetMask & (1u << i)))
                    mask = 0;
                descriptions[i].blend_state.color_write_mask = SDL_GPUColorComponentFlags(mask);
                descriptions[i].blend_state.enable_color_write_mask = true;
            }

            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vs->shader;
            info.fragment_shader = ps->shader;
            info.primitive_type = topology;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;  // TODO: PA_SU_SC_MODE_CNTL culling
            info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
            info.rasterizer_state.enable_depth_clip = true;
            info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            if (targets.depth)
                info.depth_stencil_state = DepthStencilState(depthControl, stencilRefMask);
            info.target_info.color_target_descriptions = descriptions;
            info.target_info.num_color_targets = targets.colorCount;
            info.target_info.has_depth_stencil_target = targets.depth != nullptr;
            info.target_info.depth_stencil_format = g_depthFormat;

            SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(g_device, &info);
            if (!pipeline)
                LogOnce(std::string("pipeline creation failed: ") + SDL_GetError());
            g_pipelines.emplace(hash, pipeline);
            return pipeline;
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
        std::vector<uint32_t> GuestIndices(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
        {
            uint32_t count = initiator >> 16;
            std::vector<uint32_t> indices(count);
            uint32_t offset = ReadRegister(kRegVgtIndxOffset);
            if (((initiator >> 6) & 3) != 0 || indexBase == 0)
            {
                for (uint32_t i = 0; i < count; i++)
                    indices[i] = i + offset;
                return indices;
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
            for (uint32_t& index : indices)
                if (index != (wide ? kResetIndex32 : kResetIndex16))
                    index = (index + offset) & kIndexMask;
            return indices;
        }

        // Appends the draw's triangles to out as a triangle list (strips and
        // fans expanded, honoring restarts; quads split in two).
        void AppendTriangles(std::vector<uint32_t>& out, const std::vector<uint32_t>& in, uint32_t primitive, uint32_t resetIndex,
            bool resetEnabled)
        {
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
                        out.push_back(in[j]);
                        out.push_back(in[odd ? j + 2 : j + 1]);
                        out.push_back(in[odd ? j + 1 : j + 2]);
                    }
                    start = i + 1;
                }
                break;
            }
            case kPrimTriangleFan:
                for (size_t j = 1; j + 1 < in.size(); j++)
                {
                    out.push_back(in[0]);
                    out.push_back(in[j]);
                    out.push_back(in[j + 1]);
                }
                break;
            case kPrimQuadList:
                for (size_t j = 0; j + 3 < in.size(); j += 4)
                    out.insert(out.end(), { in[j], in[j + 1], in[j + 2], in[j], in[j + 2], in[j + 3] });
                break;
            default:
                out.insert(out.end(), in.begin(), in.end());
                break;
            }
        }

        // Vertex fetch constants the shader uses: copy their data into the
        // frame's memory staging and point the system constants at it.
        // PERF: copies every vertex buffer a draw reads, every flush; vertex
        // buffers that don't change could stay resident on the GPU.
        void StageVertexData(const Shader& vs, SystemConstants& system)
        {
            for (uint32_t index : vs.info.vertexFetchConstants)
            {
                VertexFetch fetch = ReadVertexFetch(index);
                uint32_t* entry = system.vfetch[index];
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
                auto staged = g_stagedRanges.find(key);
                if (staged != g_stagedRanges.end())
                {
                    entry[0] = staged->second;
                    continue;
                }
                entry[0] = g_memoryBase + uint32_t(g_memoryStaging.size());
                g_stagedRanges.emplace(key, entry[0]);
                size_t start = g_memoryStaging.size();
                g_memoryStaging.resize(start + fetch.dwords);
                memcpy(g_memoryStaging.data() + start, GuestPtr<uint8_t>(address), size_t(fetch.dwords) * 4);
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
            int32_t gridX = int32_t(SamplesX(surfaceMsaa)), gridY = int32_t(SamplesY(surfaceMsaa));
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
                x0 = std::clamp(x0, 0, int32_t(kRenderTargetWidth)), x1 = std::clamp(x1, x0, int32_t(kRenderTargetWidth));
                y0 = std::clamp(y0, 0, int32_t(kRenderTargetHeight)), y1 = std::clamp(y1, y0, int32_t(kRenderTargetHeight));
                FillRect(target->texture, target->format, x0, y0, uint32_t(x1 - x0), uint32_t(y1 - y0), color);
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
            fprintf(stderr, " color %g %g %g %g\n", value(3), value(4), value(5), value(6));
        }

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
            // D3D-style pixel centers sit on integers; the host's are at +0.5.
            if (!(ReadRegister(kRegPaSuVtxCntl) & 1))
                windowX += 0.5f, windowY += 0.5f;

            // Render targets are in sample space: scale by this surface's
            // samples per pixel (D3D clears 1x surfaces through a 4x view; the
            // menu draws its G-buffer at 4x and lights it at 1x).
            uint32_t surfaceMsaa = SurfaceMsaa();
            float gridX = float(SamplesX(surfaceMsaa)), gridY = float(SamplesY(surfaceMsaa));
            float width = float(kRenderTargetWidth), height = float(kRenderTargetHeight);
            system.pixelScale[0] = gridX, system.pixelScale[1] = gridY;
            system.ndcScale[0] = xScale * gridX * 2.0f / width;
            system.ndcOffset[0] = (xOffset + windowX) * gridX * 2.0f / width - 1.0f;
            system.ndcScale[1] = -yScale * gridY * 2.0f / height;
            system.ndcOffset[1] = 1.0f - (yOffset + windowY) * gridY * 2.0f / height;
            system.ndcScale[2] = zScale;
            system.ndcOffset[2] = zOffset;
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
            x0 = std::clamp(x0, 0, int32_t(kRenderTargetWidth));
            x1 = std::clamp(x1, x0, int32_t(kRenderTargetWidth));
            y0 = std::clamp(y0, 0, int32_t(kRenderTargetHeight));
            y1 = std::clamp(y1, y0, int32_t(kRenderTargetHeight));
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
            for (uint32_t i = 0; i < kBoolConstantWords; i++)
                system.bools[i] = ReadRegister(kRegShaderConstantBool + i);
            for (uint32_t i = 0; i < kLoopConstants; i++)
                system.loops[i] = ReadRegister(kRegShaderConstantLoop + i);
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
        }
    }

    void Flush()
    {
        if (g_pending.empty())
            return;
        SDL_GPUCommandBuffer* commands = Commands();

        // Staged offsets already include the bases, so this flush's data goes
        // to [base, base + size) of each buffer. A replaced buffer only needs
        // to hold this flush's range: earlier passes keep the old one until
        // they finish.
        uint32_t memoryBytes = uint32_t(std::max<size_t>(g_memoryStaging.size(), 1) * 4);
        uint32_t indexBytes = uint32_t(std::max<size_t>(g_indexStaging.size(), 1) * 4);
        bool memoryCycle = g_memoryBase == 0;
        bool indexCycle = g_indexBase == 0;
        if (EnsureBuffer(g_memoryBuffer, g_memoryBufferSize, g_memoryBase * 4 + memoryBytes, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ))
            memoryCycle = false;
        if (EnsureBuffer(g_indexBuffer, g_indexBufferSize, g_indexBase * 4 + indexBytes, SDL_GPU_BUFFERUSAGE_INDEX))
            indexCycle = false;
        // Cycling is only a safety net: the buffer is kTransferAge frames
        // old, so normally no longer in use.
        SDL_GPUTransferBuffer* transfer = AcquireTransfer(memoryBytes + indexBytes);
        uint8_t* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(g_device, transfer, true));
        memcpy(mapped, g_memoryStaging.data(), g_memoryStaging.size() * 4);
        memcpy(mapped + memoryBytes, g_indexStaging.data(), g_indexStaging.size() * 4);
        SDL_UnmapGPUTransferBuffer(g_device, transfer);

        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
        SDL_GPUTransferBufferLocation source{ transfer, 0 };
        SDL_GPUBufferRegion destination{ g_memoryBuffer, g_memoryBase * 4, memoryBytes };
        SDL_UploadToGPUBuffer(copy, &source, &destination, memoryCycle);
        source.offset = memoryBytes;
        destination = { g_indexBuffer, g_indexBase * 4, indexBytes };
        SDL_UploadToGPUBuffer(copy, &source, &destination, indexCycle);
        SDL_EndGPUCopyPass(copy);
        // Next flush's offsets, 16-byte aligned.
        g_memoryBase = AlignUp(g_memoryBase * 4 + memoryBytes, 16) / 4;
        g_indexBase = AlignUp(g_indexBase * 4 + indexBytes, 16) / 4;

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
        SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, colors, g_targets.colorCount, g_targets.depth ? &depth : nullptr);

        SDL_GPUBufferBinding indexBinding{ g_indexBuffer, 0 };
        SDL_BindGPUIndexBuffer(pass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &g_memoryBuffer, 1);
        // PERF: every draw pushes ~10 KB of uniforms (both stages' float
        // constants and the system constants twice); only pushing what
        // changed, or what the shader reads, would cut that a lot.
        for (const auto& draw : g_pending)
        {
            SDL_BindGPUGraphicsPipeline(pass, draw->pipeline);
            SDL_SetGPUViewport(pass, &draw->viewport);
            SDL_SetGPUScissor(pass, &draw->scissor);
            SDL_SetGPUStencilReference(pass, draw->stencilReference);
            SDL_SetGPUBlendConstants(pass, draw->blendConstants);
            if (!draw->vertexTextures.empty())
                SDL_BindGPUVertexSamplers(pass, 0, draw->vertexTextures.data(), uint32_t(draw->vertexTextures.size()));
            if (!draw->pixelTextures.empty())
                SDL_BindGPUFragmentSamplers(pass, 0, draw->pixelTextures.data(), uint32_t(draw->pixelTextures.size()));
            SDL_PushGPUVertexUniformData(commands, 0, draw->vertexConstants, sizeof(draw->vertexConstants));
            SDL_PushGPUVertexUniformData(commands, 1, &draw->system, sizeof(draw->system));
            SDL_PushGPUFragmentUniformData(commands, 0, draw->pixelConstants, sizeof(draw->pixelConstants));
            SDL_PushGPUFragmentUniformData(commands, 1, &draw->system, sizeof(draw->system));
            if (draw->indexed)
                SDL_DrawGPUIndexedPrimitives(pass, draw->count, 1, draw->firstIndex, 0, 0);
            else
                SDL_DrawGPUPrimitives(pass, draw->count, 1, 0, 0);
        }
        SDL_EndGPURenderPass(pass);

        g_pending.clear();
        g_memoryStaging.clear();
        g_indexStaging.clear();
        g_stagedRanges.clear();
    }

    void EndFrameBuffers()
    {
        g_memoryBase = g_indexBase = 0;
        RetireTransfers();
    }

    const TargetSet& CurrentTargetSet()
    {
        return g_targets;
    }

    size_t PipelineCount()
    {
        return g_pipelines.size();
    }

    void Draw(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize)
    {
        uint32_t primitive = initiator & 0x3F;
        // 0x10-0x12 are tessellation patches here (Halo's water): not
        // supported, so they're skipped below as unknown primitive types.
        uint32_t mode = ReadRegister(kRegRbModeControl) & 7;
        if (mode != kEdramModeColorDepth && mode != kEdramModeDepthOnly)
            return;
        uint32_t vte = ReadRegister(kRegPaClVteCntl);
        bool screenSpaceRectangles = primitive == kPrimRectangleList && (vte & kVteScreenSpaceXyz) == kVteScreenSpaceXyz;
        if (mode == kEdramModeDepthOnly && screenSpaceRectangles)
            ClearColorUnderDepth(initiator);

        uint32_t colorMask = ReadRegister(kRegRbColorMask);
        uint32_t depthControl = ReadRegister(kRegRbDepthControl);
        TargetSet targets = CurrentTargets(*g_pixelShader, colorMask, depthControl, mode);
        if (targets.colorCount == 0 && !targets.depth)
            return;

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
        uint32_t blend[4];
        for (uint32_t i = 0; i < 4; i++)
            blend[i] = ReadRegister(kRegRbBlendControl[i]);
        uint32_t stencilRefMask = ReadRegister(kRegRbStencilRefMask);
        SDL_GPUGraphicsPipeline* pipeline =
            GetPipeline(g_vertexShader, g_pixelShader, topology, targets, blend, colorMask, depthControl, stencilRefMask);
        if (!pipeline)
            return;

        auto draw = std::make_unique<PendingDraw>();
        draw->pipeline = pipeline;
        draw->stencilReference = uint8_t(stencilRefMask);
        draw->blendConstants = { RegisterFloat(kRegRbBlendRed), RegisterFloat(kRegRbBlendRed + 1), RegisterFloat(kRegRbBlendRed + 2),
            RegisterFloat(kRegRbBlendRed + 3) };

        // Float constants, byte-swapped from the register file.
        const uint32_t* alu = GuestPtr<uint32_t>(kGpuRegisterBase + kRegShaderConstantAlu * 4);
        uint32_t* vertexConstants = reinterpret_cast<uint32_t*>(draw->vertexConstants);
        uint32_t* pixelConstants = reinterpret_cast<uint32_t*>(draw->pixelConstants);
        for (uint32_t i = 0; i < kStageConstants; i++)
        {
            vertexConstants[i] = __builtin_bswap32(alu[i]);
            pixelConstants[i] = __builtin_bswap32(alu[kStageConstants + i]);
        }

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

        std::vector<uint32_t> indices = GuestIndices(initiator, indexBase, dmaSize);
        if (primitive == kPrimRectangleList)
        {
            // Rectangles: the vertex shader reads the three given vertices'
            // indices from the memory buffer and makes two triangles.
            system.flags |= kSystemFlagRectangleList;
            system.rectIndices = g_memoryBase + uint32_t(g_memoryStaging.size());
            uint32_t rects = uint32_t(indices.size() / 3);
            g_memoryStaging.insert(g_memoryStaging.end(), indices.begin(), indices.begin() + rects * 3);
            draw->indexed = false;
            draw->count = rects * 6;
        }
        else
        {
            uint32_t resetIndex = ReadRegister(kRegVgtMultiPrimIbResetIndx);
            bool resetEnabled = (modeControl >> 21) & 1;
            uint32_t first = uint32_t(g_indexStaging.size());
            AppendTriangles(g_indexStaging, indices, primitive, resetIndex, resetEnabled);
            draw->firstIndex = g_indexBase + first;
            draw->count = uint32_t(g_indexStaging.size()) - first;
        }
        if (draw->count == 0)
            return;
        g_pending.push_back(std::move(draw));
    }
}
