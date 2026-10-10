#pragma once

#include <cstdint>

struct SDL_GPUDevice;
struct SDL_GPUTexture;

namespace h3::gpu::renderer
{
    // Host rendering of the command stream with SDL_GPU (Vulkan). Everything
    // here except Initialize and AcquireFrame runs on the command processor
    // thread, which records and submits its own command buffers.
    //
    // EDRAM render targets become host textures keyed by EDRAM base and
    // format. Resolves (EDRAM to memory copies) blit into host textures that
    // cover the destination memory; texture fetches from that memory sample
    // those textures. The texture the swap names is presented.

    // Main thread, once the window's device exists. Returns false (and
    // rendering stays off) if the device can't be used.
    bool Initialize(SDL_GPUDevice* device);

    // H3_RENDER=0 turns host rendering off (the command processor still runs).
    bool IsEnabled();

    // Shader microcode was loaded (host-endian dwords). type: 0 vertex, 1 pixel.
    void OnShaderLoad(uint32_t type, const uint32_t* dwords, uint32_t dwordCount);

    // A draw packet (not a resolve: the command processor checks the EDRAM
    // mode). indexBase is the guest address for indexed draws, dmaSize the
    // VGT_DMA_SIZE value (index count and endian mode).
    void OnDraw(uint32_t initiator, uint32_t indexBase, uint32_t dmaSize);

    // A draw in EDRAM copy mode: resolve the render target to memory.
    void OnResolve();

    // A swap: submit the frame and present the texture at frontbuffer (a
    // physical address), width x height.
    void OnSwap(uint32_t frontbuffer, uint32_t width, uint32_t height);

    // Main thread: the most recently presented frame, if any.
    struct Frame
    {
        SDL_GPUTexture* texture = nullptr;
        uint32_t textureWidth = 0, textureHeight = 0;
        uint32_t x = 0, y = 0, width = 0, height = 0;  // region to show
        uint64_t serial = 0;
    };
    bool AcquireFrame(Frame& frame);
}
