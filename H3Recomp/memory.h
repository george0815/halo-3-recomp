// Guest memory: the 360's 32-bit address space, reserved as one 4 GiB block
// of host memory. Guest addresses are offsets from g_base; GuestPtr/GuestRef
// turn them into host pointers (GuestRef also handles big-endian values).
// Also the page allocators (GuestHeap) behind the kernel's memory exports.
//
// See docs/ARCHITECTURE.md ("Key concepts") for the big picture.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>

#include <xbox.h>

namespace h3
{
    // The guest has a flat 32-bit address space. We reserve all 4 GiB on the
    // host and translate guest addresses as g_base + address.
    constexpr size_t kGuestMemorySize = 0x100000000ull;

    // Guest address layout, following the 360's (see Xenia's memory.cc):
    //   0x00010000 - 0x3FFFFFFF  virtual, 4 KiB pages
    //   0x40000000 - 0x6FFFFFFF  virtual, 64 KiB pages
    //   0x70000000 - 0x7FC7FFFF  runtime-owned: stacks, KPCR, KTHREAD, TLS
    //   0x7FC80000 - 0x7FC9FFFF  GPU registers (MMIO on the console; plain
    //                            memory here, see gpu/command_processor.h)
    //   0x82000000 - ...         the XEX image, then the recompiler's
    //                            function lookup table
    //   0x88000000 - 0x8BFFFFFF  DLLs (XexLoadImage) at their own bases; their
    //                            function table slots follow the executable's,
    //                            up to 0x9FFFFFFF (see kernel/modules.cpp)
    //   0xA0000000 - 0xBFFFFFFF  physical memory (MmAllocatePhysicalMemoryEx)
    //   0xC0000000 - 0xDFFFFFFF  the same physical memory again
    //   0xE0000000 - 0xFFFFFFFF  and again, starting 0x1000 in (memory.cpp)
    constexpr uint32_t kGpuRegisterBase = 0x7FC80000;
    constexpr uint32_t kPhysicalMemoryBase = 0xA0000000;
    // The 360 has 512 MiB of physical memory, so physical addresses (what the
    // GPU and audio hardware see) fit in 29 bits; the high bits of a guest
    // address only pick one of the three views onto it.
    constexpr uint32_t kPhysicalMemorySize = 0x20000000;
    constexpr uint32_t kPhysicalAddressMask = kPhysicalMemorySize - 1;
    constexpr uint32_t kPageSize4K = 0x1000;
    constexpr uint32_t kPageSize64K = 0x10000;
    constexpr uint32_t kPageSize16M = 0x1000000;

    // Rounds value up to a multiple of alignment (a power of two).
    constexpr uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }
    constexpr uint64_t AlignUp64(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

    // Host address of guest address 0 (set by InitGuestMemory).
    extern uint8_t* g_base;

    // Physical address (as hardware sees it) of a guest address in one of
    // the physical views, and the guest address (in the 0xA0000000 view)
    // of a physical address.
    constexpr uint32_t GuestToPhysical(uint32_t address) { return address & kPhysicalAddressMask; }
    constexpr uint32_t PhysicalToGuest(uint32_t physical) { return kPhysicalMemoryBase + (physical & kPhysicalAddressMask); }

    // Reserves the 4 GiB guest range and maps the physical memory views.
    // Returns false if the host can't reserve it.
    bool InitGuestMemory();

    template <typename T>
    T* GuestPtr(uint32_t address)
    {
        return reinterpret_cast<T*>(g_base + address);
    }

    // Big-endian guest values. be<T> converts on read and write.
    template <typename T>
    be<T>& GuestRef(uint32_t address)
    {
        return *reinterpret_cast<be<T>*>(g_base + address);
    }

    // A page-granular allocator over one guest address range. Memory is
    // committed lazily by the host, so reserve and commit are the same thing
    // here; released and decommitted pages read back as zero.
    class GuestHeap
    {
    public:
        // A heap over guest addresses [base, end) handing out pageSize pages.
        GuestHeap(uint32_t base, uint32_t end, uint32_t pageSize);

        // Returns the guest address, or 0 if the range is full or (for a
        // fixed address) already in use. Size is rounded up to whole pages.
        uint32_t Alloc(uint32_t size, uint32_t alignment = 0, uint32_t address = 0);
        // topDown takes the highest fitting address instead of the lowest.
        uint32_t AllocInRange(uint32_t size, uint32_t alignment, uint32_t low, uint32_t high, bool topDown = false);

        // Releases the allocation starting at address.
        bool Free(uint32_t address);

        // Zeroes pages without releasing them.
        void Decommit(uint32_t address, uint32_t size);

        // The allocation containing address, or false.
        bool Find(uint32_t address, uint32_t* allocBase, uint32_t* allocSize) const;

        bool Contains(uint32_t address) const { return address >= m_base && address < m_end; }
        // Bytes in live allocations.
        uint64_t AllocatedBytes() const;
        uint32_t PageSize() const { return m_pageSize; }

    private:
        uint32_t m_base;
        uint32_t m_end;
        uint32_t m_pageSize;
        mutable std::mutex m_mutex;
        std::map<uint32_t, uint32_t> m_allocations;  // base -> size
    };

    // The heap for virtual allocations with the given page size (4 KiB or
    // 64 KiB), for physical memory, and for the runtime's own guest objects.
    GuestHeap& VirtualHeap(uint32_t pageSize);
    GuestHeap& PhysicalHeap();
    GuestHeap& SystemHeap();

    // The heap owning a guest address, or nullptr.
    GuestHeap* HeapForAddress(uint32_t address);

    // Zeroed memory from the system heap, for runtime-owned guest objects.
    uint32_t SystemAlloc(uint32_t size);
}
