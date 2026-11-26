// Switches every decompiled replacement honors (docs/ROADMAP.md 6.0,
// replacement registry):
//
//   H3_ORIGINAL=<name>[,<name>...]  run the recompiled originals instead
//                                   (* for all of them: what a call
//                                   recording run uses)
//   H3_CHECK_NATIVE=1               run both and report differences (the
//                                   original's result is what the game gets)

#pragma once

#include "config/env.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace h3::native
{
    // Whether H3_ORIGINAL names this replacement.
    inline bool UseOriginal(const char* name)
    {
        static const std::string list = [] {
            const char* value = EnvString("H3_ORIGINAL");
            return value ? "," + std::string(value) + "," : std::string();
        }();
        return list == ",*," || list.find("," + std::string(name) + ",") != std::string::npos;
    }

    // H3_CHECK_NATIVE=1: run each native replacement and the original side
    // by side and log where they differ (tools/films/check_native.sh).
    inline bool CheckNative()
    {
        static const bool check = EnvFlag("H3_CHECK_NATIVE");
        return check;
    }

    // H3_CHECK_NATIVE bookkeeping for one replacement: counts checked calls
    // (reporting at powers of ten, as a sign of life) and reports the first
    // differences.
    class NativeCheck
    {
    public:
        explicit NativeCheck(const char* name) : m_name(name)
        {
            std::lock_guard lock(ListMutex());
            List().push_back(this);
        }

        // Every NativeCheck constructed so far (they're function statics, so
        // a replacement shows up once its first call ran), for totals.
        static std::vector<NativeCheck*>& List()
        {
            static std::vector<NativeCheck*> list;
            return list;
        }
        static std::mutex& ListMutex()
        {
            static std::mutex mutex;
            return mutex;
        }
        const char* Name() const { return m_name; }
        uint32_t Calls() const { return m_calls.load(); }
        uint32_t Differed() const { return m_reports.load(); }

        // Returns whether native and original agree; logs if they don't.
        template <typename Describe>
        bool Compare(bool same, Describe&& describe)
        {
            uint32_t n = ++m_calls;
            if (!same && m_reports++ < kMaxReports)
                fprintf(stderr, "[native] %s: %s\n", m_name, describe().c_str());
            for (uint32_t p = 1; p <= 10000000; p *= 10)
                if (n == p)
                    fprintf(stderr, "[native] %s: %u calls checked, %u differed\n", m_name, n, m_reports.load());
            return same;
        }

    private:
        static constexpr uint32_t kMaxReports = 20;
        const char* m_name;
        std::atomic<uint32_t> m_calls{ 0 }, m_reports{ 0 };
    };
}
