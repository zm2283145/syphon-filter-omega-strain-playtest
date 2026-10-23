// PORT-ONLY (checkpoint 132on2e): see online_relay.hpp.
#include "online_relay.hpp"

#include "online_hash.hpp"
#include "online_protocol.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace omega::online {
namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint16_t read_u16(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off] | (b[off + 1] << 8));
}
std::uint32_t read_u32(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}
bool same_token(const RelayToken& a, const RelayToken& b) {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}
RelayToken random_token() {
    const auto r = random_bytes(relay::kTokenSize);
    RelayToken t{};
    std::copy_n(r.begin(), std::min(r.size(), t.size()), t.begin());
    return t;
}

}  // namespace

std::string relay_vip_text(std::uint32_t vip) {
    return std::to_string(vip >> 24) + "." + std::to_string((vip >> 16) & 0xff) + "." + std::to_string((vip >> 8) & 0xff) +
           "." + std::to_string(vip & 0xff);
}

std::vector<std::uint8_t> encode_relay(const RelayPacket& p) {
    std::vector<std::uint8_t> out;
    out.reserve(relay::kHeaderSize + relay::kTokenSize + 4 + p.payload.size());
    put_u32(out, relay::kMagic);
    out.push_back(static_cast<std::uint8_t>(p.kind));
    out.push_back(0);
    put_u16(out, p.code);
    switch (p.kind) {
    case relay::Kind::Register: out.insert(out.end(), p.token.begin(), p.token.end()); break;
    case relay::Kind::Registered: put_u32(out, p.vip); break;
    case relay::Kind::Data:
        out.insert(out.end(), p.token.begin(), p.token.end());
        put_u32(out, p.vip);
        out.insert(out.end(), p.payload.begin(), p.payload.end());
        break;
    case relay::Kind::DataFrom:
        put_u32(out, p.vip);
        out.insert(out.end(), p.payload.begin(), p.payload.end());
        break;
    case relay::Kind::Refused: break;
    }
    return out;
}

std::optional<RelayPacket> decode_relay(std::span<const std::uint8_t> b) {
    if (b.size() < relay::kHeaderSize || read_u32(b, 0) != relay::kMagic) return std::nullopt;
    RelayPacket p;
    p.kind = static_cast<relay::Kind>(b[4]);
    p.code = read_u16(b, 6);
    const std::span<const std::uint8_t> rest = b.subspan(relay::kHeaderSize);
    auto payload = [&](std::size_t off) {
        if (rest.size() - off > relay::kMaxPayload) return false;
        p.payload.assign(rest.begin() + static_cast<std::ptrdiff_t>(off), rest.end());
        return true;
    };
    switch (p.kind) {
    case relay::Kind::Register:
        if (rest.size() != relay::kTokenSize) return std::nullopt;
        std::copy_n(rest.begin(), relay::kTokenSize, p.token.begin());
        return p;
    case relay::Kind::Registered:
        if (rest.size() != 4) return std::nullopt;
        p.vip = read_u32(rest, 0);
        return p;
    case relay::Kind::Data:
        if (rest.size() < relay::kTokenSize + 4) return std::nullopt;
        std::copy_n(rest.begin(), relay::kTokenSize, p.token.begin());
        p.vip = read_u32(rest, relay::kTokenSize);
        if (!payload(relay::kTokenSize + 4)) return std::nullopt;
        return p;
    case relay::Kind::DataFrom:
        if (rest.size() < 4) return std::nullopt;
        p.vip = read_u32(rest, 0);
        if (!payload(4)) return std::nullopt;
        return p;
    case relay::Kind::Refused:
        if (!rest.empty()) return std::nullopt;
        return p;
    }
    return std::nullopt;
}

std::vector<std::uint8_t> encode_relay_ticket(const RelayTicket& t) {
    return ExtWriter()
        .u16(t.port)
        .u32(t.vip)
        .str(std::string_view(reinterpret_cast<const char*>(t.token.data()), t.token.size()))
        .bytes();
}

std::optional<RelayTicket> decode_relay_ticket(std::span<const std::uint8_t> body) {
    ExtReader r(body);
    RelayTicket t;
    t.port = r.u16();
    t.vip = r.u32();
    const std::string token = r.str();
    if (!r.ok() || token.size() != relay::kTokenSize || t.port == 0 || !is_relay_virtual_ip(t.vip)) return std::nullopt;
    std::memcpy(t.token.data(), token.data(), relay::kTokenSize);
    return t;
}

std::vector<std::uint8_t> encode_relay_peer(const RelayPeer& p) {
    return ExtWriter().u32(p.world).u32(p.vip).u16(p.port).bytes();
}

std::optional<RelayPeer> decode_relay_peer(std::span<const std::uint8_t> body) {
    ExtReader r(body);
    RelayPeer p;
    p.world = r.u32();
    p.vip = r.u32();
    p.port = r.u16();
    if (!r.ok() || p.world == 0 || !is_relay_virtual_ip(p.vip) || p.port == 0) return std::nullopt;
    return p;
}

RelayCore::RelayCore(RelayLimits limits, SendFn send, WorldFn world_of, TokenFn token, LogFn log)
    : limits_(limits), send_(std::move(send)), world_of_(std::move(world_of)),
      token_(token ? std::move(token) : TokenFn(random_token)), log_(std::move(log)) {}

void RelayCore::log(const std::string& line) const {
    if (log_) log_(line);
}

std::optional<RelayTicket> RelayCore::open(std::uint32_t conn, std::uint16_t udp_port, const std::string& name) {
    close(conn);
    if (limits_.max_endpoints > 0 && endpoints_.size() >= limits_.max_endpoints) {
        log("relay: no endpoint for " + name + " (conn " + std::to_string(conn) + "): relay_max_endpoints=" +
            std::to_string(limits_.max_endpoints) + " reached");
        return std::nullopt;
    }
    Endpoint e;
    e.conn = conn;
    e.name = name;
    e.token = token_();
    // The next free index in 198.18.0.1 .. 198.19.255.254 (the /15 without its first and last address).
    constexpr std::uint32_t span = ~relay::kVirtualMask - 1;
    for (std::uint32_t tries = 0; tries < span; ++tries) {
        const std::uint32_t vip = relay::kVirtualBase + next_index_;
        next_index_ = next_index_ % span + 1;
        if (!by_vip(vip)) {
            e.vip = vip;
            break;
        }
    }
    if (!e.vip) return std::nullopt;
    endpoints_[conn] = e;
    log("relay: endpoint " + relay_vip_text(e.vip) + " for " + name + " (conn " + std::to_string(conn) + ")");
    return RelayTicket{udp_port, e.vip, e.token};
}

void RelayCore::close(std::uint32_t conn) {
    auto it = endpoints_.find(conn);
    if (it == endpoints_.end()) return;
    log("relay: endpoint " + relay_vip_text(it->second.vip) + " (" + it->second.name + ") closed: " +
        std::to_string(it->second.sent) + " datagram(s) sent, " + std::to_string(it->second.received) + " received");
    endpoints_.erase(it);
}

std::optional<std::uint32_t> RelayCore::virtual_ip(std::uint32_t conn) const {
    auto it = endpoints_.find(conn);
    if (it == endpoints_.end()) return std::nullopt;
    return it->second.vip;
}

RelayCore::Endpoint* RelayCore::by_token(const RelayToken& t) {
    Endpoint* found = nullptr;
    for (auto& [conn, e] : endpoints_)
        if (same_token(e.token, t)) found = &e;
    return found;
}

RelayCore::Endpoint* RelayCore::by_vip(std::uint32_t vip) {
    for (auto& [conn, e] : endpoints_)
        if (e.vip == vip) return &e;
    return nullptr;
}

bool RelayCore::within_rate(Endpoint& e, std::size_t bytes, std::uint64_t now_ms) {
    const double packet_cap = limits_.packet_rate > 0 ? limits_.packet_rate : 0;
    const double byte_cap = limits_.byte_rate > 0 ? limits_.byte_rate : 0;
    if (e.bucket_at == 0) {
        e.packets = packet_cap;
        e.bytes = byte_cap;
    } else if (now_ms > e.bucket_at) {
        const double dt = static_cast<double>(now_ms - e.bucket_at) / 1000.0;
        e.packets = std::min(packet_cap, e.packets + dt * packet_cap);
        e.bytes = std::min(byte_cap, e.bytes + dt * byte_cap);
    }
    e.bucket_at = std::max<std::uint64_t>(now_ms, 1);
    if (limits_.packet_rate > 0 && e.packets < 1) return false;
    if (limits_.byte_rate > 0 && e.bytes < static_cast<double>(bytes)) return false;
    if (limits_.packet_rate > 0) e.packets -= 1;
    if (limits_.byte_rate > 0) e.bytes -= static_cast<double>(bytes);
    return true;
}

void RelayCore::on_datagram(std::uint32_t ip, std::uint16_t port, std::span<const std::uint8_t> bytes,
                            std::uint64_t now_ms) {
    ++stats_.received;
    const auto p = decode_relay(bytes);
    if (!p || (p->kind != relay::Kind::Register && p->kind != relay::Kind::Data)) {
        ++stats_.dropped_bad;
        return;
    }
    Endpoint* src = by_token(p->token);
    if (!src) {
        ++stats_.dropped_token;
        if (p->kind == relay::Kind::Register) {  // smaller than the request: no amplification
            RelayPacket r;
            r.kind = relay::Kind::Refused;
            r.code = relay::UnknownToken;
            send_(ip, port, encode_relay(r));
        }
        return;
    }
    if (src->ip != ip || src->port != port) {
        log("relay: " + relay_vip_text(src->vip) + " (" + src->name + ") at " + std::to_string(ip >> 24) + "." +
            std::to_string((ip >> 16) & 0xff) + "." + std::to_string((ip >> 8) & 0xff) + "." + std::to_string(ip & 0xff) +
            ":" + std::to_string(port) + (src->ip ? " (moved)" : ""));
        src->ip = ip;
        src->port = port;
    }
    src->last_seen = std::max<std::uint64_t>(now_ms, 1);
    if (p->kind == relay::Kind::Register) {
        ++stats_.registers;
        RelayPacket r;
        r.kind = relay::Kind::Registered;
        r.vip = src->vip;
        send_(ip, port, encode_relay(r));
        return;
    }
    if (!within_rate(*src, p->payload.size(), now_ms)) {
        ++stats_.dropped_rate;
        return;
    }
    Endpoint* dst = by_vip(p->vip);
    const std::uint32_t world = world_of_ ? world_of_(src->conn) : 0;
    if (!dst || dst == src || world == 0 || !world_of_ || world_of_(dst->conn) != world) {
        ++stats_.dropped_world;
        return;
    }
    if (!dst->ip || (limits_.idle_ms > 0 && now_ms > dst->last_seen + limits_.idle_ms)) {
        ++stats_.dropped_unregistered;
        return;
    }
    if (src->sent == 0 || dst->received == 0)
        log("relay: world " + std::to_string(world) + ": first datagram " + relay_vip_text(src->vip) + " (" + src->name +
            ") -> " + relay_vip_text(dst->vip) + " (" + dst->name + ")");
    RelayPacket out;
    out.kind = relay::Kind::DataFrom;
    out.vip = src->vip;
    out.payload = p->payload;
    ++src->sent;
    ++dst->received;
    ++stats_.forwarded;
    stats_.forwarded_bytes += p->payload.size();
    send_(dst->ip, dst->port, encode_relay(out));
}

std::string RelayCore::status(std::uint64_t now_ms) const {
    std::ostringstream s;
    s << "relay: " << endpoints_.size() << " endpoint(s) (relay_max_endpoints=" << limits_.max_endpoints
      << " relay_packet_rate=" << limits_.packet_rate << " relay_byte_rate=" << limits_.byte_rate
      << " relay_idle=" << limits_.idle_ms / 1000 << "); " << stats_.received << " datagram(s) received, "
      << stats_.registers << " register(s), " << stats_.forwarded << " forwarded (" << stats_.forwarded_bytes
      << " bytes); dropped: " << stats_.dropped_bad << " bad, " << stats_.dropped_token << " unknown token, "
      << stats_.dropped_world << " not the same world, " << stats_.dropped_unregistered << " unregistered or idle, "
      << stats_.dropped_rate << " over the rate";
    for (const auto& [conn, e] : endpoints_) {
        s << "\n  " << relay_vip_text(e.vip) << " " << e.name << " conn " << conn << " world "
          << (world_of_ ? world_of_(conn) : 0) << ": ";
        if (e.ip)
            s << relay_vip_text(e.ip) << ":" << e.port << ", seen " << (now_ms >= e.last_seen ? (now_ms - e.last_seen) / 1000 : 0)
              << " s ago";
        else
            s << "not registered";
        s << ", " << e.sent << " sent, " << e.received << " received";
    }
    return s.str();
}

}  // namespace omega::online
