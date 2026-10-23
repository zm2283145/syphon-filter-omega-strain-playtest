#pragma once
// PORT-ONLY (not in the original; LAN phase 2 groundwork, checkpoint 132ln): an in-memory "network" that
// implements LanSocket, so the LAN discovery and transport tests run deterministically without real sockets.
// research/LAN_PLAY.md §8 phase 2 ("unit tests with two endpoints on loopback").
//
// Every socket has a virtual IPv4 address. A datagram sent through the network is, at send time, dropped
// (loss), copied (duplication) or held back by a random extra delay (reordering) according to the conditions,
// using a seeded generator (splitmix64), so a test run with the same seed sees the same packet fates. A
// datagram becomes receivable once the network clock reaches its delivery time; receivable datagrams are read
// in delivery-time order (send order on ties).
//
// Addressing: unicast to (ip, port); 255.255.255.255 to every socket bound to the port; a.b.c.255 to every
// socket on a.b.c.* bound to the port. Broadcasts also loop back to the sender's own socket when it is bound
// to that port, as real UDP does.

#include "lan_socket.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace omega::lan {

struct LanLoopbackConditions {
    double loss = 0.0;               // probability a datagram is dropped
    double duplicate = 0.0;          // probability a (kept) datagram is delivered twice
    double reorder = 0.0;            // probability a datagram gets an extra random delay
    std::uint64_t latency_ms = 1;    // base one-way latency
    std::uint64_t reorder_ms = 40;   // the extra delay is uniform in [1, reorder_ms]
};

class LanLoopbackNetwork : public std::enable_shared_from_this<LanLoopbackNetwork> {
public:
    // Create through make(): sockets keep the network alive.
    static std::shared_ptr<LanLoopbackNetwork> make(LanClock clock, std::uint64_t seed);

    void set_conditions(const LanLoopbackConditions& conditions) { conditions_ = conditions; }
    const LanLoopbackConditions& conditions() const { return conditions_; }

    // A socket on the virtual host `ip`.
    std::unique_ptr<LanSocket> socket(std::uint32_t ip);
    LanSocketFactory factory(std::uint32_t ip);

    // Test hook: reverse the delivery order of every datagram still in flight (deterministic reordering).
    void reverse_in_flight();
    // Test hook: drop every datagram to or from `ip` while blocked (a pulled cable).
    void set_blocked(std::uint32_t ip, bool blocked);

    struct Stats {
        std::uint64_t sent = 0, dropped = 0, duplicated = 0, reordered = 0, delivered = 0;
    };
    const Stats& stats() const { return stats_; }

    // Internal (used by the loopback sockets).
    struct Datagram {
        LanAddress from;
        std::vector<std::uint8_t> bytes;
        std::uint64_t deliver_at = 0;
        std::uint64_t order = 0;
    };
    struct Endpoint {
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        bool open = false;
        bool broadcast = false;
        bool reuse = false;
        std::vector<Datagram> inbox;
    };
    std::shared_ptr<Endpoint> bind(std::uint32_t ip, std::uint16_t port, const LanSocketOptions& options);
    void unbind(const std::shared_ptr<Endpoint>& endpoint);
    bool send(const Endpoint& from, const LanAddress& to, std::span<const std::uint8_t> bytes);
    std::optional<Datagram> receive(Endpoint& endpoint);

private:
    LanLoopbackNetwork(LanClock clock, std::uint64_t seed) : clock_(std::move(clock)), state_(seed) {}
    double next_unit();
    std::uint64_t next_u64();
    void enqueue(Endpoint& to, const LanAddress& from, std::span<const std::uint8_t> bytes, std::uint64_t at);

    LanClock clock_;
    std::uint64_t state_;
    LanLoopbackConditions conditions_{};
    std::vector<std::shared_ptr<Endpoint>> endpoints_;
    std::vector<std::uint32_t> blocked_;
    std::uint64_t order_ = 0;
    std::uint16_t next_ephemeral_ = 49152;
    Stats stats_{};
};

}  // namespace omega::lan
