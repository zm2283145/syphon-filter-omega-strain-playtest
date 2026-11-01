// PORT-ONLY (not in the original; LAN resilience testing, 133lt3): see lan_impaired_socket.hpp.
#include "lan_impaired_socket.hpp"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

namespace omega::lan {

namespace {

class ImpairedSocket final : public LanSocket {
public:
    ImpairedSocket(std::unique_ptr<LanSocket> inner, const LanImpairment& impairment, std::uint64_t stream,
                   LanClock clock, std::shared_ptr<LanImpairmentStats> stats)
        : inner_(std::move(inner)), impairment_(impairment), clock_(std::move(clock)), stats_(std::move(stats)),
          state_(impairment.seed * 0x100000001B3ull + stream) {}

    bool open(std::uint16_t port, const LanSocketOptions& options) override {
        held_.clear();
        return inner_->open(port, options);
    }
    void close() override {
        held_.clear();
        inner_->close();
    }
    bool is_open() const override { return inner_->is_open(); }
    std::uint16_t local_port() const override { return inner_->local_port(); }

    bool send_to(const LanAddress& to, std::span<const std::uint8_t> bytes) override {
        flush();
        if (!inner_->is_open()) return false;
        ++stats_->sent;
        // Every fate is drawn for every datagram, so one decision does not shift the next ones.
        const double loss_draw = next_unit();
        const double duplicate_draw = next_unit();
        const double reorder_draw = next_unit();
        const std::uint64_t delay_draw = next_u64();
        if (loss_draw < impairment_.loss) {
            ++stats_->dropped;
            return true;  // handed to the "network", which lost it
        }
        const std::uint64_t span = std::max<std::uint64_t>(1, impairment_.reorder_ms);
        const std::uint64_t now = clock_();
        if (reorder_draw < impairment_.reorder) {
            hold(to, bytes, now + 1 + delay_draw % span);
            ++stats_->delayed;
        } else {
            forward(to, bytes);
        }
        if (duplicate_draw < impairment_.duplicate) {
            hold(to, bytes, now + 1 + (delay_draw >> 20) % span);
            ++stats_->duplicated;
        }
        return true;
    }

    std::optional<std::size_t> receive_from(LanAddress& from, std::span<std::uint8_t> buffer) override {
        flush();
        return inner_->receive_from(from, buffer);
    }

    std::vector<LanAddress> broadcast_targets(std::uint16_t port) const override {
        return inner_->broadcast_targets(port);
    }

private:
    struct Held {
        LanAddress to;
        std::vector<std::uint8_t> bytes;
        std::uint64_t at = 0;
        std::uint64_t order = 0;
    };

    std::uint64_t next_u64() {  // splitmix64, as LanLoopbackNetwork
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double next_unit() { return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0); }

    void forward(const LanAddress& to, std::span<const std::uint8_t> bytes) {
        if (inner_->send_to(to, bytes)) ++stats_->forwarded;
    }
    void hold(const LanAddress& to, std::span<const std::uint8_t> bytes, std::uint64_t at) {
        held_.push_back(Held{to, std::vector<std::uint8_t>(bytes.begin(), bytes.end()), at, order_++});
    }
    void flush() {
        if (held_.empty() || !inner_->is_open()) return;
        const std::uint64_t now = clock_();
        std::vector<Held> due;
        for (auto it = held_.begin(); it != held_.end();) {
            if (it->at <= now) {
                due.push_back(std::move(*it));
                it = held_.erase(it);
            } else {
                ++it;
            }
        }
        std::sort(due.begin(), due.end(),
                  [](const Held& a, const Held& b) { return a.at != b.at ? a.at < b.at : a.order < b.order; });
        for (const Held& h : due) forward(h.to, h.bytes);
    }

    std::unique_ptr<LanSocket> inner_;
    LanImpairment impairment_;
    LanClock clock_;
    std::shared_ptr<LanImpairmentStats> stats_;
    std::uint64_t state_;
    std::uint64_t order_ = 0;
    std::vector<Held> held_;
};

bool parse_number(const std::string& text, double& out) {
    if (text.empty()) return false;
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end && *end == '\0';
}

}  // namespace

std::optional<LanImpairment> lan_parse_impairment(const std::string& text, std::string* error) {
    const auto fail = [&](const std::string& why) -> std::optional<LanImpairment> {
        if (error) *error = why;
        return std::nullopt;
    };
    LanImpairment out;
    std::stringstream in(text);
    std::string item;
    bool seen = false;
    while (std::getline(in, item, ',')) {
        if (item.empty()) continue;
        const auto eq = item.find('=');
        if (eq == std::string::npos) return fail("'" + item + "' is not key=value");
        const std::string key = item.substr(0, eq);
        double value = 0.0;
        if (!parse_number(item.substr(eq + 1), value)) return fail("'" + item + "' has no number");
        if (key == "loss" || key == "dup" || key == "duplicate" || key == "reorder") {
            if (!(value >= 0.0 && value <= 0.9)) return fail(key + " must be in [0, 0.9]");
            (key == "loss" ? out.loss : key == "reorder" ? out.reorder : out.duplicate) = value;
        } else if (key == "reorder_ms") {
            if (!(value >= 1.0 && value <= 2000.0)) return fail("reorder_ms must be in [1, 2000]");
            out.reorder_ms = static_cast<std::uint64_t>(value);
        } else if (key == "seed") {
            if (!(value >= 0.0 && value <= 4294967295.0)) return fail("seed must be a 32-bit number");
            out.seed = static_cast<std::uint64_t>(value);
        } else {
            return fail("unknown key '" + key + "' (loss, dup, reorder, reorder_ms, seed)");
        }
        seen = true;
    }
    if (!seen) return fail("empty impairment");
    return out;
}

std::string lan_impairment_text(const LanImpairment& i) {
    std::ostringstream out;
    out << "loss " << i.loss << ", dup " << i.duplicate << ", reorder " << i.reorder << " (1.." << i.reorder_ms
        << " ms), seed " << i.seed;
    return out.str();
}

std::unique_ptr<LanSocket> lan_impaired_socket(std::unique_ptr<LanSocket> inner, const LanImpairment& impairment,
                                               std::uint64_t stream, LanClock clock,
                                               std::shared_ptr<LanImpairmentStats> stats) {
    if (!stats) stats = std::make_shared<LanImpairmentStats>();
    return std::make_unique<ImpairedSocket>(std::move(inner), impairment, stream, std::move(clock), std::move(stats));
}

LanSocketFactory lan_impaired_socket_factory(LanSocketFactory inner, const LanImpairment& impairment,
                                             std::shared_ptr<LanImpairmentStats> stats, LanClock clock) {
    if (!stats) stats = std::make_shared<LanImpairmentStats>();
    auto counter = std::make_shared<std::uint64_t>(0);
    return [inner = std::move(inner), impairment, stats, clock = std::move(clock), counter] {
        return lan_impaired_socket(inner(), impairment, (*counter)++, clock, stats);
    };
}

}  // namespace omega::lan
