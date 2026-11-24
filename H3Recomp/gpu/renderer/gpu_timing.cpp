// GPU timing (H3_GPU_TIMING=<first frame>; diagnostic, slow): from that
// swap on, each draw flush runs on an otherwise idle GPU and is waited for,
// so the wait is its GPU time. Logged per flush position in the frame every
// kGpuTimingSwaps swaps, slowest first, with the labelled work between
// flushes (GpuTimedScope) and the flushes holding 2x/4x MSAA draws.

#include "gpu/renderer/internal.h"

#include "config/env.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace h3::gpu::renderer::internal
{
    namespace
    {
        constexpr uint32_t kGpuTimingSwaps = 60;
        // GPU time of one flush position in the frame (the n-th flush), summed
        // over the measuring window.
        struct FlushTiming
        {
            double seconds = 0;
            double gapSeconds = 0;  // the untracked work just before it
            uint32_t count = 0, draws = 0;
            uint32_t colorInfo = 0, depthInfo = 0;
            uint32_t msaaDraws = 0;  // draws on a 2x/4x surface (sample space)
        };
        std::vector<FlushTiming> g_flushTimings;
        uint32_t g_flushIndex = 0, g_flushTimingFrame = ~0u, g_flushTimingStart = 0;
        double g_otherSeconds = 0;  // waits for untracked work between flushes
        uint32_t g_otherWaits = 0;  // ...and how many (each costs an empty submit's time)
        double g_gapSeconds = 0;    // the wait before the current flush
        double g_msaaSeconds = 0;   // flushes with draws on 2x/4x surfaces
        std::vector<const char*> g_timedLabels;  // open GpuTimedScopes, innermost last
        std::map<std::string, double> g_labelSeconds;

        // Attributes a wait to the innermost open scope, or to untracked work.
        void AttributeWait(double seconds)
        {
            if (g_timedLabels.empty())
                g_otherSeconds += seconds, g_otherWaits += seconds > 0;
            else
                g_labelSeconds[g_timedLabels.back()] += seconds;
        }

        // Submits what's recorded so far and waits for it; returns the wait.
        double SubmitAndWait()
        {
            if (!g_commands)
                return 0;  // nothing recorded since the last submit
            g_submits++;
            auto start = std::chrono::steady_clock::now();
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(Commands());
            g_commands = nullptr;
            if (fence)
            {
                SDL_WaitForGPUFences(g_device, true, &fence, 1);
                SDL_ReleaseGPUFence(g_device, fence);
            }
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }

        // H3_GPU_TIMING: prints the measuring window's GPU time per frame:
        // draw flushes and other work (with and without the cost of an empty
        // submit and wait), MSAA flushes, each labelled scope, the 12
        // slowest flushes (by their position in the frame) and the longest
        // waits before a flush, then starts a new window.
        void ReportFlushTimings()
        {
            uint32_t frames = g_swaps - g_flushTimingStart;
            std::vector<uint32_t> order(g_flushTimings.size());
            for (uint32_t i = 0; i < order.size(); i++)
                order[i] = i;
            std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_flushTimings[a].seconds > g_flushTimings[b].seconds; });
            double total = 0;
            uint32_t flushes = 0;
            for (const FlushTiming& timing : g_flushTimings)
                total += timing.seconds, flushes += timing.count;
            // The cost of a submit and wait itself: an empty one, timed.
            static const double floor = [] {
                constexpr int kSamples = 20;
                double sum = 0;
                for (int i = 0; i < kSamples; i++)
                {
                    Commands();  // an empty command buffer
                    sum += SubmitAndWait();
                }
                return sum / kSamples;
            }();
            fprintf(stderr, "[gpu timing] frame %u: draw flushes %.2f ms/frame (%.1f flushes, empty submit %.3f ms), other work %.2f ms/frame"
                            " (without the waits' own cost: draws %.2f, other %.2f); slowest:",
                g_swaps, total * 1000 / frames, double(flushes) / frames, floor * 1000, g_otherSeconds * 1000 / frames,
                (total - flushes * floor) * 1000 / frames, (g_otherSeconds - g_otherWaits * floor) * 1000 / frames);
            g_otherSeconds = 0;
            g_otherWaits = 0;
            // Flushes with draws on 2x/4x surfaces (in sample space, their
            // pixels shade once per sample).
            fprintf(stderr, " [msaa flushes %.2f ms]", g_msaaSeconds * 1000 / frames);
            g_msaaSeconds = 0;
            for (const auto& [label, seconds] : g_labelSeconds)
                fprintf(stderr, " [%s %.2f ms]", label.c_str(), seconds * 1000 / frames);
            g_labelSeconds.clear();
            for (uint32_t i = 0; i < std::min<size_t>(order.size(), 12); i++)
            {
                const FlushTiming& t = g_flushTimings[order[i]];
                fprintf(stderr, " #%u %.2fms/%ud (c%08X d%08X msaa %u)", order[i], t.seconds * 1000 / frames, t.count ? t.draws / t.count : 0,
                    t.colorInfo, t.depthInfo, t.count ? t.msaaDraws / t.count : 0);
            }
            // The longest waits for untracked work, by the flush they precede.
            std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) { return g_flushTimings[a].gapSeconds > g_flushTimings[b].gapSeconds; });
            fprintf(stderr, "; longest gaps before:");
            for (uint32_t i = 0; i < std::min<size_t>(order.size(), 6); i++)
                fprintf(stderr, " #%u %.2fms", order[i], g_flushTimings[order[i]].gapSeconds * 1000 / frames);
            fprintf(stderr, "\n");
            g_flushTimings.clear();
            g_flushTimingStart = g_swaps;
        }
    }

    // H3_GPU_TIMING=<first frame>: on from that frame.
    bool GpuTimingActive()
    {
        static const char* setting = EnvString("H3_GPU_TIMING");
        static const uint32_t first = setting ? uint32_t(strtoul(setting, nullptr, 10)) : 0;
        return setting && g_swaps >= first;
    }

    // Before a flush: submits and times the work recorded since the last
    // one ("other work").
    void BeginTimedFlush()
    {
        if (g_flushTimingFrame != g_swaps)
        {
            g_flushTimingFrame = g_swaps;
            g_flushIndex = 0;
            if (g_swaps - g_flushTimingStart >= kGpuTimingSwaps)
                ReportFlushTimings();
        }
        g_gapSeconds = SubmitAndWait();  // everything before this flush
        AttributeWait(g_gapSeconds);
    }

    // After a flush: submits it, waits, and books its time.
    void EndTimedFlush(uint32_t draws, uint32_t msaaDraws)
    {
        double seconds = SubmitAndWait();
        if (g_flushTimings.size() <= g_flushIndex)
            g_flushTimings.resize(g_flushIndex + 1);
        FlushTiming& entry = g_flushTimings[g_flushIndex++];
        entry.seconds += seconds;
        entry.count++;
        entry.draws += draws;
        entry.colorInfo = ReadRegister(kRegColorInfo[0]);
        entry.depthInfo = ReadRegister(kRegRbDepthInfo);
        entry.msaaDraws += msaaDraws;
        if (msaaDraws)
            g_msaaSeconds += seconds;
        entry.gapSeconds += g_gapSeconds;
    }

    // Times a labelled piece of GPU work (resolves, copies, exports):
    // submits what came before so it isn't counted.
    GpuTimedScope::GpuTimedScope(const char* label)
    {
        // H3_GPU_TIMING_COARSE=1: no waits around labelled work (waiting
        // after each of hundreds of small dispatches inflates them): all
        // work between two flushes is timed together, as other work.
        // H3_GPU_TIMING_LABELS=<label>[,...]: only those labels get waits.
        static const bool coarse = EnvFlag("H3_GPU_TIMING_COARSE");
        static const char* only = EnvString("H3_GPU_TIMING_LABELS");
        if (!GpuTimingActive() || coarse)
            return;
        if (only && !strstr((std::string(",") + only + ",").c_str(), (std::string(",") + label + ",").c_str()))
            return;
        active = true;
        AttributeWait(SubmitAndWait());
        g_timedLabels.push_back(label);
    }

    // Submits and waits for the labelled work, and books its time.
    GpuTimedScope::~GpuTimedScope()
    {
        if (!active)
            return;
        AttributeWait(SubmitAndWait());
        g_timedLabels.pop_back();
    }

}
