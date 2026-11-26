// Function-level call recording and replay (call_record.h).

#include "debug/call_record.h"

#include "config/env.h"
#include "memory.h"

#include <ppc_recomp_shared.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

// The recording prologue's switch and entry (H3RecompLib/debug/record_calls.h);
// defined in every build so the option only changes H3RecompLib.
std::atomic<bool> g_h3RecordCallsOpen{ false };

#if defined(__linux__)

#include "replace.h"
#include "tags/tag_cache.h"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace h3::debug
{
    namespace
    {
        // One recorded call: the function and the registers a function can
        // take arguments in or read on entry (r1 stack, r13 thread block).
        // Vector registers aren't kept: replacements so far take none.
        struct CallRecord
        {
            uint32_t address;
            uint32_t tick;
            uint64_t r[14];  // r0-r13
            uint64_t f[14];  // f0-f13 (bit patterns)
        };
        static_assert(sizeof(CallRecord) == 232);

        constexpr uint32_t kMaxCallsPerFunction = 64;
        constexpr char kMemoryMagic[] = "H3CALLS1";

        // The registers' order in CallRecord.
#define H3_RECORD_GPRS(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13)

        void Capture(const PPCContext& ctx, CallRecord& record)
        {
#define H3_CAPTURE(n) record.r[n] = ctx.r##n.u64; record.f[n] = ctx.f##n.u64;
            H3_RECORD_GPRS(H3_CAPTURE)
#undef H3_CAPTURE
        }

        void Restore(const CallRecord& record, PPCContext& ctx)
        {
#define H3_RESTORE(n) ctx.r##n.u64 = record.r[n]; ctx.f##n.u64 = record.f[n];
            H3_RECORD_GPRS(H3_RESTORE)
#undef H3_RESTORE
        }

        // What makes two calls the same for recording: the argument
        // registers and the thread block, not the stack pointer.
        uint64_t CallKey(const CallRecord& record)
        {
            uint64_t hash = 0xCBF29CE484222325ull ^ record.address;
            auto mix = [&](uint64_t value) { hash = (hash ^ value) * 0x100000001B3ull; };
            for (int i = 3; i <= 10; i++)
                mix(record.r[i]);
            mix(record.r[13]);
            for (int i = 1; i <= 13; i++)
                mix(record.f[i]);
            return hash;
        }

        // ---- Recording ---------------------------------------------------

        struct Recorder
        {
            std::string directory;
            std::vector<uint32_t> ticks;  // sorted
            std::mutex mutex;
            uint32_t windowTick = 0;
            uint32_t windowTicks = 1;  // H3_RECORD_WINDOW
            std::vector<CallRecord> calls;
            std::unordered_set<uint64_t> seen;
            std::unordered_map<uint32_t, uint32_t> perFunction;
        };

        Recorder* GetRecorder()
        {
            static Recorder* recorder = [] () -> Recorder* {
                const char* directory = EnvString("H3_RECORD_CALLS");
                if (!directory || !*directory)
                    return nullptr;
                auto* r = new Recorder;
                r->directory = directory;
                std::string list = EnvString("H3_RECORD_TICKS") ? EnvString("H3_RECORD_TICKS") : "";
                for (size_t start = 0; start < list.size();)
                {
                    size_t end = list.find(',', start);
                    if (end == std::string::npos)
                        end = list.size();
                    if (end > start)
                        r->ticks.push_back(uint32_t(std::stoul(list.substr(start, end - start))));
                    start = end + 1;
                }
                std::sort(r->ticks.begin(), r->ticks.end());
                if (const char* window = EnvString("H3_RECORD_WINDOW"); window && *window)
                    r->windowTicks = std::max<uint32_t>(1, uint32_t(std::stoul(window)));
                std::filesystem::create_directories(r->directory);
                fprintf(stderr, "[calls] recording %zu ticks to %s\n", r->ticks.size(), directory);
                return r;
            }();
            return recorder;
        }

        bool AllZero(const uint8_t* bytes, size_t size)
        {
            const uint64_t* words = reinterpret_cast<const uint64_t*>(bytes);
            for (size_t i = 0; i < size / 8; i++)
                if (words[i])
                    return false;
            return true;
        }

        // The executable's function lookup table (host pointers of the build
        // that wrote the snapshot): not saved, rebuilt by the replay.
        constexpr uint64_t kTableStart = PPC_IMAGE_BASE + PPC_IMAGE_SIZE;
        constexpr uint64_t kTableEnd = kTableStart + PPC_CODE_SIZE * 2;

        // Writes every non-zero 4 KiB page of guest memory below the second
        // physical view (the private range, then physical memory once
        // through its 0xA0000000 view) at its guest address in a sparse file.
        bool WriteSnapshot(const std::string& path, uint64_t& written)
        {
            int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (out < 0)
                return false;
            constexpr uint64_t kEnd = 0xC0000000ull;
            ftruncate(out, kEnd);
            int pagemap = open("/proc/self/pagemap", O_RDONLY);
            std::vector<uint64_t> entries(4096);
            written = 0;
            for (uint64_t address = 0x10000; address < kEnd; address += entries.size() * kPageSize4K)
            {
                size_t count = std::min<uint64_t>(entries.size(), (kEnd - address) / kPageSize4K);
                bool physical = address >= kPhysicalMemoryBase;
                if (!physical)
                {
                    uint64_t host = uint64_t(g_base + address) / kPageSize4K;
                    if (pread(pagemap, entries.data(), count * 8, off_t(host * 8)) != ssize_t(count * 8))
                        std::fill(entries.begin(), entries.end(), ~0ull);  // can't tell: check every page
                }
                for (size_t i = 0; i < count; i++)
                {
                    uint64_t page = address + i * kPageSize4K;
                    if (page >= kTableStart && page < kTableEnd)
                        continue;
                    // Present (bit 63) or swapped (bit 62); physical pages
                    // are shared memfd pages: just look at them.
                    if (!physical && !(entries[i] >> 62))
                        continue;
                    const uint8_t* bytes = g_base + page;
                    if (AllZero(bytes, kPageSize4K))
                        continue;
                    if (pwrite(out, bytes, kPageSize4K, off_t(page)) != ssize_t(kPageSize4K))
                    {
                        close(pagemap);
                        close(out);
                        return false;
                    }
                    written += kPageSize4K;
                }
            }
            close(pagemap);
            close(out);
            return true;
        }

        void FlushWindow(Recorder& r)
        {
            std::string path = r.directory + "/tick_" + std::to_string(r.windowTick) + ".calls";
            if (FILE* file = fopen(path.c_str(), "wb"))
            {
                fwrite(kMemoryMagic, 1, 8, file);
                fwrite(r.calls.data(), sizeof(CallRecord), r.calls.size(), file);
                fclose(file);
            }
            fprintf(stderr, "[calls] tick %u: %zu calls of %zu functions\n", r.windowTick, r.calls.size(),
                r.perFunction.size());
            r.calls.clear();
            r.seen.clear();
            r.perFunction.clear();
        }
    }

    void OnRecordTick(uint32_t levelTime)
    {
        Recorder* r = GetRecorder();
        if (!r)
            return;
        std::lock_guard lock(r->mutex);
        // A window stays open for windowTicks ticks (or until the level
        // time goes back: a revert or a new level).
        if (g_h3RecordCallsOpen.load())
        {
            if (levelTime >= r->windowTick && levelTime < r->windowTick + r->windowTicks)
                return;
            g_h3RecordCallsOpen.store(false);
            FlushWindow(*r);
        }
        if (!std::binary_search(r->ticks.begin(), r->ticks.end(), levelTime))
            return;
        auto start = std::chrono::steady_clock::now();
        uint64_t written = 0;
        std::string path = r->directory + "/tick_" + std::to_string(levelTime) + ".mem";
        if (!WriteSnapshot(path, written))
        {
            fprintf(stderr, "[calls] couldn't write %s\n", path.c_str());
            return;
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        fprintf(stderr, "[calls] tick %u: snapshot %.0f MB in %.1f s\n", levelTime, double(written) / 1e6, seconds);
        r->windowTick = levelTime;
        g_h3RecordCallsOpen.store(true);
    }
}

// The prologue's entry: the function's address (parsed from its name once),
// and its registers if this exact call isn't recorded yet.
extern "C" void h3RecordCall(uint32_t* address, const char* name, const void* context)
{
    using namespace h3::debug;
    if (*address == 0)
    {
        // __imp__sub_<address>; functions with other names (_xstart, the
        // register save helpers) get 1, which no replacement has.
        const char* sub = strstr(name, "sub_");
        uint32_t parsed = sub ? uint32_t(strtoul(sub + 4, nullptr, 16)) : 0;
        *address = parsed ? parsed : 1;
    }
    Recorder* r = GetRecorder();
    if (!r)
        return;
    CallRecord record{};
    record.address = *address;
    Capture(*static_cast<const PPCContext*>(context), record);
    uint64_t key = CallKey(record);
    std::lock_guard lock(r->mutex);
    if (!g_h3RecordCallsOpen.load(std::memory_order_relaxed))
        return;
    uint32_t& count = r->perFunction[record.address];
    if (count >= kMaxCallsPerFunction || !r->seen.insert(key).second)
        return;
    count++;
    record.tick = r->windowTick;
    r->calls.push_back(record);
}

namespace h3::debug
{
    namespace
    {
        // ---- Replay ------------------------------------------------------

        // Loads a snapshot: the guest memory it holds, the rest zero.
        bool LoadSnapshot(const std::filesystem::path& path)
        {
            int in = open(path.c_str(), O_RDONLY);
            if (in < 0)
                return false;
            off_t end = lseek(in, 0, SEEK_END);
            for (off_t data = lseek(in, 0, SEEK_DATA); data >= 0 && data < end;)
            {
                off_t hole = lseek(in, data, SEEK_HOLE);
                if (hole < 0)
                    hole = end;
                if (pread(in, g_base + data, size_t(hole - data), data) != ssize_t(hole - data))
                {
                    close(in);
                    return false;
                }
                data = lseek(in, hole, SEEK_DATA);
            }
            close(in);
            // This build's functions in the lookup table (indirect calls).
            memset(g_base + kTableStart, 0, size_t(kTableEnd - kTableStart));
            for (PPCFuncMapping* mapping = PPCFuncMappings; mapping->host; mapping++)
                PPC_LOOKUP_FUNC(g_base, mapping->guest) = mapping->host;
            return true;
        }

        std::vector<CallRecord> LoadCalls(const std::filesystem::path& path)
        {
            std::vector<CallRecord> calls;
            FILE* file = fopen(path.c_str(), "rb");
            if (!file)
                return calls;
            char magic[8];
            if (fread(magic, 1, 8, file) == 8 && memcmp(magic, kMemoryMagic, 8) == 0)
            {
                CallRecord record;
                while (fread(&record, sizeof(record), 1, file) == 1)
                    calls.push_back(record);
            }
            fclose(file);
            return calls;
        }

        PPCFunc* HostFunction(uint32_t address)
        {
            for (PPCFuncMapping* mapping = PPCFuncMappings; mapping->host; mapping++)
                if (mapping->guest == address)
                    return mapping->host;
            return nullptr;
        }

        // Totals per replacement over all snapshots.
        struct Totals
        {
            uint64_t recorded = 0;  // calls replayed
            uint64_t calls = 0, differed = 0;  // of them, answered natively (not nullopt) and compared
            bool hung = false, crashed = false;
        };

        // A pipe message from a child: calls checked and differences.
        struct ChildResult
        {
            uint32_t calls, differed;
        };
    }

    int ReplayCalls(const std::filesystem::path& directory, const std::string& only)
    {
        setenv("H3_CHECK_NATIVE", "1", 1);
        const std::vector<native::Replacement>& replacements = native::Replacements();
        auto wanted = [&](const native::Replacement& r) {
            if (only.empty())
                return true;
            char hex[16];
            snprintf(hex, sizeof(hex), "%08X", r.address);
            std::string list = "," + only + ",";
            return list.find("," + std::string(r.name) + ",") != std::string::npos ||
                   list.find("," + std::string(hex) + ",") != std::string::npos;
        };

        std::vector<std::filesystem::path> snapshots;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".mem")
                snapshots.push_back(entry.path());
        std::sort(snapshots.begin(), snapshots.end());
        if (snapshots.empty())
        {
            fprintf(stderr, "[replay] no snapshots (tick_*.mem) in %s\n", directory.string().c_str());
            return 1;
        }

        std::unordered_map<uint32_t, Totals> totals;
        auto start = std::chrono::steady_clock::now();
        for (const auto& snapshot : snapshots)
        {
            std::vector<CallRecord> calls = LoadCalls(std::filesystem::path(snapshot).replace_extension(".calls"));
            std::unordered_map<uint32_t, std::vector<const CallRecord*>> byFunction;
            for (const CallRecord& call : calls)
                byFunction[call.address].push_back(&call);
            bool any = false;
            for (const native::Replacement& r : replacements)
                any |= wanted(r) && byFunction.count(r.address);
            if (!any)
                continue;
            if (!LoadSnapshot(snapshot))
            {
                fprintf(stderr, "[replay] couldn't load %s\n", snapshot.string().c_str());
                return 1;
            }
            native::tags::OnTagsLoaded();
            for (const native::Replacement& r : replacements)
            {
                auto found = byFunction.find(r.address);
                if (!wanted(r) || found == byFunction.end())
                    continue;
                PPCFunc* function = HostFunction(r.address);
                if (!function)
                    continue;
                int fds[2];
                if (pipe(fds) != 0)
                    return 1;
                fflush(stdout);
                fflush(stderr);
                pid_t child = fork();
                if (child == 0)
                {
                    close(fds[0]);
                    alarm(30);
                    for (const CallRecord* call : found->second)
                    {
                        alignas(0x40) PPCContext ctx{};
                        Restore(*call, ctx);
                        function(ctx, g_base);
                    }
                    ChildResult result{ 0, 0 };
                    std::lock_guard lock(native::NativeCheck::ListMutex());
                    for (const native::NativeCheck* check : native::NativeCheck::List())
                        if (strcmp(check->Name(), r.name) == 0)
                        {
                            result.calls += check->Calls();
                            result.differed += check->Differed();
                        }
                    write(fds[1], &result, sizeof(result));
                    fflush(stderr);
                    _exit(0);
                }
                close(fds[1]);
                ChildResult result{ 0, 0 };
                ssize_t got = read(fds[0], &result, sizeof(result));
                close(fds[0]);
                int status = 0;
                waitpid(child, &status, 0);
                Totals& t = totals[r.address];
                t.recorded += found->second.size();
                if (got == ssize_t(sizeof(result)))
                {
                    t.calls += result.calls;
                    t.differed += result.differed;
                }
                else if (WIFSIGNALED(status) && WTERMSIG(status) == SIGALRM)
                    t.hung = true;
                else
                    t.crashed = true;
            }
        }

        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        int failed = 0;
        for (const native::Replacement& r : replacements)
        {
            if (!wanted(r))
                continue;
            auto found = totals.find(r.address);
            if (found == totals.end())
            {
                printf("  %-48s no recorded calls\n", r.name);
                continue;
            }
            const Totals& t = found->second;
            // A call the native body declines (nullopt) runs the original
            // and isn't compared: "fell back". All of them falling back
            // means the replacement never answered, so it isn't verified.
            const char* verdict = t.hung      ? "HUNG"
                : t.crashed                   ? "CRASHED"
                : t.differed                  ? "DIFFERS"
                : t.calls == 0                ? "ALL FELL BACK (unverified)"
                                              : "ok";
            printf("  %-48s %6llu calls, %llu differed  %s", r.name, (unsigned long long)t.calls,
                (unsigned long long)t.differed, verdict);
            if (t.recorded > t.calls)
                printf(" (%llu fell back)", (unsigned long long)(t.recorded - t.calls));
            printf("\n");
            failed += t.hung || t.crashed || t.differed;
        }
        printf("[replay] %zu snapshots, %.1f s; %d replacement(s) failed\n", snapshots.size(), seconds, failed);
        return failed ? 1 : 0;
    }
}

#else

namespace h3::debug
{
    void OnRecordTick(uint32_t) {}

    int ReplayCalls(const std::filesystem::path&, const std::string&)
    {
        fprintf(stderr, "[replay] call replay needs Linux\n");
        return 1;
    }
}

extern "C" void h3RecordCall(uint32_t*, const char*, const void*) {}

#endif
