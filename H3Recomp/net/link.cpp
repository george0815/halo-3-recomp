// System link transport (link.h).

#include "net/link.h"
#include "config/env.h"
#include "config/settings.h"
#include "hooks/tick_rate.h"
#include "memory.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>

#ifndef H3_GIT_COMMIT
#define H3_GIT_COMMIT "unknown"
#endif
#ifndef H3_VERSION
#define H3_VERSION "dev"
#endif

namespace h3::net
{
    namespace
    {
#ifdef _WIN32
        using Socket = SOCKET;
        constexpr Socket kNoSocket = INVALID_SOCKET;
        void CloseSocket(Socket s) { closesocket(s); }
#else
        using Socket = int;
        constexpr Socket kNoSocket = -1;
        void CloseSocket(Socket s) { close(s); }
#endif

        // The header in front of every datagram, big-endian on the wire.
        constexpr uint32_t kMagic = 0x4833534C;  // "H3SL"
        constexpr uint8_t kVersion = 1;
        constexpr size_t kHeaderSize = 24;
        constexpr size_t kMaxDatagram = 1500;  // the game's own datagrams fit an Ethernet frame
        constexpr size_t kMaxQueued = 512;     // per port; the oldest go first

        constexpr uint8_t kFlagSecure = 1;   // sent to the destination's secure address
        constexpr uint8_t kFlagRelayed = 2;  // forwarded by a game in between (not again)

        struct Header
        {
            uint8_t version = kVersion;
            uint8_t flags = 0;
            uint16_t tickRate = 0;
            uint32_t build = 0;
            uint32_t sourceIp = 0, destIp = 0;
            uint16_t sourcePort = 0, destPort = 0;
        };

        void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
        void Put32(uint8_t* p, uint32_t v) { Put16(p, uint16_t(v >> 16)), Put16(p + 2, uint16_t(v)); }
        uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
        uint32_t Get32(const uint8_t* p) { return uint32_t(Get16(p)) << 16 | Get16(p + 2); }

        void WriteHeader(uint8_t* out, const Header& h)
        {
            Put32(out, kMagic);
            out[4] = h.version;
            out[5] = h.flags;
            Put16(out + 6, h.tickRate);
            Put32(out + 8, h.build);
            Put32(out + 12, h.sourceIp);
            Put32(out + 16, h.destIp);
            Put16(out + 20, h.sourcePort);
            Put16(out + 22, h.destPort);
        }

        bool ReadHeader(const uint8_t* in, size_t size, Header& h)
        {
            if (size < kHeaderSize || Get32(in) != kMagic)
                return false;
            h.version = in[4];
            h.flags = in[5];
            h.tickRate = Get16(in + 6);
            h.build = Get32(in + 8);
            h.sourceIp = Get32(in + 12);
            h.destIp = Get32(in + 16);
            h.sourcePort = Get16(in + 20);
            h.destPort = Get16(in + 22);
            return true;
        }

        // What games must share to play together (the tick rate is in the
        // header too): this release's version (builds of one version run
        // the simulation alike), the game's code (another executable or
        // title update: its code section, never written at run time), and
        // whether the 60 Hz content fixes run (they change the simulation).
        // FNV-1a of all three.
        constexpr uint32_t kGameCodeBegin = 0x82080000, kGameCodeEnd = 0x8270E554;  // .text of default.xex

        uint32_t Fnv(uint32_t hash, const uint8_t* bytes, size_t size)
        {
            for (size_t i = 0; i < size; i++)
                hash = (hash ^ bytes[i]) * 16777619u;
            return hash;
        }

        uint32_t BuildId()
        {
            uint32_t hash = Fnv(2166136261u, reinterpret_cast<const uint8_t*>(H3_VERSION), strlen(H3_VERSION));
            hash = Fnv(hash, GuestPtr<uint8_t>(kGameCodeBegin), kGameCodeEnd - kGameCodeBegin);
            uint8_t fixes = hooks::ContentFixes() ? 1 : 0;
            return Fnv(hash, &fixes, 1);
        }

        // A real (IPv4) address and port, network byte order inside.
        struct Endpoint
        {
            sockaddr_in address{};
            bool operator<(const Endpoint& o) const
            {
                return std::make_pair(address.sin_addr.s_addr, address.sin_port) <
                    std::make_pair(o.address.sin_addr.s_addr, o.address.sin_port);
            }
        };

        std::string Describe(const sockaddr_in& a)
        {
            char text[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &a.sin_addr, text, sizeof(text));
            return std::string(text) + ":" + std::to_string(ntohs(a.sin_port));
        }

        bool g_enabled = false;
        bool g_log = false;
        Socket g_socket = kNoSocket;
        uint32_t g_localIp = 0;
        uint32_t g_build = 0;
        uint16_t g_tickRate = 0;
        uint16_t g_port = 0;
        std::vector<Endpoint> g_peers;  // from [network] peers, and the LAN broadcast

        std::mutex g_mutex;
        std::condition_variable g_arrived;
        std::map<uint16_t, std::deque<Datagram>> g_ports;  // bound virtual ports
        std::map<uint32_t, Endpoint> g_learned;            // virtual IP -> where it sends from
        std::set<uint32_t> g_rejected;                      // builds already reported

        // Resolves "host" or "host:port" (IPv4).
        bool Resolve(const std::string& text, Endpoint& endpoint)
        {
            std::string host = text;
            uint16_t port = config::kDefaultLinkPort;
            if (size_t colon = text.rfind(':'); colon != std::string::npos)
            {
                host = text.substr(0, colon);
                port = uint16_t(atoi(text.c_str() + colon + 1));
            }
            addrinfo hints{}, *result = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result)
                return false;
            endpoint.address = *reinterpret_cast<const sockaddr_in*>(result->ai_addr);
            endpoint.address.sin_port = htons(port);
            freeaddrinfo(result);
            return true;
        }

        void SendTo(const Endpoint& endpoint, const uint8_t* packet, size_t size)
        {
            sendto(g_socket, reinterpret_cast<const char*>(packet), int(size), 0,
                reinterpret_cast<const sockaddr*>(&endpoint.address), sizeof(endpoint.address));
        }

        void ReceiveLoop()
        {
            uint8_t packet[kHeaderSize + kMaxDatagram];
            for (;;)
            {
                sockaddr_in from{};
                socklen_t fromSize = sizeof(from);
                int size = int(recvfrom(g_socket, reinterpret_cast<char*>(packet), sizeof(packet), 0,
                    reinterpret_cast<sockaddr*>(&from), &fromSize));
                if (size < 0)
                    continue;  // timeout or a transient error (ICMP port unreachable on Windows)
                Header header;
                if (!ReadHeader(packet, size_t(size), header))
                    continue;
                if (header.sourceIp == g_localIp)
                    continue;  // our own broadcast, back from the LAN
                if (header.version != kVersion || header.build != g_build || header.tickRate != g_tickRate)
                {
                    std::lock_guard lock(g_mutex);
                    if (g_rejected.insert(header.build ^ header.tickRate ^ header.version).second)
                        fprintf(stderr,
                            "[net] ignoring %s: another build or tick rate (build %08X, %u Hz; ours %08X, %u Hz)\n",
                            Describe(from).c_str(), header.build, header.tickRate, g_build, g_tickRate);
                    continue;
                }
                std::lock_guard lock(g_mutex);
                // A packet for another game: Halo's sessions are a mesh,
                // and over the internet joiners often know only the host.
                // A game that knows where the destination is forwards it
                // there once; the destination learns the sender at the
                // forwarder, so replies come back the same way.
                if (header.destIp != g_localIp && header.destIp != kBroadcast)
                {
                    auto destination = g_learned.find(header.destIp);
                    if (!(header.flags & kFlagRelayed) && destination != g_learned.end())
                    {
                        packet[5] = uint8_t(header.flags | kFlagRelayed);
                        SendTo(destination->second, packet, size_t(size));
                        g_learned.try_emplace(header.sourceIp, Endpoint{ from });
                    }
                    continue;
                }
                // A relayed packet doesn't replace a direct route.
                bool isNew = false;
                if (!(header.flags & kFlagRelayed))
                    isNew = g_learned.insert_or_assign(header.sourceIp, Endpoint{ from }).second;
                else
                    isNew = g_learned.try_emplace(header.sourceIp, Endpoint{ from }).second;
                if (isNew)
                    fprintf(stderr, "[net] game %u.%u.%u.%u is at %s\n", header.sourceIp >> 24, (header.sourceIp >> 16) & 0xFF,
                        (header.sourceIp >> 8) & 0xFF, header.sourceIp & 0xFF, Describe(from).c_str());
                auto port = g_ports.find(header.destPort);
                if (port == g_ports.end())
                    continue;
                if (port->second.size() >= kMaxQueued)
                    port->second.pop_front();
                port->second.push_back({ header.sourceIp, header.sourcePort, (header.flags & kFlagSecure) != 0,
                    std::vector<uint8_t>(packet + kHeaderSize, packet + size) });
                if (g_log)
                {
                    // Small datagrams (handshakes) in full.
                    char hex[3 * 64 + 1] = "";
                    size_t bytes = size_t(size) - kHeaderSize;
                    for (size_t i = 0; i < bytes && i < 64 && bytes <= 64; i++)
                        snprintf(hex + i * 3, 4, "%02X ", packet[kHeaderSize + i]);
                    fprintf(stderr, "[net] %zu bytes for port %u from %08X:%u %s\n", bytes, header.destPort, header.sourceIp,
                        header.sourcePort, hex);
                }
                g_arrived.notify_all();
            }
        }
    }

    bool Start()
    {
        const config::NetworkSettings& settings = config::Active().network;
        if (!settings.systemLink)
            return false;
        g_log = EnvFlag("H3_LOG_NET");
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
            return false;
#endif
        g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_socket == kNoSocket)
            return false;
        int on = 1;
        setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(settings.port);
        if (bind(g_socket, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0)
        {
            fprintf(stderr, "[net] can't open UDP port %u: system link is off\n", settings.port);
            CloseSocket(g_socket);
            g_socket = kNoSocket;
            return false;
        }
        g_port = settings.port;

        // A virtual IP of our own, new each run (10.x.y.z, never .0 or .255).
        std::random_device random;
        g_localIp = 0x0A000000u | (random() & 0x00FFFFFEu);
        if ((g_localIp & 0xFF) == 0)
            g_localIp |= 1;
        g_build = BuildId();
        g_tickRate = uint16_t(config::Active().video.frameRate);

        // Broadcasts go to the LAN (on the default port, and ours if it's
        // another, for a second game on this PC) and to every friend.
        for (uint16_t port : { config::kDefaultLinkPort, settings.port })
        {
            Endpoint lan;
            lan.address.sin_family = AF_INET;
            lan.address.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            lan.address.sin_port = htons(port);
            if (std::none_of(g_peers.begin(), g_peers.end(), [&](const Endpoint& e) { return !(e < lan) && !(lan < e); }))
                g_peers.push_back(lan);
        }
        for (const std::string& peer : settings.peers)
        {
            Endpoint endpoint;
            if (Resolve(peer, endpoint))
                g_peers.push_back(endpoint);
            else
                fprintf(stderr, "[net] can't find %s\n", peer.c_str());
        }

        // The receive thread stops every 200 ms only to notice nothing;
        // a timeout keeps a dead socket from hanging it on exit.
#ifdef _WIN32
        DWORD timeout = 200;
#else
        timeval timeout{ 0, 200'000 };
#endif
        setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::thread(ReceiveLoop).detach();
        g_enabled = true;
        fprintf(stderr, "[net] system link on UDP port %u, game address %u.%u.%u.%u, version %s (commit %s, id %08X), %u Hz, %zu friend(s)\n",
            g_port, g_localIp >> 24, (g_localIp >> 16) & 0xFF, (g_localIp >> 8) & 0xFF, g_localIp & 0xFF, H3_VERSION, H3_GIT_COMMIT, g_build,
            g_tickRate, settings.peers.size());
        return true;
    }

    bool Enabled() { return g_enabled; }
    uint32_t LocalIp() { return g_localIp; }

    bool Send(uint16_t sourcePort, uint32_t destIp, uint16_t destPort, const uint8_t* data, size_t size, bool secure)
    {
        if (!g_enabled || size > kMaxDatagram)
            return false;
        uint8_t packet[kHeaderSize + kMaxDatagram];
        WriteHeader(packet, { kVersion, uint8_t(secure ? kFlagSecure : 0), g_tickRate, g_build, g_localIp, destIp, sourcePort, destPort });
        memcpy(packet + kHeaderSize, data, size);
        size_t total = kHeaderSize + size;

        std::vector<Endpoint> targets;
        {
            std::lock_guard lock(g_mutex);
            auto learned = g_learned.find(destIp);
            if (destIp != kBroadcast && learned != g_learned.end())
                targets.push_back(learned->second);
            else
            {
                // A broadcast, or a game not heard from yet: everyone we
                // know of, and every game heard from (the LAN broadcast may
                // not reach it).
                std::set<Endpoint> all(g_peers.begin(), g_peers.end());
                for (const auto& [ip, endpoint] : g_learned)
                    all.insert(endpoint);
                targets.assign(all.begin(), all.end());
            }
        }
        for (const Endpoint& endpoint : targets)
            SendTo(endpoint, packet, total);
        if (g_log)
            fprintf(stderr, "[net] %zu bytes from port %u to %08X:%u (%zu targets)\n", size, sourcePort, destIp, destPort,
                targets.size());
        return true;
    }

    void Bind(uint16_t port)
    {
        std::lock_guard lock(g_mutex);
        g_ports[port];
    }

    void Unbind(uint16_t port)
    {
        std::lock_guard lock(g_mutex);
        g_ports.erase(port);
    }

    bool Receive(uint16_t port, Datagram& datagram, int timeoutMs)
    {
        std::unique_lock lock(g_mutex);
        auto ready = [&] {
            auto it = g_ports.find(port);
            return it == g_ports.end() || !it->second.empty();
        };
        if (timeoutMs < 0)
            g_arrived.wait(lock, ready);
        else if (timeoutMs > 0)
            g_arrived.wait_for(lock, std::chrono::milliseconds(timeoutMs), ready);
        auto it = g_ports.find(port);
        if (it == g_ports.end() || it->second.empty())
            return false;
        datagram = std::move(it->second.front());
        it->second.pop_front();
        return true;
    }

    bool Pending(uint16_t port)
    {
        std::lock_guard lock(g_mutex);
        auto it = g_ports.find(port);
        return it != g_ports.end() && !it->second.empty();
    }

    // A datagram landing between the caller's check and this wait isn't
    // seen until the timeout: callers wait in short slices.
    void WaitForAny(int timeoutMs)
    {
        std::unique_lock lock(g_mutex);
        if (timeoutMs < 0)
            g_arrived.wait(lock);
        else
            g_arrived.wait_for(lock, std::chrono::milliseconds(timeoutMs));
    }
}
