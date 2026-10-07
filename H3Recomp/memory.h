#pragma once

#include <cstddef>
#include <cstdint>

namespace h3
{
    // The guest has a flat 32-bit address space. We reserve all 4 GiB on the
    // host and translate guest addresses as g_base + address.
    constexpr size_t kGuestMemorySize = 0x100000000ull;

    // Regions the runtime owns. The game image itself lives at PPC_IMAGE_BASE
    // (normally 0x82000000) and the recompiler's function lookup table sits
    // directly after it.
    constexpr uint32_t kMainStackBase = 0x70000000;
    constexpr uint32_t kMainStackSize = 0x00100000;
    constexpr uint32_t kPcrAddress    = 0x7FFF0000;
    constexpr uint32_t kPcrSize       = 0x00001000;

    extern uint8_t* g_base;

    bool InitGuestMemory();

    template <typename T>
    T* GuestPtr(uint32_t address)
    {
        return reinterpret_cast<T*>(g_base + address);
    }
}
