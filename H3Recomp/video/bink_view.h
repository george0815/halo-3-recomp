// Reading the game's Bink movie frames directly from guest memory.

#pragma once

#include <cstdint>

namespace h3::video
{
    // The game's Bink video read straight from the frame buffers Bink
    // decodes into, bypassing the GPU. The window shows it until the
    // renderer presents its first frame (a leftover from before rendering
    // existed, still handy when debugging the renderer).
    // Halo 3 registers one BINKFRAMEBUFFERS with Bink for its movie player;
    // the planes are linear 8-bit YUV 4:2:0.
    struct BinkFrame
    {
        uint32_t width, height;  // luma; chroma is half each way
        const uint8_t* y;
        const uint8_t* u;  // Bink's cB
        const uint8_t* v;  // Bink's cR
        uint32_t yPitch, uPitch, vPitch;
    };

    // The current frame buffer set, or false if the movie
    // player's buffers aren't set up (no movie has played yet).
    bool GetBinkFrame(BinkFrame& frame);
}
