// H3_WATCH_WRITES=<first swap>-<last swap>:<physical address>:<bytes>
// (or v<guest address> for any other guest address, e.g. MMIO registers)
// Write-protects that physical range (in all three physical views) during
// those frames and counts the host instructions that write to it. Each
// write faults, the page is unprotected for one single-stepped instruction,
// then protected again. Results go to write_watch.txt as offsets into the
// executable (symbolize with nm, like tools/h3profile.py).
//
// Linux x86-64 only; meant for finding which recompiled function writes a
// piece of guest memory.

#include "memory.h"
#include "config/env.h"

#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

extern "C" char __executable_start;

namespace h3::debug
{
    namespace
    {
        constexpr uint32_t kViews[] = { 0xA0000000, 0xC0000000, 0xE0000000 };
        constexpr size_t kMaxSites = 4096;

        uint32_t g_first = 0, g_last = 0, g_address = 0, g_bytes = 0;
        bool g_virtual = false;  // g_address is a plain guest address, not physical
        std::atomic<bool> g_armed{ false };
        struct Site
        {
            uint64_t rip;
            uint64_t count;
            uint32_t firstOffset;
        };
        Site g_sites[kMaxSites];
        size_t g_siteCount = 0;
        std::mutex g_mutex;
        thread_local uint8_t* t_page = nullptr;  // page to re-protect after the step

        // Host address of `offset` in one physical view (or of the guest
        // address itself for v-addresses).
        uint8_t* HostAddress(uint32_t view, uint32_t offset)
        {
            if (g_virtual)
                return g_base + offset;
            // The 0xE0000000 view starts 0x1000 into physical memory.
            return g_base + view + offset - (view == 0xE0000000 ? 0x1000 : 0);
        }

        // Applies a protection to the watched range in every view.
        void Protect(int protection)
        {
            long pageSize = sysconf(_SC_PAGESIZE);
            for (uint32_t view : kViews)
            {
                uintptr_t start = reinterpret_cast<uintptr_t>(HostAddress(view, g_address)) & ~uintptr_t(pageSize - 1);
                uintptr_t end = reinterpret_cast<uintptr_t>(HostAddress(view, g_address + g_bytes));
                mprotect(reinterpret_cast<void*>(start), end - start, protection);
            }
        }

        // Whether a host address is in the watched range (and its offset).
        bool InRange(uint8_t* address, uint32_t* offset)
        {
            for (uint32_t view : kViews)
            {
                uint8_t* start = HostAddress(view, g_address);
                if (address >= start && address < start + g_bytes)
                {
                    *offset = uint32_t(address - start);
                    return true;
                }
            }
            return false;
        }

        // Counts a write from a host instruction.
        void Record(uint64_t rip, uint32_t offset)
        {
            std::lock_guard lock(g_mutex);
            for (size_t i = 0; i < g_siteCount; i++)
                if (g_sites[i].rip == rip)
                {
                    g_sites[i].count++;
                    return;
                }
            if (g_siteCount < kMaxSites)
                g_sites[g_siteCount++] = { rip, 1, offset };
        }

        // A write hit a protected page: record it, unprotect the page and
        // single-step the instruction.
        void OnSegv(int, siginfo_t* info, void* context)
        {
            auto* uc = static_cast<ucontext_t*>(context);
            uint8_t* address = static_cast<uint8_t*>(info->si_addr);
            uint32_t offset;
            long pageSize = sysconf(_SC_PAGESIZE);
            uint8_t* page = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(address) & ~uintptr_t(pageSize - 1));
            // A protected page outside the range itself (the range's first
            // and last pages are shared): step over it without recording.
            bool inRange = InRange(address, &offset);
            bool onWatchedPage = inRange;
            if (!inRange)
            {
                for (uint32_t view : kViews)
                {
                    uint8_t* start = HostAddress(view, g_address);
                    uint8_t* firstPage = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(start) & ~uintptr_t(pageSize - 1));
                    if (address >= firstPage && address < start + g_bytes + pageSize)
                        onWatchedPage = true;
                }
            }
            if (!onWatchedPage)
            {
                signal(SIGSEGV, SIG_DFL);
                return;  // a real crash: re-fault with the default handler
            }
            if (inRange)
                Record(uint64_t(uc->uc_mcontext.gregs[REG_RIP]), offset);
            mprotect(page, size_t(pageSize), PROT_READ | PROT_WRITE);
            t_page = page;
            uc->uc_mcontext.gregs[REG_EFL] |= 0x100;  // trap flag: stop after one instruction
        }

        // After the single step: protect the page again.
        void OnTrap(int, siginfo_t*, void* context)
        {
            auto* uc = static_cast<ucontext_t*>(context);
            uc->uc_mcontext.gregs[REG_EFL] &= ~0x100LL;
            if (t_page)
            {
                if (g_armed.load())
                    mprotect(t_page, size_t(sysconf(_SC_PAGESIZE)), PROT_READ);
                t_page = nullptr;
            }
        }

        // Writes write_watch.txt.
        void Write()
        {
            FILE* file = fopen("write_watch.txt", "w");
            if (!file)
                return;
            fprintf(file, "# base %lx\n", static_cast<unsigned long>(reinterpret_cast<uint64_t>(&__executable_start)));
            std::lock_guard lock(g_mutex);
            for (size_t i = 0; i < g_siteCount; i++)
                fprintf(file, "%lx %lu first offset 0x%X\n",
                    static_cast<unsigned long>(g_sites[i].rip - reinterpret_cast<uint64_t>(&__executable_start)),
                    static_cast<unsigned long>(g_sites[i].count), g_sites[i].firstOffset);
            fclose(file);
            fprintf(stderr, "[watch] %zu write sites -> write_watch.txt\n", g_siteCount);
        }
    }

    // Called on every swap.
    void UpdateWriteWatch(uint32_t frame)
    {
        static const bool parsed = [] {
            const char* spec = EnvString("H3_WATCH_WRITES");
            if (!spec)
                return false;
            char* end;
            g_first = uint32_t(strtoul(spec, &end, 0));
            g_last = *end == '-' ? uint32_t(strtoul(end + 1, &end, 0)) : g_first;
            if (*end == ':' && end[1] == 'v')
            {
                g_virtual = true;
                end++;
            }
            g_address = *end == ':' || g_virtual ? uint32_t(strtoul(end + 1, &end, 16)) : 0;
            if (!g_virtual)
                g_address = GuestToPhysical(g_address);
            g_bytes = *end == ':' ? uint32_t(strtoul(end + 1, &end, 0)) : 0;
            if (!g_bytes)
                return false;
            struct sigaction action{};
            action.sa_flags = SA_SIGINFO | SA_NODEFER;
            action.sa_sigaction = OnSegv;
            sigaction(SIGSEGV, &action, nullptr);
            action.sa_sigaction = OnTrap;
            sigaction(SIGTRAP, &action, nullptr);
            return true;
        }();
        if (!parsed)
            return;
        if (frame == g_first && !g_armed.load())
        {
            g_armed = true;
            Protect(PROT_READ);
            fprintf(stderr, "[watch] armed at frame %u\n", frame);
        }
        else if (frame == g_last + 1 && g_armed.load())
        {
            g_armed = false;
            Protect(PROT_READ | PROT_WRITE);
            Write();
        }
    }
}
#else
namespace h3::debug
{
    void UpdateWriteWatch(uint32_t) {}
}
#endif
