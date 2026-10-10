// xam.xex network exports (NetDll_*): enough for an offline console.
// Reference: Xenia's src/xenia/kernel/xam/xam_net.cc. Every NetDll export
// takes the caller type first, so the real arguments start at r4.

#include <ppc_recomp_shared.h>

#include "../memory.h"

#include <cstring>
#include <random>

using namespace h3;

// INT NetDll_XNetRandom(DWORD caller, BYTE* buffer, UINT length)
// Halo makes its session and game ids with this; zeros don't do.
PPC_FUNC(__imp__NetDll_XNetRandom)
{
    static std::mt19937_64 generator{ std::random_device{}() };
    uint8_t* buffer = GuestPtr<uint8_t>(ctx.r4.u32);
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
        buffer[i] = uint8_t(generator());
    ctx.r3.u64 = 0;
}

// DWORD NetDll_XNetGetTitleXnAddr(DWORD caller, XNADDR* address)
// XNADDR (36 bytes): IN_ADDR ina, IN_ADDR inaOnline, WORD wPortOnline,
// BYTE abEnet[6], BYTE abOnline[20]. A static address on a local network.
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    uint32_t address = ctx.r4.u32;
    if (address != 0)
    {
        uint8_t* out = GuestPtr<uint8_t>(address);
        memset(out, 0, 36);
        const uint8_t ina[4] = { 127, 0, 0, 1 };
        const uint8_t mac[6] = { 0x00, 0x22, 0x48, 0x33, 0x00, 0x01 };
        memcpy(out, ina, 4);
        memcpy(out + 10, mac, 6);
    }
    constexpr uint32_t kXnAddrEthernet = 0x02, kXnAddrStatic = 0x04;
    ctx.r3.u64 = kXnAddrEthernet | kXnAddrStatic;
}

// DWORD NetDll_XNetGetEthernetLinkStatus(DWORD caller)
// No cable: the game stays offline.
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus)
{
    ctx.r3.u64 = 0;
}
