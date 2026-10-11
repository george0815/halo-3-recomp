// xboxkrnl variable exports. Reference: Xenia's xboxkrnl_module.cc.

#include "variables.h"
#include "imports.h"
#include "modules.h"

#include "../memory.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace h3::kernel
{
    namespace
    {
        // xboxkrnl.exe ordinals, from XenonUtils/xbox/xboxkrnl_table.inc.
        enum : uint16_t
        {
            kOrdExThreadObjectType = 0x01B,
            kOrdKeTimeStampBundle = 0x0AD,
            kOrdXboxKrnlVersion = 0x158,
            kOrdXexExecutableModuleHandle = 0x193,
            kOrdExLoadedCommandLine = 0x1AE,
            kOrdVdGlobalDevice = 0x1BE,
            kOrdVdGpuClockInMHz = 0x1C0,
            kOrdVdHSIOCalibrationLock = 0x1C1,
            kOrdKeCertMonitorData = 0x266,
        };

        uint32_t g_timeStampBundle = 0;

        // X_TIME_STAMP_BUNDLE: interrupt time (100 ns since boot), system
        // time (FILETIME), tick count (ms since boot). The real kernel
        // updates it from the clock interrupt; Xenia uses a 1 ms timer.
        void UpdateTimeStampBundle(std::chrono::steady_clock::time_point boot)
        {
            constexpr uint64_t kUnixEpochAsFileTime = 116444736000000000ull;
            auto sinceBoot = std::chrono::steady_clock::now() - boot;
            auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
            GuestRef<uint64_t>(g_timeStampBundle + 0x00) =
                uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(sinceBoot).count() / 100);
            GuestRef<uint64_t>(g_timeStampBundle + 0x08) =
                kUnixEpochAsFileTime + std::chrono::duration_cast<std::chrono::nanoseconds>(sinceEpoch).count() / 100;
            GuestRef<uint32_t>(g_timeStampBundle + 0x10) =
                uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(sinceBoot).count());
        }

        // Creates KeTimeStampBundle (interrupt time, system time, tick count) and a
        // host thread that keeps it ticking every millisecond, as the console's clock
        // interrupt does.
        void StartTimeStampBundle()
        {
            g_timeStampBundle = SystemAlloc(0x18);
            auto boot = std::chrono::steady_clock::now();
            UpdateTimeStampBundle(boot);
            std::thread([boot]
            {
                for (;;)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    UpdateTimeStampBundle(boot);
                }
            }).detach();
        }

        // Returns the guest address of the variable, or 0 if it's unknown.
        uint32_t CreateVariable(uint16_t ordinal)
        {
            switch (ordinal)
            {
            case kOrdKeTimeStampBundle:
                StartTimeStampBundle();
                return g_timeStampBundle;

            case kOrdVdHSIOCalibrationLock:
            {
                uint32_t cs = SystemAlloc(0x1C);
                InitCriticalSection(cs, 0);
                return cs;
            }

            case kOrdVdGpuClockInMHz:
            {
                uint32_t clock = SystemAlloc(4);
                GuestRef<uint32_t>(clock) = 500;
                return clock;
            }

            case kOrdXboxKrnlVersion:
            {
                // X_VERSION: major, minor, build, qfe. 2.0.17559.0 is the
                // last retail kernel.
                uint32_t version = SystemAlloc(8);
                GuestRef<uint16_t>(version + 0) = 2;
                GuestRef<uint16_t>(version + 2) = 0;
                GuestRef<uint16_t>(version + 4) = 17559;
                GuestRef<uint16_t>(version + 6) = 0;
                return version;
            }

            case kOrdExLoadedCommandLine:
            {
                constexpr std::string_view kCommandLine = "\"default.xex\"";
                uint32_t commandLine = SystemAlloc(0x400);
                memcpy(GuestPtr<char>(commandLine), kCommandLine.data(), kCommandLine.size());
                return commandLine;
            }

            case kOrdXexExecutableModuleHandle:
            {
                uint32_t handle = SystemAlloc(4);
                GuestRef<uint32_t>(handle) = ExecutableModule().ldrEntry;
                return handle;
            }

            // Pointers the game only stores or compares; zero is fine.
            case kOrdExThreadObjectType:
            case kOrdVdGlobalDevice:
            case kOrdKeCertMonitorData:
                return SystemAlloc(4);
            }
            return 0;
        }
    }

    uint32_t KernelVariable(uint16_t ordinal)
    {
        static std::mutex mutex;
        static std::unordered_map<uint16_t, uint32_t> variables;

        std::lock_guard lock(mutex);
        auto it = variables.find(ordinal);
        if (it == variables.end())
            it = variables.emplace(ordinal, CreateVariable(ordinal)).first;
        return it->second;
    }
}
