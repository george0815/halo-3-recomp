#pragma once

#include <cstdint>

struct PPCContext;

namespace h3::kernel
{
    // Fallback for every import without a real implementation yet. The
    // generated weak stubs (H3RecompLib/ppc/import_stubs.cpp) call this.
    // Logs the import once and returns 0 in r3, or aborts if the
    // H3_STRICT_IMPORTS environment variable is set.
    void UnimplementedImport(const char* name, PPCContext& ctx);

    // Generated stubs for imports from another title module (DLL-to-DLL)
    // call this: it calls export `ordinal` of the loaded `library`.
    // Implemented in modules.cpp.
    void CallModuleImport(const char* library, uint32_t ordinal, PPCContext& ctx, uint8_t* base);

    // Logs a guest request to stop (bug check, return to firmware) and exits.
    [[noreturn]] void GuestFatal(const char* what, PPCContext& ctx);

    // Initialises a guest RTL_CRITICAL_SECTION (RtlInitializeCriticalSection).
    void InitCriticalSection(uint32_t cs, uint32_t spinCount);

    // Argument `index` (0-based) of an export. The first eight are in r3-r10;
    // the rest are in the caller's parameter save area, one 8-byte slot each
    // starting at r1 + 0x50, with the 32-bit value in the low half.
    uint32_t StackArgument(const PPCContext& ctx, uint32_t index);

    // NTSTATUS values returned to the guest.
    constexpr uint32_t kStatusSuccess = 0x00000000;
    constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
    constexpr uint32_t kStatusNotImplemented = 0xC0000002;
    constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
    constexpr uint32_t kStatusNoMemory = 0xC0000017;
    constexpr uint32_t kStatusAccessDenied = 0xC0000022;
    constexpr uint32_t kStatusMemoryNotAllocated = 0xC00000A0;
}
