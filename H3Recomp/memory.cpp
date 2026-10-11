// Guest memory implementation: reserving the 4 GiB guest range, aliasing the
// three views of physical memory onto one block of host memory, and the
// page-granular heaps (GuestHeap) that the kernel's allocation exports use.
// Interface and address map: memory.h.

#include "memory.h"

#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace h3
{
    uint8_t* g_base = nullptr;

    namespace
    {
        // Physical memory is visible through three views, as on the console:
        // 0xA0000000 (64 KiB pages), 0xC0000000 (16 MiB pages) and
        // 0xE0000000 (4 KiB pages). Titles and D3D use whichever view an
        // allocation's page size implies, and also build addresses in one
        // view from another, so all three must alias the same memory. The
        // 0xE0000000 view starts 0x1000 into physical memory (Xenia's
        // memory.cc maps it the same way).
        constexpr uint32_t kPhysicalViewC = 0xC0000000;
        constexpr uint32_t kPhysicalViewE = 0xE0000000;
        constexpr uint32_t kViewEOffset = 0x1000;

#ifndef _WIN32
        int g_physicalFd = -1;

        // Maps `size` bytes of the physical memory file, from `offset`, at guest
        // address `guest`.
        bool MapPhysicalView(uint32_t guest, uint64_t offset, uint64_t size)
        {
            void* view = mmap(g_base + guest, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, g_physicalFd, offset);
            return view != MAP_FAILED;
        }
#endif

        // Physical offset of a guest address in one of the views, or -1.
        int64_t PhysicalOffset(uint32_t address)
        {
            if (address >= kPhysicalViewE)
                return int64_t(address - kPhysicalViewE) + kViewEOffset;
            if (address >= kPhysicalMemoryBase)
                return (address - kPhysicalMemoryBase) & (kPhysicalMemorySize - 1);
            return -1;
        }
    }

    bool InitGuestMemory()
    {
        // Pages are committed lazily by the OS, so reserving the whole range
        // up front costs nothing until the guest touches it.
#ifdef _WIN32
        // TODO: the physical views aren't aliased on Windows yet (needs a
        // section mapped with MapViewOfFile3 into placeholders).
        void* memory = VirtualAlloc(nullptr, kGuestMemorySize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        void* memory = mmap(nullptr, kGuestMemorySize, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (memory == MAP_FAILED)
            return false;
        g_base = static_cast<uint8_t*>(memory);

        // Physical memory is a sparse memfd mapped at each view.
        g_physicalFd = memfd_create("h3-physical", 0);
        if (g_physicalFd < 0 || ftruncate(g_physicalFd, kPhysicalMemorySize) != 0)
            return false;
        if (!MapPhysicalView(kPhysicalMemoryBase, 0, kPhysicalMemorySize) ||
            !MapPhysicalView(kPhysicalViewC, 0, kPhysicalMemorySize) ||
            !MapPhysicalView(kPhysicalViewE, kViewEOffset, kPhysicalMemorySize - kViewEOffset))
            return false;
#endif
        g_base = static_cast<uint8_t*>(memory);
        return g_base != nullptr;
    }

    // Releases the host pages behind a range so they read back as zero (and
    // stop using host memory).
    static void ZeroPages(uint32_t address, uint32_t size)
    {
#ifdef _WIN32
        // Guest pages are at least 4 KiB, so this lines up with host pages.
        VirtualFree(g_base + address, size, MEM_DECOMMIT);
        VirtualAlloc(g_base + address, size, MEM_COMMIT, PAGE_READWRITE);
#else
        // Shared (physical) pages keep their contents after MADV_DONTNEED;
        // punching a hole in the memfd frees them and reads back zero.
        if (int64_t offset = PhysicalOffset(address); offset >= 0)
            fallocate(g_physicalFd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, offset, size);
        else
            // Private anonymous pages read back as zero after MADV_DONTNEED.
            madvise(g_base + address, size, MADV_DONTNEED);
#endif
    }


    GuestHeap::GuestHeap(uint32_t base, uint32_t end, uint32_t pageSize)
        : m_base(base), m_end(end), m_pageSize(pageSize)
    {
    }

    uint32_t GuestHeap::Alloc(uint32_t size, uint32_t alignment, uint32_t address)
    {
        if (address == 0)
            return AllocInRange(size, alignment, m_base, m_end);

        std::lock_guard lock(m_mutex);
        uint64_t start = address & ~uint64_t(m_pageSize - 1);
        uint64_t end = AlignUp64(uint64_t(address) + std::max<uint32_t>(size, 1), m_pageSize);
        if (start < m_base || end > m_end)
            return 0;

        // Reject overlap with the allocation before and any after.
        auto next = m_allocations.lower_bound(uint32_t(start));
        if (next != m_allocations.end() && next->first < end)
            return 0;
        if (next != m_allocations.begin())
        {
            auto prev = std::prev(next);
            if (uint64_t(prev->first) + prev->second > start)
                return 0;
        }

        m_allocations.emplace(uint32_t(start), uint32_t(end - start));
        return uint32_t(start);
    }

    uint32_t GuestHeap::AllocInRange(uint32_t size, uint32_t alignment, uint32_t low, uint32_t high, bool topDown)
    {
        std::lock_guard lock(m_mutex);
        uint64_t alignTo = std::max(alignment, m_pageSize);
        uint64_t bytes = AlignUp64(std::max<uint32_t>(size, 1), m_pageSize);
        low = std::max(low, m_base);
        high = std::min(high, m_end);

        if (topDown)
        {
            // First fit from the top, walking the gaps downwards.
            if (bytes > high)
                return 0;
            uint64_t candidate = (uint64_t(high) - bytes) & ~(alignTo - 1);
            for (auto it = m_allocations.rbegin(); it != m_allocations.rend(); ++it)
            {
                const auto& [allocBase, allocSize] = *it;
                if (allocBase >= candidate + bytes)
                    continue;
                if (uint64_t(allocBase) + allocSize <= candidate)
                    break;
                if (allocBase < bytes)
                    return 0;
                candidate = (uint64_t(allocBase) - bytes) & ~(alignTo - 1);
            }
            if (candidate < low)
                return 0;
            m_allocations.emplace(uint32_t(candidate), uint32_t(bytes));
            return uint32_t(candidate);
        }

        // First fit over the gaps between allocations.
        uint64_t candidate = AlignUp64(low, alignTo);
        for (const auto& [allocBase, allocSize] : m_allocations)
        {
            if (uint64_t(allocBase) + allocSize <= candidate)
                continue;
            if (candidate + bytes <= allocBase)
                break;
            candidate = AlignUp64(uint64_t(allocBase) + allocSize, alignTo);
        }

        if (candidate + bytes > high)
            return 0;

        m_allocations.emplace(uint32_t(candidate), uint32_t(bytes));
        return uint32_t(candidate);
    }

    bool GuestHeap::Free(uint32_t address)
    {
        uint32_t size;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_allocations.find(address);
            if (it == m_allocations.end())
                return false;
            size = it->second;
            m_allocations.erase(it);
        }
        ZeroPages(address, size);
        return true;
    }

    void GuestHeap::Decommit(uint32_t address, uint32_t size)
    {
        uint32_t start = address & ~(m_pageSize - 1);
        ZeroPages(start, uint32_t(AlignUp64(uint64_t(address) + size, m_pageSize) - start));
    }

    uint64_t GuestHeap::AllocatedBytes() const
    {
        std::lock_guard lock(m_mutex);
        uint64_t total = 0;
        for (const auto& [base, size] : m_allocations)
            total += size;
        return total;
    }

    bool GuestHeap::Find(uint32_t address, uint32_t* allocBase, uint32_t* allocSize) const
    {
        std::lock_guard lock(m_mutex);
        auto it = m_allocations.upper_bound(address);
        if (it == m_allocations.begin())
            return false;
        --it;
        if (address >= uint64_t(it->first) + it->second)
            return false;
        *allocBase = it->first;
        *allocSize = it->second;
        return true;
    }

    // Heap ranges, following the 360's address map (memory.h).
    namespace
    {
        constexpr uint32_t kVirtual4KBase = 0x00010000;   // the first 64 KiB stay unmapped (null pointer guard)
        constexpr uint32_t kVirtual64KBase = 0x40000000;  // virtual memory with 64 KiB pages starts here
        constexpr uint32_t kSystemBase = 0x70000000;      // runtime-owned objects (stacks, KPCR, ...) up to the GPU registers
    }

    // Virtual memory (NtAllocateVirtualMemory): a 4 KiB-page heap and a
    // 64 KiB-page heap, as on the console.
    GuestHeap& VirtualHeap(uint32_t pageSize)
    {
        static GuestHeap heap4k(kVirtual4KBase, kVirtual64KBase, kPageSize4K);
        static GuestHeap heap64k(kVirtual64KBase, kSystemBase, kPageSize64K);
        return pageSize >= kPageSize64K ? heap64k : heap4k;
    }

    // Physical memory (MmAllocatePhysicalMemoryEx), handed out through the
    // 0xA0000000 view.
    GuestHeap& PhysicalHeap()
    {
        static GuestHeap heap(kPhysicalMemoryBase, kPhysicalMemoryBase + kPhysicalMemorySize, kPageSize4K);
        return heap;
    }

    // Guest memory the runtime itself allocates (thread stacks, KPCR,
    // KTHREAD, TLS, kernel object stand-ins the game can see).
    GuestHeap& SystemHeap()
    {
        static GuestHeap heap(kSystemBase, kGpuRegisterBase, kPageSize4K);
        return heap;
    }

    GuestHeap* HeapForAddress(uint32_t address)
    {
        for (GuestHeap* heap : { &VirtualHeap(kPageSize4K), &VirtualHeap(kPageSize64K), &PhysicalHeap(), &SystemHeap() })
        {
            if (heap->Contains(address))
                return heap;
        }
        return nullptr;
    }

    uint32_t SystemAlloc(uint32_t size)
    {
        // Fresh pages are already zero.
        return SystemHeap().Alloc(size);
    }
}
