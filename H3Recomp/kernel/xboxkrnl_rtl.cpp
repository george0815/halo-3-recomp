// xboxkrnl Rtl exports: strings, memory helpers, XEX headers. Reference:
// Xenia's xboxkrnl_rtl.cc.

#include <ppc_recomp_shared.h>

#include "../memory.h"

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
