// Runtime side of H3RecompLib/debug/first_calls.h: the first call of each
// recompiled function, in order, with the frame it happened in
// (first_calls.txt); and with H3_TRACE_WINDOW=<first>-<last> (frames),
// every function called in that window, in first-call order with call
// counts (window_calls.txt).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

std::atomic<bool> g_h3TraceWindowOpen{ false };

namespace h3::debug
{
    std::atomic<uint32_t> g_traceFrame{ 0 };  // frames presented (video/window.cpp)

    namespace
    {
        std::mutex g_windowMutex;
        std::unordered_map<const char*, uint32_t> g_windowCounts;
        std::vector<std::pair<const char*, uint32_t>> g_windowOrder;  // name, frame
        bool g_windowDumped = false;
    }

    // Called once per presented frame: opens and closes the window.
    void UpdateTraceWindow(uint32_t frame)
    {
        static const char* setting = getenv("H3_TRACE_WINDOW");
        if (!setting || g_windowDumped)
            return;
        static uint32_t first = 0, last = 0;
        static bool parsed = sscanf(setting, "%u-%u", &first, &last) == 2;
        if (!parsed)
            return;
        if (frame >= first && frame <= last)
            g_h3TraceWindowOpen.store(true);
        else if (frame > last && g_h3TraceWindowOpen.exchange(false))
        {
            std::lock_guard lock(g_windowMutex);
            if (FILE* file = fopen("window_calls.txt", "w"))
            {
                for (const auto& [name, at] : g_windowOrder)
                    fprintf(file, "%u %u %s\n", at, g_windowCounts[name], name);
                fclose(file);
            }
            g_windowDumped = true;
        }
    }
}

extern "C" void h3FirstCall(const char* name)
{
    static std::mutex mutex;
    static FILE* file = fopen("first_calls.txt", "w");
    std::lock_guard lock(mutex);
    if (file)
    {
        fprintf(file, "%u %s\n", h3::debug::g_traceFrame.load(), name);
        fflush(file);
    }
}

extern "C" void h3WindowCall(const char* name)
{
    using namespace h3::debug;
    std::lock_guard lock(g_windowMutex);
    if (g_windowCounts[name]++ == 0)
        g_windowOrder.emplace_back(name, g_traceFrame.load());
}
