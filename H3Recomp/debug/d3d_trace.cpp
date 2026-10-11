// D3D call tracing (d3d_trace.h).

#include "debug/d3d_trace.h"
#include "config/env.h"

#include <ppc_recomp_shared.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

namespace h3::d3dtrace
{
    namespace
    {
        // H3_TRACE_D3D_FRAME picks the frame the report is written at
        // (default 150); the 50 frames before it are sampled.
        uint32_t DumpFrame()
        {
            static uint32_t frame = []
            {
                const char* value = EnvString("H3_TRACE_D3D_FRAME");
                return value ? std::max(uint32_t(strtoul(value, nullptr, 0)), 51u) : 150u;
            }();
            return frame;
        }
        constexpr size_t kSamples = 4;

        struct Sample
        {
            uint32_t frame, lr, r[6];
            double f1;
            uint32_t ret;
            bool done;
        };

        struct Stats
        {
            uint64_t total = 0, window = 0;
            std::map<uint32_t, uint64_t> callers;
            std::vector<Sample> samples;
        };

        // Each thread counts into its own table, so tracing never makes one
        // guest thread wait for another (a shared lock here deadlocked the
        // game). The report merges the tables without locking; a few racy
        // counts don't matter for a debug trace.
        struct ThreadStats
        {
            std::vector<Stats> stats;
            std::vector<std::pair<size_t, int>> open;  // index, sample slot or -1
        };

        std::mutex g_registryMutex;
        std::vector<ThreadStats*> g_registry;
        std::atomic<uint32_t> g_frame{ 0 };

        // The calling thread's counters.
        ThreadStats& Current()
        {
            thread_local ThreadStats* stats = []
            {
                auto* created = new ThreadStats;  // leaked: outlives its thread for the report
                created->stats.resize(kFunctionCount);
                std::lock_guard lock(g_registryMutex);
                g_registry.push_back(created);
                return created;
            }();
            return *stats;
        }

        // Writes d3d_trace.txt: call counts and sampled arguments.
        void WriteReport(uint32_t frame)
        {
            std::vector<Stats> merged(kFunctionCount);
            {
                std::lock_guard lock(g_registryMutex);
                for (ThreadStats* thread : g_registry)
                {
                    for (size_t f = 0; f < kFunctionCount; f++)
                    {
                        const Stats& from = thread->stats[f];
                        Stats& to = merged[f];
                        to.total += from.total;
                        to.window += from.window;
                        for (auto& [lr, count] : from.callers)
                            to.callers[lr] += count;
                        for (const Sample& sample : from.samples)
                            if (to.samples.size() < kSamples)
                                to.samples.push_back(sample);
                    }
                }
            }

            FILE* out = fopen("d3d_trace.txt", "w");
            if (!out)
                return;
            constexpr uint32_t frames = 50;
            for (size_t f = 0; f < kFunctionCount; f++)
            {
                Stats& s = merged[f];
                if (s.total == 0)
                    continue;
                fprintf(out, "sub_%08X total=%llu perframe=%.2f callers=%zu\n", kFunctions[f],
                    (unsigned long long)s.total, double(s.window) / frames, s.callers.size());
                size_t n = 0;
                for (auto& [lr, count] : s.callers)
                    if (n++ < 8)
                        fprintf(out, "    lr=%08X x%llu\n", lr, (unsigned long long)count);
                for (auto& sm : s.samples)
                    fprintf(out, "    f%u lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X f1=%g -> %08X\n",
                        sm.frame, sm.lr, sm.r[0], sm.r[1], sm.r[2], sm.r[3], sm.r[4], sm.r[5], sm.f1, sm.ret);
            }
            fclose(out);
            fprintf(stderr, "[d3dtrace] wrote d3d_trace.txt at frame %u\n", frame);
        }
    }

    void Enter(size_t index, PPCContext& ctx)
    {
        ThreadStats& thread = Current();
        Stats& s = thread.stats[index];
        uint32_t frame = g_frame.load(std::memory_order_relaxed);
        bool sampling = frame >= DumpFrame() - 50;
        s.total++;
        if (sampling)
            s.window++;
        s.callers[uint32_t(ctx.lr)]++;
        int slot = -1;
        if (sampling && s.samples.size() < kSamples)
        {
            slot = int(s.samples.size());
            s.samples.push_back({ frame, uint32_t(ctx.lr),
                { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32 }, ctx.f1.f64, 0, false });
        }
        thread.open.push_back({ index, slot });
    }

    void Leave(size_t index, PPCContext& ctx)
    {
        ThreadStats& thread = Current();
        // Guest threads can leave a hooked function without returning
        // through it (thread exit, fibers), so the stack may not match.
        if (!thread.open.empty())
        {
            auto [i, slot] = thread.open.back();
            thread.open.pop_back();
            if (i == index && slot >= 0)
            {
                thread.stats[i].samples[slot].ret = ctx.r3.u32;
                thread.stats[i].samples[slot].done = true;
            }
        }

        if (index != kFunctionCount - 1)  // the last entry is Swap
            return;
        uint32_t frame = ++g_frame;
        if (frame == DumpFrame())
            WriteReport(frame);
    }
}
