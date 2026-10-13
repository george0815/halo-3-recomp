// Shared internals of the renderer (gpu/renderer.h is the public side).
//
// The renderer is split by job across gpu/renderer/*.cpp:
//
//   shaders.cpp   translated shader cache (Xenos microcode -> SPIR-V)
//   pipelines.cpp host shaders and pipelines: the manifest, warm-up and
//                 asynchronous creation on worker threads
//   copy.cpp      fullscreen copy/convert passes (EDRAM-style bit copies)
//   targets.cpp   EDRAM render targets as host textures, format aliasing
//   resolve.cpp   resolves (EDRAM -> memory), write-backs, resolved views
//   textures.cpp  textures decoded from guest memory, samplers, binding
//   draw.cpp      turning a draw packet into a recorded host draw; flushing
//   memexport.cpp vertex shaders' memory exports, run as compute shaders
//   debug.cpp     PNG/memory dumps for debugging
//   ../renderer.cpp  initialization, the public entry points, presentation
//
// Everything runs on the command processor thread except Initialize and
// AcquireFrame (gpu/renderer.h), so this state needs no locking; the one
// exception is pipelines.cpp, whose shader and pipeline maps are shared with
// its worker threads (and locked there).

#pragma once

#include "config/env.h"
#include "gpu/command_processor.h"
#include "gpu/renderer.h"
#include "gpu/registers.h"
#include "gpu/shader/translator.h"
#include "gpu/texture.h"
#include "gpu/xenos.h"
#include "memory.h"

#include <SDL3/SDL.h>
#include <shaderc/shaderc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace h3::gpu::renderer::internal
{
    // ------------------------------------------------------------------
    // Registers the renderer reads beyond gpu/registers.h
    // ------------------------------------------------------------------

    constexpr uint32_t kRegPaClClipCntl = 0x2204;   // bit 16: clip disable; bits 0-5: user clip planes
    constexpr uint32_t kRegPaSuScModeCntl = 0x2205;  // culling, poly offset; bit 16: window offset enable; bit 21: primitive reset enable
    constexpr uint32_t kRegPaClVteCntl = 0x2206;     // which viewport scale/offset parts apply; bits 8-10 position formats
    constexpr uint32_t kRegRbDepthControl = 0x2200;  // depth/stencil test state
    constexpr uint32_t kRegRbColorMask = 0x2104;     // 4 bits (RGBA) per render target
    constexpr uint32_t kRegRbStencilRefMask = 0x210D;  // ref : 8, mask : 8 @8, write mask : 8 @16
    constexpr uint32_t kRegRbBlendRed = 0x2105;      // blend constant; then green, blue, alpha
    constexpr uint32_t kRegRbColorControl = 0x2202;  // alpha test function : 3, enable @3
    constexpr uint32_t kRegRbAlphaRef = 0x210E;      // alpha test reference (float)
    constexpr uint32_t kRegRbBlendControl[4] = { 0x2201, 0x2209, 0x220A, 0x220B };  // per render target
    constexpr uint32_t kRegColorInfo[4] = { kRegRbColorInfo, kRegRbColor1Info, kRegRbColor2Info, kRegRbColor3Info };
    constexpr uint32_t kRegVgtIndxOffset = 0x2102;   // added to every index
    constexpr uint32_t kRegVgtMultiPrimIbResetIndx = 0x2103;  // strip/fan restart index
    constexpr uint32_t kRegSqContextMisc = 0x2181;   // bits 8-15: pixel parameter register
    constexpr uint32_t kRegPaSuPointSize = 0x2280;   // point half-size, 12.4 fixed point: height bits 0-15, width 16-31
    constexpr uint32_t kRegPaSuVtxCntl = 0x2302;     // bit 0: pixel center (0 = D3D, integer centers)
    constexpr uint32_t kRegVgtOutputPathCntl = 0x2284;  // bits 0-1: 1 = tessellation enabled
    constexpr uint32_t kRegVgtHosCntl = 0x2285;      // bits 0-1: tessellation mode (discrete, continuous, adaptive)
    constexpr uint32_t kRegVgtHosMaxTessLevel = 0x2286;  // float; the tessellator adds 1

    // RB_COLOR_INFO / RB_DEPTH_INFO fields.
    constexpr uint32_t kEdramBaseMask = 0xFFF;  // EDRAM base, in 80x16-sample tiles
    inline uint32_t EdramBase(uint32_t info) { return info & kEdramBaseMask; }
    inline uint32_t ColorFormat(uint32_t info) { return (info >> 16) & 0xF; }      // ColorRenderTargetFormat
    inline bool DepthIsFloat(uint32_t info) { return (info >> 16) & 1; }           // D24FS8 rather than D24S8
    inline int32_t ColorExpBias(uint32_t info) { return int32_t(info << 6) >> 26; }  // signed 6 bits @20

    // ColorRenderTargetFormat values with special handling.
    constexpr uint32_t kColorFormat8888 = 0;
    constexpr uint32_t kColorFormat8888Gamma = 1;
    constexpr uint32_t kColorFormat2101010Float = 3;          // 7e3 floats
    constexpr uint32_t kColorFormat2101010FloatAs16161616 = 12;

    // Xenos primitive types (VGT_DRAW_INITIATOR bits 0-5).
    enum : uint32_t
    {
        kPrimPointList = 1,
        kPrimLineList = 2,
        kPrimLineStrip = 3,
        kPrimTriangleList = 4,
        kPrimTriangleFan = 5,
        kPrimTriangleStrip = 6,
        kPrimRectangleList = 8,
        kPrimQuadList = 13,
        kPrimTrianglePatch = 17,  // with VGT_OUTPUT_PATH_CNTL tessellation enabled
    };

    // ------------------------------------------------------------------
    // Sizes and limits
    // ------------------------------------------------------------------

    // Host render targets cover any EDRAM surface Halo 3 uses (1152 or 1280
    // wide) in sample space: a 4x surface's pixel covers 2x2 host texels, so
    // every view of the same EDRAM lines up.
    constexpr uint32_t kRenderTargetWidth = 2560;
    constexpr uint32_t kRenderTargetHeight = 1280;

    // Halo 3's main surfaces are 1152x640 at 4x: 2304x1280 host texels. Debug
    // dumps of render targets save this much.
    constexpr uint32_t kDumpTargetWidth = 2304;
    constexpr uint32_t kDumpTargetHeight = 1280;

    // Samples per pixel along x and y for RB_SURFACE_INFO's MSAA mode:
    // 1x, 2x (one above the other), 4x (2x2).
    inline uint32_t SamplesX(uint32_t msaa) { return msaa == 2 ? 2 : 1; }
    inline uint32_t SamplesY(uint32_t msaa) { return msaa == 0 ? 1 : 2; }

    // Render scale (settings.ini render_scale, H3_RENDER_SCALE overrides):
    // render targets and the textures resolved from them are this many
    // times the guest's size in each direction, so the game renders at
    // 1280x720 times the scale. Set once by Initialize.
    // (At most config::kMaxRenderScale.)
    inline uint32_t g_renderScale = 1;

    // Host render target size (sample space times the render scale).
    inline uint32_t TargetWidth() { return kRenderTargetWidth * g_renderScale; }
    inline uint32_t TargetHeight() { return kRenderTargetHeight * g_renderScale; }
    // Host texels per guest pixel along x / y for an MSAA mode.
    inline uint32_t GridX(uint32_t msaa) { return SamplesX(msaa) * g_renderScale; }
    inline uint32_t GridY(uint32_t msaa) { return SamplesY(msaa) * g_renderScale; }
    // The current surface's MSAA mode (RB_SURFACE_INFO bits 16-17).
    inline uint32_t SurfaceMsaa() { return (ReadRegister(kRegRbSurfaceInfo) >> 16) & 3; }

    // ------------------------------------------------------------------
    // Shared state
    // ------------------------------------------------------------------

    inline SDL_GPUDevice* g_device = nullptr;
    inline bool g_enabled = false;
    // Depth format of every depth target (falls back to D24S8 where D32S8
    // isn't supported).
    inline SDL_GPUTextureFormat g_depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
    inline shaderc_compiler_t g_compiler = nullptr;
    inline SDL_GPUSampler* g_sampler = nullptr;       // linear clamp, for placeholders
    inline SDL_GPUSampler* g_pointSampler = nullptr;  // nearest clamp, for copies
    inline bool g_noTextures = false;   // H3_NO_TEXTURES: placeholders instead of textures from memory
    inline bool g_noWriteBack = false;  // H3_NO_WRITEBACK: small resolves stay on the GPU

    // Swaps seen, and resolves within the current frame (for dumps).
    inline uint32_t g_swaps = 0;
    inline uint32_t g_frameResolves = 0;
    // Draw packets (draws and resolves) this frame, numbered like
    // H3_GPU_CAPTURE's "draw #n".
    inline uint32_t g_frameDraws = 0;

    // 1x1 gray textures bound where a texture isn't available.
    inline SDL_GPUTexture* g_placeholder2D = nullptr;
    inline SDL_GPUTexture* g_placeholder3D = nullptr;
    inline SDL_GPUTexture* g_placeholderCube = nullptr;
    inline SDL_GPUTexture* g_placeholderArray = nullptr;  // 2D array (stacked textures)

    // The command buffer being recorded (created on demand, submitted at
    // swaps and when a write-back needs a fence).
    inline SDL_GPUCommandBuffer* g_commands = nullptr;
    inline SDL_GPUCommandBuffer* Commands()
    {
        if (!g_commands)
            g_commands = SDL_AcquireGPUCommandBuffer(g_device);
        return g_commands;
    }

    // ------------------------------------------------------------------
    // Small helpers
    // ------------------------------------------------------------------

    // FNV-1a style hash, eight bytes at a time: keys, and the shader cache's
    // file names (so it must never change, or every cached shader is lost).
    uint64_t Hash(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);
    // A fast hash for large data that's re-hashed often and never stored
    // (texture memory, to see whether it changed): xxHash's XXH3, roughly
    // ten times FNV's speed.
    uint64_t ContentHash(const void* data, size_t size);

    // Prints a message the first time it's seen (for "unsupported X" notes
    // that would otherwise repeat every frame).
    void LogOnce(const std::string& message);

    // Logs work on the GPU thread that took long enough to show as a hitch
    // (H3_LOG_SLOW=1): construct at the start of the work, logs at scope end.
    struct SlowTimer
    {
        const char* what;
        std::string detail = {};
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~SlowTimer();
    };

    // A register holding a float.
    inline float RegisterFloat(uint32_t index)
    {
        uint32_t bits = ReadRegister(index);
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // Sign-extends a 15-bit field (window offsets).
    inline int32_t Signed15(uint32_t value) { return int32_t(value << 17) >> 17; }

    // PA_SC_WINDOW_OFFSET as signed x, y.
    inline void WindowOffset(int32_t& x, int32_t& y)
    {
        uint32_t offset = ReadRegister(kRegPaScWindowOffset);
        x = Signed15(offset & 0x7FFF);
        y = Signed15((offset >> 16) & 0x7FFF);
    }

    // A vertex fetch constant (2 dwords): where a vertex buffer is.
    struct VertexFetch
    {
        bool valid = false;  // type 3 (vertex) in the low bits
        uint32_t address = 0;  // physical
        uint32_t dwords = 0;   // size
        uint32_t endian = 0;   // Xenos endian mode
    };
    // Vertex fetch constant `index` (0-95). D3D also puts resolve and clear
    // rectangles in constant 0.
    VertexFetch ReadVertexFetch(uint32_t index);
    // Float `i` of a vertex buffer, byte-swapped as the fetch constant says.
    float VertexFloat(const VertexFetch& fetch, uint32_t i);

    // Texture fetch constant `index` (0-31), parsed.
    texture::FetchConstant ReadTextureFetch(uint32_t index);

    // ------------------------------------------------------------------
    // Shaders (shaders.cpp)
    // ------------------------------------------------------------------

    // A translated guest shader.
    // A host shader: one SPIR-V program, shared by every guest shader that
    // translates to the same GLSL. Made on the GPU thread or a worker
    // (pipelines.cpp); shader is set once it exists.
    struct HostShader
    {
        std::atomic<SDL_GPUShader*> shader{ nullptr };
        std::atomic<bool> failed{ false };
    };

    struct Shader
    {
        bool ok = false;               // translated
        HostShader* host = nullptr;    // the compiled program (maybe still compiling)
        shader::ShaderInfo info;       // what it reads (constants, fetches, textures)
        std::string cacheName;         // SPIR-V file in the shader cache (also its id)
        std::vector<uint32_t> microcode;  // kept for memexport vertex shaders (compute variant, memexport.cpp)
        uint64_t captureHash = 0;      // FNV-1a of the guest microcode, as H3_GPU_CAPTURE names gpu_shaders/
    };

    // Shaders most recently loaded by the command stream (IM_LOAD).
    inline Shader* g_vertexShader = nullptr;
    inline Shader* g_pixelShader = nullptr;

    // Where compiled SPIR-V is cached between runs (empty: no cache).
    inline std::filesystem::path g_shaderCacheDirectory;

    // The host shader for guest microcode (translated and compiled once).
    Shader* LoadShader(bool vertex, const uint32_t* dwords, uint32_t count);
    // Number of distinct guest shaders seen (for logs).
    size_t ShaderCount();
    // Compiles one of the renderer's own GLSL shaders (copies).
    SDL_GPUShader* CompileInternal(const char* glsl, bool vertex, uint32_t samplers, uint32_t uniformBuffers);
    // The shader cache file name for translated GLSL ("<16 hex digits>.spv").
    std::string ShaderCacheName(bool vertex, const std::string& glsl);
    // SPIR-V for translated GLSL: from the shader cache, or compiled and
    // stored there. Any thread.
    enum class ShaderStage { Vertex, Fragment, Compute };
    std::vector<uint8_t> CompileSpirv(ShaderStage stage, const std::string& glsl, const std::string& cacheName);
    inline std::vector<uint8_t> CompileSpirv(bool vertex, const std::string& glsl, const std::string& cacheName)
    {
        return CompileSpirv(vertex ? ShaderStage::Vertex : ShaderStage::Fragment, glsl, cacheName);
    }

    // ------------------------------------------------------------------
    // Shader and pipeline creation (pipelines.cpp)
    // ------------------------------------------------------------------

    // Async compilation (settings.ini async_shaders, H3_ASYNC_SHADERS):
    // shaders and pipelines that aren't ready are made on worker threads and
    // the draws needing them are skipped meanwhile.
    inline bool g_asyncShaders = false;

    // Everything a host pipeline is made from, as plain data (recorded in
    // the shader cache's manifest and hashed as the pipeline's key, so no
    // padding: keep it 16 uint32s after the ids, and value-initialize it).
    struct PipelineDesc
    {
        uint64_t vs = 0, ps = 0;      // shader cache ids (ShaderCacheId)
        uint32_t topology = 0;        // SDL_GPUPrimitiveType
        uint32_t colorCount = 0;
        uint32_t depthFormat = 0;     // SDL_GPUTextureFormat; 0 = no depth target
        uint32_t colorFormats[4] = {};
        uint32_t blend[4] = {};       // RB_BLENDCONTROL per target
        uint32_t colorMask = 0;       // RB_COLOR_MASK
        uint32_t depthControl = 0;    // RB_DEPTHCONTROL
        uint32_t stencilMasks = 0;    // RB_STENCILREFMASK without the reference
        uint32_t psColorTargetMask = 0;  // targets the pixel shader writes
        uint32_t rasterizer = 0;      // kRasterizer* bits below (culling)
    };
    // PipelineDesc::rasterizer: PA_SU_SC_MODE_CNTL's cull and face bits.
    constexpr uint32_t kRasterizerCullFront = 1u << 0;
    constexpr uint32_t kRasterizerCullBack = 1u << 1;
    constexpr uint32_t kRasterizerFrontClockwise = 1u << 2;  // else counter-clockwise
    constexpr uint32_t kRasterizerMask = 7;               // the PA_SU_SC_MODE_CNTL bits above
    // PA_CL_CLIP_CNTL clip disable: no depth clipping (depth is clamped).
    constexpr uint32_t kRasterizerNoDepthClip = 1u << 3;
    constexpr uint32_t kClipCntlClipDisable = 1u << 16;
    static_assert(sizeof(PipelineDesc) == 16 + 16 * 4);

    // Polygon offset (depth bias) of a draw, from PA_SU_SC_MODE_CNTL and
    // the PA_SU_POLY_OFFSET_* registers. Halo uses it for decals (on the
    // main depth buffer) and shadow maps (shadow acne without it).
    struct PolygonOffset
    {
        float slope = 0.0f;     // factor for the depth's slope per host texel
        float constant = 0.0f;  // added to the depth (0..1 depth range units)
    };
    // The current draw's polygon offset; zero when disabled. pixelScale:
    // host texels per guest pixel (samples per pixel times render scale).
    PolygonOffset CurrentPolygonOffset(uint32_t modeControl, float pixelScale);

    // The id in a shader cache name (its 16 hex digits).
    uint64_t ShaderCacheId(const std::string& cacheName);
    // The host shader for translated GLSL with a cache name: made now, or on
    // a worker when async; shared with warm-up and other guest shaders.
    HostShader* RequestHostShader(bool vertex, const std::string& glsl, uint32_t samplers, const std::string& cacheName);
    // Whether a guest shader's program exists (wait: block until it does,
    // unless it failed).
    bool ShaderReady(const Shader& shader, bool wait);
    // The pipeline for a description: null if it failed, or (async) while a
    // worker creates it.
    SDL_GPUGraphicsPipeline* GetPipeline(const PipelineDesc& desc);
    // Starts recreating everything in the shader cache's manifest on worker
    // threads (settings.ini precompile_shaders). Call once at start.
    void StartShaderWarmup();

    // ------------------------------------------------------------------
    // Copies (copy.cpp)
    // ------------------------------------------------------------------

    // What a copy pass does to each pixel (the copy shader's mode.x).
    //   Value:          the source as is (depth into R32_FLOAT), times scale
    //   DepthBytes:     float depth as the 24_8 word's bytes (stencil in the
    //                   low byte), for depth resolved to 8_8_8_8 and sampled so
    //   FloatTo7e3:     2_10_10_10_FLOAT values as their 7e3 bits, for EDRAM
    //                   drawn as 7e3 and then used as 2_10_10_10
    //   LinearToGamma:  encode for 8_8_8_8_GAMMA memory (about sqrt)
    //   GammaToLinear:  the reverse
    //   Fill:           writes one color (scale) to every pixel
    enum class CopyMode : int32_t
    {
        Value = 0,
        DepthBytes = 1,
        FloatTo7e3 = 2,
        LinearToGamma = 3,
        GammaToLinear = 4,
        Fill = 5,
    };

    // Copies a w x h rectangle of source (at sx, sy, in source texels) into
    // destination (at dx, dy) through a fullscreen pass, converting by mode.
    // samplesX/Y average that many source texels per destination pixel
    // (resolving MSAA). Not inside a render pass.
    void ConvertCopy(SDL_GPUTexture* source, SDL_GPUTexture* destination, SDL_GPUTextureFormat destinationFormat,
        uint32_t destinationWidth, uint32_t destinationHeight, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w,
        uint32_t h, CopyMode mode, bool depthFloat = false, float scale = 1.0f, int32_t samplesX = 1, int32_t samplesY = 1,
        uint32_t sampleMask = 0);  // sampleMask: average only those samples (bit n: sample n) of an MSAA source

    // Writes depth from 24_8 depth words drawn as 8_8_8_8 color (Halo's
    // depth restore; see draw.cpp): the rectangle of source (in texels)
    // into the same rectangle of the depth target.
    void RestoreDepthFromBytes(SDL_GPUTexture* source, SDL_GPUTexture* depth, uint32_t width, uint32_t height, const SDL_Rect& rect,
        bool depthFloat);

    // Writes stencil from the red byte of 8_8_8_8 color drawn at the depth
    // buffer's EDRAM base (Halo's per-tile stencil dither pattern; see
    // draw.cpp): the rectangle of source (in texels) into the same
    // rectangle of the depth target's stencil.
    void RestoreStencilFromBytes(SDL_GPUTexture* source, SDL_GPUTexture* depth, uint32_t width, uint32_t height, const SDL_Rect& rect);

    // Writes width x height of source (from sourceX, sourceY) through the
    // display gamma ramp into an RGBA8 destination (presentation).
    // Post-processing (postprocess.cpp): FXAA on the finished frame if
    // enabled; replaces the frame with its output (RGBA8, at 0,0).
    void ApplyAntiAliasing(Frame& frame, SDL_GPUTextureFormat& format);
    void ApplyGammaRamp(SDL_GPUTexture* source, int32_t sourceX, int32_t sourceY, SDL_GPUTexture* destination, uint32_t width,
        uint32_t height, const GammaRamp& ramp);

    // Fills a rectangle of a render target with one color.
    void FillRect(SDL_GPUTexture* destination, SDL_GPUTextureFormat format, int32_t x, int32_t y, uint32_t w, uint32_t h,
        const float color[4]);

    // ------------------------------------------------------------------
    // Render targets (targets.cpp)
    // ------------------------------------------------------------------

    // An EDRAM surface as a host texture (one per EDRAM base and format).
    struct RenderTarget
    {
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        uint32_t xenosFormat = 0;  // ColorRenderTargetFormat, or depth format
        // Color targets: holds its base's contents as of write number
        // syncedWrite (see targets.cpp, EDRAM aliasing); false until the
        // first carry-over into it.
        bool synced = false;
        uint64_t syncedWrite = 0;
    };

    // Video memory held by one kind of host resource (for the H3_LOG_FPS
    // "[vram]" line): how many there are and their bytes.
    struct VideoMemoryUse
    {
        size_t count = 0;
        uint64_t bytes = 0;
    };
    // Bytes of a host texture of a format and size (all levels' base only).
    inline uint64_t HostTextureBytes(SDL_GPUTextureFormat format, uint32_t width, uint32_t height, uint32_t layers = 1)
    {
        return SDL_CalculateGPUTextureFormatSize(format, width, height, layers);
    }

    // The host texture for EDRAM at base in a format (created on first use).
    RenderTarget* GetRenderTarget(uint32_t base, uint32_t xenosFormat, bool depth);
    // A color target, carrying over what another format last drew at the
    // same base (see targets.cpp: EDRAM aliasing).
    RenderTarget* GetColorTarget(uint32_t base, uint32_t xenosFormat);
    // The color target last used at an EDRAM base, or null.
    RenderTarget* ColorTargetAt(uint32_t base);
    // Records that the current color target at an EDRAM base was written
    // in a rectangle (host texels), so a later format switch there copies
    // only what changed.
    void RecordColorWrite(uint32_t base, const SDL_Rect& rect);
    // Formats whose host texture holds floats (no unorm clamping/encoding).
    bool IsFloatHostFormat(SDL_GPUTextureFormat format);
    // Host render targets and their video memory (for logs).
    VideoMemoryUse RenderTargetMemory();

    // GPU passes by kind and the pixels they cover (the scissor or blit
    // rectangle), for H3_LOG_FPS's "[passes]" line: where fill rate goes
    // at a render scale.
    enum class PassKind : uint8_t
    {
        Draw,           // a Flush render pass (its draws aren't counted in pixels)
        Copy,           // ConvertCopy / FillRect (copy.cpp)
        DepthRestore,
        StencilRestore, // one per bit plus the clear
        Blit,           // format carry-overs, resolves, resolved views
        Count,
    };
    struct PassCounter
    {
        uint64_t passes = 0, pixels = 0;
    };
    inline PassCounter g_passCounters[size_t(PassKind::Count)];
    inline void CountPass(PassKind kind, uint64_t pixels)
    {
        g_passCounters[size_t(kind)].passes++;
        g_passCounters[size_t(kind)].pixels += pixels;
    }
    // 2_10_10_10_FLOAT and its alias: 7e3 floats in EDRAM.
    inline bool Is7e3(uint32_t xenosFormat)
    {
        return xenosFormat == kColorFormat2101010Float || xenosFormat == kColorFormat2101010FloatAs16161616;
    }

    // The host textures a draw renders into.
    struct TargetSet
    {
        SDL_GPUTexture* color[4] = {};
        SDL_GPUTextureFormat colorFormat[4] = {};
        uint32_t colorCount = 0;  // highest bound slot + 1
        SDL_GPUTexture* depth = nullptr;

        bool operator==(const TargetSet& other) const
        {
            return std::memcmp(color, other.color, sizeof(color)) == 0 && colorCount == other.colorCount &&
                depth == other.depth;
        }
    };
    // Host targets for the current EDRAM state, the color mask, the pixel
    // shader's outputs and the depth state.
    TargetSet CurrentTargets(const Shader& ps, uint32_t colorMask, uint32_t depthControl, uint32_t edramMode);

    // ------------------------------------------------------------------
    // Resolves (resolve.cpp)
    // ------------------------------------------------------------------

    // Host texture standing in for a resolve destination in memory: a tiled
    // pitch x height image of bpp-byte texels at physical address base.
    // The host texture is g_renderScale times pitch x height (HostWidth /
    // HostHeight); positions inside it are guest texels times the scale.
    struct ResolvedTexture
    {
        uint32_t base = 0, size = 0;
        uint32_t pitch = 0, height = 0, bpp = 4;  // guest layout (texels)
        uint32_t xenosFormat = 0;  // the resolve's destination texture format
        SDL_GPUTextureFormat hostFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
        SDL_GPUTexture* texture = nullptr;
        uint32_t generation = 0;  // bumped by every resolve into it
        bool depthFloat = false;  // depth resolves: from a 20e4 float depth buffer

        // Smallest host texture side: one 32x32 macro tile.
        static constexpr uint32_t kMinSize = 32;
        // Host texture size, and the guest-texel size it stands for.
        uint32_t GuestWidth() const { return std::max(pitch, kMinSize); }
        uint32_t GuestHeight() const { return std::max(height, kMinSize); }
        uint32_t HostWidth() const { return GuestWidth() * g_renderScale; }
        uint32_t HostHeight() const { return GuestHeight() * g_renderScale; }
    };

    // The resolved texture covering an address (physical, or through a CPU
    // view of physical memory), or null.
    ResolvedTexture* FindResolved(uint32_t address);
    // A texture fetch from resolved memory: the resolved texture itself, or
    // a copy at the fetch's size/position (see resolve.cpp).
    SDL_GPUTexture* ResolvedView(ResolvedTexture& resolved, const texture::FetchConstant& fetch, uint32_t address);
    // Executes the resolve the current registers describe.
    void Resolve();
    // Forgets resolved textures overlapping physical memory [begin, end)
    // that something else wrote (memexport), so fetches decode the memory.
    void InvalidateResolved(uint32_t begin, uint32_t end);
    // Completes write-backs whose GPU downloads finished (wait: all of them).
    void PollWriteBacks(bool wait = false);
    // Frees views of resolved textures (ResolvedView) that went unused for
    // a while. Once per frame. ResolvedViewCount is for logs.
    void TrimResolvedViews();
    size_t ResolvedViewCount();
    // Resolved textures and resolved views, and their video memory (logs).
    VideoMemoryUse ResolvedMemory();
    VideoMemoryUse ResolvedViewMemory();

    // ------------------------------------------------------------------
    // Textures (textures.cpp)
    // ------------------------------------------------------------------

    // System constants shared by both shader stages (layout: translator.h).
    struct SystemConstants
    {
        uint32_t bools[8];
        uint32_t loops[32];
        uint32_t vfetch[96][4];    // per vertex fetch constant: offset in the memory buffer, dwords, endian
        float ndcScale[4];
        float ndcOffset[4];
        uint32_t psParamGen;
        uint32_t flags;            // kSystemFlag* below
        uint32_t rectIndices;
        uint32_t tessSegments;     // tessellated patches: segments per edge
        uint32_t tessPatchBase;    // tessellated patches: first patch index
        float depthBiasSlope;      // polygon offset slope factor (pixel shaders)
        float pointSize;           // point list diameter in host pixels (PA_SU_POINT_SIZE)
        uint32_t pad;
        uint32_t textures[32][4];  // per texture fetch constant: swizzle, signs, exp adjust, enabled
        float colorExpBias[4];     // 2^RB_COLOR_INFO exp bias, per render target
        float alphaRef;
        float pixelScale[2];       // host texels per guest pixel
        float renderScale;         // g_renderScale
        float targetSize[2];       // host render target size (texels)
        uint32_t queryId;          // occlusion query draws: the id written (queries.cpp)
        uint32_t pad2;
    };

    // SystemConstants::flags bits (must match translator.h).
    constexpr uint32_t kSystemFlagRectangleList = 1u << 0;
    constexpr uint32_t kSystemFlagRoundDepth24 = 1u << 1;
    constexpr uint32_t kSystemFlagVteShift = 2;          // 3 bits: PA_CL_VTE_CNTL position formats
    constexpr uint32_t kSystemFlagAlphaFuncShift = 5;    // 3 bits: alpha test function
    constexpr uint32_t kSystemFlagAlphaTest = 1u << 8;
    constexpr uint32_t kSystemFlagSnapToPixels = 1u << 9;  // snap vertices to guest pixels
    constexpr uint32_t kSystemFlagTessellated = 1u << 10;  // tessellated triangle patches (grid per patch)
    constexpr uint32_t kSystemFlagApproxGamma = 1u << 11;  // gamma decode by squaring (H3_APPROX_GAMMA)
    // H3_APPROX_GAMMA=1: approximate the piecewise-linear gamma curve as
    // gamma 2 (square / square root), as before the exact curve, to compare.
    inline bool ApproxGamma()
    {
        static const bool approx = EnvFlag("H3_APPROX_GAMMA");
        return approx;
    }
    constexpr uint32_t kSystemFlagClampColorShift = 12;  // 4 bits: clamp color output i

    // Texture/sampler bindings for a shader's textures, filling the texture
    // part of the system constants.
    std::vector<SDL_GPUTextureSamplerBinding> BindTextures(const Shader& shader, SystemConstants& system);
    // Number of cached memory textures, and the video memory they hold
    // (for logs).
    size_t TextureCount();
    uint64_t TextureBytes();
    // Frees cached memory textures that went unused for long, or the least
    // recently used ones while over the cache's budget. Once per frame.
    void TrimTextureCache();
    // Texture uploads and their bytes so far (for logs).
    inline uint64_t g_textureUploads = 0, g_textureUploadBytes = 0;

    // ------------------------------------------------------------------
    // Drawing (draw.cpp)
    // ------------------------------------------------------------------

    // Records the draw the current registers describe.
    void Draw(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize);
    // A draw's guest indices (host-endian, VGT_INDX_OFFSET added), or
    // 0..n-1 plus the offset for auto-indexed draws.
    std::vector<uint32_t> GuestDrawIndices(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize);
    // The bool and loop constants of the system constants.
    void SetControlConstants(SystemConstants& system);

    // ------------------------------------------------------------------
    // Memory export (memexport.cpp)
    // ------------------------------------------------------------------

    // Runs a memexport vertex shader's exports for the current draw (its
    // compute variant); the results reach guest memory a frame or two later.
    void RunMemExport(const Shader& vs, uint32_t initiator, uint32_t indexBase, uint32_t dmaSize);
    // Submits the command buffer with a fence if memexport results are
    // waiting in it. Call before any other submit of g_commands.
    void FenceMemExports();
    // Writes back memexport results whose GPU work finished.
    void PollMemExports();

    // ------------------------------------------------------------------
    // Occlusion queries (queries.cpp)
    // ------------------------------------------------------------------

    // A sample count report (EVENT_WRITE_ZPD) at a guest address.
    void OnOcclusionReport(uint32_t address);
    // For a draw: the occlusion query id to draw it into the query texture
    // with (depth-only draws inside a query), or 0 for a normal draw.
    uint32_t OcclusionQueryDraw(uint32_t mode);
    // The R32_UINT query id texture, and the pixel shader writing ids.
    SDL_GPUTexture* OcclusionQueryTexture();
    SDL_GPUShader* OcclusionQueryFragmentShader();
    // PipelineDesc::ps of query draws (not a shader cache id).
    constexpr uint64_t kOcclusionQueryPixelShader = 0x5155455259494400ull;  // "QUERYID"
    // PipelineDesc::ps of H3_HIGHLIGHT_VS draws: solid magenta (debug.cpp).
    constexpr uint64_t kHighlightPixelShader = 0x484947484C494748ull;  // "HIGHLIGH"
    SDL_GPUShader* HighlightFragmentShader();
    // H3_HIGHLIGHT_VS=<hex prefix of a captured vs_<hash>.bin>: that
    // shader's draws go out magenta, depth test off (where does it draw?).
    bool HighlightDraw(const Shader& vs);
    // H3_LOG_VS_DATA=<hex prefix>[:<record dwords>]: logs each draw of that
    // vertex shader: its vertex fetch ranges, which of this frame's memory
    // exports write them, VS constants it indexes, and the records (first 4
    // dwords as floats).
    void LogVertexData(const Shader& vs, uint32_t initiator);
    // H3_VERIFY_VS_DATA=1 with H3_LOG_VS_DATA: the logged draws' records
    // are also read back from the draw buffer after the flush's export
    // scatter (what the GPU really sees), and all-zero ones reported.
    struct VsDataCheck
    {
        uint32_t frame, draw, address, dwords, stride;
        std::vector<uint32_t> records;
    };
    extern std::vector<VsDataCheck> g_vsDataChecks;
    // Runs the pending checks on this flush's draw buffer (submits and
    // waits; replaces g_commands).
    void VerifyVsData(SDL_GPUBuffer* buffer, const std::unordered_map<uint64_t, uint32_t>& staged);
    // Debugging: this frame's export streams overlapping physical
    // [begin, end), as text.
    std::string FrameExportsOverlapping(uint32_t begin, uint32_t end);
    // H3_DEBUG_VS_POSITIONS=1 with H3_LOG_VS_DATA: runs a logged draw's
    // vertex shader as compute and logs each vertex's clip-space position
    // ("ODD": behind the eye or far off screen). Auto-indexed draws only;
    // waits for the GPU.
    void DebugVertexPositions(const Shader& vs, uint32_t initiator);
    // Per frame: count this frame's query samples (before the frame's
    // submit), and take in counts that came back.
    void EndFrameQueries();
    void PollOcclusionQueries();
    // A nearest, clamped sampler (copy.cpp).
    SDL_GPUSampler* PointSampler();
    // Writes this frame's memory exports so far into a buffer holding
    // copies of guest ranges ((physical address << 32 | dwords), dword
    // offset): draws read particle state their update dispatches just
    // wrote (guest memory gets it a frame later).
    void ScatterFrameExports(SDL_GPUBuffer* target, const std::vector<std::pair<uint64_t, uint32_t>>& ranges);
    // Whether exports from earlier frames, still in flight, write physical
    // memory [begin, end).
    bool MemExportPending(uint32_t begin, uint32_t end);
    // Before a draw or dispatch with this vertex shader: if it fetches
    // vertex data that earlier frames' exports still in flight write, waits
    // for them and applies them to guest memory. The console's exports land
    // immediately; without this GPU particles updated from state two frames
    // old. (This frame's own exports still land a frame late.)
    void SyncMemExportsFor(const Shader& shader);
    // How many times that waited (for logs).
    inline uint64_t g_memExportSyncs = 0;
    // Command buffer submissions (for H3_LOG_FPS's "[passes]" line), and
    // time spent waiting for earlier frames' GPU work (SubmitFrame).
    inline uint64_t g_submits = 0;
    inline double g_frameWaitSeconds = 0;
    inline uint64_t g_stagedBytes = 0;       // vertex data uploaded by Flush
    inline uint64_t g_stagedIndexBytes = 0;  // index data uploaded by Flush
    // Records everything pending into the command buffer. Call before
    // anything that can't happen inside a render pass.
    void Flush();
    // Per-swap bookkeeping of the draw buffers.
    void EndFrameBuffers();
    // Draw buffers (vertex/index GPU buffers) and upload buffers, for logs.
    VideoMemoryUse DrawBufferMemory();
    VideoMemoryUse UploadBufferMemory();
    // The targets pending draws render into (for draw dumps).
    const TargetSet& CurrentTargetSet();
    // Number of pipelines created (for logs).
    size_t PipelineCount();

    // ------------------------------------------------------------------
    // Debugging (debug.cpp)
    // ------------------------------------------------------------------

    // Saves width x height of a texture as a PNG (RGBA8, RGBA16F, RGB10A2 or
    // R32F). Submits the command buffer and waits (debugging only).
    void DumpTexture(SDL_GPUTexture* texture, uint32_t width, uint32_t height, const char* path,
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
    // H3_DUMP_DRAWS: dump a color target after the current draw.
    void DumpAfterDraw();
    // H3_DUMP_PRIM: dump color target 0 before/after draws of a primitive type.
    void DumpAroundPrimitive(uint32_t primitive, bool after);
    // H3_DUMP_RESTORE: dump the depth bytes and the depth target after a
    // depth restore (draw.cpp).
    void DumpDepthAfterRestore(SDL_GPUTexture* bytes, SDL_GPUTextureFormat bytesFormat, SDL_GPUTexture* depth);
    // H3_DUMP_MEMORY: dump guest memory at this swap.
    void DumpMemoryAtSwap();
    // H3_POKE: writes bytes into guest memory at a swap (experiments).
    void PokeMemoryAtSwap();
    // H3_DUMP_FRAME / H3_SNAPSHOT_EVERY: dump the presented frame.
    void DumpFrameAtSwap(uint32_t frontbuffer, SDL_GPUTexture* texture, uint32_t width, uint32_t height, SDL_GPUTextureFormat format);
}
