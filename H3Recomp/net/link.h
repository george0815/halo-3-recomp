// System link: the game's LAN traffic carried over one host UDP port.
//
// The game's sockets (kernel/xnet.cpp) are virtual: they live on a virtual
// LAN where every game has a virtual IP (10.x.y.z, in its XNADDR) and
// binds whatever ports it likes. A datagram the game sends is wrapped in a
// small header (virtual source and destination, the build) and sent from
// the one real socket ([network] port in settings.ini) to the real address
// the destination lives at: learned from packets it sent us, or, for a
// broadcast or a game not heard from yet, every friend in [network] peers
// plus the real LAN's broadcast address. So a LAN needs no setup, the
// internet needs one forwarded UDP port (or both sides listing each other,
// which opens most NATs), and two games run side by side on one PC.
// A game passes on a packet meant for another game it knows (once), so
// joiners who know only the host still reach each other (Halo's sessions
// connect every game to every other). Packets from another build or tick
// rate are dropped: the games would fall out of sync.

#pragma once

#include <cstdint>
#include <vector>

namespace h3::net
{
    constexpr uint32_t kBroadcast = 0xFFFFFFFF;

    // A datagram for a virtual socket.
    struct Datagram
    {
        uint32_t sourceIp = 0;    // virtual, host byte order
        uint16_t sourcePort = 0;  // host byte order
        bool secure = false;      // sent to our secure address (XNet's 0.x.y.z form; xnet.cpp)
        std::vector<uint8_t> data;
    };

    // Opens the real socket and starts receiving, if [network] system_link
    // is on. False if it's off or the port can't be opened.
    bool Start();
    bool Enabled();

    // This game's virtual IP (host byte order).
    uint32_t LocalIp();

    // Sends a datagram from a virtual port to a virtual address (or
    // kBroadcast). False if the link is down.
    // secure: the game addressed it to the destination's secure address.
    bool Send(uint16_t sourcePort, uint32_t destIp, uint16_t destPort, const uint8_t* data, size_t size, bool secure);

    // Virtual ports: datagrams arriving for a bound port queue up there
    // until Receive takes them.
    void Bind(uint16_t port);
    void Unbind(uint16_t port);
    // Takes the oldest datagram for a port; waits up to timeoutMs for one
    // (0: don't wait, negative: forever). False if none came.
    bool Receive(uint16_t port, Datagram& datagram, int timeoutMs);
    // Whether a datagram is waiting for a port.
    bool Pending(uint16_t port);
    // Waits up to timeoutMs (negative: forever) for any datagram to arrive;
    // for select.
    void WaitForAny(int timeoutMs);
}
