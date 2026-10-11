// XMA hardware decoder emulation (see the comment below).

#pragma once

#include <cstdint>

// The XMA hardware decoder (Xenia's XmaDecoder / XmaContext): 320 contexts
// of 64 bytes in physical memory that the game fills in (input packet
// buffers, an output ring of 16-bit PCM) and kicks; a worker thread decodes
// them with FFmpeg's single-frame XMA decoder (tools/build_ffmpeg.sh).
//
// The registers at 0x7FEA0000 are MMIO on the console. Here they're plain
// memory: the context array address is stored there for the game to read,
// and the game's kick/lock writes are caught by hooking the two functions
// that make them (hooks/xma.cpp).
namespace h3::apu
{
    // Allocates the context array and starts the worker. Call once at boot,
    // before the game reads the XMA registers.
    void InitXma();

    // XMACreateContext / XMAReleaseContext: guest address of a context, 0
    // when none are free.
    uint32_t AllocateXmaContext();
    void ReleaseXmaContext(uint32_t guestAddress);

    // The kick / lock registers: start decoding a context / stop it.
    void EnableXmaContext(uint32_t id);
    void DisableXmaContext(uint32_t id);
}
