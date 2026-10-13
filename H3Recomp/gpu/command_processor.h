#pragma once

#include <cstdint>

namespace h3::gpu
{
    // The Xenos command processor: the GPU's front end. The game's Direct3D
    // writes PM4 command packets into a ring buffer in memory; a thread here
    // consumes them as the real GPU would: register writes, constant and
    // shader loads, memory and fence writes, waits, indirect buffers,
    // predicated tiling and interrupts. Draws, resolves and swaps go on to
    // the renderer (gpu/renderer.h). Reference: Xenia's
    // src/xenia/gpu/command_processor.cc.
    //
    // GPU registers live in guest memory at kGpuRegisterBase (memory.h), where
    // the console maps them as MMIO. The CPU's stores land there directly, and
    // the command processor reads and writes the same words.

    // Presets the registers the game reads before the GPU does anything.
    // Call once guest memory exists.
    void Initialize();

    // VdInitializeRingBuffer: ring base (physical) and log2 of its size in
    // quadwords. Starts the command processor thread on first call.
    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2);

    // VdEnableRingBufferRPtrWriteBack: where the read pointer is written back.
    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2);

    // VdSetGraphicsInterruptCallback. Starts the 60 Hz vblank interrupts.
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // The packet VdSwap writes into the ring buffer: a type-3 packet with
    // this opcode (unused by the hardware; Xenia uses the same) and 63
    // payload dwords.
    constexpr uint32_t kOpcodeHostSwap = 0x64;
    constexpr uint32_t kSwapPacketDwords = 64;

    // A type-3 packet header (VdSwap builds its packet with this).
    uint32_t MakePacketType3(uint32_t opcode, uint32_t payloadDwords);

    // The display controller's gamma ramp, which the console applies to the
    // front buffer at scanout (D3D programs it through the DC_LUT_*
    // registers in the command stream). Piecewise linear: 128 segments over
    // 10-bit input, each with a base and a slope per channel, as 16-bit
    // values where 0xFFC0 is 1.0 (10 bits, shifted left 6). Xenia applies it
    // the same way when presenting.
    struct GammaRamp
    {
        static constexpr uint32_t kSegments = 128;
        bool valid = false;  // a ramp has been written
        uint16_t base[kSegments][3] = {};
        uint16_t delta[kSegments][3] = {};
    };
    // The latest ramp (thread-safe copy).
    GammaRamp CurrentGammaRamp();

    // How many times the command stream has written the vertex (stage 0)
    // or pixel (stage 1) shader float constants: unchanged means a draw
    // sees the same constants as the last one that read them.
    uint32_t AluConstantGeneration(uint32_t stage);
    // The float constants as the command stream wrote them, host-endian
    // (1024 dwords per stage: vertex, then pixel), so a draw's snapshot is
    // a plain copy instead of a byte swap of the register file (~8% of the
    // command processor thread). Command processor thread only.
    const uint32_t* AluConstantsHost();

    // GPU register access (big-endian words at kGpuRegisterBase).
    uint32_t ReadRegister(uint32_t index);
    void WriteRegister(uint32_t index, uint32_t value);
}
