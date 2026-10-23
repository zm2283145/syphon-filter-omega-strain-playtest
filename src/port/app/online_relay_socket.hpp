#pragma once
// PORT-ONLY (not in the original; checkpoint 132on2e, research/ONLINE.md §6.11): the game side of the in-level UDP
// relay through omega_server (online/online_relay.hpp). A RelaySocket wraps the online game's LAN socket (the
// LanTransport's, below the in-level replication, which it does not see): a datagram to a relay virtual address
// (198.18.0.0/15) goes to omega_server's relay port inside a Data datagram with this login's token; a DataFrom from
// the relay comes out as an ordinary datagram from the sender's virtual address. Everything else passes straight
// through, so a direct connection (the preferred path) is unchanged. While the login holds a relay ticket the socket
// registers its address with the relay (Register) at open and every kRegisterMs after, which also keeps a home
// router's UDP mapping toward the relay open.
//
// An original PS2 session never meets any of this: its client is not the port and gets no ticket.

#include "lan_socket.hpp"
#include "online_relay.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace omega {

// The relay state shared by the RelaySockets of one online session (the OnlineLobbyHost sets it from the ticket).
struct RelayLink {
    static constexpr std::uint64_t kRegisterMs = 5000;
    bool active = false;             // a ticket is held (the server's relay is on, cap::Relay negotiated)
    lan::LanAddress server{};        // omega_server's relay UDP endpoint
    online::RelayToken token{};
    std::uint32_t vip = 0;           // this login's virtual address
    std::uint32_t generation = 0;    // bumped at each new ticket (the sockets register again at once)
    bool registered = false;         // a Registered arrived for the current ticket
    std::uint64_t sent = 0, sent_bytes = 0, received = 0, received_bytes = 0, refused = 0;
    lan::LanClock clock = lan::lan_steady_clock();
    std::function<void(const std::string&)> log;

    void set_ticket(std::uint32_t server_ip, const online::RelayTicket& ticket);
    void clear();
};

class RelaySocket final : public lan::LanSocket {
public:
    RelaySocket(std::unique_ptr<lan::LanSocket> inner, std::shared_ptr<RelayLink> link);
    bool open(std::uint16_t port, const lan::LanSocketOptions& options) override;
    void close() override { inner_->close(); }
    bool is_open() const override { return inner_->is_open(); }
    std::uint16_t local_port() const override { return inner_->local_port(); }
    bool send_to(const lan::LanAddress& to, std::span<const std::uint8_t> bytes) override;
    std::optional<std::size_t> receive_from(lan::LanAddress& from, std::span<std::uint8_t> buffer) override;
    std::vector<lan::LanAddress> broadcast_targets(std::uint16_t port) const override {
        return inner_->broadcast_targets(port);
    }

private:
    void keep_registered();

    std::unique_ptr<lan::LanSocket> inner_;
    std::shared_ptr<RelayLink> link_;
    std::uint32_t registered_generation_ = 0;
    std::uint64_t register_ms_ = 0;
    bool sent_register_ = false;
};

// The game socket factory of an online LanSession: `inner`'s sockets wrapped in RelaySockets over `link`.
lan::LanSocketFactory relay_socket_factory(lan::LanSocketFactory inner, std::shared_ptr<RelayLink> link);

}  // namespace omega
