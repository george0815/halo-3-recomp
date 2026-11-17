// xam.xex network exports (NetDll_*). Reference: Xenia's
// src/xenia/kernel/xam/xam_net.cc. Most NetDll exports take the caller
// type first, so the real arguments start at r4 (inet_addr, __WSAFDIsSet
// and WSAGetLastError don't).
//
// With [network] system_link off (the default) the console has no network
// cable and the game stays offline. On, the game's sockets are virtual
// sockets on the system link's virtual LAN (net/link.h): datagram sockets
// (UDP, and VDP, the 360's game-and-voice datagrams, carried as they are)
// queue what arrives for their port; each game's XNADDR holds its virtual
// IP, and the XNet security layer (keys, XNetConnect) has nothing to do.
// H3_LOG_NET=1 logs the calls.

#include <ppc_recomp_shared.h>

#include "imports.h"
#include "../config/env.h"
#include "../memory.h"
#include "../net/link.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace h3;
using namespace h3::kernel;

namespace
{
    bool LogNet()
    {
        static const bool log = EnvFlag("H3_LOG_NET");
        return log;
    }

    // The link starts the first time the game looks at the network.
    bool LinkUp()
    {
        static std::once_flag once;
        static bool up = false;
        std::call_once(once, [] { up = net::Start(); });
        return up;
    }

    // Winsock error codes (the 360 uses Windows').
    constexpr uint32_t kWsaEinval = 10022;
    constexpr uint32_t kWsaEwouldblock = 10035;
    constexpr uint32_t kWsaEnotsock = 10038;
    constexpr uint32_t kWsaEmsgsize = 10040;
    constexpr uint32_t kWsaEprotonosupport = 10043;
    constexpr uint32_t kWsaEopnotsupp = 10045;
    constexpr uint32_t kWsaEafnosupport = 10047;
    constexpr uint32_t kWsaEaddrinuse = 10048;
    constexpr uint32_t kWsaEnetdown = 10050;
    constexpr uint32_t kWsaEnotconn = 10057;
    constexpr uint32_t kSocketError = 0xFFFFFFFF;  // also INVALID_SOCKET

    thread_local uint32_t t_lastError = 0;

    // Sets WSAGetLastError and returns SOCKET_ERROR (or INVALID_SOCKET).
    uint32_t Fail(uint32_t error)
    {
        t_lastError = error;
        return kSocketError;
    }

    constexpr uint32_t kAfInet = 2;
    constexpr uint32_t kSockDgram = 2;
    constexpr uint32_t kIpprotoUdp = 17, kIpprotoVdp = 254;
    constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
    constexpr uint32_t kMaxFdSet = 64;

    // A game socket.
    struct VirtualSocket
    {
        uint32_t type = 0, protocol = 0;
        uint16_t port = 0;  // 0 until bound (or the first send picks one)
        bool nonBlocking = false;
        uint32_t connectedIp = 0;  // connect() on a datagram socket: the default peer
        uint16_t connectedPort = 0;
    };

    std::mutex g_socketsMutex;
    std::map<uint32_t, std::shared_ptr<VirtualSocket>> g_sockets;
    uint32_t g_nextSocket = 0x100;
    uint16_t g_nextEphemeral = 49152;

    std::shared_ptr<VirtualSocket> Find(uint32_t handle)
    {
        std::lock_guard lock(g_socketsMutex);
        auto it = g_sockets.find(handle);
        return it == g_sockets.end() ? nullptr : it->second;
    }

    bool PortTaken(uint16_t port)
    {
        for (const auto& [handle, s] : g_sockets)
            if (s->port == port)
                return true;
        return false;
    }

    // Binds a socket to a port (0: the next free ephemeral one).
    uint32_t BindPort(VirtualSocket& s, uint16_t port)
    {
        std::lock_guard lock(g_socketsMutex);
        if (port == 0)
        {
            do
                port = g_nextEphemeral = g_nextEphemeral == 65535 ? 49152 : g_nextEphemeral + 1;
            while (PortTaken(port));
        }
        else if (PortTaken(port))
            return Fail(kWsaEaddrinuse);
        s.port = port;
        net::Bind(port);
        return 0;
    }

    // SOCKADDR_IN in guest memory: WORD family, WORD port, IN_ADDR (all
    // big-endian, so the guest reads give host-order values).
    void ReadAddress(uint32_t address, uint32_t& ip, uint16_t& port)
    {
        port = GuestRef<uint16_t>(address + 2);
        ip = GuestRef<uint32_t>(address + 4);
    }

    void WriteAddress(uint32_t address, uint32_t sizePointer, uint32_t ip, uint16_t port)
    {
        if (address == 0)
            return;
        memset(GuestPtr<uint8_t>(address), 0, 16);
        GuestRef<uint16_t>(address) = uint16_t(kAfInet);
        GuestRef<uint16_t>(address + 2) = port;
        GuestRef<uint32_t>(address + 4) = ip;
        if (sizePointer != 0)
            GuestRef<uint32_t>(sizePointer) = 16;
    }

    // Secure addresses: XNetXnAddrToInAddr gives the game an IN_ADDR in
    // 0.0.0.0/8 for a peer, not its IP, and the game checks for that range
    // before it connects (sub_823A15A0: first byte 0). Virtual IPs are
    // 10.x.y.z, so a game's secure address is 0.x.y.z. Datagrams sent to a
    // secure address arrive from the sender's secure address, as on a 360.
    uint32_t SecureAddress(uint32_t virtualIp) { return virtualIp & 0x00FFFFFF; }
    bool IsSecureAddress(uint32_t ip) { return ip != 0 && (ip >> 24) == 0; }
    uint32_t VirtualIp(uint32_t ip) { return IsSecureAddress(ip) ? 0x0A000000u | ip : ip; }

    // XNADDR (36 bytes): IN_ADDR ina, IN_ADDR inaOnline, WORD wPortOnline,
    // BYTE abEnet[6], BYTE abOnline[20]. A game's carries its virtual IP
    // three times: in ina, in the MAC (the low three bytes; virtual IPs are
    // 10.x.y.z) and in abOnline (with a mark), the console's unique id on
    // a 360. The game may pass on only part of an XNADDR (the session's
    // host address), so XnAddrIp finds it in whichever part there is.
    constexpr uint32_t kOnlineMark = 0x4833534C;  // "H3SL"

    void WriteXnAddr(uint32_t address, uint32_t ip)
    {
        uint8_t* out = GuestPtr<uint8_t>(address);
        memset(out, 0, 36);
        GuestRef<uint32_t>(address) = ip;
        const uint8_t mac[6] = { 0x00, 0x22, 0x48, uint8_t(ip >> 16), uint8_t(ip >> 8), uint8_t(ip) };
        memcpy(out + 10, mac, 6);
        GuestRef<uint32_t>(address + 16) = ip;
        GuestRef<uint32_t>(address + 20) = kOnlineMark;
    }

    // The virtual IP an XNADDR stands for (0 if none).
    uint32_t XnAddrIp(uint32_t address)
    {
        if (uint32_t ina = GuestRef<uint32_t>(address))
            return ina;
        if (GuestRef<uint32_t>(address + 20) == kOnlineMark)
            return GuestRef<uint32_t>(address + 16);
        const uint8_t* mac = GuestPtr<uint8_t>(address + 10);
        if (mac[0] == 0x00 && mac[1] == 0x22 && mac[2] == 0x48)
            return 0x0A000000u | uint32_t(mac[3]) << 16 | uint32_t(mac[4]) << 8 | mac[5];
        return 0;
    }

    // Session keys the game registered (XNetRegisterKey): XNKIDs, 8 bytes
    // each. A game's sender is "associated" through its session's key.
    std::mutex g_keysMutex;
    std::vector<uint64_t> g_registeredKeys;

    // A datagram for a socket, waiting as the socket says (or not at all).
    bool TakeDatagram(VirtualSocket& s, net::Datagram& datagram, bool wait)
    {
        return net::Receive(s.port, datagram, wait ? -1 : 0);
    }

    // Copies a datagram into the game's buffer (truncated, as Winsock does).
    uint32_t Deliver(const net::Datagram& datagram, uint32_t buffer, uint32_t length)
    {
        uint32_t size = std::min<uint32_t>(length, uint32_t(datagram.data.size()));
        memcpy(GuestPtr<uint8_t>(buffer), datagram.data.data(), size);
        if (size < datagram.data.size())
            return Fail(kWsaEmsgsize);
        return size;
    }

    uint32_t SendDatagram(VirtualSocket& s, uint32_t buffer, uint32_t length, uint32_t ip, uint16_t port)
    {
        if (!net::Enabled())
            return Fail(kWsaEnetdown);
        if (s.port == 0 && BindPort(s, 0) == kSocketError)
            return kSocketError;
        bool secure = IsSecureAddress(ip);
        if (!net::Send(s.port, ip == 0xFFFFFFFF || (ip & 0xFF) == 0xFF ? net::kBroadcast : VirtualIp(ip), port, GuestPtr<uint8_t>(buffer),
                length, secure))
            return Fail(kWsaEmsgsize);
        return length;
    }

    // Logs a call's raw arguments the first few times (H3_LOG_NET).
    void LogCall(const char* name, const PPCContext& ctx)
    {
        if (!LogNet())
            return;
        static std::mutex mutex;
        static std::map<std::string, uint32_t> counts;
        std::lock_guard lock(mutex);
        if (counts[name]++ < 8)
            fprintf(stderr, "[net] %s(%08X, %08X, %08X, %08X, %08X, %08X, %08X) lr=%08X\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, uint32_t(ctx.lr));
    }
}

// --- XNet ----------------------------------------------------------------------

// INT NetDll_XNetRandom(DWORD caller, BYTE* buffer, UINT length)
// Halo makes its session and game ids with this; zeros don't do. It also
// places the game state at a random offset (up to 64 KB) in its memory.
// H3_RANDOM_SEED=<n> seeds the generator with n instead, so runs place
// memory alike (film replays compared word by word, docs/ROADMAP.md 6.1).
PPC_FUNC(__imp__NetDll_XNetRandom)
{
    static std::mt19937_64 generator{ EnvString("H3_RANDOM_SEED") ? EnvNumber("H3_RANDOM_SEED") : std::random_device{}() };
    uint8_t* buffer = GuestPtr<uint8_t>(ctx.r4.u32);
    for (uint32_t i = 0; i < ctx.r5.u32; i++)
        buffer[i] = uint8_t(generator());
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetStartup(DWORD caller, XNetStartupParams* params) / XNetCleanup(DWORD caller)
PPC_FUNC(__imp__NetDll_XNetStartup)
{
    LogCall("XNetStartup", ctx);
    LinkUp();
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XNetCleanup)
{
    ctx.r3.u64 = 0;
}

// DWORD NetDll_XNetGetTitleXnAddr(DWORD caller, XNADDR* address)
// Offline: a static address on a local network. With system link, the
// game's virtual IP.
PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    uint32_t address = ctx.r4.u32;
    if (address != 0)
        WriteXnAddr(address, LinkUp() ? net::LocalIp() : 0x7F000001);
    constexpr uint32_t kXnAddrEthernet = 0x02, kXnAddrStatic = 0x04;
    ctx.r3.u64 = kXnAddrEthernet | kXnAddrStatic;
}

// DWORD NetDll_XNetGetEthernetLinkStatus(DWORD caller)
// No cable (the game stays offline) unless system link is on.
PPC_FUNC(__imp__NetDll_XNetGetEthernetLinkStatus)
{
    constexpr uint32_t kLinkActive = 0x01, kLink100Mbps = 0x02, kLinkFullDuplex = 0x08;
    ctx.r3.u64 = LinkUp() ? kLinkActive | kLink100Mbps | kLinkFullDuplex : 0;
}

// INT NetDll_XNetCreateKey(DWORD caller, XNKID* kid, XNKEY* key)
// A random session id (marked system link: top nibble 0) and key.
PPC_FUNC(__imp__NetDll_XNetCreateKey)
{
    LogCall("XNetCreateKey", ctx);
    static std::mt19937 generator{ std::random_device{}() };
    if (ctx.r4.u32 != 0)
    {
        uint8_t* kid = GuestPtr<uint8_t>(ctx.r4.u32);
        for (uint32_t i = 0; i < 8; i++)
            kid[i] = uint8_t(generator());
        kid[0] &= 0x0F;
    }
    if (ctx.r5.u32 != 0)
    {
        uint8_t* key = GuestPtr<uint8_t>(ctx.r5.u32);
        for (uint32_t i = 0; i < 16; i++)
            key[i] = uint8_t(generator());
    }
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetRegisterKey(DWORD caller, XNKID*, XNKEY*) / XNetUnregisterKey(DWORD caller, XNKID*)
PPC_FUNC(__imp__NetDll_XNetRegisterKey)
{
    LogCall("XNetRegisterKey", ctx);
    if (ctx.r4.u32 != 0)
    {
        uint64_t kid = GuestRef<uint64_t>(ctx.r4.u32);
        std::lock_guard lock(g_keysMutex);
        if (std::find(g_registeredKeys.begin(), g_registeredKeys.end(), kid) == g_registeredKeys.end())
            g_registeredKeys.push_back(kid);
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XNetUnregisterKey)
{
    if (ctx.r4.u32 != 0)
    {
        uint64_t kid = GuestRef<uint64_t>(ctx.r4.u32);
        std::lock_guard lock(g_keysMutex);
        std::erase(g_registeredKeys, kid);
    }
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetXnAddrToInAddr(DWORD caller, XNADDR* xna, XNKID* kid, IN_ADDR* ina)
// The address to send to is the game's virtual IP. The game builds its
// game channel's address from this and gives up on joining (without a
// word to the host) if it's 0 (sub_823A0E60 / sub_823DD1C0).
PPC_FUNC(__imp__NetDll_XNetXnAddrToInAddr)
{
    LogCall("XNetXnAddrToInAddr", ctx);
    if (ctx.r4.u32 != 0 && ctx.r6.u32 != 0)
        GuestRef<uint32_t>(ctx.r6.u32) = SecureAddress(XnAddrIp(ctx.r4.u32));
    if (LogNet() && ctx.r4.u32 != 0)
    {
        const uint8_t* x = GuestPtr<uint8_t>(ctx.r4.u32);
        fprintf(stderr, "[net]   XNADDR ina %u.%u.%u.%u online %u.%u.%u.%u port %u mac %02X:%02X:%02X:%02X:%02X:%02X\n", x[0], x[1], x[2],
            x[3], x[4], x[5], x[6], x[7], unsigned(x[8] << 8 | x[9]), x[10], x[11], x[12], x[13], x[14], x[15]);
    }
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetServerToInAddr(DWORD caller, IN_ADDR ina, DWORD serviceId, IN_ADDR* out)
PPC_FUNC(__imp__NetDll_XNetServerToInAddr)
{
    LogCall("XNetServerToInAddr", ctx);
    if (ctx.r6.u32 != 0)
        GuestRef<uint32_t>(ctx.r6.u32) = ctx.r4.u32;
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetInAddrToXnAddr(DWORD caller, IN_ADDR ina, XNADDR* xna, XNKID* kid)
// Who sent a datagram: their XNADDR, and the session key they're
// associated through (on a 360, the key of the security association; here
// the game's registered session, system link having one at a time).
PPC_FUNC(__imp__NetDll_XNetInAddrToXnAddr)
{
    LogCall("XNetInAddrToXnAddr", ctx);
    if (ctx.r5.u32 != 0)
        WriteXnAddr(ctx.r5.u32, VirtualIp(ctx.r4.u32));
    if (ctx.r6.u32 != 0)
    {
        std::lock_guard lock(g_keysMutex);
        GuestRef<uint64_t>(ctx.r6.u32) = g_registeredKeys.empty() ? 0 : g_registeredKeys.front();
    }
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetUnregisterInAddr(DWORD caller, IN_ADDR ina)
PPC_FUNC(__imp__NetDll_XNetUnregisterInAddr)
{
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetXnAddrToMachineId(DWORD caller, XNADDR* xna, ULONGLONG* machineId)
PPC_FUNC(__imp__NetDll_XNetXnAddrToMachineId)
{
    LogCall("XNetXnAddrToMachineId", ctx);
    if (ctx.r4.u32 != 0 && ctx.r5.u32 != 0)
        GuestRef<uint64_t>(ctx.r5.u32) = 0xFA00000000000000ull | XnAddrIp(ctx.r4.u32);
    ctx.r3.u64 = 0;
}

// INT NetDll_XNetConnect(DWORD caller, IN_ADDR ina) / XNetGetConnectStatus(DWORD caller, IN_ADDR ina)
// There's no key exchange to do: every game is reachable at once.
PPC_FUNC(__imp__NetDll_XNetConnect)
{
    LogCall("XNetConnect", ctx);
    if (LogNet())
        fprintf(stderr, "[net] XNetConnect %08X\n", ctx.r4.u32);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XNetGetConnectStatus)
{
    LogCall("XNetGetConnectStatus", ctx);
    constexpr uint32_t kConnected = 2;  // XNET_CONNECT_STATUS_CONNECTED
    ctx.r3.u64 = kConnected;
}

// QoS (probing hosts for latency and their published data): log for now.
PPC_FUNC(__imp__NetDll_XNetQosListen)
{
    LogCall("XNetQosListen", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_XNetQosRelease)
{
    LogCall("XNetQosRelease", ctx);
    ctx.r3.u64 = 0;
}

// --- Sockets -------------------------------------------------------------------

// INT NetDll_WSAStartup(DWORD caller, WORD version, WSADATA* data) / WSACleanup(DWORD caller)
PPC_FUNC(__imp__NetDll_WSAStartup)
{
    LogCall("WSAStartup", ctx);
    LinkUp();
    if (ctx.r5.u32 != 0)
    {
        // WSADATA: WORD wVersion, WORD wHighVersion, then descriptions.
        GuestRef<uint16_t>(ctx.r5.u32) = 0x0202;
        GuestRef<uint16_t>(ctx.r5.u32 + 2) = 0x0202;
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__NetDll_WSACleanup)
{
    ctx.r3.u64 = 0;
}

// INT NetDll_WSAGetLastError(void)
PPC_FUNC(__imp__NetDll_WSAGetLastError)
{
    ctx.r3.u64 = t_lastError;
}

// SOCKET NetDll_socket(DWORD caller, INT af, INT type, INT protocol)
PPC_FUNC(__imp__NetDll_socket)
{
    LogCall("socket", ctx);
    uint32_t af = ctx.r4.u32, type = ctx.r5.u32, protocol = ctx.r6.u32;
    if (af != kAfInet)
    {
        ctx.r3.u64 = Fail(kWsaEafnosupport);
        return;
    }
    if (type != kSockDgram || (protocol != 0 && protocol != kIpprotoUdp && protocol != kIpprotoVdp))
    {
        // Only datagrams so far: say which stream socket the game wanted.
        fprintf(stderr, "[net] socket type %u protocol %u not supported\n", type, protocol);
        ctx.r3.u64 = Fail(kWsaEprotonosupport);
        return;
    }
    auto s = std::make_shared<VirtualSocket>();
    s->type = type;
    s->protocol = protocol;
    std::lock_guard lock(g_socketsMutex);
    uint32_t handle = g_nextSocket++;
    g_sockets[handle] = s;
    ctx.r3.u64 = handle;
}

// INT NetDll_closesocket(DWORD caller, SOCKET s)
PPC_FUNC(__imp__NetDll_closesocket)
{
    LogCall("closesocket", ctx);
    std::lock_guard lock(g_socketsMutex);
    auto it = g_sockets.find(ctx.r4.u32);
    if (it == g_sockets.end())
    {
        ctx.r3.u64 = Fail(kWsaEnotsock);
        return;
    }
    if (it->second->port != 0)
        net::Unbind(it->second->port);
    g_sockets.erase(it);
    ctx.r3.u64 = 0;
}

// INT NetDll_shutdown(DWORD caller, SOCKET s, INT how)
PPC_FUNC(__imp__NetDll_shutdown)
{
    ctx.r3.u64 = Find(ctx.r4.u32) ? 0 : Fail(kWsaEnotsock);
}

// INT NetDll_ioctlsocket(DWORD caller, SOCKET s, LONG cmd, ULONG* argp)
PPC_FUNC(__imp__NetDll_ioctlsocket)
{
    LogCall("ioctlsocket", ctx);
    auto s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = Fail(kWsaEnotsock);
        return;
    }
    uint32_t command = ctx.r5.u32, argument = ctx.r6.u32;
    if (command == kFionbio && argument != 0)
        s->nonBlocking = GuestRef<uint32_t>(argument) != 0;
    else if (command == kFionread && argument != 0)
        GuestRef<uint32_t>(argument) = s->port != 0 && net::Pending(s->port) ? 1 : 0;
    else
    {
        ctx.r3.u64 = Fail(kWsaEinval);
        return;
    }
    ctx.r3.u64 = 0;
}

// INT NetDll_setsockopt(DWORD caller, SOCKET s, INT level, INT name, CHAR* value, INT length)
// Broadcast, buffer sizes, reuse: nothing to set on a virtual socket.
PPC_FUNC(__imp__NetDll_setsockopt)
{
    LogCall("setsockopt", ctx);
    ctx.r3.u64 = Find(ctx.r4.u32) ? 0 : Fail(kWsaEnotsock);
}

// INT NetDll_bind(DWORD caller, SOCKET s, SOCKADDR* name, INT length)
PPC_FUNC(__imp__NetDll_bind)
{
    LogCall("bind", ctx);
    auto s = Find(ctx.r4.u32);
    if (!s || ctx.r5.u32 == 0)
    {
        ctx.r3.u64 = Fail(s ? kWsaEinval : kWsaEnotsock);
        return;
    }
    uint32_t ip;
    uint16_t port;
    ReadAddress(ctx.r5.u32, ip, port);
    ctx.r3.u64 = BindPort(*s, port);
    if (LogNet())
        fprintf(stderr, "[net] socket %X bound to port %u\n", ctx.r4.u32, s->port);
}

// INT NetDll_connect(DWORD caller, SOCKET s, SOCKADDR* name, INT length)
// A datagram socket's default destination.
PPC_FUNC(__imp__NetDll_connect)
{
    LogCall("connect", ctx);
    auto s = Find(ctx.r4.u32);
    if (!s || ctx.r5.u32 == 0)
    {
        ctx.r3.u64 = Fail(s ? kWsaEinval : kWsaEnotsock);
        return;
    }
    ReadAddress(ctx.r5.u32, s->connectedIp, s->connectedPort);
    ctx.r3.u64 = 0;
}

// Stream sockets aren't made, so these have nothing to work on.
PPC_FUNC(__imp__NetDll_listen)
{
    LogCall("listen", ctx);
    ctx.r3.u64 = Fail(kWsaEopnotsupp);
}

PPC_FUNC(__imp__NetDll_accept)
{
    LogCall("accept", ctx);
    ctx.r3.u64 = Fail(kWsaEopnotsupp);
}

// INT NetDll_recvfrom(DWORD caller, SOCKET s, CHAR* buffer, INT length, INT flags, SOCKADDR* from, INT* fromLength)
PPC_FUNC(__imp__NetDll_recvfrom)
{
    LogCall("recvfrom", ctx);
    auto s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = Fail(kWsaEnotsock);
        return;
    }
    if (s->port == 0)
    {
        ctx.r3.u64 = Fail(kWsaEinval);
        return;
    }
    net::Datagram datagram;
    if (!TakeDatagram(*s, datagram, !s->nonBlocking))
    {
        ctx.r3.u64 = Fail(kWsaEwouldblock);
        return;
    }
    WriteAddress(ctx.r8.u32, ctx.r9.u32, datagram.secure ? SecureAddress(datagram.sourceIp) : datagram.sourceIp,
        datagram.sourcePort);
    ctx.r3.u64 = Deliver(datagram, ctx.r5.u32, ctx.r6.u32);
}

// INT NetDll_recv(DWORD caller, SOCKET s, CHAR* buffer, INT length, INT flags)
PPC_FUNC(__imp__NetDll_recv)
{
    LogCall("recv", ctx);
    auto s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = Fail(kWsaEnotsock);
        return;
    }
    net::Datagram datagram;
    if (s->port == 0 || !TakeDatagram(*s, datagram, !s->nonBlocking))
    {
        ctx.r3.u64 = Fail(s->port == 0 ? kWsaEnotconn : kWsaEwouldblock);
        return;
    }
    ctx.r3.u64 = Deliver(datagram, ctx.r5.u32, ctx.r6.u32);
}

// INT NetDll_sendto(DWORD caller, SOCKET s, CHAR* buffer, INT length, INT flags, SOCKADDR* to, INT toLength)
PPC_FUNC(__imp__NetDll_sendto)
{
    LogCall("sendto", ctx);
    auto s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = Fail(kWsaEnotsock);
        return;
    }
    uint32_t ip = s->connectedIp;
    uint16_t port = s->connectedPort;
    if (ctx.r8.u32 != 0)
        ReadAddress(ctx.r8.u32, ip, port);
    ctx.r3.u64 = SendDatagram(*s, ctx.r5.u32, ctx.r6.u32, ip, port);
}

// INT NetDll_send(DWORD caller, SOCKET s, CHAR* buffer, INT length, INT flags)
PPC_FUNC(__imp__NetDll_send)
{
    LogCall("send", ctx);
    auto s = Find(ctx.r4.u32);
    if (!s)
    {
        ctx.r3.u64 = Fail(kWsaEnotsock);
        return;
    }
    if (s->connectedPort == 0)
    {
        ctx.r3.u64 = Fail(kWsaEnotconn);
        return;
    }
    ctx.r3.u64 = SendDatagram(*s, ctx.r5.u32, ctx.r6.u32, s->connectedIp, s->connectedPort);
}

// INT NetDll_select(DWORD caller, INT nfds, FD_SET* read, FD_SET* write, FD_SET* except, TIMEVAL* timeout)
// FD_SET: u_int fd_count, SOCKET fd_array[64]. Datagram sockets are
// always writable; readable when a datagram waits. Sets are rewritten to
// the ready sockets.
PPC_FUNC(__imp__NetDll_select)
{
    LogCall("select", ctx);
    uint32_t readSet = ctx.r5.u32, writeSet = ctx.r6.u32, exceptSet = ctx.r7.u32, timeout = ctx.r8.u32;
    int timeoutMs = -1;
    if (timeout != 0)
        timeoutMs = int(GuestRef<uint32_t>(timeout)) * 1000 + int(GuestRef<uint32_t>(timeout + 4)) / 1000;

    auto members = [](uint32_t set) {
        std::vector<uint32_t> handles;
        if (set != 0)
            for (uint32_t i = 0; i < std::min<uint32_t>(GuestRef<uint32_t>(set), kMaxFdSet); i++)
                handles.push_back(GuestRef<uint32_t>(set + 4 + i * 4));
        return handles;
    };
    std::vector<uint32_t> readers = members(readSet), writers = members(writeSet);
    auto ready = [&](std::vector<uint32_t>& readable) {
        readable.clear();
        for (uint32_t handle : readers)
            if (auto s = Find(handle); s && s->port != 0 && net::Pending(s->port))
                readable.push_back(handle);
        return !readable.empty() || !writers.empty();
    };
    std::vector<uint32_t> readable;
    auto start = std::chrono::steady_clock::now();
    while (!ready(readable))
    {
        int elapsed = int(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        if (timeoutMs >= 0 && elapsed >= timeoutMs)
            break;
        net::WaitForAny(timeoutMs < 0 ? 10 : std::min(10, timeoutMs - elapsed));
    }
    auto write = [](uint32_t set, const std::vector<uint32_t>& handles) {
        if (set == 0)
            return;
        GuestRef<uint32_t>(set) = uint32_t(handles.size());
        for (size_t i = 0; i < handles.size(); i++)
            GuestRef<uint32_t>(set + 4 + uint32_t(i) * 4) = handles[i];
    };
    write(readSet, readable);
    write(writeSet, writers);
    write(exceptSet, {});
    ctx.r3.u64 = readable.size() + writers.size();
}

// INT NetDll___WSAFDIsSet(SOCKET s, FD_SET* set)
PPC_FUNC(__imp__NetDll___WSAFDIsSet)
{
    uint32_t handle = ctx.r3.u32, set = ctx.r4.u32;
    uint32_t found = 0;
    if (set != 0)
        for (uint32_t i = 0; i < std::min<uint32_t>(GuestRef<uint32_t>(set), kMaxFdSet); i++)
            if (GuestRef<uint32_t>(set + 4 + i * 4) == handle)
                found = 1;
    ctx.r3.u64 = found;
}

// ULONG NetDll_inet_addr(CHAR* text): dotted quad to a big-endian address.
PPC_FUNC(__imp__NetDll_inet_addr)
{
    LogCall("inet_addr", ctx);
    unsigned a, b, c, d;
    const char* text = ctx.r3.u32 ? GuestPtr<char>(ctx.r3.u32) : "";
    if (sscanf(text, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
        ctx.r3.u64 = (a << 24) | (b << 16) | (c << 8) | d;
    else
        ctx.r3.u64 = 0xFFFFFFFF;  // INADDR_NONE
}
