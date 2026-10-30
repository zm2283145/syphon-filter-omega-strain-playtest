#include "agent_network_codec.hpp"

#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {

constexpr std::size_t kMaxPayload = 0x100;  // 002823e0: 0x10c including the 12-byte envelope

std::uint32_t word(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4)
        throw std::runtime_error("AgentData network codec: word outside record");
    return std::uint32_t(bytes[offset]) | std::uint32_t(bytes[offset + 1]) << 8 |
           std::uint32_t(bytes[offset + 2]) << 16 | std::uint32_t(bytes[offset + 3]) << 24;
}

void put_word(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void put_short(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint8_t byte() {
        need(1);
        return bytes_[at_++];
    }
    std::uint16_t short_word() {
        need(2);
        const auto v = std::uint16_t(bytes_[at_]) | std::uint16_t(bytes_[at_ + 1]) << 8;
        at_ += 2;
        return static_cast<std::uint16_t>(v);
    }
    std::uint32_t long_word() {
        need(4);
        const auto v = word(bytes_, at_);
        at_ += 4;
        return v;
    }
    std::span<const std::uint8_t> take(std::size_t n) {
        need(n);
        const auto v = bytes_.subspan(at_, n);
        at_ += n;
        return v;
    }
    std::size_t remaining() const { return bytes_.size() - at_; }
private:
    void need(std::size_t n) const {
        if (n > bytes_.size() - at_)
            throw std::runtime_error("AgentData network codec: truncated packet");
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t at_{};
};

}  // namespace

std::vector<std::uint8_t> encode_agent_network_update(
    std::uint8_t client, std::uint8_t stats_only, std::uint8_t awards,
    std::span<const std::uint8_t, agent_record_size> agent, const AgentLoadState& state) {
    if (client >= 4) throw std::runtime_error("AgentData network codec: client outside four slots");
    std::vector<std::uint8_t> out{client, stats_only, awards};
    if (stats_only) {
        // 00330f00: statistics 14..30 only, followed by no other fields.
        for (std::size_t i = 14; i < 31; ++i) put_word(out, word(agent, 8 + i * 4));
    } else {
        // 00331870: eight cBitArray_224 chunks, 31 statistics, one word per
        // current level record, the name, time bits and completion byte.
        if (word(agent, 0xc8) != state.level_records.size())
            throw std::runtime_error("AgentData network codec: +c8 level count differs from state");
        for (std::size_t i = 0; i < 8; ++i) put_word(out, word(agent, 0x84 + i * 4));
        for (std::size_t i = 0; i < 31; ++i) put_word(out, word(agent, 8 + i * 4));
        for (const auto& level : state.level_records) put_word(out, word(level, 0));
        const auto name_begin = agent.begin() + 0x8ac;
        const auto name_end = name_begin + 16;
        const auto nul = std::find(name_begin, name_end, std::uint8_t{0});
        if (nul == name_end) throw std::runtime_error("AgentData network codec: name lacks NUL in 16 bytes");
        const auto length = static_cast<std::uint16_t>(nul - name_begin + 1);
        put_short(out, length);  // 00281c80: strlen(name)+1, little-endian
        out.insert(out.end(), name_begin, nul + 1);
        put_word(out, word(agent, 0xd4));
        out.push_back(agent[0xb74]);
    }
    if (out.size() > kMaxPayload)
        throw std::runtime_error("AgentData network codec: stream exceeds 256-byte packet data");
    return out;
}

AgentNetworkUpdate decode_agent_network_update(std::span<const std::uint8_t> bytes,
                                               std::size_t expected_level_records) {
    if (bytes.size() > kMaxPayload)
        throw std::runtime_error("AgentData network codec: stream exceeds 256-byte packet data");
    Reader in(bytes);
    AgentNetworkUpdate out;
    out.client = in.byte();
    out.stats_only = in.byte();
    out.awards = in.byte();
    if (out.client >= 4) throw std::runtime_error("AgentData network codec: client outside four slots");
    if (out.stats_only) {
        for (auto& v : out.stats_tail) v = in.long_word();
    } else {
        // The level count is not on the wire; +c8 of the receiving record
        // controls how many words the original 003314f0 consumes.
        for (auto& v : out.objective_bits) v = in.long_word();
        for (auto& v : out.statistics) v = in.long_word();
        if (expected_level_records > in.remaining() / 4)
            throw std::runtime_error("AgentData network codec: level records exceed packet");
        out.level_words.reserve(expected_level_records);
        for (std::size_t i = 0; i < expected_level_records; ++i) out.level_words.push_back(in.long_word());
        const auto n = in.short_word();
        if (n == 0 || n > 16)
            throw std::runtime_error("AgentData network codec: name length outside 1..16");
        const auto name = in.take(n);
        if (name.back() != 0 || std::find(name.begin(), name.end() - 1, std::uint8_t{0}) != name.end() - 1)
            throw std::runtime_error("AgentData network codec: name is not a single NUL-terminated string");
        out.name.assign(reinterpret_cast<const char*>(name.data()), n - 1);
        out.mission_time_bits = in.long_word();
        out.completed = in.byte();
    }
    if (in.remaining() != 0) throw std::runtime_error("AgentData network codec: trailing packet bytes");
    return out;
}

}  // namespace omega
