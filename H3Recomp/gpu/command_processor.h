#pragma once

#include <cstdint>

namespace h3::gpu
{
    // A stand-in for the Xenos command processor. It consumes the PM4
    // command stream the game's Direct3D writes into the ring buffer, and
    // performs everything the CPU can observe: register writes, memory and
    // fence writes, waits, indirect buffers and interrupts. Nothing is drawn
    // (draw and state packets are skipped); the point is to keep the game
    // running until real graphics exist. Reference: Xenia's
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

    uint32_t MakePacketType3(uint32_t opcode, uint32_t payloadDwords);

    // Guest address of a GPU (physical) address.
    uint32_t PhysicalToGuest(uint32_t address);

    // GPU register access (big-endian words at kGpuRegisterBase).
    uint32_t ReadRegister(uint32_t index);
    void WriteRegister(uint32_t index, uint32_t value);
}
