#pragma once
// PORT-ONLY (not in the original; checkpoint 132on2e, research/ONLINE.md §6.11, research/ONLINE_DEPLOY.md §2): the
// optional UDP relay through omega_server for the in-level traffic. The original's in-level traffic went peer to peer
// from the joiners to the host (ONLINE.md §3.3); a joiner behind a home router (NAT) cannot reach a host that is
// behind one too. The relay is a transport layer BELOW the port's LAN in-level path: the game's LanTransport datagrams
// go unchanged inside a relay datagram to omega_server's relay UDP port, which forwards them to the other endpoint of
// the same world. Direct connection stays preferred (online_relay=auto tries it first).
//
// Addressing: each relay endpoint (one logged-in port session with cap::Relay) gets a virtual IPv4 address in the RFC
// 2544 benchmark range 198.18.0.0/15 (never a real host on the Internet) and the fixed virtual port kVirtualPort. The
// game's socket wrapper (online_relay_socket.hpp) sends a datagram for a virtual address through the relay and
// reports what the relay delivers as coming from the sender's virtual address, so LanTransport sees an ordinary peer.
//
// Who gets a relay endpoint: only a port session that announced cap::Relay, after its login, and only while the
// server's relay is on (relay_port). The endpoint's token (16 random bytes) goes to the client in ext::RelayTicket over
// its authenticated TCP session; a datagram must carry it. An original (PS2-kind) session never gets a ticket, an
// endpoint or any relay message (send_ext gates every extension on the session kind and capabilities).
//
// Wire (little endian; every datagram starts with kMagic, the kind, a flags byte 0 and a u16 code):
//   Register   [hdr code 0][token 16]                     client -> server: (re)binds the token to this address
//   Registered [hdr code 0][own virtual ip u32]           server -> client
//   Data       [hdr code 0][token 16][dest vip u32][payload] client -> server
//   DataFrom   [hdr code 0][src vip u32][payload]         server -> client
//   Refused    [hdr code: RefuseCode]                     server -> client (an unknown token's Register only)

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::online {

namespace relay {
constexpr std::uint32_t kMagic = 0x314c524fu;  // "ORL1"
constexpr std::size_t kHeaderSize = 8;
constexpr std::size_t kTokenSize = 16;
constexpr std::size_t kMaxPayload = 1400;            // LanTransport's datagrams are at most 1200 bytes
constexpr std::uint32_t kVirtualBase = 0xC6120000u;  // 198.18.0.0
constexpr std::uint32_t kVirtualMask = 0xFFFE0000u;  // /15
constexpr std::uint16_t kVirtualPort = 47525;        // kLanGamePort: every virtual endpoint's port
enum class Kind : std::uint8_t { Register = 1, Registered = 2, Data = 3, DataFrom = 4, Refused = 5 };
enum RefuseCode : std::uint16_t { UnknownToken = 1 };
}  // namespace relay

using RelayToken = std::array<std::uint8_t, relay::kTokenSize>;

constexpr bool is_relay_virtual_ip(std::uint32_t ip) { return (ip & relay::kVirtualMask) == relay::kVirtualBase; }

struct RelayPacket {
    relay::Kind kind = relay::Kind::Register;
    std::uint16_t code = 0;
    RelayToken token{};       // Register / Data
    std::uint32_t vip = 0;    // Registered: own; Data: destination; DataFrom: source
    std::vector<std::uint8_t> payload;  // Data / DataFrom
};
std::vector<std::uint8_t> encode_relay(const RelayPacket& p);
// nullopt: not a relay datagram (wrong magic, unknown kind, short, payload above kMaxPayload).
std::optional<RelayPacket> decode_relay(std::span<const std::uint8_t> bytes);

// ext::RelayTicket's body: [u16 relay UDP port][u32 own virtual ip][str token (16 bytes)].
struct RelayTicket {
    std::uint16_t port = 0;
    std::uint32_t vip = 0;
    RelayToken token{};
};
std::vector<std::uint8_t> encode_relay_ticket(const RelayTicket& t);
std::optional<RelayTicket> decode_relay_ticket(std::span<const std::uint8_t> body);
// ext::RelayPeer's body: [u32 world][u32 the host's virtual ip][u16 its virtual port].
struct RelayPeer {
    std::uint32_t world = 0;
    std::uint32_t vip = 0;
    std::uint16_t port = relay::kVirtualPort;
};
std::vector<std::uint8_t> encode_relay_peer(const RelayPeer& p);
std::optional<RelayPeer> decode_relay_peer(std::span<const std::uint8_t> body);

struct RelayLimits {
    std::size_t max_endpoints = 64;   // relay endpoints at once (more logins get no ticket)
    int packet_rate = 200;            // datagrams per second per endpoint (burst: one second's worth)
    int byte_rate = 256 * 1024;       // payload bytes per second per endpoint (burst: one second's worth)
    std::uint64_t idle_ms = 60000;    // an endpoint silent this long must Register again before it receives
};

// The relay's state: the endpoints (by TCP connection), their tokens, virtual addresses and observed UDP addresses,
// and the forwarding with its limits. Transport independent: the caller feeds datagrams and sends what `send` gets.
class RelayCore {
public:
    using SendFn = std::function<void(std::uint32_t ip, std::uint16_t port, std::vector<std::uint8_t> bytes)>;
    using WorldFn = std::function<std::uint32_t(std::uint32_t conn)>;  // the connection's world (0: none)
    using TokenFn = std::function<RelayToken()>;
    using LogFn = std::function<void(const std::string& line)>;

    RelayCore(RelayLimits limits, SendFn send, WorldFn world_of, TokenFn token = {}, LogFn log = {});

    // A new endpoint for a logged-in connection (replacing its previous one); nullopt when max_endpoints is reached.
    std::optional<RelayTicket> open(std::uint32_t conn, std::uint16_t udp_port, const std::string& name);
    void close(std::uint32_t conn);
    std::optional<std::uint32_t> virtual_ip(std::uint32_t conn) const;
    // One received datagram (ip / port in host byte order; now_ms on a monotonic clock).
    void on_datagram(std::uint32_t ip, std::uint16_t port, std::span<const std::uint8_t> bytes, std::uint64_t now_ms);

    struct Stats {
        std::uint64_t received = 0, registers = 0, forwarded = 0, forwarded_bytes = 0;
        std::uint64_t dropped_bad = 0, dropped_token = 0, dropped_world = 0, dropped_unregistered = 0,
                      dropped_rate = 0;
    };
    const Stats& stats() const { return stats_; }
    std::size_t endpoint_count() const { return endpoints_.size(); }
    const RelayLimits& limits() const { return limits_; }
    // The admin console's `relay` text: the limits, the counters, then one line per endpoint.
    std::string status(std::uint64_t now_ms) const;

private:
    struct Endpoint {
        std::uint32_t conn = 0;
        std::string name;
        RelayToken token{};
        std::uint32_t vip = 0;
        std::uint32_t ip = 0;  // observed (0: not registered yet)
        std::uint16_t port = 0;
        std::uint64_t last_seen = 0;
        double packets = 0, bytes = 0;  // token buckets
        std::uint64_t bucket_at = 0;
        std::uint64_t sent = 0, received = 0;
    };
    Endpoint* by_token(const RelayToken& t);
    Endpoint* by_vip(std::uint32_t vip);
    bool within_rate(Endpoint& e, std::size_t bytes, std::uint64_t now_ms);
    void log(const std::string& line) const;

    RelayLimits limits_;
    SendFn send_;
    WorldFn world_of_;
    TokenFn token_;
    LogFn log_;
    std::map<std::uint32_t, Endpoint> endpoints_;  // by conn
    std::uint32_t next_index_ = 1;
    Stats stats_;
};

std::string relay_vip_text(std::uint32_t vip);  // "198.18.0.1"

}  // namespace omega::online
