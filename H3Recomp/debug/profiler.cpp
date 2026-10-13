// Sampling profiler for finding hot spots without perf:
// H3_PROFILE=<first frame>-<last frame> samples the instruction pointer of
// whichever thread is using CPU, 1000 times per CPU second (SIGPROF), and
// writes profile.txt: the executable mappings ("# " lines), then one
// "<thread id> <address> [<return address>...]" line per sample (return
// addresses with H3_PROFILE_STACKS=1). tools/h3profile.py turns that into a
// per-function report.
//
// H3_PROFILE_WALL=1 instead samples only the command processor thread, 1000
// times per wall-clock second whether it runs or not, so time it spends
// blocked (in the GPU driver, on locks) shows up as samples in the blocking
// call.

#include <algorithm>
#include "config/env.h"
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/syscall.h>
#include <ctime>
#include <sys/time.h>
#include <ucontext.h>
#include <unwind.h>
#include <unistd.h>

extern "C" char __executable_start;

namespace h3::debug
{
    namespace
    {
        constexpr uint32_t kMaxSamples = 1u << 19;
        constexpr uint32_t kMaxFrames = 16;
        struct Sample
        {
            uint32_t thread;
            uint32_t frames;
            uint64_t address;
            uint64_t stack[kMaxFrames];  // with H3_PROFILE_STACKS: return addresses, from the signal handler out
        };
        Sample* g_samples = nullptr;
        std::atomic<uint32_t> g_count{ 0 };
        std::atomic<bool> g_running{ false };
        bool g_done = false;
        bool g_stacks = false;
        std::atomic<uint32_t> g_wallThread{ 0 };  // H3_PROFILE_WALL: the thread to sample
        timer_t g_wallTimer{};

        // Unwinder callback: records one return address of the sample.
        _Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* argument)
        {
            Sample& sample = *static_cast<Sample*>(argument);
            if (sample.frames >= kMaxFrames)
                return _URC_END_OF_STACK;
            sample.stack[sample.frames++] = _Unwind_GetIP(context);
            return _URC_NO_REASON;
        }

        // SIGPROF handler: takes one sample (thread, instruction pointer, stack).
        void OnSignal(int, siginfo_t*, void* context)
        {
            if (!g_running.load(std::memory_order_relaxed))
                return;
            uint32_t index = g_count.fetch_add(1, std::memory_order_relaxed);
            if (index >= kMaxSamples)
                return;
            auto* uc = static_cast<ucontext_t*>(context);
            Sample& sample = g_samples[index];
            sample.thread = uint32_t(syscall(SYS_gettid));
            sample.address = uint64_t(uc->uc_mcontext.gregs[REG_RIP]);
            sample.frames = 0;
            // Not async-signal-safe in theory (the unwinder can take the
            // loader lock); fine for a debugging aid.
            if (g_stacks)
                _Unwind_Backtrace(CollectFrame, &sample);
        }

        // Starts or stops the profiling timer: CPU time for the whole
        // process, or wall time aimed at one thread (H3_PROFILE_WALL).
        void SetTimer(bool on)
        {
            constexpr long kIntervalNs = 1'000'000;
            static const bool wall = EnvFlag("H3_PROFILE_WALL");
            if (!wall)
            {
                itimerval timer{};
                if (on)
                    timer.it_interval.tv_usec = timer.it_value.tv_usec = kIntervalNs / 1000;
                setitimer(ITIMER_PROF, &timer, nullptr);
                return;
            }
            if (on)
            {
                sigevent event{};
                event.sigev_notify = SIGEV_THREAD_ID;
                event.sigev_signo = SIGPROF;
                event._sigev_un._tid = int(g_wallThread.load());
                if (timer_create(CLOCK_MONOTONIC, &event, &g_wallTimer) != 0)
                {
                    fprintf(stderr, "[profile] no wall-clock timer for thread %u\n", g_wallThread.load());
                    return;
                }
                itimerspec spec{};
                spec.it_interval.tv_nsec = spec.it_value.tv_nsec = kIntervalNs;
                timer_settime(g_wallTimer, 0, &spec, nullptr);
            }
            else
                timer_delete(g_wallTimer);
        }
    }

    // The thread H3_PROFILE_WALL samples (the command processor registers
    // itself).
    void SetWallProfileThread()
    {
        g_wallThread = uint32_t(syscall(SYS_gettid));
    }

    // Called once per presented frame.
    void UpdateProfiler(uint32_t frame)
    {
        static const char* setting = EnvString("H3_PROFILE");
        if (!setting || g_done)
            return;
        static uint32_t first = 0, last = 0;
        static bool parsed = sscanf(setting, "%u-%u", &first, &last) == 2;
        if (!parsed)
            return;
        if (frame >= first && !g_running.load() && frame <= last)
        {
            g_samples = static_cast<Sample*>(calloc(kMaxSamples, sizeof(Sample)));
            g_stacks = EnvFlag("H3_PROFILE_STACKS");
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
                {
                    const Sample& sample = g_samples[i];
                    fprintf(file, "%u %lx", sample.thread, static_cast<unsigned long>(sample.address));
                    for (uint32_t f = 0; f < sample.frames; f++)
                        fprintf(file, " %lx", static_cast<unsigned long>(sample.stack[f]));
                    fprintf(file, "\n");
                }
                fclose(file);
            }
            fprintf(stderr, "[profile] %u samples written to profile.txt (frames %u-%u)\n", count, first, last);
        }
    }
}
