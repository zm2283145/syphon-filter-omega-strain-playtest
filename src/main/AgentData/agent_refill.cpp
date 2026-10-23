#include "agent_refill.hpp"
#include <algorithm>
#include <cstring>
#include <optional>
#include <stdexcept>

namespace omega {
namespace {

using Record = std::span<std::uint8_t, agent_record_size>;

std::int32_t rd32(std::span<const std::uint8_t> b, std::size_t o) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + o, 4);
    return static_cast<std::int32_t>(v);
}
void wr32(std::span<std::uint8_t> b, std::size_t o, std::int32_t v) { std::memcpy(b.data() + o, &v, 4); }
std::int32_t add32(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t times5(std::int32_t v) { return static_cast<std::int32_t>(5u * static_cast<std::uint32_t>(v)); }

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }  // 0012b270

// The interned form of a name: what 003fda40 (strncpy 16 + tolower) and the
// pool rows hold.
std::string interned(std::string_view s) {
    if (s.empty() || s.size() >= 16) throw std::runtime_error("Agent refill: a name is empty or 16+ characters (overruns the 16-byte key)");
    std::string k(s);
    for (auto& c : k) {
        if (c == 0 || static_cast<unsigned char>(c) >= 0x80) throw std::runtime_error("Agent refill: a name is not plain ASCII");
        c = lower(c);
    }
    return k;
}

// 00333890(this, pattern, s, '*'): the prefix up to the pattern's first '*',
// then the suffix after its last '*', case-insensitively; either side may run
// out first.
bool wildcard(std::string_view p, std::string_view s) {
    std::size_t i = 0;
    while (i < p.size() && i < s.size() && p[i] != '*') {
        if (lower(p[i]) != lower(s[i])) return false;
        ++i;
    }
    auto a = static_cast<std::ptrdiff_t>(p.size()) - 1, b = static_cast<std::ptrdiff_t>(s.size()) - 1;
    while (a >= 0 && b >= 0 && p[static_cast<std::size_t>(a)] != '*') {
        if (lower(p[static_cast<std::size_t>(a)]) != lower(s[static_cast<std::size_t>(b)])) return false;
        --a;
        --b;
    }
    return true;
}

// 003fe650: the pool id a name interns to, as its lowercase text; nullopt
// (-1) when the name is not in the string pool.
std::optional<std::string> pool_key(const AwardTables& tables, std::string_view name) {
    std::string key = interned(name);
    for (const auto& u : tables.unpooled)
        if (interned(u) == key) return std::nullopt;
    return key;
}

std::string pooled_text(const AwardTables& tables, std::string_view name) {
    auto key = pool_key(tables, name);
    if (!key) throw std::runtime_error("Agent refill: a level or objective name is not in the string pool (its text is read through id -1)");
    return *key;
}

// 00330aa0 / 003308e0: the bit of the first row with the same pool id; 0 when
// there is none.
template <class Rows>
std::int32_t bit_of(const AwardTables& tables, const Rows& rows, std::string_view name) {
    const auto key = pool_key(tables, name);
    for (const auto& row : rows)
        if (pool_key(tables, row.name) == key) return std::max(row.bit, 0);  // 003308e0 / 00330aa0 clamp
    return 0;
}

// 00330870: word (bit < 0 ? bit + 31 : bit) >> 5 (arithmetic) of +84, shift
// bit & 31; a negative bit reads +84 (-31..-1) or the stat words below it.
bool objective_done(std::span<const std::uint8_t> agent, std::int32_t bit) {
    const std::int64_t word = (bit < 0 ? std::int64_t{bit} + 31 : std::int64_t{bit}) >> 5;
    const std::int64_t offset = 0x84 + 4 * word;
    if (offset < 8 || offset > 0xc0) throw std::runtime_error("Agent refill: an objective bit lies outside +8..+c3");
    return (static_cast<std::uint32_t>(rd32(agent, static_cast<std::size_t>(offset))) >> (static_cast<std::uint32_t>(bit) & 31)) & 1;
}

std::int32_t stat(std::span<const std::uint8_t> agent, std::uint8_t index) {
    const std::size_t offset = 8 + 4 * std::size_t{index};
    if (offset == 0xc4 || offset == 0xcc) throw std::runtime_error("Agent refill: a rating reads the level vector's pointer words");
    return rd32(agent, offset);
}

void mark_flag(std::uint8_t& flag, std::uint32_t mark) {
    if (mark != 0) flag = 2;
    else if (flag == 0) flag = 1;
}

struct Refill {
    Record agent;
    AgentLoadState& state;
    const AwardTables& tables;
    std::uint32_t mark;
    const AgentRefillHost& host;
    AgentRefillResult result;

    bool objective_done(std::int32_t bit) const { return omega::objective_done(agent, bit); }
    std::int32_t stat(std::uint8_t index) const { return omega::stat(agent, index); }
    void unlock(std::int32_t id) { award_unlock(agent, tables, id); }
    std::int32_t score() const { return award_score(agent, state, tables); }

    // 00335070
    void award_pass() {
        for (std::size_t list = 0; list < agent_award_lists; ++list)
            for (std::size_t j = 0; j < state.awards[list].size(); ++j)  // the EE indexes the registry by j unchecked
                award_element_step(agent, state.awards[list][j], tables.awards[list].at(j).unlock_id, mark, tables);
        for (std::size_t k = 0; k < tables.ratings.size(); ++k) {  // 003351e0
            const auto& r = tables.ratings[k];
            if (stat(r.stat) >= r.threshold) {
                mark_flag(state.award_bits.at(k), mark);
                unlock(r.unlock_id);
            }
        }
        const std::int32_t s = score();
        for (std::size_t k = 0; k < tables.ranks.size(); ++k)
            if (s >= tables.ranks[k].threshold) {
                unlock(tables.ranks[k].unlock_id);
                if (!host.rank_value) throw std::runtime_error("Agent refill: no +b50 value for a rank row");
                wr32(agent, 0xb50, static_cast<std::int32_t>(host.rank_value(k)));
                result.rank = k;
            }
    }

    // 00335660(agent, name, record): objective count +18 and completed +1c over
    // the objectives matching sprintf("%s_*", prefix) and not "*_COMPLETED".
    // The prefix is the name's first three bytes plus its last character when
    // that is a digit; the EE copies the three bytes even past the NUL, so a
    // name under three characters is its whole self and the digit falls after
    // the NUL.
    void count_objectives(std::string_view level_name, AgentLevelRecord& record) {
        const std::string name = pooled_text(tables, level_name);
        std::string prefix = name.substr(0, 3);
        if (name.size() >= 3 && name.back() >= '0' && name.back() <= '9') prefix += name.back();
        const std::string pattern = prefix + "_*";
        std::span<std::uint8_t> r(record);
        wr32(r, 0x18, 0);
        wr32(r, 0x1c, 0);
        for (const auto& o : tables.objectives) {
            const std::string objective = pooled_text(tables, o.name);
            if (!wildcard(pattern, objective) || wildcard("*_COMPLETED", objective)) continue;
            wr32(r, 0x18, add32(rd32(r, 0x18), 1));
            if (objective_done(bit_of(tables, tables.objectives, objective)))  // 00333b10
                wr32(r, 0x1c, add32(rd32(r, 0x1c), 1));
        }
    }
};

std::size_t element_count(std::span<const std::uint8_t> agent, std::size_t offset) {
    const auto count = rd32(agent, offset);
    return count > 0 ? static_cast<std::size_t>(count) : 0;
}

}  // namespace

bool award_wildcard(std::string_view pattern, std::string_view s) { return wildcard(pattern, s); }

void award_unlock(Record agent, const AwardTables& tables, std::int32_t id) {
    if (id < 0) return;
    std::int32_t bit = 0;
    if (id < 10000) {
        // 00147840: record id, or 0 unless (u32)id < the record count.
        if (tables.weapons.empty()) throw std::runtime_error("Agent refill: an unlock names a weapon with no weapon records");
        const auto k = static_cast<std::uint32_t>(id) < tables.weapons.size() ? static_cast<std::size_t>(id) : 0;
        bit = bit_of(tables, tables.unlocks, tables.weapons[k]);  // 00333050
    } else {
        bit = (id - 10000) & 0xff;
    }
    const auto u = static_cast<std::uint32_t>(bit);
    if (u >= (0xb50 - 0xb08) * 8) throw std::runtime_error("Agent refill: an unlock bit lies past +b4f (the rank pointer +b50)");
    const std::size_t o = 0xb08 + 4 * (u >> 5);
    wr32(agent, o, static_cast<std::int32_t>(static_cast<std::uint32_t>(rd32(agent, o)) | 1u << (u & 31)));
}

std::int32_t award_score(std::span<const std::uint8_t, agent_record_size> agent, const AgentLoadState& state,
    const AwardTables& tables) {
    std::int32_t s = 0;
    for (const auto& o : tables.objectives)
        if (objective_done(agent, o.bit)) s = add32(s, o.points);
    for (const auto& r : tables.ratings)
        if (!(stat(agent, r.stat) < r.threshold)) s = add32(s, r.points);
    for (std::size_t list = 0; list < agent_award_lists; ++list)
        for (std::size_t j = 0; j < state.awards[list].size(); ++j)
            if (state.awards[list][j].flag != 0) s = add32(s, tables.awards[list].at(j).points);
    return add32(s, add32(times5(rd32(agent, 0x40)), times5(rd32(agent, 0x44))));
}

void award_element_step(Record agent, AgentAwardEntry& entry, std::int32_t unlock_id, std::uint32_t mark,
    const AwardTables& tables) {
    if (!std::all_of(entry.bits.begin(), entry.bits.end(), [](std::uint8_t b) { return b == 2; })) return;
    mark_flag(entry.flag, mark);
    award_unlock(agent, tables, unlock_id);
}

AgentRefillResult refill_agent(Record agent, AgentLoadState& state, const AwardTables& tables, std::uint32_t mark,
    std::uint32_t recount, const AgentRefillHost& host) {
    for (std::size_t i = 0; i < agent_award_lists; ++i)
        if (state.awards[i].size() != element_count(agent, 0xaac + 0x10 * i))
            throw std::runtime_error("Agent refill: an award list does not match its count word");
    if (state.level_records.size() != static_cast<std::uint32_t>(rd32(agent, 0xc8)))
        throw std::runtime_error("Agent refill: the level records do not match the +c8 count");
    if (!host.set_awards_and_bonuses) throw std::runtime_error("Agent refill: awards::SetAwardsAndBonuses is not hosted");

    Refill f{agent, state, tables, mark, host, {}};
    if (recount != 0)
        for (const std::size_t o : {0x40u, 0x44u, 0x48u, 0x4cu}) wr32(agent, o, 0);
    for (std::int32_t i = 0; i < add32(rd32(agent, 0xc8), -1); ++i) {  // +c8 is re-read each pass
        const auto& level = tables.levels.at(static_cast<std::size_t>(i));
        auto& record = state.level_records.at(static_cast<std::size_t>(i));
        std::span<std::uint8_t> r(record);
        f.count_objectives(level.name, record);
        wr32(r, 0xc, level.pair_second);
        wr32(r, 0x8, level.pair_first);
        if (rd32(r, 0xc) <= 0) wr32(r, 0x4, -1);
        if (recount != 0) {
            if (rd32(r, 0) > 0 && rd32(r, 8) >= rd32(r, 0)) wr32(agent, 0x40, add32(rd32(agent, 0x40), 1));
            if (rd32(r, 4) > 0 && rd32(r, 0xc) >= rd32(r, 4)) wr32(agent, 0x44, add32(rd32(agent, 0x44), 1));
            if (rd32(r, 0x14) == 0) wr32(agent, 0x48, add32(rd32(agent, 0x48), 1));
            if (rd32(r, 0x10) == 0) wr32(agent, 0x4c, add32(rd32(agent, 0x4c), 1));
        }
    }
    std::fill(agent.begin() + 0xb08, agent.begin() + 0xb2c, std::uint8_t{0});
    wr32(agent, 0xb5c, 0);
    state.parts.clear();
    for (int pass = 0; pass < 2; ++pass) {
        host.set_awards_and_bonuses(agent, state);
        f.award_pass();
    }
    return f.result;
}

std::uint32_t create_agent(Record agent, AgentLoadState& state, const AwardTables& tables, std::int32_t slot,
    std::uint32_t nosave, bool card_busy, const AgentRefillHost& host, const std::function<std::uint32_t()>& save) {
    if (state.level_count < 0 || static_cast<std::size_t>(state.level_count) != tables.levels.size())
        throw std::runtime_error("Agent create: *00532af4 is the level table's count");
    reset_agent(agent, state);
    refill_agent(agent, state, tables, 0, 1, host);
    wr32(agent, 4, slot);
    agent[0xb70] = 1;
    if (nosave != 0 || card_busy) return 0;
    if (!save) throw std::runtime_error("Agent create: no save");
    return save();
}

void set_agent_name(Record agent, std::string_view name, std::uint32_t& message_count) {
    if (name.find('\0') != std::string_view::npos) throw std::runtime_error("Agent name: a NUL inside the name");
    if (0x8ac + name.size() + 1 > 0x9a4) throw std::runtime_error("Agent name: the name overruns the appearance pointer +9a4");
    std::copy(name.begin(), name.end(), agent.begin() + 0x8ac);
    agent[0x8ac + name.size()] = 0;
    if (message_count != 0) {
        agent[0xb73] = 1;
        message_count = 0;
    }
}

void set_agent_objective(Record agent, const AwardTables& tables, std::string_view name) {
    const auto bit = static_cast<std::uint32_t>(bit_of(tables, tables.objectives, name));
    if (bit >= 0x20 * 8) throw std::runtime_error("Agent objective 00333b90: a bit past +a3 (its +a4 word lies past +c3)");
    for (const std::size_t base : {0x84u, 0xa4u}) {
        const std::size_t o = base + 4 * (bit >> 5);
        wr32(agent, o, static_cast<std::int32_t>(static_cast<std::uint32_t>(rd32(agent, o)) | 1u << (bit & 31)));
    }
}

namespace {

// The EE FPU operands 00335830 compares: a NaN / infinity pattern (exponent
// 255, a large finite value on the EE) or a denormal (read as zero) differ
// from the host's IEEE compare.
float end_level_time(std::span<const std::uint8_t> agent) {
    std::uint32_t u = 0;
    std::memcpy(&u, agent.data() + 0xd4, 4);
    const std::uint32_t exponent = u >> 23 & 0xff;
    if (exponent == 0xff || (exponent == 0 && (u & 0x7fffff) != 0))
        throw std::runtime_error("Agent end level 00335830: a +d4 with exponent 255 or a denormal (EE FPU operand) is not hosted");
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// 00335830's prefix (as 00336060): bytes 0..2 copied even past the NUL, then
// the last character when it is a digit (ctype bit 4 of 0x499c71).
std::string end_level_prefix(std::string_view name) {
    if (name.size() < 2) throw std::runtime_error("Agent end level 00335830: a level name under 2 characters (the prefix reads past its NUL)");
    for (const char c : name)
        if (c == 0 || static_cast<unsigned char>(c) >= 0x80) throw std::runtime_error("Agent end level 00335830: a level name byte >= 0x80 or NUL (ctype index)");
    std::string prefix(name.substr(0, 3));
    if (name.size() >= 3 && name.back() >= '0' && name.back() <= '9') prefix += name.back();
    return prefix;
}

bool equal_no_case(std::string_view a, std::string_view b) {  // 00128e80 == 0
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool requirement_bit(const std::array<std::uint32_t, 8>& words, std::int32_t bit) {
    if (bit < 0) return false;
    return (words[static_cast<std::size_t>(bit) >> 5] >> (bit & 31) & 1) != 0;
}

}  // namespace

void end_agent_level(Record agent, AgentLoadState& state, const AwardTables& tables, const AgentEndLevelInput& input,
    const AgentEndLevelHost& host) {
    for (const char c : input.level)
        if (c == 0 || static_cast<unsigned char>(c) >= 0x80)
            throw std::runtime_error("Agent end level 00335830: a level name byte >= 0x80 or NUL (00128e80's ctype index)");
    agent[0xb74] = static_cast<std::uint8_t>(input.completed);
    if (end_level_time(agent) < 0.0F) {
        if (!host.mission_time) throw std::runtime_error("Agent end level 00335830: the mission clock 00245890 is not hosted");
        const float t = host.mission_time();
        std::memcpy(agent.data() + 0xd4, &t, 4);
    }
    if (state.level_records.size() != static_cast<std::uint32_t>(std::max(rd32(agent, 0xc8), 0)))
        throw std::runtime_error("Agent end level 00335830: the level records do not match the +c8 count");
    for (std::int32_t i = 0; i < rd32(agent, 0xc8); ++i) {
        const std::string name = pooled_text(tables, tables.levels.at(static_cast<std::size_t>(i)).name);
        if (!equal_no_case(name, input.level) || input.completed == 0) continue;
        std::span<std::uint8_t> r(state.level_records[static_cast<std::size_t>(i)]);
        wr32(agent, 0x3c, add32(rd32(agent, 0x3c), 1));
        const std::string pattern = end_level_prefix(name) + "_*";
        bool all = true;
        for (const auto& o : tables.objectives) {
            const std::string objective = pooled_text(tables, o.name);
            if (!wildcard(pattern, objective) || wildcard("*_COMPLETED", objective)) continue;
            if (o.bit >= 0x20 * 8)
                throw std::runtime_error("Agent end level 00335830: an objective bit past +c3 (the +a4 / requirement words)");
            if (o.bit >= 0 && (static_cast<std::uint32_t>(rd32(agent, 0xa4 + 4 * (static_cast<std::size_t>(o.bit) >> 5))) >> (o.bit & 31) & 1))
                continue;
            if (requirement_bit(input.multiplayer ? input.multi : input.solo, o.bit)) {
                all = false;
                break;
            }
        }
        if (all) {
            agent[0xb71] = 1;
            const std::size_t at = input.multiplayer ? 4 : 0;
            const std::int32_t word = rd32(r, at);
            const float time = end_level_time(agent);
            if (word >= (1 << 24)) throw std::runtime_error("Agent end level 00335830: a time word of 2^24 or more (cvt.s.w rounding) is not hosted");
            if (word < 0 || time < static_cast<float>(word)) {
                if (!(time > -2147483648.0F && time < 2147483648.0F))
                    throw std::runtime_error("Agent end level 00335830: a +d4 outside the cvt.w.s range is not hosted");
                wr32(r, at, static_cast<std::int32_t>(time));  // cvt.w.s truncates
            }
        }
        if (rd32(r, 0x14) < 0 || rd32(agent, 0x70) < rd32(r, 0x14)) wr32(r, 0x14, rd32(agent, 0x70));
        if (rd32(r, 0x10) < 0 || rd32(agent, 0x6c) < rd32(r, 0x10)) wr32(r, 0x10, rd32(agent, 0x6c));
        set_agent_objective(agent, tables, end_level_prefix(input.level) + "_COMPLETED");
    }
    if (!host.refill) throw std::runtime_error("Agent end level 00335830: the refill 00335e50 is not hosted");
    host.refill();
    if (!host.parts)
        throw std::runtime_error("Agent end level 00335830: the parts tail 002888c0 / 00332500 / 00405a60 / 002887c0 is not hosted");
    host.parts();
}

}  // namespace omega
