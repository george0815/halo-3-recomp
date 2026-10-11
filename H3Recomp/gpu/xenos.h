// Small Xenos GPU definitions shared by the command processor and the
// renderer.

#pragma once

#include <cstdint>

namespace h3::gpu
{
    // Xenos endian modes: how the GPU swaps bytes when it reads or writes
    // memory (fetch constants, index buffers, memory writes carry one).
    enum Endian : uint32_t
    {
        kEndianNone = 0,
        kEndian8in16 = 1,   // swap bytes within each 16-bit half
        kEndian8in32 = 2,   // full 32-bit byte swap (the big-endian case)
        kEndian16in32 = 3,  // swap the 16-bit halves
    };

    // Applies an endian mode to a 32-bit word (the swap is its own inverse).
    inline uint32_t SwapEndian32(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case kEndian8in16: return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
        case kEndian8in32: return __builtin_bswap32(value);
        case kEndian16in32: return (value << 16) | (value >> 16);
        default: return value;
        }
    }
}
