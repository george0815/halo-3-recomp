// xboxkrnl Rtl exports: strings, memory helpers, XEX headers. Reference:
// Xenia's xboxkrnl_rtl.cc.

#include <ppc_recomp_shared.h>

#include "../memory.h"
#include "objects.h"
#include "thread.h"

#include <cstring>

using namespace h3;

// ANSI_STRING / UNICODE_STRING: USHORT Length (bytes, no terminator),
// USHORT MaximumLength, PCHAR/PWCHAR Buffer.

// void RtlInitAnsiString(PANSI_STRING dest, PCSZ source)
PPC_FUNC(__imp__RtlInitAnsiString)
{
    uint32_t dest = ctx.r3.u32;
    uint32_t source = ctx.r4.u32;
    uint16_t length = source ? uint16_t(strlen(GuestPtr<const char>(source))) : 0;
    GuestRef<uint16_t>(dest + 0) = length;
    GuestRef<uint16_t>(dest + 2) = source ? length + 1 : 0;
    GuestRef<uint32_t>(dest + 4) = source;
}

// void RtlInitUnicodeString(PUNICODE_STRING dest, PCWSTR source)
PPC_FUNC(__imp__RtlInitUnicodeString)
{
    uint32_t dest = ctx.r3.u32;
    uint32_t source = ctx.r4.u32;
    uint16_t chars = 0;
    if (source)
    {
        while (GuestRef<uint16_t>(source + chars * 2) != 0)
            chars++;
    }
    GuestRef<uint16_t>(dest + 0) = chars * 2;
    GuestRef<uint16_t>(dest + 2) = source ? (chars + 1) * 2 : 0;
    GuestRef<uint32_t>(dest + 4) = source;
}

// SIZE_T RtlCompareMemory(PVOID a, PVOID b, SIZE_T length)
// Returns how many leading bytes match.
PPC_FUNC(__imp__RtlCompareMemory)
{
    const uint8_t* a = GuestPtr<const uint8_t>(ctx.r3.u32);
    const uint8_t* b = GuestPtr<const uint8_t>(ctx.r4.u32);
    uint32_t length = ctx.r5.u32;
    uint32_t matched = 0;
    while (matched < length && a[matched] == b[matched])
        matched++;
    ctx.r3.u64 = matched;
}

// SIZE_T RtlCompareMemoryUlong(PVOID source, SIZE_T length, ULONG pattern)
// Returns how many leading bytes, in whole ULONGs, equal the pattern.
PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    uint32_t source = ctx.r3.u32;
    uint32_t length = ctx.r4.u32 & ~3u;
    uint32_t pattern = ctx.r5.u32;
    uint32_t matched = 0;
    while (matched < length && GuestRef<uint32_t>(source + matched) == pattern)
        matched += 4;
    ctx.r3.u64 = matched;
}

// void RtlFillMemoryUlong(PVOID dest, SIZE_T length, ULONG pattern)
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    uint32_t dest = ctx.r3.u32;
    uint32_t length = ctx.r4.u32 & ~3u;
    for (uint32_t offset = 0; offset < length; offset += 4)
        GuestRef<uint32_t>(dest + offset) = ctx.r5.u32;
}

// PVOID RtlImageXexHeaderField(PVOID xexHeader, DWORD key)
// Optional header lookup on a guest copy of XEX headers (the LDR entry's
// xex_header_base). Keys ending in 0x00 hold their value inline, 0x01 means
// the value field itself is the data, anything else is an offset from the
// header start. Returns 0 if the key isn't there.
PPC_FUNC(__imp__RtlImageXexHeaderField)
{
    uint32_t header = ctx.r3.u32;
    uint32_t key = ctx.r4.u32;
    uint32_t count = GuestRef<uint32_t>(header + 0x14);
    ctx.r3.u64 = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t entry = header + 0x18 + i * 8;
        if (GuestRef<uint32_t>(entry) != key)
            continue;
        uint32_t value = GuestRef<uint32_t>(entry + 4);
        switch (key & 0xFF)
        {
        case 0x00: ctx.r3.u64 = value; break;
        case 0x01: ctx.r3.u64 = entry + 4; break;
        default: ctx.r3.u64 = header + value; break;
        }
        return;
    }
}

// --- Time --------------------------------------------------------------------
//
// LARGE_INTEGER times count 100 ns since 1601-01-01. TIME_FIELDS is eight
// SHORTs: Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday
// (0 = Sunday).

namespace
{
    constexpr int64_t kTicksPerMillisecond = 10000;
    constexpr int64_t kTicksPerDay = 86400000ll * kTicksPerMillisecond;
    // Days from 1601-01-01 to 1970-01-01.
    constexpr int64_t kDaysTo1970 = 134774;

    // Howard Hinnant's civil_from_days / days_from_civil (days since 1970).
    void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d)
    {
        z += 719468;
        int64_t era = (z >= 0 ? z : z - 146096) / 146097;
        unsigned doe = unsigned(z - era * 146097);
        unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        y = int64_t(yoe) + era * 400;
        unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        unsigned mp = (5 * doy + 2) / 153;
        d = doy - (153 * mp + 2) / 5 + 1;
        m = mp < 10 ? mp + 3 : mp - 9;
        y += m <= 2;
    }

    // Days since 1970-01-01 for a Gregorian date (Howard Hinnant's algorithm).
    int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
    {
        y -= m <= 2;
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = unsigned(y - era * 400);
        unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + int64_t(doe) - 719468;
    }
}

// void RtlTimeToTimeFields(PLARGE_INTEGER time, PTIME_FIELDS fields)
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    int64_t ticks = int64_t(uint64_t(GuestRef<uint64_t>(ctx.r3.u32)));
    uint32_t fields = ctx.r4.u32;
    int64_t days = ticks / kTicksPerDay;
    int64_t rest = ticks % kTicksPerDay / kTicksPerMillisecond;
    int64_t year;
    unsigned month, day;
    CivilFromDays(days - kDaysTo1970, year, month, day);
    GuestRef<uint16_t>(fields + 0) = uint16_t(year);
    GuestRef<uint16_t>(fields + 2) = uint16_t(month);
    GuestRef<uint16_t>(fields + 4) = uint16_t(day);
    GuestRef<uint16_t>(fields + 6) = uint16_t(rest / 3600000);
    GuestRef<uint16_t>(fields + 8) = uint16_t(rest / 60000 % 60);
    GuestRef<uint16_t>(fields + 10) = uint16_t(rest / 1000 % 60);
    GuestRef<uint16_t>(fields + 12) = uint16_t(rest % 1000);
    GuestRef<uint16_t>(fields + 14) = uint16_t((days + 1) % 7);  // 1601-01-01 was a Monday
}

// BOOLEAN RtlTimeFieldsToTime(PTIME_FIELDS fields, PLARGE_INTEGER time)
PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    uint32_t fields = ctx.r3.u32;
    int64_t year = int16_t(uint16_t(GuestRef<uint16_t>(fields)));
    unsigned month = GuestRef<uint16_t>(fields + 2), day = GuestRef<uint16_t>(fields + 4);
    unsigned hour = GuestRef<uint16_t>(fields + 6), minute = GuestRef<uint16_t>(fields + 8);
    unsigned second = GuestRef<uint16_t>(fields + 10), millisecond = GuestRef<uint16_t>(fields + 12);
    if (year < 1601 || month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59 ||
        millisecond > 999)
    {
        ctx.r3.u64 = 0;
        return;
    }
    int64_t days = DaysFromCivil(year, month, day) + kDaysTo1970;
    int64_t ms = ((int64_t(hour) * 60 + minute) * 60 + second) * 1000 + millisecond;
    GuestRef<uint64_t>(ctx.r4.u32) = uint64_t(days * kTicksPerDay + ms * kTicksPerMillisecond);
    ctx.r3.u64 = 1;
}

// void RtlRaiseException(PEXCEPTION_RECORD record)
// The game raises one exception on purpose: MSVC's set-thread-name code,
// which a debugger would catch. Record its name (for stuck-thread reports);
// anything else is logged and ignored, as no handler would run anyway.
// EXCEPTION_RECORD: code +0, flags +4, record +8, address +12, parameter
// count +16, parameters from +20. Thread name parameters: type (0x1000),
// name, thread id (-1 = the calling thread), flags.
PPC_FUNC(__imp__RtlRaiseException)
{
    constexpr uint32_t kSetThreadNameCode = 0x406D1388;
    constexpr uint32_t kSetThreadNameType = 0x1000;
    constexpr uint32_t kCurrentThreadId = 0xFFFFFFFF;
    constexpr uint32_t kMaxNameLength = 64;
    uint32_t record = ctx.r3.u32;
    uint32_t code = GuestRef<uint32_t>(record + 0);
    if (code == kSetThreadNameCode && GuestRef<uint32_t>(record + 20) == kSetThreadNameType)
    {
        uint32_t nameAddress = GuestRef<uint32_t>(record + 24);
        uint32_t threadId = GuestRef<uint32_t>(record + 28);
        std::string name = nameAddress ? std::string(GuestPtr<const char>(nameAddress), strnlen(GuestPtr<const char>(nameAddress), kMaxNameLength)) : "";
        // Halo names its threads from the main thread with its own thread
        // numbers, which don't match ours: only the calling thread's name
        // can be attached (for stuck-thread reports); log the rest.
        if (threadId == kCurrentThreadId)
        {
            uint32_t kthread = h3::kernel::CurrentThread()->kthread;
            h3::kernel::SetThreadName(kthread, name);
            fprintf(stderr, "[kernel] thread %08X is \"%s\"\n", kthread, name.c_str());
        }
        else
            fprintf(stderr, "[kernel] game thread #%u is \"%s\"\n", threadId, name.c_str());
        return;
    }
    fprintf(stderr, "[kernel] RtlRaiseException code %08X at %08X (ignored)\n", code, uint32_t(GuestRef<uint32_t>(record + 12)));
}
