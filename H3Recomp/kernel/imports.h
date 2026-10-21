// Shared helpers for kernel export implementations: the fallback for
// unimplemented imports, argument access beyond r10, XOVERLAPPED
// completion, and the NTSTATUS codes exports return.

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

    // Finishes an XOVERLAPPED (result, length, extended error; signals its
    // event). Returns ERROR_IO_PENDING, what an async call returns for it,
    // or `result` itself when there's no XOVERLAPPED.
    uint32_t CompleteOverlapped(uint32_t overlapped, uint32_t result, uint32_t length = 0);

    // CompleteOverlapped with a separate extended error, for calls that
    // report failure XAM's way: result ERROR_FUNCTION_FAILED and the cause
    // as an HRESULT in dwExtendedError (XGetOverlappedExtendedError). The
    // game checks for exactly that pair (e.g. an empty XamEnumerate:
    // 1627 + 0x80070012, sub_82580B30).
    uint32_t CompleteOverlappedEx(uint32_t overlapped, uint32_t result, uint32_t extendedError, uint32_t length = 0);

    // HRESULT_FROM_WIN32: a Win32 error code as a failure HRESULT.
    constexpr uint32_t HresultFromWin32(uint32_t error) { return error == 0 ? 0 : 0x80070000 | (error & 0xFFFF); }

    // The local profile signed in on user 0 (xam_user.cpp); offline XUIDs
    // start with 0xE.
    constexpr uint64_t kLocalUserXuid = 0xE000000048330001ull;

    // NTSTATUS values returned to the guest.
    constexpr uint32_t kStatusSuccess = 0x00000000;
    constexpr uint32_t kStatusUnsuccessful = 0xC0000001;
    constexpr uint32_t kStatusNotImplemented = 0xC0000002;
    constexpr uint32_t kStatusInvalidParameter = 0xC000000D;
    constexpr uint32_t kStatusNoMemory = 0xC0000017;
    constexpr uint32_t kStatusAccessDenied = 0xC0000022;
    constexpr uint32_t kStatusMemoryNotAllocated = 0xC00000A0;
}
