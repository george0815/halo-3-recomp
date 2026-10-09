#pragma once

#include <cstdint>

namespace h3::kernel
{
    // What the kernel needs to know about the running XEX, read from its
    // optional headers at load time.
    struct ModuleInfo
    {
        uint32_t systemFlags = 0;
        uint32_t stackSize = 0;

        // XEX_HEADER_TLS_INFO
        uint32_t tlsSlotCount = 64;
        uint32_t tlsRawDataAddress = 0;
        uint32_t tlsDataSize = 0;
        uint32_t tlsRawDataSize = 0;
    };

    extern ModuleInfo g_module;

    // xex points at the start of the XEX file (its headers).
    void LoadModuleInfo(const uint8_t* xex);
}
