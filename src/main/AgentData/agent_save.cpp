#include "agent_save.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}

void write_u32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    std::memcpy(bytes.data() + offset, &value, 4);
}

class StreamCursor {
public:
    explicit StreamCursor(AgentSaveBuffer& buffer) : buffer_(buffer) {}
    void word(std::uint32_t value) {
        check(4);
        write_u32(buffer_, at_, value);
        at_ += 4;
    }
    void bytes(std::span<const std::uint8_t> source) {
        check(source.size());
        std::copy(source.begin(), source.end(), buffer_.begin() + static_cast<std::ptrdiff_t>(at_));
        at_ += source.size();
    }
    void skip(std::size_t n) {
        check(n);
        at_ += n;
    }
    std::size_t at() const { return at_; }

private:
    void check(std::size_t n) const {
        if (at_ + n > buffer_.size()) throw std::runtime_error("Agent save: the stream overruns the 0x4e20 buffer");
    }
    AgentSaveBuffer& buffer_;
    std::size_t at_ = 0;
};

}  // namespace

std::uint32_t agent_save_key_word(std::uint32_t t0_count, std::uint32_t seed_533870) {
    std::uint32_t x = t0_count + seed_533870;
    x += x << 16;
    return x ^ 0xffffu;
}

std::optional<std::string> encode_agent_file_name(std::span<const std::uint8_t> name, std::int32_t slot,
    bool font_ready, std::int32_t glyph_count, std::uint32_t max) {
    if (!font_ready || glyph_count == 0) return std::nullopt;
    const auto end = std::find(name.begin(), name.end(), std::uint8_t{0});
    if (end == name.end()) throw std::runtime_error("Agent file name: the name has no terminating NUL");
    const auto length = static_cast<std::uint32_t>(end - name.begin());
    if (!(length * 2 + 1 < max)) return std::nullopt;
    if (slot < 0 || slot >= 10) return std::nullopt;
    std::string out;
    for (auto it = name.begin(); it != end; ++it) {
        const std::int32_t c = *it;
        if (c < 0x20) return std::nullopt;
        const std::int32_t index = c - 0x20;
        if (!(index < glyph_count)) return std::nullopt;
        out.push_back(static_cast<char>('0' + index / 0x3e));
        out.push_back(static_cast<char>('@' + index % 0x3e));
    }
    out.push_back(static_cast<char>('0' + slot));
    return out;
}

void agent_save_cipher(std::span<std::uint8_t> buffer, std::string_view key) {
    if (buffer.size() < 8) throw std::runtime_error("Agent save cipher: the buffer is shorter than the header");
    if (key.empty()) throw std::runtime_error("Agent save cipher: empty key");
    auto k = static_cast<std::int8_t>(buffer[4]);
    const auto step = static_cast<std::int8_t>(buffer[5]);
    std::size_t j = 0;
    for (std::size_t i = 8; i < buffer.size(); ++i) {
        buffer[i] ^= static_cast<std::uint8_t>(k + static_cast<std::int8_t>(key[j]));
        if (++j >= key.size()) {
            j = 0;
            k = static_cast<std::int8_t>(k + step);
        }
    }
}

AgentSaveResult save_agent(AgentSaveBuffer& buffer, std::span<std::uint8_t, agent_record_size> agent,
    const AgentSaveSources& sources, const AgentCardState& card, const AgentSaveWriter& write) {
    AgentSaveResult result;
    if (agent[0x8ac] == 0) return result;
    result.flushed_messages = true;
    if (card.busy || !card.card_present) return result;
    const std::span<const std::uint8_t> record = agent;
    const auto record_count = static_cast<std::int32_t>(read_u32(record, 0xc8));
    const std::size_t records = record_count > 0 ? static_cast<std::size_t>(record_count) : 0;
    if (sources.level_records.size() != records * agent_level_record_size)
        throw std::runtime_error("Agent save: level records do not match the agent's +c8 count");
    StreamCursor out(buffer);
    out.word(agent_save_version);
    out.word(sources.key_word);
    out.bytes(record.subspan(0x8, 0x38));
    out.bytes(record.subspan(0x84, 0x20));
    out.bytes(record.subspan(0x8ac, 0x10));
    out.bytes(record.subspan(0x8cc, 0x10));
    out.bytes(record.subspan(0x8bc, 0x10));
    out.bytes(record.subspan(0x8dc, 0x40));
    out.bytes(record.subspan(0x91c, 0x80));
    out.word(read_u32(record, 0x99c));
    out.bytes(record.subspan(0x95c, 0x40));
    out.word(read_u32(record, 0x9a0));
    out.bytes(sources.appearance);
    out.bytes(record.subspan(0x9a8, 0x100));
    out.word(read_u32(record, 0xd8));
    out.bytes(record.subspan(0xdc, 0x7d0));
    out.bytes(sources.level_records);
    out.bytes(record.subspan(0xb68, 1));
    out.skip(3);
    out.word(sources.message_count);
    for (std::size_t i = 0; i < agent_message_slots; ++i) {
        if (static_cast<std::int32_t>(i) < static_cast<std::int32_t>(sources.message_count))
            out.bytes(sources.messages.subspan(i * agent_message_entry_size, agent_message_entry_size));
        else
            out.bytes(sources.stack_filler);
    }
    std::uint32_t checksum = 0;
    for (std::size_t i = 0; i < out.at(); i += 4) checksum ^= read_u32(buffer, i);
    out.word(checksum);
    agent_save_cipher(buffer);
    result.size = out.at();
    const auto name = encode_agent_file_name(record.subspan(0x8ac), static_cast<std::int32_t>(read_u32(record, 4)),
        card.font_ready, card.glyph_count);
    result.written = true;
    result.result = write(name ? *name : std::string{}, std::span<const std::uint8_t>(buffer).first(result.size));
    agent[0xb72] = 0;
    agent[0xb73] = 0;
    return result;
}

}  // namespace omega
