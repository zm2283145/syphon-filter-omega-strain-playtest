// PORT-ONLY (not in the original; checkpoint 132ln): the in-memory LanSocket network for deterministic LAN
// tests. See lan_loopback.hpp and research/LAN_PLAY.md §8 phase 2.
#include "lan_loopback.hpp"

#include <algorithm>
#include <utility>

namespace omega::lan {

namespace {

class LoopbackSocket final : public LanSocket {
public:
    LoopbackSocket(std::shared_ptr<LanLoopbackNetwork> network, std::uint32_t ip)
        : network_(std::move(network)), ip_(ip) {}
    ~LoopbackSocket() override { close(); }

    bool open(std::uint16_t port, const LanSocketOptions& options) override {
        close();
        endpoint_ = network_->bind(ip_, port, options);
        return endpoint_ != nullptr;
    }
    void close() override {
        if (endpoint_) network_->unbind(endpoint_);
        endpoint_.reset();
    }
    bool is_open() const override { return endpoint_ != nullptr; }
    std::uint16_t local_port() const override { return endpoint_ ? endpoint_->port : 0; }
    bool send_to(const LanAddress& to, std::span<const std::uint8_t> bytes) override {
        return endpoint_ && network_->send(*endpoint_, to, bytes);
    }
    std::optional<std::size_t> receive_from(LanAddress& from, std::span<std::uint8_t> buffer) override {
        if (!endpoint_) return std::nullopt;
        auto datagram = network_->receive(*endpoint_);
        if (!datagram) return std::nullopt;
        from = datagram->from;
        const std::size_t size = std::min(buffer.size(), datagram->bytes.size());
        std::copy_n(datagram->bytes.begin(), size, buffer.begin());
        return size;
    }
    std::vector<LanAddress> broadcast_targets(std::uint16_t port) const override {
        return {LanAddress{kLanBroadcastIp, port}};
    }

private:
    std::shared_ptr<LanLoopbackNetwork> network_;
    std::uint32_t ip_;
    std::shared_ptr<LanLoopbackNetwork::Endpoint> endpoint_;
};

}  // namespace

std::shared_ptr<LanLoopbackNetwork> LanLoopbackNetwork::make(LanClock clock, std::uint64_t seed) {
    return std::shared_ptr<LanLoopbackNetwork>(new LanLoopbackNetwork(std::move(clock), seed));
}

std::unique_ptr<LanSocket> LanLoopbackNetwork::socket(std::uint32_t ip) {
    return std::make_unique<LoopbackSocket>(shared_from_this(), ip);
}

LanSocketFactory LanLoopbackNetwork::factory(std::uint32_t ip) {
    auto self = shared_from_this();
    return [self, ip] { return self->socket(ip); };
}

std::uint64_t LanLoopbackNetwork::next_u64() {
    // splitmix64: fixed arithmetic, so every platform draws the same stream for a seed.
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

double LanLoopbackNetwork::next_unit() {
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

std::shared_ptr<LanLoopbackNetwork::Endpoint> LanLoopbackNetwork::bind(std::uint32_t ip, std::uint16_t port,
                                                                       const LanSocketOptions& options) {
    if (port == 0) {
        for (int tries = 0; tries < 16384 && port == 0; ++tries) {
            const std::uint16_t candidate = next_ephemeral_;
            next_ephemeral_ = static_cast<std::uint16_t>(next_ephemeral_ == 65535 ? 49152 : next_ephemeral_ + 1);
            const bool used = std::any_of(endpoints_.begin(), endpoints_.end(),
                                          [&](const auto& e) { return e->ip == ip && e->port == candidate; });
            if (!used) port = candidate;
        }
        if (port == 0) return nullptr;
    } else {
        for (const auto& existing : endpoints_)
            if (existing->ip == ip && existing->port == port && !(existing->reuse && options.reuse_address))
                return nullptr;
    }
    auto endpoint = std::make_shared<Endpoint>();
    endpoint->ip = ip;
    endpoint->port = port;
    endpoint->open = true;
    endpoint->broadcast = options.broadcast;
    endpoint->reuse = options.reuse_address;
    endpoints_.push_back(endpoint);
    return endpoint;
}

void LanLoopbackNetwork::unbind(const std::shared_ptr<Endpoint>& endpoint) {
    endpoint->open = false;
    endpoints_.erase(std::remove(endpoints_.begin(), endpoints_.end(), endpoint), endpoints_.end());
}

void LanLoopbackNetwork::set_blocked(std::uint32_t ip, bool blocked) {
    blocked_.erase(std::remove(blocked_.begin(), blocked_.end(), ip), blocked_.end());
    if (blocked) blocked_.push_back(ip);
}

void LanLoopbackNetwork::enqueue(Endpoint& to, const LanAddress& from, std::span<const std::uint8_t> bytes,
                                 std::uint64_t at) {
    to.inbox.push_back(Datagram{from, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), at, order_++});
}

bool LanLoopbackNetwork::send(const Endpoint& from, const LanAddress& to, std::span<const std::uint8_t> bytes) {
    const bool is_broadcast = to.ip == kLanBroadcastIp || (to.ip & 0xFFu) == 0xFFu;
    if (is_broadcast && !from.broadcast) return false;  // EACCES without SO_BROADCAST
    ++stats_.sent;
    const LanAddress source{from.ip, from.port};
    std::vector<Endpoint*> targets;
    for (const auto& endpoint : endpoints_) {
        if (endpoint->port != to.port) continue;
        const bool match = to.ip == kLanBroadcastIp ? true
                           : is_broadcast           ? (endpoint->ip & 0xFFFFFF00u) == (to.ip & 0xFFFFFF00u)
                                                    : endpoint->ip == to.ip;
        if (!match) continue;
        targets.push_back(endpoint.get());
        if (!is_broadcast) break;  // a unicast to a shared port reaches one socket
    }
    const auto blocked = [&](std::uint32_t ip) {
        return std::find(blocked_.begin(), blocked_.end(), ip) != blocked_.end();
    };
    const std::uint64_t now = clock_();
    for (Endpoint* target : targets) {
        // Every fate is drawn for every target, so a run's draws do not depend on which sockets are open.
        const double loss_draw = next_unit();
        const double duplicate_draw = next_unit();
        const double reorder_draw = next_unit();
        const std::uint64_t delay_draw = next_u64();
        if (blocked(from.ip) || blocked(target->ip) || loss_draw < conditions_.loss) {
            ++stats_.dropped;
            continue;
        }
        std::uint64_t at = now + conditions_.latency_ms;
        if (reorder_draw < conditions_.reorder && conditions_.reorder_ms > 0) {
            at += 1 + delay_draw % conditions_.reorder_ms;
            ++stats_.reordered;
        }
        enqueue(*target, source, bytes, at);
        if (duplicate_draw < conditions_.duplicate) {
            enqueue(*target, source, bytes, at + 1 + delay_draw % 7);
            ++stats_.duplicated;
        }
    }
    return true;
}

std::optional<LanLoopbackNetwork::Datagram> LanLoopbackNetwork::receive(Endpoint& endpoint) {
    const std::uint64_t now = clock_();
    auto best = endpoint.inbox.end();
    for (auto it = endpoint.inbox.begin(); it != endpoint.inbox.end(); ++it) {
        if (it->deliver_at > now) continue;
        if (best == endpoint.inbox.end() || it->deliver_at < best->deliver_at ||
            (it->deliver_at == best->deliver_at && it->order < best->order))
            best = it;
    }
    if (best == endpoint.inbox.end()) return std::nullopt;
    Datagram datagram = std::move(*best);
    endpoint.inbox.erase(best);
    ++stats_.delivered;
    return datagram;
}

void LanLoopbackNetwork::reverse_in_flight() {
    for (const auto& endpoint : endpoints_) {
        auto& inbox = endpoint->inbox;
        std::sort(inbox.begin(), inbox.end(), [](const Datagram& a, const Datagram& b) {
            return a.deliver_at != b.deliver_at ? a.deliver_at < b.deliver_at : a.order < b.order;
        });
        const std::size_t count = inbox.size();
        for (std::size_t i = 0; i < count / 2; ++i) {
            std::swap(inbox[i].deliver_at, inbox[count - 1 - i].deliver_at);
            std::swap(inbox[i].order, inbox[count - 1 - i].order);
        }
    }
}

}  // namespace omega::lan
