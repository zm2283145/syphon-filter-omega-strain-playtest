// PORT-ONLY (checkpoint 132on2e): see online_relay_socket.hpp.
#include "online_relay_socket.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace omega {

void RelayLink::set_ticket(std::uint32_t server_ip, const online::RelayTicket& ticket) {
    active = true;
    server = {server_ip, ticket.port};
    token = ticket.token;
    vip = ticket.vip;
    registered = false;
    ++generation;
}

void RelayLink::clear() {
    active = false;
    registered = false;
    vip = 0;
    ++generation;
}

RelaySocket::RelaySocket(std::unique_ptr<lan::LanSocket> inner, std::shared_ptr<RelayLink> link)
    : inner_(std::move(inner)), link_(std::move(link)) {}

bool RelaySocket::open(std::uint16_t port, const lan::LanSocketOptions& options) {
    if (!inner_->open(port, options)) return false;
    sent_register_ = false;
    keep_registered();
    return true;
}

void RelaySocket::keep_registered() {
    if (!link_->active || !inner_->is_open()) return;
    const std::uint64_t now = link_->clock();
    if (sent_register_ && registered_generation_ == link_->generation && now - register_ms_ < RelayLink::kRegisterMs) return;
    online::RelayPacket p;
    p.kind = online::relay::Kind::Register;
    p.token = link_->token;
    const auto bytes = online::encode_relay(p);
    inner_->send_to(link_->server, bytes);
    if (!sent_register_ || registered_generation_ != link_->generation) {
        if (link_->log)
            link_->log("relay: Register from local port " + std::to_string(inner_->local_port()) + " to " +
                       lan::lan_address_text(link_->server) + " (virtual " + online::relay_vip_text(link_->vip) + ")");
    }
    sent_register_ = true;
    registered_generation_ = link_->generation;
    register_ms_ = now;
}

bool RelaySocket::send_to(const lan::LanAddress& to, std::span<const std::uint8_t> bytes) {
    keep_registered();
    if (!online::is_relay_virtual_ip(to.ip)) return inner_->send_to(to, bytes);
    if (!link_->active || bytes.size() > online::relay::kMaxPayload) return false;
    online::RelayPacket p;
    p.kind = online::relay::Kind::Data;
    p.token = link_->token;
    p.vip = to.ip;
    p.payload.assign(bytes.begin(), bytes.end());
    if (!inner_->send_to(link_->server, online::encode_relay(p))) return false;
    ++link_->sent;
    link_->sent_bytes += bytes.size();
    return true;
}

std::optional<std::size_t> RelaySocket::receive_from(lan::LanAddress& from, std::span<std::uint8_t> buffer) {
    keep_registered();
    std::array<std::uint8_t, 2048> raw{};
    for (;;) {
        lan::LanAddress src;
        const auto n = inner_->receive_from(src, raw);
        if (!n) return std::nullopt;
        const std::span<const std::uint8_t> got(raw.data(), std::min(*n, raw.size()));
        const auto p = link_->active && src == link_->server ? online::decode_relay(got) : std::nullopt;
        if (!p) {
            const std::size_t k = std::min(got.size(), buffer.size());
            std::memcpy(buffer.data(), got.data(), k);
            from = src;
            return k;
        }
        switch (p->kind) {
        case online::relay::Kind::DataFrom: {
            const std::size_t k = std::min(p->payload.size(), buffer.size());
            std::memcpy(buffer.data(), p->payload.data(), k);
            from = {p->vip, online::relay::kVirtualPort};
            ++link_->received;
            link_->received_bytes += p->payload.size();
            return k;
        }
        case online::relay::Kind::Registered:
            if (p->vip == link_->vip && !link_->registered) {
                link_->registered = true;
                if (link_->log) link_->log("relay: registered as " + online::relay_vip_text(p->vip));
            }
            continue;
        case online::relay::Kind::Refused:
            ++link_->refused;
            link_->registered = false;
            if (link_->log && link_->refused == 1) link_->log("relay: Register refused (code " + std::to_string(p->code) + ")");
            continue;
        default: continue;
        }
    }
}

lan::LanSocketFactory relay_socket_factory(lan::LanSocketFactory inner, std::shared_ptr<RelayLink> link) {
    return [inner = std::move(inner), link = std::move(link)]() -> std::unique_ptr<lan::LanSocket> {
        return std::make_unique<RelaySocket>(inner(), link);
    };
}

}  // namespace omega
