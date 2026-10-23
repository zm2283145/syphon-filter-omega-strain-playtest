#include "award_registry.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace omega {
namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("AWARDS registries: " + what); }

constexpr std::string_view score_types = "AgentData_ScoreTypes";
constexpr std::string_view special_types = "AgentData_SpecialBonusTypes";
constexpr std::int32_t special_unlock_base = 10000;  // 0x2710

// S string, I int, A AgentData_ScoreTypes, B AgentData_SpecialBonusTypes.
struct Overload {
    std::string_view name;
    std::string_view kinds;
    AwardsNative native;
};
constexpr Overload overloads[] = {
    {"client::Print", "S", AwardsNative::print},
    {"game::RegisterObjective", "SI", AwardsNative::objective2},
    {"game::RegisterObjective", "SIII", AwardsNative::objective4},
    {"game::RegisterDossier", "SS", AwardsNative::dossier},
    {"game::RegisterLevel", "SII", AwardsNative::level3},
    {"game::RegisterLevel", "SIII", AwardsNative::level4},
    {"game::RegisterBonusLevel", "SSI", AwardsNative::bonus_level},
    {"game::RegisterRank", "SI", AwardsNative::rank2},
    {"game::RegisterRank", "SIS", AwardsNative::rank3_unlock},
    {"game::RegisterRank", "SIB", AwardsNative::rank3_special},
    {"game::RegisterRank", "SISS", AwardsNative::rank4},
    {"game::RegisterRating", "SIAII", AwardsNative::rating5},
    {"game::RegisterRating", "SIAIIS", AwardsNative::rating6_unlock},
    {"game::RegisterRating", "SIAIIB", AwardsNative::rating6_special},
    {"game::RegisterSpecialRating", "SISI", AwardsNative::special_rating4},
    {"game::RegisterSpecialRating", "SISIS", AwardsNative::special_rating5_unlock},
    {"game::RegisterSpecialRating", "SISIB", AwardsNative::special_rating5_special},
    {"game::RegisterCommendation", "SISI", AwardsNative::commendation4},
    {"game::RegisterCommendation", "SISIS", AwardsNative::commendation5_unlock},
    {"game::RegisterCommendation", "SISIB", AwardsNative::commendation5_special},
    {"game::RegisterMedal", "SISI", AwardsNative::medal4},
    {"game::RegisterMedal", "SISIS", AwardsNative::medal5_unlock},
    {"game::RegisterMedal", "SISIB", AwardsNative::medal5_special},
    {"game::RegisterOmega", "SSI", AwardsNative::omega},
    {"game::RegisterOmegaRequirement", "SIII", AwardsNative::omega_requirement4},
    {"game::RegisterOmegaRequirement", "SIIIS", AwardsNative::omega_requirement5},
};

const Overload& overload_of(AwardsNative native) {
    for (const auto& o : overloads)
        if (o.native == native) return o;
    fail("unknown native");
}

char kind_of(const Script& script, Word type) {
    const auto key = script.type_key(type);
    if (key == "primitive:3" || key == "primitive:7") return 'S';
    if (key == "primitive:1") return 'I';
    if (key == "enum:" + std::string(score_types)) return 'A';
    if (key == "enum:" + std::string(special_types)) return 'B';
    return '?';
}

std::int32_t s32(Word w) { return static_cast<std::int32_t>(w); }
std::int32_t next(std::int32_t& counter) {
    const auto old = counter;
    counter = static_cast<std::int32_t>(static_cast<std::uint32_t>(counter) + 1u);
    return old;
}

struct Args {
    const AwardsInitCall& call;
    std::string_view kinds;
    Word word(std::size_t i) const {
        const auto* w = std::get_if<Word>(&call.args[i]);
        if (!w) fail("argument kind mismatch");
        return *w;
    }
    std::int32_t integer(std::size_t i) const { return s32(word(i)); }
    const AwardsText& text(std::size_t i) const {
        const auto* t = std::get_if<AwardsText>(&call.args[i]);
        if (!t || kinds[i] != 'S') fail("argument kind mismatch");
        return *t;
    }
    // 0032ff50 / 0032fd70 / 0032ea40: (u8)value + 10000.
    std::int32_t special(std::size_t i) const { return static_cast<std::int32_t>(word(i) & 0xffu) + special_unlock_base; }
};

// 003337a0 and the inline copies in 0032ffd0 / 0032fe00 / 0032e260: null or
// an id < 0 -> -1; the first unlock row with that key gives its +14; none -> -1.
std::int32_t resolve_unlock(const AwardRegistries& r, const StringPool& pool, const AwardsText& name) {
    const auto id = award_intern(pool, name);
    if (id < 0) return -1;
    for (const auto& row : r.unlocks)
        if (row.key == id) return row.weapon;
    return -1;
}

// 0032eb00 / 00330d10: sprintf(0x4b99a8 "S_RANK_%2.2d", index + 1) interned.
std::int32_t rank_title(const StringPool& pool, std::int32_t index) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "S_RANK_%2.2d", static_cast<int>(static_cast<std::uint32_t>(index) + 1u));
    return award_intern(pool, std::string(buffer));
}

// 0032f090: bit index of the 8-word set; index < 0 is skipped.
void set_bit(std::array<std::uint32_t, 8>& bits, std::int32_t index) {
    if (index < 0) return;
    if (index >= 256) fail("objective bit 256 or above writes past the 8-word set");
    bits[static_cast<std::size_t>(index) >> 5] |= 1u << (index & 31);
}

AwardEntry award(const StringPool& pool, const AwardsText& name, std::int32_t points, const AwardsText& requirement,
                 std::int32_t count, std::int32_t unlock) {
    return {award_intern(pool, name), award_intern(pool, requirement), count, unlock, points};
}

std::size_t award_list(AwardsNative n) {
    switch (n) {
    case AwardsNative::special_rating4: case AwardsNative::special_rating5_unlock: case AwardsNative::special_rating5_special: return 0;
    case AwardsNative::commendation4: case AwardsNative::commendation5_unlock: case AwardsNative::commendation5_special: return 1;
    default: return 2;
    }
}

class RecordingHost final : public Host {
public:
    std::vector<AwardsInitCall> calls;
    std::optional<Value> call(const Script& script, const Function& function, const std::vector<Value>& arguments) override {
        const auto native = awards_native(script, function);
        const auto kinds = overload_of(native).kinds;
        if (arguments.size() != kinds.size()) fail("argument count mismatch");
        AwardsInitCall c{native, {}};
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            const auto& v = arguments[i];
            if (kinds[i] == 'S') {
                if (const auto* s = std::get_if<std::string>(&v)) c.args.emplace_back(AwardsText{*s});
                else if (const auto* w = std::get_if<Word>(&v); w && *w == 0) c.args.emplace_back(AwardsText{});
                else fail("a string argument is not a string");
            } else if (const auto* w = std::get_if<Word>(&v)) {
                c.args.emplace_back(*w);
            } else fail("a word argument is not a word");
        }
        calls.push_back(std::move(c));
        return std::nullopt;
    }
    Word enum_value(const std::string& name, Word member) override { return awards_enum_value(name, member); }
};

}  // namespace

AwardsNative awards_native(const Script& script, const Function& function) {
    if (!function.external || function.return_type != 4
        || function.dispatch_fields != std::array<Word, 5>{0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU, 0})
        fail("unbound native " + function.name);
    std::string kinds;
    for (const auto type : function.parameters) kinds += kind_of(script, type);
    for (const auto& o : overloads)
        if (o.name == function.name && o.kinds == kinds) return o.native;
    fail("unbound native " + function.name + " (" + kinds + ")");
}

Word awards_enum_value(const std::string& name, Word member) {
    if (name == score_types) {
        if (member >= 30) fail("AgentData_ScoreTypes member out of range");
        return member < 13 ? member : member + 1;
    }
    if (name == special_types) {
        static constexpr std::array<Word, 6> values{3, 4, 2, 1, 0, 5};
        if (member >= values.size()) fail("AgentData_SpecialBonusTypes member out of range");
        return values[member];
    }
    fail("unhosted enum " + name);
}

std::vector<AwardsInitCall> read_awards_init(const Script& awards) {
    RecordingHost host;
    Vm vm(awards, host);
    vm.initialize();
    vm.invoke("awards::Init");
    return std::move(host.calls);
}

std::int32_t award_intern(const StringPool& pool, const AwardsText& name) {
    return name ? pool.lookup(*name) : -1;
}

void register_weapon_unlocks(AwardRegistries& r, const StringPool& pool, std::span<const std::string> weapon_names) {
    for (std::size_t i = 0; i < weapon_names.size(); ++i) {
        const auto bit = next(r.bit_counter);
        r.unlocks.push_back({award_intern(pool, weapon_names[i]), bit, static_cast<std::int32_t>(i)});
    }
}

void apply_awards_call(AwardRegistries& r, const StringPool& pool, const AwardsInitCall& call) {
    const auto kinds = overload_of(call.native).kinds;
    if (call.args.size() != kinds.size()) fail("argument count mismatch");
    const Args a{call, kinds};
    using N = AwardsNative;
    switch (call.native) {
    case N::print:
        a.text(0);
        return;
    case N::objective2:
    case N::objective4: {
        const auto index = next(r.objective_counter);
        r.objectives.push_back({award_intern(pool, a.text(0)), index, a.integer(1)});
        if (call.native == N::objective4) {
            if (a.word(2) != 0) set_bit(r.solo, index);
            if (a.word(3) != 0) set_bit(r.multi, index);
        }
        return;
    }
    case N::level3:
    case N::level4: {
        const auto bit = next(r.bit_counter);
        r.levels.push_back({award_intern(pool, a.text(0)), bit, call.native == N::level4 ? a.integer(3) : -1,
                            a.integer(1), a.integer(2)});
        return;
    }
    case N::rank2:
    case N::rank3_unlock:
    case N::rank3_special:
    case N::rank4: {
        AwardRankEntry row;
        row.name = a.text(0);
        row.index = static_cast<std::int32_t>(r.ranks.size());
        row.threshold = a.integer(1);
        row.unlock = call.native == N::rank3_special ? a.special(2)
                   : call.native == N::rank2 ? -1 : resolve_unlock(r, pool, a.text(2));
        if (call.native == N::rank4 && a.text(3)) {
            std::array<char, 16> text{};
            const auto& t = *a.text(3);
            for (std::size_t i = 0; i < 16 && i < t.size() && t[i] != '\0'; ++i) text[i] = t[i];
            row.text = text;
        }
        row.title_key = rank_title(pool, row.index);
        r.ranks.push_back(std::move(row));
        return;
    }
    case N::dossier: {
        AwardRankEntry row;
        row.name = a.text(0);
        const auto& wanted = a.text(1);
        for (const auto& rank : r.ranks) {  // 00129218 strcmp, the first equal raw name
            if (!rank.name || !wanted) fail("a dossier compares a null rank name");
            if (std::strcmp(rank.name->c_str(), wanted->c_str()) == 0) {
                row.threshold = rank.threshold;
                break;
            }
        }
        row.index = 0;
        row.unlock = -1;
        row.title_key = rank_title(pool, 0);
        r.dossiers.push_back(std::move(row));
        return;
    }
    case N::rating5:
    case N::rating6_unlock:
    case N::rating6_special: {
        const auto unlock = call.native == N::rating6_special ? a.special(5)
                          : call.native == N::rating6_unlock ? resolve_unlock(r, pool, a.text(5)) : -1;
        r.ratings.push_back({award_intern(pool, a.text(0)), a.integer(1), static_cast<std::uint8_t>(a.word(2) & 0xffu),
                             a.integer(3), a.integer(4), unlock});
        return;
    }
    case N::special_rating4: case N::commendation4: case N::medal4:
        r.awards[award_list(call.native)].push_back(award(pool, a.text(0), a.integer(1), a.text(2), a.integer(3), -1));
        return;
    case N::special_rating5_unlock: case N::commendation5_unlock: case N::medal5_unlock:
        r.awards[award_list(call.native)].push_back(
            award(pool, a.text(0), a.integer(1), a.text(2), a.integer(3), resolve_unlock(r, pool, a.text(4))));
        return;
    case N::special_rating5_special: case N::commendation5_special: case N::medal5_special:
        r.awards[award_list(call.native)].push_back(award(pool, a.text(0), a.integer(1), a.text(2), a.integer(3), a.special(4)));
        return;
    case N::bonus_level:
        r.awards[3].push_back(award(pool, a.text(0), 0, a.text(1), a.integer(2), -1));
        return;
    case N::omega:
        r.awards[4].push_back(award(pool, a.text(0), 0, a.text(1), a.integer(2), -1));
        return;
    case N::omega_requirement4:
    case N::omega_requirement5: {
        AwardOmegaRequirementEntry row;
        row.text_key = award_intern(pool, a.text(0));
        row.name = a.text(0);
        row.a = a.integer(1);
        row.b = a.integer(2);
        row.c = a.integer(3);
        row.unlock = call.native == N::omega_requirement5 ? resolve_unlock(r, pool, a.text(4)) : -1;
        r.omega_requirements.push_back(std::move(row));
        return;
    }
    }
    fail("unknown native");
}

AwardRegistries build_award_registries(const StringPool& pool, std::span<const std::string> weapon_names,
                                       std::span<const AwardsInitCall> calls) {
    AwardRegistries r;
    register_weapon_unlocks(r, pool, weapon_names);
    for (const auto& call : calls) apply_awards_call(r, pool, call);
    return r;
}

AwardTables to_award_tables(const AwardRegistries& r, const StringPool& pool, std::span<const std::string> weapon_names) {
    // Character 1 never occurs in a pool key (32..126), so this name is no pool key.
    static const std::string unpooled = "\x01unpooled";
    bool any_unpooled = false;
    auto name_of = [&](std::int32_t key) -> std::string {
        if (key < 0) {
            any_unpooled = true;
            return unpooled;
        }
        const auto& bytes = pool.entries().at(static_cast<std::size_t>(key)).key;
        const auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
        if (end == bytes.begin()) fail("a registry name interns to an empty pool key");
        return std::string(bytes.begin(), end);
    };
    auto pooled_name = [&](std::int32_t key) {
        if (key < 0) fail("a level or objective name is not in the string pool");
        return name_of(key);
    };
    AwardTables t;
    for (const auto& l : r.levels) t.levels.push_back({pooled_name(l.key), l.pair_first, l.pair_second});
    for (const auto& o : r.objectives) t.objectives.push_back({pooled_name(o.key), o.index, o.points});
    for (std::size_t list = 0; list < agent_award_lists; ++list)
        for (const auto& e : r.awards[list]) t.awards[list].push_back({name_of(e.key), e.unlock, e.points});
    for (const auto& q : r.ratings) t.ratings.push_back({q.points, q.stat, q.threshold, q.unlock});
    for (const auto& k : r.ranks) t.ranks.push_back({k.threshold, k.unlock});
    for (const auto& u : r.unlocks) t.unlocks.push_back({name_of(u.key), u.bit, u.weapon});
    for (const auto& w : weapon_names) t.weapons.push_back(name_of(award_intern(pool, w)));
    if (any_unpooled) t.unpooled.push_back(unpooled);
    return t;
}

}  // namespace omega
