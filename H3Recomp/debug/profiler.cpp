// Sampling profiler for finding hot spots without perf:
// H3_PROFILE=<first frame>-<last frame> samples the instruction pointer of
// whichever thread is using CPU, 1000 times per CPU second (SIGPROF), and
// writes profile.txt: the executable mappings ("# " lines), then one
// "<thread id> <address>" line per sample. tools/h3profile.py turns that into
// a per-function report.

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" char __executable_start;

namespace h3::debug
{
    namespace
    {
        constexpr uint32_t kMaxSamples = 1u << 22;
        struct Sample
        {
            uint32_t thread;
            uint64_t address;
        };
        Sample* g_samples = nullptr;
        std::atomic<uint32_t> g_count{ 0 };
        std::atomic<bool> g_running{ false };
        bool g_done = false;

        void OnSignal(int, siginfo_t*, void* context)
        {
            if (!g_running.load(std::memory_order_relaxed))
                return;
            uint32_t index = g_count.fetch_add(1, std::memory_order_relaxed);
            if (index >= kMaxSamples)
                return;
            auto* uc = static_cast<ucontext_t*>(context);
            g_samples[index] = { uint32_t(syscall(SYS_gettid)), uint64_t(uc->uc_mcontext.gregs[REG_RIP]) };
        }

        void SetTimer(bool on)
        {
            itimerval timer{};
            if (on)
                timer.it_interval.tv_usec = timer.it_value.tv_usec = 1000;
            setitimer(ITIMER_PROF, &timer, nullptr);
        }
    }

    // Called once per presented frame.
    void UpdateProfiler(uint32_t frame)
    {
        static const char* setting = getenv("H3_PROFILE");
        if (!setting || g_done)
            return;
        static uint32_t first = 0, last = 0;
        static bool parsed = sscanf(setting, "%u-%u", &first, &last) == 2;
        if (!parsed)
            return;
        if (frame >= first && !g_running.load() && frame <= last)
        {
            g_samples = static_cast<Sample*>(calloc(kMaxSamples, sizeof(Sample)));
            struct sigaction action{};
            action.sa_sigaction = OnSignal;
            action.sa_flags = SA_SIGINFO | SA_RESTART;
            sigaction(SIGPROF, &action, nullptr);
            g_running.store(true);
            SetTimer(true);
            fprintf(stderr, "[profile] started at frame %u\n", frame);
        }
        else if (frame > last && g_running.load())
        {
            SetTimer(false);
            g_running.store(false);
            g_done = true;
            uint32_t count = std::min(g_count.load(), kMaxSamples);
            uint64_t base = reinterpret_cast<uint64_t>(&__executable_start);
            if (FILE* file = fopen("profile.txt", "w"))
            {
                // Executable mappings, so samples in shared libraries (the
                // GPU driver, libc) can be attributed too.
                fprintf(file, "# base %lx\n", static_cast<unsigned long>(base));
                if (FILE* maps = fopen("/proc/self/maps", "r"))
                {
                    char line[512];
                    while (fgets(line, sizeof(line), maps))
                        if (strstr(line, " r-xp ") || strstr(line, " r-xs "))
                            fprintf(file, "# map %s", line);
                    fclose(maps);
                }
                for (uint32_t i = 0; i < count; i++)
                    fprintf(file, "%u %lx\n", g_samples[i].thread, static_cast<unsigned long>(g_samples[i].address));
                fclose(file);
            }
            fprintf(stderr, "[profile] %u samples written to profile.txt (frames %u-%u)\n", count, first, last);
        }
    }
}
