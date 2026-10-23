#include "agent_load.hpp"
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

void zero(std::span<std::uint8_t> agent, std::size_t from, std::size_t to) {
    std::fill(agent.begin() + static_cast<std::ptrdiff_t>(from), agent.begin() + static_cast<std::ptrdiff_t>(to),
        std::uint8_t{0});
}

std::size_t element_count(std::span<const std::uint8_t> agent, std::size_t offset) {
    const auto count = static_cast<std::int32_t>(read_u32(agent, offset));
    return count > 0 ? static_cast<std::size_t>(count) : 0;
}

// The load's cursor *0052fdf0 over the buffer. The cursor global sits right
// after the buffer, so a read past 0x4e20 would read the cursor itself.
class LoadCursor {
public:
    explicit LoadCursor(const AgentSaveBuffer& buffer) : buffer_(buffer) {}
    std::span<const std::uint8_t> take(std::size_t n) {
        check(n);
        const auto out = std::span<const std::uint8_t>(buffer_).subspan(at_, n);
        at_ += n;
        return out;
    }
    std::uint32_t word() { return read_u32(take(4), 0); }
    void skip(std::size_t n) {
        check(n);
        at_ += n;
    }
    std::size_t at() const { return at_; }

private:
    void check(std::size_t n) const {
        if (at_ + n > buffer_.size()) throw std::runtime_error("Agent load: the stream overruns the 0x4e20 buffer");
    }
    const AgentSaveBuffer& buffer_;
    std::size_t at_ = 0;
};

void copy_to(std::span<std::uint8_t> agent, std::size_t offset, std::span<const std::uint8_t> source) {
    std::copy(source.begin(), source.end(), agent.begin() + static_cast<std::ptrdiff_t>(offset));
}

// 00129360 (strcpy) into the record. Only `source` is defined; a string
// without a NUL in it would read stale memory. The longest strings run over
// the neighbouring fields (reproduced: later reads overwrite some of them),
// but one reaching the appearance pointer +9a4 would corrupt it.
void copy_string(std::span<std::uint8_t> agent, std::size_t offset, std::span<const std::uint8_t> source) {
    const auto end = std::find(source.begin(), source.end(), std::uint8_t{0});
    if (end == source.end()) throw std::runtime_error("Agent load: a string has no NUL in the bytes it was copied from");
    const auto n = static_cast<std::size_t>(end - source.begin()) + 1;
    if (offset + n > 0x9a4) throw std::runtime_error("Agent load: a string overruns the appearance pointer +9a4");
    copy_to(agent, offset, source.first(n));
}

// The < 0x10 string form: length word, the bytes into 0052fe00 (0x80 bytes;
// the message count follows), the cursor rounded up to a word, then strcpy
// from the scratch.
void read_short_string(LoadCursor& in, AgentLoadState& state, std::span<std::uint8_t> agent, std::size_t offset) {
    const std::uint32_t length = in.word();
    if (length > agent_string_scratch_size)
        throw std::runtime_error("Agent load: a string length overruns the 0052fe00 scratch");
    const auto bytes = in.take(length);
    std::copy(bytes.begin(), bytes.end(), state.string_scratch.begin());
    in.skip(((length + 3) & ~3u) - length);
    copy_string(agent, offset, state.string_scratch);
}

}  // namespace

void reset_agent(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state) {
    const std::span<const std::uint8_t> record = agent;
    for (std::size_t i = 0; i < agent_award_lists; ++i)
        if (state.awards[i].size() != element_count(record, 0xaac + 0x10 * i))
            throw std::runtime_error("Agent reset: an award list does not match its count word");
    if (state.award_bits.size() != element_count(record, 0xafc))
        throw std::runtime_error("Agent reset: the +b00 bytes do not match the +afc count");
    if (state.level_records.size() != read_u32(record, 0xc8))
        throw std::runtime_error("Agent reset: the level records do not match the +c8 count");
    const auto levels = static_cast<std::uint32_t>(state.level_count);
    if (levels > agent_level_vector_limit) throw std::runtime_error("Agent reset: the level count aborts the vector insert");

    zero(agent, 0x8, 0x84);
    for (auto& list : state.awards)
        for (auto& entry : list) {
            entry.flag = 0;
            std::fill(entry.bits.begin(), entry.bits.end(), std::uint8_t{0});
        }
    std::fill(state.award_bits.begin(), state.award_bits.end(), std::uint8_t{0});
    zero(agent, 0x84, 0xc4);
    zero(agent, 0xb08, 0xb50);
    write_u32(agent, 0xd8, 0);
    write_u32(agent, 0xb50, 0);
    agent[0xb71] = 0;
    agent[0xb70] = 0;
    // 00337490(agent + c4, 0, ...) then 00337490(agent + c4, *00532af4, fill).
    state.level_records.clear();
    AgentLevelRecord fill{};
    for (const std::size_t word : {0u, 1u, 4u, 5u}) write_u32(fill, 4 * word, 0xffffffffu);
    write_u32(fill, 0x18, state.reset_record_tail[0]);
    write_u32(fill, 0x1c, state.reset_record_tail[1]);
    state.level_records.assign(levels, fill);
    write_u32(agent, 0xc8, levels);
    agent[0xb72] = 0;
    agent[0xb74] = 0;
    write_u32(agent, 0xb6c, 0xffffffffu);
    agent[0xb68] = 0;
}

AgentLoadResult load_agent(AgentSaveBuffer& buffer, std::span<std::uint8_t, agent_record_size> agent,
    std::optional<std::span<const std::uint8_t>> name, std::int32_t slot, AgentLoadState& state,
    const AgentCardState& card, const AgentLoadReader& read) {
    AgentLoadResult result;
    const std::span<const std::uint8_t> record = agent;
    const auto file_name = encode_agent_file_name(name ? *name : record.subspan(0x8ac),
        slot < 0 ? static_cast<std::int32_t>(read_u32(record, 4)) : slot, card.font_ready, card.glyph_count);
    result.file_name = file_name ? *file_name : std::string{};
    reset_agent(agent, state);
    if (card.busy || !card.card_present || !card.card_formatted) return result;
    result.cursor = 0;
    result.read_called = true;
    if (!read(result.file_name, buffer)) return result;
    state.last_card_id = card.card_id;

    LoadCursor in(buffer);
    const auto version = static_cast<std::int32_t>(in.word());
    write_u32(agent, 0, static_cast<std::uint32_t>(version));
    const auto finish = [&](std::int32_t value) {
        result.cursor = in.at();
        result.result = value;
        return result;
    };
    if (version >= 0xc) {
        in.skip(4);
        agent_save_cipher(buffer);
    }
    copy_to(agent, 0x8, in.take(version >= 0xb ? 0x38 : 0x1c));
    copy_to(agent, 0x84, in.take(0x20));
    copy_to(agent, 0x8ac, in.take(0x10));
    copy_to(agent, 0x8cc, in.take(0x10));
    copy_to(agent, 0x8bc, in.take(0x10));
    if (version >= 0x10) {
        // Each string goes through the 0x80-byte stack temp sp+0x110; the
        // 0x40-byte copies leave its upper half as the previous copy left it.
        std::array<std::uint8_t, 0x80> temp{};
        auto bytes = in.take(0x40);
        std::copy(bytes.begin(), bytes.end(), temp.begin());
        copy_string(agent, 0x8dc, std::span<const std::uint8_t>(temp).first(0x40));
        bytes = in.take(0x80);
        std::copy(bytes.begin(), bytes.end(), temp.begin());
        copy_string(agent, 0x91c, temp);
        write_u32(agent, 0x99c, in.word());
        bytes = in.take(0x40);
        std::copy(bytes.begin(), bytes.end(), temp.begin());
        copy_string(agent, 0x95c, temp);
        write_u32(agent, 0x9a0, in.word());
    } else {
        read_short_string(in, state, agent, 0x8dc);
        read_short_string(in, state, agent, 0x91c);
        write_u32(agent, 0x99c, in.word());
        read_short_string(in, state, agent, 0x95c);
        write_u32(agent, 0x9a0, in.word());
    }
    if (version >= 4) {
        const auto bytes = in.take(agent_appearance_size);
        std::copy(bytes.begin(), bytes.end(), state.appearance.begin());
    }
    if (version >= 0xa) copy_to(agent, 0x9a8, in.take(0x100));
    write_u32(agent, 0xd8, version >= 6 ? in.word() : 0);
    if (version >= 5)
        copy_to(agent, 0xdc, in.take(0x7d0));
    else
        zero(agent, 0xdc, 0xdc + 0x7d0);
    if (version >= 8)
        for (auto& level : state.level_records) {
            const auto bytes = in.take(agent_level_record_size);
            std::copy(bytes.begin(), bytes.end(), level.begin());
        }
    if (version >= 0xd) {
        agent[0xb68] = in.take(1)[0];
        in.skip(3);
    }
    if (version >= 0xe) {
        const auto count = static_cast<std::int32_t>(in.word());
        if (count > static_cast<std::int32_t>(agent_message_slots)) return finish(2);
        state.message_count = static_cast<std::uint32_t>(count);
        for (std::int32_t i = 0; i < static_cast<std::int32_t>(agent_message_slots); ++i) {
            const auto bytes = in.take(agent_message_entry_size);
            if (i < count)
                std::copy(bytes.begin(), bytes.end(),
                    state.messages.begin() + static_cast<std::ptrdiff_t>(i * agent_message_entry_size));
        }
    }
    if (version >= 0x11) {
        std::uint32_t checksum = 0;
        for (std::size_t i = 0; i < in.at(); i += 4) checksum ^= read_u32(buffer, i);
        if (checksum != in.word()) return finish(2);
    }
    write_u32(agent, 4, static_cast<std::uint32_t>(slot));
    result.refill = version >= 3;
    agent[0xb72] = 0;
    agent[0xb73] = 0;
    agent[0xb70] = 1;
    return finish(0);
}

}  // namespace omega
