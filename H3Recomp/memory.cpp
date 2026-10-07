#include "memory.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace h3
{
    uint8_t* g_base = nullptr;

    bool InitGuestMemory()
    {
        // Pages are committed lazily by the OS, so reserving the whole range
        // up front costs nothing until the guest touches it.
#ifdef _WIN32
        void* memory = VirtualAlloc(nullptr, kGuestMemorySize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        void* memory = mmap(nullptr, kGuestMemorySize, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (memory == MAP_FAILED)
            memory = nullptr;
#endif
        g_base = static_cast<uint8_t*>(memory);
        return g_base != nullptr;
    }
}
