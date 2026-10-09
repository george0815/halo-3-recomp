// xboxkrnl module exports (XexLoadImage and friends). Reference: Xenia's
// xboxkrnl_modules.cc.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "modules.h"
#include "../memory.h"

#include <xex.h>

#include <cstdio>
#include <cstring>

using namespace h3;
using namespace h3::kernel;

namespace
{
    constexpr uint32_t kStatusInvalidHandle = 0xC0000008;
    constexpr uint32_t kStatusNoSuchFile = 0xC000000F;
    constexpr uint32_t kStatusNotFound = 0xC0000225;
    constexpr uint32_t kStatusOrdinalNotFound = 0xC0000262;
    constexpr uint32_t kErrorNotFound = 0x00000490;

    // A null handle means the executable.
    const Module* ModuleOrExecutable(uint32_t handle)
    {
        return handle == 0 ? &ExecutableModule() : ModuleFromHandle(handle);
    }
}

// NTSTATUS XexLoadImage(LPCSTR name, DWORD flags, DWORD minVersion, PHANDLE handle)
PPC_FUNC(__imp__XexLoadImage)
{
    const char* name = GuestPtr<const char>(ctx.r3.u32);
    uint32_t handlePtr = ctx.r6.u32;

    Module* module = LoadModule(name, ctx);
    if (handlePtr != 0)
        GuestRef<uint32_t>(handlePtr) = module != nullptr ? module->ldrEntry : 0;
    ctx.r3.u64 = module != nullptr ? kStatusSuccess : kStatusNoSuchFile;
}

// NTSTATUS XexUnloadImage(HANDLE handle)
// Modules stay mapped: their code is linked into the executable anyway, and
// a reload then finds them in place.
PPC_FUNC(__imp__XexUnloadImage)
{
    Module* module = ModuleFromHandle(ctx.r3.u32);
    if (module == nullptr)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }
    if (module->loadCount > 0)
        module->loadCount--;
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS XexGetProcedureAddress(HANDLE handle, DWORD ordinal, PVOID* address)
PPC_FUNC(__imp__XexGetProcedureAddress)
{
    uint32_t ordinal = ctx.r4.u32;
    uint32_t addressPtr = ctx.r5.u32;
    if (addressPtr != 0)
        GuestRef<uint32_t>(addressPtr) = 0;

    const Module* module = ModuleOrExecutable(ctx.r3.u32);
    if (module == nullptr)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    // Values above 16 bits are pointers to export names, which XEX modules
    // don't have.
    uint32_t address = ordinal <= 0xFFFF ? GetProcAddress(*module, ordinal) : 0;
    if (address == 0)
    {
        fprintf(stderr, "[loader] %s has no export %u\n", module->name.c_str(), ordinal);
        ctx.r3.u64 = kStatusOrdinalNotFound;
        return;
    }
    if (addressPtr != 0)
        GuestRef<uint32_t>(addressPtr) = address;
    ctx.r3.u64 = kStatusSuccess;
}

// DWORD XexGetModuleHandle(LPCSTR name, PHANDLE handle)
// A null name means the executable.
PPC_FUNC(__imp__XexGetModuleHandle)
{
    uint32_t namePtr = ctx.r3.u32;
    uint32_t handlePtr = ctx.r4.u32;

    const Module* module = namePtr == 0 ? &ExecutableModule() : FindModule(GuestPtr<const char>(namePtr));
    if (module == nullptr)
        fprintf(stderr, "[loader] XexGetModuleHandle: %s isn't loaded\n", GuestPtr<const char>(namePtr));
    if (handlePtr != 0)
        GuestRef<uint32_t>(handlePtr) = module != nullptr ? module->ldrEntry : 0;
    ctx.r3.u64 = module != nullptr ? kStatusSuccess : kErrorNotFound;
}

// NTSTATUS XexGetModuleSection(HANDLE handle, LPCSTR name, PVOID* data, PDWORD size)
// Sections are the XEX resources (XEX_HEADER_RESOURCE_INFO).
PPC_FUNC(__imp__XexGetModuleSection)
{
    const Module* module = ModuleOrExecutable(ctx.r3.u32);
    const char* name = GuestPtr<const char>(ctx.r4.u32);
    uint32_t dataPtr = ctx.r5.u32;
    uint32_t sizePtr = ctx.r6.u32;
    if (module == nullptr)
    {
        ctx.r3.u64 = kStatusInvalidHandle;
        return;
    }

    if (auto* info = static_cast<const be<uint32_t>*>(getOptHeaderPtr(module->headers.data(), XEX_HEADER_RESOURCE_INFO)))
    {
        // A size, then 16-byte entries: char name[8], address, size.
        auto* entries = reinterpret_cast<const uint8_t*>(info + 1);
        uint32_t count = (info->get() - 4) / 16;
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* entry = entries + i * 16;
            if (strncmp(reinterpret_cast<const char*>(entry), name, 8) != 0)
                continue;
            if (dataPtr != 0)
                GuestRef<uint32_t>(dataPtr) = reinterpret_cast<const be<uint32_t>*>(entry + 8)->get();
            if (sizePtr != 0)
                GuestRef<uint32_t>(sizePtr) = reinterpret_cast<const be<uint32_t>*>(entry + 12)->get();
            ctx.r3.u64 = kStatusSuccess;
            return;
        }
    }

    fprintf(stderr, "[loader] %s has no section %.8s\n", module->name.c_str(), name);
    ctx.r3.u64 = kStatusNotFound;
}
