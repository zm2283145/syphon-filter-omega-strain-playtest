#include "agent_profile_view.hpp"

#include "agent_card.hpp"
#include "agent_refill.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {

[[noreturn]] void fail(std::uint32_t address, const std::string& what) {
    char a[16];
    std::snprintf(a, sizeof a, "%08x", address);
    throw std::runtime_error(std::string("agent profile ") + a + ": " + what + " is not hosted");
}

std::int32_t s32(AgentRecordView agent, std::size_t at) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(agent[at]) | static_cast<std::uint32_t>(agent[at + 1]) << 8 |
                                     static_cast<std::uint32_t>(agent[at + 2]) << 16 |
                                     static_cast<std::uint32_t>(agent[at + 3]) << 24);
}

// The level-state entries (+d8 count, +dc four bytes each) inside the record.
std::size_t level_state_count(AgentRecordView agent, std::uint32_t address) {
    const std::int32_t n = s32(agent, 0xd8);
    if (n < 0) return 0;
    if (0xdc + 4 * static_cast<std::size_t>(n) > agent_record_size) fail(address, "a level-state count past the record");
    return static_cast<std::size_t>(n);
}

// 003309c0 / 00330aa0: the bit of the row with key 003fdb40(key), -1 when none;
// both return 0 for a negative result.
template <class Rows, class Bit>
std::int32_t registry_bit(const Rows& rows, std::int32_t id, Bit bit) {
    std::int32_t r = -1;
    for (const auto& row : rows)
        if (row.key == id) {
            r = bit(row);
            break;
        }
    return r < 0 ? 0 : r;
}

std::int32_t word_bit(AgentRecordView agent, std::size_t base, std::int32_t bit) {
    const std::size_t at = base + 4 * static_cast<std::size_t>(bit >> 5);
    if (at + 4 > agent_record_size) throw std::runtime_error("agent profile: a bit past the record");
    return ((static_cast<std::uint32_t>(s32(agent, at)) >> (bit & 31)) & 1) != 0;
}

const char* gate_table(std::int32_t level, bool rank) {
    // 0x48c1b0 (names) / 0x48c200 (ranks), 18 words each.
    switch (level) {
    case 3: return rank ? "S_RANK_18" : "Player_Stone";
    case 6: return rank ? "S_RANK_21" : "Player_Lian";
    case 10: return rank ? "S_RANK_19" : "Player_Mujari";
    case 15: return rank ? "S_RANK_22" : "Player_Logan";
    default: return nullptr;
    }
}

std::optional<std::string> gate_string(const AgentGate& gate, const AgentStrings& strings, bool rank) {
    if (!gate.app_35 || gate.mp_5721c8 || gate.app_24c == -1) return std::nullopt;
    if (gate.app_24c < 0 || gate.app_24c >= 18) fail(rank ? 0x2c9070 : 0x2c90f0, "a level index outside the table");
    const char* key = gate_table(gate.app_24c, rank);
    if (!key) return std::nullopt;
    return strings(key);
}

}  // namespace

std::int32_t agent_level_flag(AgentRecordView agent, std::int32_t a1, std::int32_t a2) {
    const std::size_t n = level_state_count(agent, 0x332860);
    for (std::size_t i = 0; i < n; ++i) {
        const auto* e = &agent[0xdc + 4 * i];
        if (static_cast<std::int8_t>(e[3]) != 0) continue;
        if (static_cast<std::int8_t>(e[0]) == a1 && (a2 == -1 || static_cast<std::int8_t>(e[1]) == a2)) return 1;
    }
    return 0;
}

std::int32_t agent_zeus_has(AgentRecordView agent, std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    const std::size_t n = level_state_count(agent, 0x3329b0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto* e = &agent[0xdc + 4 * i];
        if (static_cast<std::int8_t>(e[0]) == a1 && static_cast<std::int8_t>(e[1]) == a2 &&
            static_cast<std::int8_t>(e[2]) == a3)
            return 1;
    }
    return 0;
}

std::int32_t agent_zeus_viewed(AgentRecordView agent, std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    const std::size_t n = level_state_count(agent, 0x332950);
    for (std::size_t i = 0; i < n; ++i) {
        const auto* e = &agent[0xdc + 4 * i];
        if (static_cast<std::int8_t>(e[0]) == a1 && static_cast<std::int8_t>(e[1]) == a2 &&
            static_cast<std::int8_t>(e[2]) == a3)
            return e[3] != 0 ? 1 : 0;
    }
    return 0;
}

void agent_level_start(AgentRecord agent, std::int32_t a1, std::int32_t a2, std::int32_t a3, std::int32_t t0) {
    const std::size_t n = level_state_count(agent, 0x3328d0);
    for (std::size_t i = 0; i < n; ++i) {
        auto* e = &agent[0xdc + 4 * i];
        if (static_cast<std::int8_t>(e[0]) != a1 || static_cast<std::int8_t>(e[1]) != a2 ||
            static_cast<std::int8_t>(e[2]) != a3)
            continue;
        const auto v = static_cast<std::uint8_t>(t0);
        if (e[3] != v) {
            e[3] = v;
            agent[0xb72] = 1;
        }
        return;
    }
}

void agent_level_progress(AgentRecord agent, std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    const std::int32_t count = s32(agent, 0xd8);
    if (count < 0) fail(0x332a10, "a negative level-state count (the entry lands below +dc)");
    const auto n = static_cast<std::size_t>(count);
    if (0xdc + 4 * n + 4 > 0x8ac) fail(0x332a10, "a level-state entry reaching the name at +8ac");
    const std::int32_t b1 = static_cast<std::int32_t>(static_cast<std::uint32_t>(a2) - 1u);
    const std::int32_t b2 = static_cast<std::int32_t>(static_cast<std::uint32_t>(a3) - 1u);
    for (std::size_t i = 0; i < n; ++i) {
        const auto* e = &agent[0xdc + 4 * i];
        if (static_cast<std::int8_t>(e[0]) == a1 && static_cast<std::int8_t>(e[1]) == b1 &&
            static_cast<std::int8_t>(e[2]) == b2)
            return;
    }
    auto* e = &agent[0xdc + 4 * n];
    e[0] = static_cast<std::uint8_t>(a1);
    e[1] = static_cast<std::uint8_t>(b1);
    e[2] = static_cast<std::uint8_t>(b2);
    e[3] = 0;
    const auto next = static_cast<std::uint32_t>(count) + 1u;
    for (std::size_t k = 0; k < 4; ++k) agent[0xd8 + k] = static_cast<std::uint8_t>(next >> (8 * k));
    agent[0xb72] = 1;
}

std::string agent_lower_key(const std::string& name) {
    if (name.size() >= 16) fail(0x3fda40, "a key of 16 or more characters (no NUL for 003fdb40)");
    std::string k = name;
    for (auto& c : k) {
        if (static_cast<unsigned char>(c) >= 0x80) fail(0x3fda40, "a byte >= 0x80 (tolower's table)");
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return k;
}

std::int32_t agent_level_done(AgentRecordView agent, const AwardRegistries& registries, const StringPool& pool,
                              const std::string& key16) {
    const std::int32_t id = award_intern(pool, key16);
    const std::int32_t bit = registry_bit(registries.levels, id, [](const AwardLevelEntry& r) { return r.bit; });
    return word_bit(agent, 0xb08, bit);
}

std::int32_t agent_objective_done(AgentRecordView agent, const AwardRegistries& registries, const StringPool& pool,
                                  const std::string& name) {
    const std::int32_t id = award_intern(pool, agent_lower_key(name));
    const std::int32_t bit =
        registry_bit(registries.objectives, id, [](const AwardObjectiveEntry& r) { return r.index; });
    return word_bit(agent, 0x84, bit);
}

std::string agent_short_name(const std::string& s) {
    const std::string c = s.substr(0, s.find('\0'));
    if (c.size() < 3) fail(0x336060, "a name under 3 characters");
    const unsigned char last = static_cast<unsigned char>(c.back());
    if (last >= 0x80) fail(0x336060, "a last byte >= 0x80 (lb before the ctype table)");
    std::string r = c.substr(0, 3);
    if (last >= '0' && last <= '9') r.push_back(static_cast<char>(last));   // ctype bit 4
    return r;
}

std::optional<std::string> agent_name(AgentRecordView agent, const AgentGate& gate, const AgentStrings& strings) {
    if (auto s = gate_string(gate, strings, false)) return s;
    const char* p = reinterpret_cast<const char*>(agent.data() + 0x8ac);
    const std::size_t room = agent_record_size - 0x8ac;
    const std::size_t n = strnlen(p, room);
    if (n == room) fail(0x336170, "an agent name without a NUL");
    return std::string(p, n);
}

std::optional<std::string> agent_rank(AgentRecordView agent, const AgentGate& gate, const AgentStrings& strings,
                                      const AwardRegistries& registries,
                                      const std::function<std::uint32_t(std::size_t)>& rank_value) {
    if (auto s = gate_string(gate, strings, true)) return s;
    const auto b50 = static_cast<std::uint32_t>(s32(agent, 0xb50));
    const AwardRankEntry* row = nullptr;
    for (std::size_t k = 0; k < registries.ranks.size(); ++k)
        if (rank_value(k) == b50) {
            row = &registries.ranks[k];
            break;
        }
    if (!row) fail(0x336140, "a +b50 that is no rank row");
    if (!row->name) fail(0x33614c, "a rank row with a null name (sprintf of NULL)");
    const std::string text = "S_" + *row->name;
    if (text.size() + 1 > 0x10) fail(0x336150, "a rank key overflowing the 16-byte stack buffer");
    return strings(text);
}

std::vector<AgentAwardRow> agent_award_rows(const AgentLoadState& state, const AwardRegistries& registries,
                                            std::int32_t kind) {
    if (kind < 0 || kind >= static_cast<std::int32_t>(agent_award_lists)) fail(0x3334c0, "an award kind past the lists");
    const auto& elements = state.awards[static_cast<std::size_t>(kind)];
    const auto& rows = registries.awards[static_cast<std::size_t>(kind)];
    if (rows.size() < elements.size()) fail(0x3334c0, "an agent list longer than the registry");
    std::vector<AgentAwardRow> out;
    for (std::size_t i = 0; i < elements.size(); ++i) out.push_back({rows[i].key, elements[i].flag, rows[i].unlock});
    return out;
}

std::vector<AgentOmegaItem> agent_omega_items(const AgentLoadState& state, const AwardRegistries& registries,
                                              std::int32_t index) {
    const auto& elements = state.awards[4];
    if (index < 0 || static_cast<std::size_t>(index) >= elements.size()) fail(0x3332c4, "an omega element past the list");
    const auto& e = elements[static_cast<std::size_t>(index)];
    std::vector<AgentOmegaItem> out;
    for (std::size_t i = 0; i < e.bits.size(); ++i) {
        if (i >= registries.omega_requirements.size()) fail(0x333324, "an omega element byte past the requirement rows");
        const auto& r = registries.omega_requirements[i];
        out.push_back({r.name, e.bits[i], r.a, r.b, r.c, r.unlock});
    }
    return out;
}

AgentOmegaItem agent_omega_last(const AwardRegistries& registries) {
    if (registries.omega_requirements.empty()) fail(0x333460, "no omega requirement rows (row -1)");
    const auto& r = registries.omega_requirements.back();
    return {r.name, 2, r.a, r.b, r.c, r.unlock};
}

std::vector<std::string> agent_level_objective_keys(const AwardRegistries& registries, const StringPool& pool,
                                                    const std::string& name) {
    const std::string s = name.substr(0, name.find('\0'));
    if (s.size() < 3) fail(0x335454, "a level name under 3 characters (reads past it)");
    const auto last = static_cast<unsigned char>(s.back());
    if (last >= 0x80) fail(0x335488, "a last byte >= 0x80 (lb before the ctype table)");
    std::string short_name = s.substr(0, 3);
    if (last >= '0' && last <= '9') short_name.push_back(static_cast<char>(last));
    const std::string pattern = short_name + "_*";  // 004b92b8 "%s_*" into 00532998
    std::vector<std::string> out;
    for (const auto& row : registries.objectives) {
        const std::string k = pool_key_text(pool, row.key);
        if (award_wildcard(pattern, k) && !award_wildcard("*_COMPLETED", k)) out.push_back(k);
    }
    return out;
}

std::string pool_key_text(const StringPool& pool, std::int32_t id) {
    if (id < 0) return {};
    const auto& entries = pool.entries();
    if (static_cast<std::size_t>(id) >= entries.size()) fail(0x3fdae0, "a pool id past the pool's rows");
    const auto& k = entries[static_cast<std::size_t>(id)].key;
    const auto end = std::find(k.begin(), k.end(), std::uint8_t{0});
    if (end == k.end()) fail(0x3fdae0, "a pool key with no NUL");
    return std::string(k.begin(), end);
}

std::vector<AgentAwardItem> agent_award_items(const AgentLoadState& state, const AwardRegistries& registries,
                                              const StringPool& pool, std::int32_t kind, std::int32_t index) {
    if (kind == 4) fail(0x333300, "the omega requirement items (kind 4)");
    if (kind < 0 || kind >= static_cast<std::int32_t>(agent_award_lists)) fail(0x333260, "an award kind past the lists");
    const auto& elements = state.awards[static_cast<std::size_t>(kind)];
    const auto& rows = registries.awards[static_cast<std::size_t>(kind)];
    if (index < 0 || static_cast<std::size_t>(index) >= elements.size() || static_cast<std::size_t>(index) >= rows.size())
        fail(0x333260, "an award index past the list");
    const auto& e = elements[static_cast<std::size_t>(index)];
    std::vector<AgentAwardItem> out;
    for (std::size_t i = 0; i < e.bits.size(); ++i)
        out.push_back({pool_key_text(pool, rows[static_cast<std::size_t>(index)].requirement + static_cast<std::int32_t>(i)),
                       e.bits[i]});
    return out;
}

std::int32_t card_space_needed(std::int32_t size, bool save_directory_present) {
    const std::int32_t v = ((size + 0x3ff) >> 10) + 1;
    if (!save_directory_present) return v + 0x2e;
    return size ? v : 0;
}

void construct_agent(AgentRecord agent, AgentLoadState& state, const AwardRegistries& registries) {
    const auto put = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) agent[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    std::fill(agent.begin(), agent.end(), std::uint8_t{0});
    state = AgentLoadState{};
    agent[0xd0] = 1;  // 00336e60: the level vector's +c
    put(0x99c, 0xffffffffu);
    put(0x9a0, 0xffffffffu);
    for (std::size_t k = 0; k < agent_award_lists; ++k) {
        agent[0xaa8 + 0x10 * k + 0xc] = 1;
        for (const AwardEntry& row : registries.awards[k]) {
            if (row.count < 0) fail(0x336900, "a negative award requirement count");
            state.awards[k].push_back({0, std::vector<std::uint8_t>(static_cast<std::size_t>(row.count), 0)});
        }
        put(0xaac + 0x10 * k, static_cast<std::uint32_t>(state.awards[k].size()));
    }
    agent[0xb04] = 1;
    state.award_bits.assign(registries.ratings.size(), 0);
    put(0xafc, static_cast<std::uint32_t>(state.award_bits.size()));
    state.level_count = static_cast<std::int32_t>(registries.levels.size());
    reset_agent(agent, state);
    agent[0x8ac] = agent[0x8bc] = agent[0x8cc] = 0;
    put(0x0, 0xffffffffu);
    put(0x4, 0);
    state.appearance.fill(0);
    std::fill(agent.begin() + 0x9a8, agent.begin() + 0xaa8, std::uint8_t{0});
}

std::uint8_t card_save_present(bool present, bool formatted, bool directory, std::span<const std::string> entries,
                               bool font_ready, std::int32_t map_count) {
    if (!(present && formatted && directory)) return 0;
    for (const std::string& name : entries) {
        if (name.find('.') != std::string::npos) continue;
        const auto c = static_cast<unsigned char>(name.empty() ? 0 : name[0]);
        if (c >= 0x80) fail(0x2885d4, "a card name byte >= 0x80 (before the ctype table)");
        if (!(c >= '0' && c <= '9')) continue;
        if (decode_agent_file_name(name, 0x10, font_ready, map_count)) return 1;
    }
    return 0;
}

std::int32_t agent_clp(AgentRecordView agent, const AgentLoadState& state, const AwardRegistries& registries) {
    std::uint32_t total = 0;
    for (const auto& row : registries.objectives) {  // 00330870(agent + 84, index)
        const std::int32_t bit = row.index;
        const std::int32_t word = bit / 32;  // signed: 00330870 divides with the sign fix-up
        if (word < 0) fail(0x330870, "a negative objective bit");
        const std::size_t at = 0x84 + 4 * static_cast<std::size_t>(word);
        if (at + 4 > agent_record_size) fail(0x330870, "an objective bit past the record");
        if ((static_cast<std::uint32_t>(s32(agent, at)) >> (bit & 31)) & 1) total += static_cast<std::uint32_t>(row.points);
    }
    for (const auto& row : registries.ratings) {
        const std::size_t at = 8 + 4 * static_cast<std::size_t>(row.stat);
        if (at + 4 > agent_record_size) fail(0x3305d0, "a rating stat past the record");
        if (s32(agent, at) >= row.threshold) total += static_cast<std::uint32_t>(row.points);
    }
    for (std::int32_t kind = 0; kind < static_cast<std::int32_t>(agent_award_lists); ++kind) {
        const auto rows = agent_award_rows(state, registries, kind);
        const auto& reg = registries.awards[static_cast<std::size_t>(kind)];
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].flag) total += static_cast<std::uint32_t>(reg[i].points);
    }
    total += 5u * static_cast<std::uint32_t>(s32(agent, 0x40));
    total += 5u * static_cast<std::uint32_t>(s32(agent, 0x44));
    return static_cast<std::int32_t>(total);
}

std::vector<AgentRatingRow> agent_rating_rows(AgentRecordView agent, const AgentLoadState& state,
                                              const AwardRegistries& registries) {
    std::vector<AgentRatingRow> out;  // 00299510: the list cleared first
    for (std::size_t i = 0; i < registries.ratings.size(); ++i) {
        const auto& row = registries.ratings[i];
        const std::size_t at = 8 + 4 * static_cast<std::size_t>(row.stat);  // lbu +8
        if (at + 4 > agent_record_size) fail(0x333680, "a rating stat past the record");
        const std::int32_t word = s32(agent, at);
        std::int32_t pct = 0;
        if (!(word < row.threshold)) pct = 100;
        else if (row.low < word) {
            // subu / (x * 5 * 5 * 4) / div: 32-bit wrapping arithmetic, a signed divide.
            const auto num = static_cast<std::int32_t>((static_cast<std::uint32_t>(word) - static_cast<std::uint32_t>(row.low)) * 100u);
            const auto den = static_cast<std::int32_t>(static_cast<std::uint32_t>(row.threshold) - static_cast<std::uint32_t>(row.low));
            if (den == 0) fail(0x3336c4, "a rating's threshold equal to its low bound");
            if (num == std::numeric_limits<std::int32_t>::min() && den == -1) fail(0x3336c4, "a rating divide overflow");
            pct = num / den;
        }
        if (i >= state.award_bits.size()) fail(0x333790, "a rating row past the agent's +b00 bytes");
        out.push_back({row.key, static_cast<std::int8_t>(state.award_bits[i]), row.unlock, pct});
    }
    return out;
}

}  // namespace omega
