// xboxkrnl memory exports. Reference: Xenia's xboxkrnl_memory.cc.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "../memory.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace h3;
using namespace h3::kernel;

namespace
{
    constexpr uint32_t kMemCommit = 0x1000;
    constexpr uint32_t kMemReserve = 0x2000;
    constexpr uint32_t kMemDecommit = 0x4000;
    constexpr uint32_t kMemFree = 0x10000;
    constexpr uint32_t kMemPrivate = 0x20000;
    constexpr uint32_t kMemReset = 0x80000;
    constexpr uint32_t kMemTopDown = 0x100000;
    constexpr uint32_t kMemLargePages = 0x20000000;
    constexpr uint32_t kMem16MBPages = 0x80000000;

    constexpr uint32_t kPageReadOnly = 0x02;
    constexpr uint32_t kPageReadWrite = 0x04;

    // Physical memory is mapped at 0xA0000000; physical address 0 is there.
    constexpr uint32_t kPhysicalBase = kPhysicalMemoryBase;
    constexpr uint32_t kPhysicalSize = 0x20000000;

    uint32_t RoundUp(uint32_t value, uint32_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }
}

// NTSTATUS NtAllocateVirtualMemory(PVOID* baseAddress, PSIZE_T regionSize,
//     ULONG allocationType, ULONG protect, BOOL debugMemory)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    auto& baseRef = GuestRef<uint32_t>(ctx.r3.u32);
    auto& sizeRef = GuestRef<uint32_t>(ctx.r4.u32);
    uint32_t type = ctx.r5.u32;

    // Some titles pass negative sizes.
    uint32_t size = int32_t(sizeRef.get()) < 0 ? uint32_t(-int32_t(sizeRef.get())) : sizeRef.get();
    if (size == 0 || !(type & (kMemCommit | kMemReserve | kMemReset)))
    {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }

    uint32_t pageSize = (type & kMemLargePages) ? kPageSize64K : kPageSize4K;
    uint32_t requested = baseRef.get() & ~(pageSize - 1);
    size = RoundUp(size, requested ? pageSize : kPageSize64K);

    uint32_t address = 0;
    if (requested != 0)
    {
        GuestHeap* heap = HeapForAddress(requested);
        if (heap == nullptr || heap->PageSize() != pageSize)
        {
            ctx.r3.u64 = kStatusAccessDenied;
            return;
        }

        // Committing part of an earlier reservation.
        uint32_t allocBase, allocSize;
        if (heap->Find(requested, &allocBase, &allocSize))
        {
            if (uint64_t(requested) + size <= uint64_t(allocBase) + allocSize)
                address = requested;
        }
        else
        {
            address = heap->Alloc(size, pageSize, requested);
        }
    }
    else
    {
        GuestHeap& heap = VirtualHeap(pageSize);
        address = (type & kMemTopDown) ? heap.AllocInRange(size, pageSize, 0, 0xFFFFFFFF, true)
                                       : heap.Alloc(size, pageSize);
    }

    if (address == 0)
    {
        ctx.r3.u64 = kStatusNoMemory;
        return;
    }

    baseRef = address;
    sizeRef = size;
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtFreeVirtualMemory(PVOID* baseAddress, PSIZE_T regionSize,
//     ULONG freeType, BOOL debugMemory)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    auto& baseRef = GuestRef<uint32_t>(ctx.r3.u32);
    auto& sizeRef = GuestRef<uint32_t>(ctx.r4.u32);
    uint32_t address = baseRef;

    GuestHeap* heap = address ? HeapForAddress(address) : nullptr;
    if (heap == nullptr)
    {
        ctx.r3.u64 = kStatusMemoryNotAllocated;
        return;
    }
    if (heap == &PhysicalHeap())
    {
        ctx.r3.u64 = kStatusAccessDenied;
        return;
    }

    uint32_t allocBase, allocSize;
    if (!heap->Find(address, &allocBase, &allocSize))
    {
        ctx.r3.u64 = kStatusUnsuccessful;
        return;
    }

    if (ctx.r5.u32 == kMemDecommit)
    {
        uint32_t size = RoundUp(sizeRef, heap->PageSize());
        heap->Decommit(address, size);
        sizeRef = size;
    }
    else
    {
        if (address != allocBase)
        {
            ctx.r3.u64 = kStatusUnsuccessful;
            return;
        }
        heap->Free(address);
        sizeRef = allocSize;
    }

    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtQueryVirtualMemory(PVOID baseAddress, PMEMORY_BASIC_INFORMATION info)
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    uint32_t address = ctx.r3.u32;
    uint32_t info = ctx.r4.u32;

    GuestHeap* heap = HeapForAddress(address);
    if (heap == nullptr)
    {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }

    // MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect,
    // RegionSize, State, Protect, Type.
    uint32_t page = address & ~(heap->PageSize() - 1);
    uint32_t allocBase, allocSize;
    bool allocated = heap->Find(address, &allocBase, &allocSize);
    GuestRef<uint32_t>(info + 0x00) = page;
    GuestRef<uint32_t>(info + 0x04) = allocated ? allocBase : 0;
    GuestRef<uint32_t>(info + 0x08) = allocated ? kPageReadWrite : 0;
    GuestRef<uint32_t>(info + 0x0C) = allocated ? allocBase + allocSize - page : heap->PageSize();
    GuestRef<uint32_t>(info + 0x10) = allocated ? kMemCommit : kMemFree;
    GuestRef<uint32_t>(info + 0x14) = allocated ? kPageReadWrite : 0;
    GuestRef<uint32_t>(info + 0x18) = allocated ? kMemPrivate : 0;
    ctx.r3.u64 = kStatusSuccess;
}

// PVOID MmAllocatePhysicalMemoryEx(ULONG flags, SIZE_T size, ULONG protect,
//     ULONG_PTR minAddress, ULONG_PTR maxAddress, ULONG alignment)
// minAddress/maxAddress bound the physical address, not the virtual one.
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32;
    uint32_t protect = ctx.r5.u32;
    uint32_t minAddress = ctx.r6.u32;
    uint32_t maxAddress = ctx.r7.u32;
    uint32_t alignment = ctx.r8.u32;

    if (!(protect & (kPageReadOnly | kPageReadWrite)))
    {
        ctx.r3.u64 = 0;
        return;
    }

    uint32_t pageSize = kPageSize4K;
    if (protect & kMem16MBPages)
        pageSize = 0x1000000;
    else if (protect & kMemLargePages)
        pageSize = kPageSize64K;

    uint32_t low = kPhysicalBase + std::min(minAddress, kPhysicalSize - 1);
    uint64_t high = uint64_t(kPhysicalBase) + std::min(maxAddress, kPhysicalSize - 1) + 1;
    // Top-down, like the console's kernel (and Xenia). Map files hold
    // absolute pointers into tag memory that assume this placement.
    ctx.r3.u64 = PhysicalHeap().AllocInRange(RoundUp(size, pageSize), std::max(alignment, pageSize),
        low, uint32_t(std::min<uint64_t>(high, 0xFFFFFFFF)), true);

    static const bool log = getenv("H3_LOG_MEMORY") != nullptr;
    if (log)
        fprintf(stderr, "[memory] physical alloc 0x%X (protect 0x%X, range 0x%X-0x%X, align 0x%X) -> 0x%08X\n",
            size, protect, minAddress, maxAddress, alignment, ctx.r3.u32);
}

// void MmFreePhysicalMemory(ULONG type, PVOID address)
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    PhysicalHeap().Free(ctx.r4.u32);
}

// PHYSICAL_ADDRESS MmGetPhysicalAddress(PVOID address)
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    uint32_t address = ctx.r3.u32;
    ctx.r3.u64 = address >= kPhysicalBase ? address & (kPhysicalSize - 1) : 0;
}

// SIZE_T MmQueryAllocationSize(PVOID address)
PPC_FUNC(__imp__MmQueryAllocationSize)
{
    uint32_t allocBase, allocSize;
    GuestHeap* heap = HeapForAddress(ctx.r3.u32);
    ctx.r3.u64 = heap && heap->Find(ctx.r3.u32, &allocBase, &allocSize) ? allocSize : 0;
}

// ULONG MmQueryAddressProtect(PVOID address)
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    uint32_t allocBase, allocSize;
    GuestHeap* heap = HeapForAddress(ctx.r3.u32);
    ctx.r3.u64 = heap && heap->Find(ctx.r3.u32, &allocBase, &allocSize) ? kPageReadWrite : 0;
}

// void MmSetAddressProtect(PVOID address, ULONG size, ULONG protect)
// Guest memory is always read/write on the host.
PPC_FUNC(__imp__MmSetAddressProtect)
{
}

// BOOL MmIsAddressValid(PVOID address)
PPC_FUNC(__imp__MmIsAddressValid)
{
    uint32_t address = ctx.r3.u32;
    uint32_t allocBase, allocSize;
    GuestHeap* heap = HeapForAddress(address);
    bool inImage = address >= PPC_IMAGE_BASE && address < PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
    ctx.r3.u64 = inImage || (heap && heap->Find(address, &allocBase, &allocSize));
}

// PVOID ExAllocatePoolTypeWithTag(SIZE_T size, ULONG tag, POOL_TYPE type)
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag)
{
    ctx.r3.u64 = SystemAlloc(ctx.r3.u32);
}

// PVOID ExAllocatePool(SIZE_T size)
PPC_FUNC(__imp__ExAllocatePool)
{
    ctx.r3.u64 = SystemAlloc(ctx.r3.u32);
}

// void ExFreePool(PVOID address)
PPC_FUNC(__imp__ExFreePool)
{
    SystemHeap().Free(ctx.r3.u32);
}

// PVOID MmCreateKernelStack(DWORD size, DWORD unknown)
// Returns the top of a new stack (stacks grow down), as in Xenia.
PPC_FUNC(__imp__MmCreateKernelStack)
{
    uint32_t size = (ctx.r3.u32 + 0xFFF) & ~0xFFFu;
    uint32_t alignment = (ctx.r3.u32 & 0xF000) ? 0x1000 : 0x10000;
    uint32_t bottom = SystemHeap().Alloc(size, alignment);
    ctx.r3.u64 = bottom != 0 ? bottom + size : 0;
}

// void MmDeleteKernelStack(PVOID stackBase, PVOID stackLimit)
// stackBase is the top MmCreateKernelStack returned; stackLimit the bottom.
PPC_FUNC(__imp__MmDeleteKernelStack)
{
    SystemHeap().Free(ctx.r4.u32);
}

// NTSTATUS MmQueryStatistics(PMM_STATISTICS stats)
// MM_STATISTICS (0x68 bytes, the caller sets Length): page counts for a
// 512 MB console. Title usage is what the physical heap has handed out.
PPC_FUNC(__imp__MmQueryStatistics)
{
    uint32_t stats = ctx.r3.u32;
    if (stats == 0 || GuestRef<uint32_t>(stats) != 0x68)
    {
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    constexpr uint32_t kTotalPages = 0x20000;  // 512 MB of 4 KB pages
    constexpr uint32_t kKernelPages = 0x100;
    constexpr uint32_t kSystemPages = 0x800;
    uint32_t titlePages = uint32_t(PhysicalHeap().AllocatedBytes() / 4096);
    uint32_t available = kTotalPages - kKernelPages - kSystemPages - std::min(titlePages, kTotalPages - kKernelPages - kSystemPages);
    memset(GuestPtr<uint8_t>(stats + 4), 0, 0x64);
    GuestRef<uint32_t>(stats + 0x04) = kTotalPages;
    GuestRef<uint32_t>(stats + 0x08) = kKernelPages;
    GuestRef<uint32_t>(stats + 0x0C) = available;               // title available pages
    GuestRef<uint32_t>(stats + 0x10) = 0x2FFE0000;              // title total virtual bytes
    GuestRef<uint32_t>(stats + 0x14) = titlePages * 4096;       // title reserved virtual bytes
    GuestRef<uint32_t>(stats + 0x18) = titlePages;              // title physical pages
    GuestRef<uint32_t>(stats + 0x38) = kSystemPages;            // system available pages
    GuestRef<uint32_t>(stats + 0x44) = kSystemPages;            // system physical pages
    GuestRef<uint32_t>(stats + 0x64) = kTotalPages - 1;         // highest physical page
    ctx.r3.u64 = kStatusSuccess;
}
