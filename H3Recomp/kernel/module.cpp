#include "module.h"

#include <xex.h>

namespace h3::kernel
{
    ModuleInfo g_module;

    void LoadModuleInfo(const uint8_t* xex)
    {
        // Keys ending in 0x00 hold their value inline; others point at data.
        if (auto* flags = static_cast<const be<uint32_t>*>(getOptHeaderPtr(xex, XEX_HEADER_SYSTEM_FLAGS)))
            g_module.systemFlags = *flags;

        if (auto* stack = static_cast<const be<uint32_t>*>(getOptHeaderPtr(xex, XEX_HEADER_DEFAULT_STACK_SIZE)))
            g_module.stackSize = *stack;

        if (auto* tls = static_cast<const be<uint32_t>*>(getOptHeaderPtr(xex, XEX_HEADER_TLS_INFO)))
        {
            g_module.tlsSlotCount = tls[0];
            g_module.tlsRawDataAddress = tls[1];
            g_module.tlsDataSize = tls[2];
            g_module.tlsRawDataSize = tls[3];
        }
    }
}
