#pragma once

#include <cstdint>

namespace h3::gpu
{
    // Debug aid: with H3_GPU_CAPTURE=<frame>, writes gpu_capture.txt with
    // every draw, resolve and shader load the command processor sees in that
    // frame (counted in swaps), along with the register state that matters
    // for rendering. Command processor thread only.
    namespace capture
    {
        // Shader microcode was loaded (IM_LOAD / IM_LOAD_IMMEDIATE).
        void OnShaderLoad(uint32_t type, uint32_t guestAddress, uint32_t dwords);

        // A draw packet. indexBase/indexWords are 0 for auto-indexed draws.
        void OnDraw(uint32_t initiator, uint32_t indexBase, uint32_t indexWords);

        // A swap: ends the frame.
        void OnSwap(uint32_t frame);
    }
}
