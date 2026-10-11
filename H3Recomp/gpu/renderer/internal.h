// Shared internals of the renderer (gpu/renderer.h is the public side).
//
// The renderer is split by job across gpu/renderer/*.cpp:
//
//   shaders.cpp   translated shader cache (Xenos microcode -> SPIR-V)
//   copy.cpp      fullscreen copy/convert passes (EDRAM-style bit copies)
//   targets.cpp   EDRAM render targets as host textures, format aliasing
//   resolve.cpp   resolves (EDRAM -> memory), write-backs, resolved views
//   textures.cpp  textures decoded from guest memory, samplers, binding
//   draw.cpp      turning a draw packet into a recorded host draw; flushing
//   debug.cpp     PNG/memory dumps for debugging
//   ../renderer.cpp  initialization, the public entry points, presentation
//
// Everything runs on the command processor thread except Initialize and
// AcquireFrame (gpu/renderer.h), so this state needs no locking.

#pragma once

#include "gpu/command_processor.h"
#include "gpu/registers.h"
#include "gpu/shader/translator.h"
#include "gpu/texture.h"
#include "gpu/xenos.h"
#include "memory.h"

#include <SDL3/SDL.h>
#include <shaderc/shaderc.h>

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
    constexpr uint32_t kRegPaSuVtxCntl = 0x2302;     // bit 0: pixel center (0 = D3D, integer centers)

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

    // FNV-1a style hash, eight bytes at a time (shader microcode and texture
    // memory get hashed often).
    // PERF: textures are hashed whole when re-checked; a faster hash (xxHash,
    // or hashing a sample of the memory) would cut texture check time.
    uint64_t Hash(const void* data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);

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
    struct Shader
    {
        bool ok = false;               // translated and compiled
        SDL_GPUShader* shader = nullptr;
        shader::ShaderInfo info;       // what it reads (constants, fetches, textures)
        std::string cacheName;         // SPIR-V file in the shader cache (for logs)
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
        uint32_t h, CopyMode mode, bool depthFloat = false, float scale = 1.0f, int32_t samplesX = 1, int32_t samplesY = 1);

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
    };

    // The host texture for EDRAM at base in a format (created on first use).
    RenderTarget* GetRenderTarget(uint32_t base, uint32_t xenosFormat, bool depth);
    // A color target, carrying over what another format last drew at the
    // same base (see targets.cpp: EDRAM aliasing).
    RenderTarget* GetColorTarget(uint32_t base, uint32_t xenosFormat);
    // The color target last used at an EDRAM base, or null.
    RenderTarget* ColorTargetAt(uint32_t base);
    // Formats whose host texture holds floats (no unorm clamping/encoding).
    bool IsFloatHostFormat(SDL_GPUTextureFormat format);
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
    struct ResolvedTexture
    {
        uint32_t base = 0, size = 0;
        uint32_t pitch = 0, height = 0, bpp = 4;
        uint32_t xenosFormat = 0;  // the resolve's destination texture format
        SDL_GPUTextureFormat hostFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
        SDL_GPUTexture* texture = nullptr;
        uint32_t generation = 0;  // bumped by every resolve into it
        bool depthFloat = false;  // depth resolves: from a 20e4 float depth buffer
    };

    // The resolved texture covering an address (physical, or through a CPU
    // view of physical memory), or null.
    ResolvedTexture* FindResolved(uint32_t address);
    // A texture fetch from resolved memory: the resolved texture itself, or
    // a copy at the fetch's size/position (see resolve.cpp).
    SDL_GPUTexture* ResolvedView(ResolvedTexture& resolved, const texture::FetchConstant& fetch, uint32_t address);
    // Executes the resolve the current registers describe.
    void Resolve();
    // Completes write-backs whose GPU downloads finished (wait: all of them).
    void PollWriteBacks(bool wait = false);

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
        uint32_t pad;
        uint32_t textures[32][4];  // per texture fetch constant: swizzle, signs, exp adjust, enabled
        float colorExpBias[4];     // 2^RB_COLOR_INFO exp bias, per render target
        float alphaRef;
        float pixelScale[2];       // host texels per guest pixel
        uint32_t pad2;
    };

    // SystemConstants::flags bits (must match translator.h).
    constexpr uint32_t kSystemFlagRectangleList = 1u << 0;
    constexpr uint32_t kSystemFlagRoundDepth24 = 1u << 1;
    constexpr uint32_t kSystemFlagVteShift = 2;          // 3 bits: PA_CL_VTE_CNTL position formats
    constexpr uint32_t kSystemFlagAlphaFuncShift = 5;    // 3 bits: alpha test function
    constexpr uint32_t kSystemFlagAlphaTest = 1u << 8;
    constexpr uint32_t kSystemFlagClampColorShift = 12;  // 4 bits: clamp color output i

    // Texture/sampler bindings for a shader's textures, filling the texture
    // part of the system constants.
    std::vector<SDL_GPUTextureSamplerBinding> BindTextures(const Shader& shader, SystemConstants& system);
    // Number of cached memory textures (for logs).
    size_t TextureCount();

    // ------------------------------------------------------------------
    // Drawing (draw.cpp)
    // ------------------------------------------------------------------

    // Records the draw the current registers describe.
    void Draw(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize);
    // Records everything pending into the command buffer. Call before
    // anything that can't happen inside a render pass.
    void Flush();
    // Per-swap bookkeeping of the draw buffers.
    void EndFrameBuffers();
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
    // H3_DUMP_MEMORY: dump guest memory at this swap.
    void DumpMemoryAtSwap();
    // H3_DUMP_FRAME / H3_SNAPSHOT_EVERY: dump the presented frame.
    void DumpFrameAtSwap(uint32_t frontbuffer, SDL_GPUTexture* texture, uint32_t width, uint32_t height);
}
