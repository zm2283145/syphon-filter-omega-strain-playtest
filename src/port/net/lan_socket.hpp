#pragma once
// PORT-ONLY (not in the original; LAN phase 2 groundwork, checkpoint 132ln): the small socket wrapper under
// LanDiscovery and LanTransport, so both build with MSVC (Winsock2) and on Linux (POSIX sockets), plus the clock
// type both components take so their timing can be injected by tests. research/LAN_PLAY.md §5.2 (option M).
//
// Stands in for the original's platform socket layer, which the port does not port: socket construction
// Socket_Construct 0x2F6CC8, the RT UDP socket init 0x0030af68, the connected / addressed writes
// Socket_WriteConnected 0x2F5E10 / Socket_WriteAddressed 0x2F5F20 / Socket_Write 0x301608, and the IOP ring
// NetRing_Commit 0x2F5460 / the addressed-send boundary 0x0034ccf0. Here a socket is one non-blocking IPv4 UDP
// datagram socket; the platform calls live only in lan_socket.cpp. lan_loopback.hpp implements the same
// interface as an in-memory network for deterministic tests.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::lan {

// PORT-ONLY: milliseconds on a monotonic clock. Components read time only through a LanClock, so a test passes
// a lambda over its own counter and steps it explicitly.
using LanClock = std::function<std::uint64_t()>;

// PORT-ONLY: the default LanClock (std::chrono::steady_clock in milliseconds).
LanClock lan_steady_clock();

// PORT-ONLY: an IPv4 endpoint, both fields in host byte order.
struct LanAddress {
    std::uint32_t ip = 0;
    std::uint16_t port = 0;
    friend bool operator==(const LanAddress&, const LanAddress&) = default;
};

constexpr std::uint32_t lan_ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return (std::uint32_t{a} << 24) | (std::uint32_t{b} << 16) | (std::uint32_t{c} << 8) | std::uint32_t{d};
}
constexpr std::uint32_t kLanBroadcastIp = 0xFFFFFFFFu;  // 255.255.255.255
constexpr std::uint32_t kLanLoopbackIp = lan_ipv4(127, 0, 0, 1);

std::string lan_address_text(const LanAddress& address);       // "a.b.c.d:port"
std::optional<std::uint32_t> lan_parse_ipv4(const std::string& text);  // dotted quad only

struct LanSocketOptions {
    bool broadcast = false;      // SO_BROADCAST: may send to broadcast addresses
    bool reuse_address = false;  // SO_REUSEADDR: share the port (discovery: host and browser on one machine)
};

// PORT-ONLY: one non-blocking UDP socket bound to INADDR_ANY (or to one local address, make_udp_socket).
class LanSocket {
public:
    virtual ~LanSocket() = default;
    // Bind to the port (0 = an ephemeral port). False when the socket cannot be created or bound.
    virtual bool open(std::uint16_t port, const LanSocketOptions& options) = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;
    // The bound port (the ephemeral one after open(0)).
    virtual std::uint16_t local_port() const = 0;
    // Send one datagram. False when it could not be handed to the network (the caller treats it as lost).
    virtual bool send_to(const LanAddress& to, std::span<const std::uint8_t> bytes) = 0;
    // Receive one pending datagram into buffer; its size, or nullopt when none is pending. A datagram longer
    // than the buffer is truncated (the transport sizes its buffer above the 1200-byte datagram limit).
    virtual std::optional<std::size_t> receive_from(LanAddress& from, std::span<std::uint8_t> buffer) = 0;
    // The addresses a LAN broadcast to `port` goes to: 255.255.255.255 and each up interface's directed
    // broadcast (research/LAN_PLAY.md §5.3).
    virtual std::vector<LanAddress> broadcast_targets(std::uint16_t port) const = 0;
};

using LanSocketFactory = std::function<std::unique_ptr<LanSocket>()>;

// PORT-ONLY: a real UDP socket (Winsock2 on Windows, POSIX sockets elsewhere). bind_ip 0 binds INADDR_ANY; another
// address (checkpoint 132ln5f: 127.0.0.1, two instances on one machine without a firewall change) binds only that
// one, and the socket then has no broadcast targets.
std::unique_ptr<LanSocket> make_udp_socket(std::uint32_t bind_ip = 0);
LanSocketFactory lan_udp_socket_factory(std::uint32_t bind_ip = 0);

}  // namespace omega::lan
