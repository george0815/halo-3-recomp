#include "memory.h"

#include <algorithm>

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

    static void ZeroPages(uint32_t address, uint32_t size)
    {
#ifdef _WIN32
        // Guest pages are at least 4 KiB, so this lines up with host pages.
        VirtualFree(g_base + address, size, MEM_DECOMMIT);
        VirtualAlloc(g_base + address, size, MEM_COMMIT, PAGE_READWRITE);
#else
        // Private anonymous pages read back as zero after MADV_DONTNEED.
        madvise(g_base + address, size, MADV_DONTNEED);
#endif
    }

    static uint64_t AlignUp(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
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
        uint64_t end = AlignUp(uint64_t(address) + std::max<uint32_t>(size, 1), m_pageSize);
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
        uint64_t bytes = AlignUp(std::max<uint32_t>(size, 1), m_pageSize);
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
        uint64_t candidate = AlignUp(low, alignTo);
        for (const auto& [allocBase, allocSize] : m_allocations)
        {
            if (uint64_t(allocBase) + allocSize <= candidate)
                continue;
            if (candidate + bytes <= allocBase)
                break;
            candidate = AlignUp(uint64_t(allocBase) + allocSize, alignTo);
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
        ZeroPages(start, uint32_t(AlignUp(uint64_t(address) + size, m_pageSize) - start));
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

    GuestHeap& VirtualHeap(uint32_t pageSize)
    {
        static GuestHeap heap4k(0x00010000, 0x40000000, kPageSize4K);
        static GuestHeap heap64k(0x40000000, 0x70000000, kPageSize64K);
        return pageSize >= kPageSize64K ? heap64k : heap4k;
    }

    GuestHeap& PhysicalHeap()
    {
        static GuestHeap heap(kPhysicalMemoryBase, 0xC0000000, kPageSize4K);
        return heap;
    }

    GuestHeap& SystemHeap()
    {
        static GuestHeap heap(0x70000000, kGpuRegisterBase, kPageSize4K);
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
