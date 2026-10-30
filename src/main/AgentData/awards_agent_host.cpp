#include "awards_agent_host.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("AWARDS agent natives: " + what); }

std::int32_t rd32(std::span<const std::uint8_t> b, std::size_t o) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + o, 4);
    return static_cast<std::int32_t>(v);
}
void wr32(std::span<std::uint8_t> b, std::size_t o, std::int32_t v) { std::memcpy(b.data() + o, &v, 4); }
std::int32_t add32(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }  // 0012b270

// The award lists in registry order (00532a70 + 0x10 L, agent + ab0 + 0x10 L).
enum List : std::size_t { special_rating = 0, commendation = 1, medal = 2, bonus_level = 3, omega_list = 4 };

struct Args {
    const std::vector<Value>& values;
    std::size_t expected;
    Args(const std::vector<Value>& v, std::size_t n) : values(v), expected(n) {
        if (v.size() != n) fail("argument count mismatch");
    }
    Word word(std::size_t i) const {
        const auto* w = std::get_if<Word>(&values[i]);
        if (!w) fail("an integer argument is not a word");
        return *w;
    }
    // A script string: the text, or the null pointer (a Word 0).
    AwardsText text(std::size_t i) const {
        if (const auto* s = std::get_if<std::string>(&values[i])) {
            if (s->find('\0') != std::string::npos) fail("a NUL inside a string argument");
            return *s;
        }
        if (const auto* w = std::get_if<Word>(&values[i]); w && *w == 0) return std::nullopt;
        fail("a string argument is neither text nor null");
    }
};

struct Natives {
    AwardsAgentContext& c;

    void check_state() const {
        for (std::size_t i = 0; i < agent_award_lists; ++i) {
            const auto n = rd32(c.agent, 0xaac + 0x10 * i);  // the EE loops j < count, signed
            if (static_cast<std::size_t>(std::max(n, 0)) != c.state.awards[i].size())
                fail("an award list does not match its count word");
        }
        if (static_cast<std::uint32_t>(rd32(c.agent, 0xc8)) != c.state.level_records.size())
            fail("the level records do not match the +c8 count");
        if (static_cast<std::uint32_t>(rd32(c.agent, 0xb5c)) != c.state.parts.size())
            fail("the parts vector does not match the +b5c count");
    }

    // 003fda40 then the pool id: strncpy(key, name, 16) and lowercase; a null
    // name crashes in strncpy.
    std::int32_t key16(const AwardsText& name) const {
        if (!name) fail("a null name reaches 003fda40 (strncpy)");
        return award_intern(c.pool, name);
    }

    // 003fdae0(id): id < 0 -> "" (0x4be400); else the pool row's 16-byte key
    // as stored, which must hold a NUL.
    std::string key_text(std::int32_t id) const {
        if (id < 0) return {};
        const auto& entries = c.pool.entries();
        if (static_cast<std::size_t>(id) >= entries.size()) fail("a pool id past the pool's rows");
        const auto& k = entries[static_cast<std::size_t>(id)].key;
        const auto end = std::find(k.begin(), k.end(), std::uint8_t{0});
        if (end == k.end()) fail("a pool key with no NUL (003fdae0 reads past the row)");
        return std::string(k.begin(), end);
    }

    AgentAwardEntry& element(std::size_t list, std::size_t j) const {
        auto& v = c.state.awards[list];
        if (j >= v.size()) fail("an award element past the agent's list (the EE indexes it unchecked)");
        return v[j];
    }

    void set_unlock_bit(std::int32_t bit) const {
        const auto u = static_cast<std::uint32_t>(bit);
        if (u >= (0xb50 - 0xb08) * 8) fail("an unlock bit past +b4f (the rank pointer +b50)");
        const std::size_t o = 0xb08 + 4 * (u >> 5);
        wr32(c.agent, o, static_cast<std::int32_t>(static_cast<std::uint32_t>(rd32(c.agent, o)) | 1u << (u & 31)));
    }

    // 00333170(agent, L, name): the first registry row of list L whose
    // requirement range [requirement, requirement + count) holds the pool id
    // gives &element(L, j).bits[id - requirement]; none -> null.
    std::uint8_t* requirement(std::size_t list, const AwardsText& name) const {
        const auto id = award_intern(c.pool, name);
        const auto& rows = c.registries.awards[list];
        for (std::size_t j = 0; j < rows.size(); ++j) {
            const auto& row = rows[j];
            if (id < row.requirement || !(id < add32(row.requirement, row.count))) continue;
            auto& bits = element(list, j).bits;
            const auto k = static_cast<std::uint32_t>(id) - static_cast<std::uint32_t>(row.requirement);
            if (k >= bits.size()) fail("a requirement bit past the element's byte vector");
            return &bits[k];
        }
        return nullptr;
    }
    void unlock_requirement(std::size_t list, const AwardsText& name) const {  // 00332f60 family
        if (auto* p = requirement(list, name); p && *p == 0) *p = 1;
    }
    void complete_requirement(std::size_t list, const AwardsText& name) const {  // 00332fd0 family
        if (auto* p = requirement(list, name)) *p = 2;
    }

    // 00335350(agent, element, row, 0) and the element's flag.
    bool step_and_flag(std::size_t list, std::size_t j) const {
        auto& e = element(list, j);
        award_element_step(c.agent, e, c.registries.awards[list][j].unlock, 0, c.tables);
        return e.flag != 0;
    }

    // 00332da0 / 00332c20 / 00332ce0 / 0032f680: the first row whose key is
    // the pool id.
    Word has_award(std::size_t list, const AwardsText& name) const {
        const auto id = award_intern(c.pool, name);
        const auto& rows = c.registries.awards[list];
        for (std::size_t j = 0; j < rows.size(); ++j)
            if (rows[j].key == id) return step_and_flag(list, j) ? 1 : 0;
        return 0;
    }

    std::int32_t stat_word(std::uint8_t index) const {
        const std::size_t offset = 8 + 4 * std::size_t{index};
        if (offset == 0xc4 || offset == 0xcc) fail("a rating reads the level vector's capacity or pointer word");
        return rd32(c.agent, offset);
    }

    bool objective_bit(std::int32_t bit) const {
        const auto u = static_cast<std::uint32_t>(bit);
        if (u >= 0x40 * 8) fail("an objective bit outside +84..+c3");
        return (static_cast<std::uint32_t>(rd32(c.agent, 0x84 + 4 * (u >> 5))) >> (u & 31)) & 1;
    }

    // 00330aa0 / 003308e0 / 003309c0: the first row with the key, else -1;
    // a negative result is returned as 0 (bgez / move v0, zero), so an
    // unknown name means bit 0.
    template <class Rows, class Field>
    static std::int32_t first_bit(const Rows& rows, std::int32_t id, Field field) {
        for (const auto& row : rows)
            if (row.key == id) return std::max(row.*field, 0);
        return 0;
    }

    // 00332610(agent, name)
    void unlock_part(const AwardsText& name) const {
        std::optional<std::size_t> found;
        for (std::size_t i = 0; !found && i < c.parts.size(); ++i) {
            if (!name) fail("a null part name reaches strcmp");
            if (*name == c.parts[i].name) found = i;  // 00129218 strcmp
        }
        if (!found) {
            const std::string text = "partname " + (name ? *name : std::string("(null)")) + " is not in table\n";
            if (text.size() + 1 > 0xf0 - 0x50) fail("the missing-part message overruns 00332610's frame");
            c.missing_parts.push_back(text);
            return;
        }
        const auto row = static_cast<std::uint32_t>(*found);
        if (std::find(c.state.parts.begin(), c.state.parts.end(), row) != c.state.parts.end()) return;
        // 00330280 -> 00139200: insert one at the end.
        const auto count = static_cast<std::uint32_t>(rd32(c.agent, 0xb5c));
        const auto cap = static_cast<std::uint32_t>(rd32(c.agent, 0xb58));
        const std::uint32_t needed = count + 1;
        if (needed > agent_parts_limit) fail("the parts vector grows past 0x1800000 (00139200 aborts)");
        if (needed > cap) {
            std::uint32_t n = cap ? cap : 1;
            while (n < needed) n = n < 0xc00000 ? n * 2 : agent_parts_limit;
            wr32(c.agent, 0xb58, static_cast<std::int32_t>(n));
        }
        c.state.parts.push_back(row);
        wr32(c.agent, 0xb5c, static_cast<std::int32_t>(needed));
    }

    // 0032f420 UnlockZeus / 0032f340 LockZeus: entries {a, b - 1, c - 1, 0} at
    // +dc, count +d8; a match compares the sign-extended bytes 0..2 with the
    // words.
    std::optional<std::size_t> zeus_find(Word a, Word b, Word c2) const {
        const auto count = rd32(c.agent, 0xd8);
        if (count <= 0) return std::nullopt;
        if (static_cast<std::size_t>(count) > agent_zeus_capacity) fail("the Zeus count reads past +8ab");
        for (std::size_t j = 0; j < static_cast<std::size_t>(count); ++j) {
            const auto* e = c.agent.data() + 0xdc + 4 * j;
            if (static_cast<Word>(static_cast<std::int32_t>(static_cast<std::int8_t>(e[0]))) == a
                && static_cast<Word>(static_cast<std::int32_t>(static_cast<std::int8_t>(e[1]))) == b - 1
                && static_cast<Word>(static_cast<std::int32_t>(static_cast<std::int8_t>(e[2]))) == c2 - 1)
                return j;
        }
        return std::nullopt;
    }
    void unlock_zeus(Word a, Word b, Word c2) const {
        if (zeus_find(a, b, c2)) return;
        const auto count = rd32(c.agent, 0xd8);
        if (count < 0 || static_cast<std::size_t>(count) >= agent_zeus_capacity) fail("a Zeus entry past +8ab");
        wr32(c.agent, 0xd8, count + 1);
        auto* e = c.agent.data() + 0xdc + 4 * static_cast<std::size_t>(count);
        e[0] = static_cast<std::uint8_t>(a);
        e[1] = static_cast<std::uint8_t>(b - 1);
        e[2] = static_cast<std::uint8_t>(c2 - 1);
        e[3] = 0;
        c.agent[0xb72] = 1;
    }
    void lock_zeus(Word a, Word b, Word c2) const {
        const auto j = zeus_find(a, b, c2);
        if (!j) return;
        const auto count = rd32(c.agent, 0xd8) - 1;
        wr32(c.agent, 0xd8, count);
        std::memcpy(c.agent.data() + 0xdc + 4 * *j, c.agent.data() + 0xdc + 4 * static_cast<std::size_t>(count), 4);
        c.agent[0xb72] = 1;
    }

    // 0032f9c0: the levels of +c8 (re-read each step) by strcasecmp of the
    // level name's key text.
    Word has_solo_partime(const AwardsText& name) const {
        for (std::int32_t s = 0; s < rd32(c.agent, 0xc8); ++s) {
            const auto k = static_cast<std::size_t>(s);
            if (k >= c.registries.levels.size()) fail("a level record past the level registry");
            const std::string text = key_text(c.registries.levels[k].key);
            if (!name) fail("a null level name reaches strcasecmp");
            bool equal = text.size() == name->size();
            for (std::size_t i = 0; i < name->size(); ++i) {
                if (static_cast<unsigned char>((*name)[i]) >= 0x80) fail("a level name byte >= 0x80 (newlib's signed ctype)");
                if (equal && lower(text[i]) != lower((*name)[i])) equal = false;
            }
            if (!equal) continue;
            const auto& record = c.state.level_records.at(k);
            const auto need = rd32(record, 0);
            if (need > 0 && !(c.registries.levels[k].pair_first < need)) return 1;
        }
        return 0;
    }

    // 0032fae0: the last rank (by row order) whose threshold the score
    // reached and is no lower than the previous pick.
    Word has_rank(const AwardsText& name) const {
        if (c.port_rank_override) {
            const std::int32_t selected = *c.port_rank_override;
            if (selected == -1) return 0;
            if (selected < 0 || static_cast<std::size_t>(selected) >= c.registries.ranks.size())
                fail("a port rank override outside the registered rank rows");
            const auto& rank_name = c.registries.ranks[static_cast<std::size_t>(selected)].name;
            if (!rank_name || !name) fail("a null rank name reaches strcmp");
            return *rank_name == *name ? 1 : 0;
        }
        const auto s = award_score(c.agent, c.state, c.tables);  // 003305d0
        std::int32_t t0 = 0;
        std::optional<std::size_t> pick;
        const auto& ranks = c.registries.ranks;
        for (std::size_t j = 0; j < ranks.size(); ++j) {
            const auto thr = ranks[j].threshold;
            if (s < thr || thr < t0) continue;
            t0 = thr;
            pick = j;
        }
        if (!pick) return 0;
        const auto& rank_name = ranks[*pick].name;
        if (!rank_name || !name) fail("a null rank name reaches strcmp");
        return *rank_name == *name ? 1 : 0;
    }

    // 003339e0: every objective row whose key text matches the pattern must
    // have its bit set.
    Word are_objectives_complete(const AwardsText& pattern) const {
        for (const auto& row : c.registries.objectives) {
            if (!pattern) fail("a null objective pattern reaches 00333890");
            if (!award_wildcard(*pattern, key_text(row.key))) continue;
            if (row.index < 0 || !objective_bit(row.index)) return 0;
        }
        return 1;
    }
};

struct Signature {
    std::string_view name;
    std::vector<std::string_view> parameters;  // after the AgentData
    std::string_view result;
};

const std::vector<Signature>& signatures() {
    static const std::vector<Signature> table{
        {"GetStat", {"enum:AgentData_ScoreTypes"}, "primitive:1"},                 // 0032f5c0
        {"IsObjectiveComplete", {"primitive:7"}, "primitive:1"},                   // -> 00333b10
        {"AreObjectivesComplete", {"primitive:3"}, "primitive:1"},                 // -> 003339e0
        {"HasRank", {"primitive:7"}, "primitive:1"},                               // 0032fae0
        {"HasMedal", {"primitive:7"}, "primitive:1"},                              // 0032fab0 -> 00332ce0
        {"HasSoloPartime", {"primitive:7"}, "primitive:1"},                        // 0032f9c0
        {"HasOmega", {}, "primitive:1"},
        {"HasRating", {"primitive:7"}, "primitive:1"},                             // -> 00332e60
        {"HasCommendation", {"primitive:7"}, "primitive:1"},                       // -> 00332c20
        {"HasSpecialRating", {"primitive:7"}, "primitive:1"},                      // -> 00332da0
        {"HasAllSpecialRatings", {}, "primitive:1"},
        {"HasAllRatings", {}, "primitive:1"},
        {"HasBonusLevel", {"primitive:7"}, "primitive:1"},                         // 0032f680
        {"UnlockLevel", {"primitive:7"}, "primitive:4"},                           // -> 003330c0
        {"UnlockWeapon", {"primitive:7"}, "primitive:4"},                          // -> 00333050
        {"UnlockSpecial", {"enum:AgentData_SpecialBonusTypes"}, "primitive:4"},
        {"UnlockZeus", {"primitive:1", "primitive:1", "primitive:1"}, "primitive:4"},  // 0032f420
        {"LockZeus", {"primitive:1", "primitive:1", "primitive:1"}, "primitive:4"},    // 0032f340
        {"UnlockPart", {"primitive:3"}, "primitive:4"},                            // -> 00332610
        {"UnlockMedalRequirement", {"primitive:7"}, "primitive:4"},
        {"UnlockSpecialRatingRequirement", {"primitive:7"}, "primitive:4"},
        {"UnlockCommendationRequirement", {"primitive:7"}, "primitive:4"},
        {"UnlockOmegaRequirement", {"primitive:7"}, "primitive:4"},
        {"UnlockBonusLevelRequirement", {"primitive:7"}, "primitive:4"},
        {"CompleteMedalRequirement", {"primitive:7"}, "primitive:4"},
        {"CompleteSpecialRatingRequirement", {"primitive:7"}, "primitive:4"},
        {"CompleteCommendationRequirement", {"primitive:7"}, "primitive:4"},
        {"CompleteOmegaRequirement", {"primitive:7"}, "primitive:4"},
        {"CompleteBonusLevelRequirement", {"primitive:7"}, "primitive:4"},
    };
    return table;
}

std::optional<std::size_t> requirement_list(std::string_view name, std::string_view prefix) {
    if (name.substr(0, prefix.size()) != prefix || name.size() < prefix.size() + 11
        || name.substr(name.size() - 11) != "Requirement")
        return std::nullopt;
    const auto kind = name.substr(prefix.size(), name.size() - prefix.size() - 11);
    if (kind == "SpecialRating") return special_rating;
    if (kind == "Commendation") return commendation;
    if (kind == "Medal") return medal;
    if (kind == "BonusLevel") return bonus_level;
    if (kind == "Omega") return omega_list;
    return std::nullopt;
}

}  // namespace

std::optional<Word> call_awards_agent_native(AwardsAgentContext& context, std::string_view name,
    const std::vector<Value>& arguments) {
    Natives n{context};
    n.check_state();
    if (name == "GetStat") {  // (u8)value < 31 ? the word +8 + 4 value : 0
        const auto v = static_cast<std::uint8_t>(Args(arguments, 1).word(0));
        return v < 31 ? static_cast<Word>(rd32(context.agent, 8 + 4 * std::size_t{v})) : 0;
    }
    if (name == "IsObjectiveComplete") {
        const auto id = n.key16(Args(arguments, 1).text(0));
        const auto bit = Natives::first_bit(context.registries.objectives, id, &AwardObjectiveEntry::index);
        return bit < 0 ? 0 : (n.objective_bit(bit) ? 1 : 0);
    }
    if (name == "AreObjectivesComplete") return n.are_objectives_complete(Args(arguments, 1).text(0));
    if (name == "HasRank") return n.has_rank(Args(arguments, 1).text(0));
    if (name == "HasMedal") return n.has_award(medal, Args(arguments, 1).text(0));
    if (name == "HasCommendation") return n.has_award(commendation, Args(arguments, 1).text(0));
    if (name == "HasSpecialRating") return n.has_award(special_rating, Args(arguments, 1).text(0));
    if (name == "HasBonusLevel") return n.has_award(bonus_level, Args(arguments, 1).text(0));
    if (name == "HasSoloPartime") return n.has_solo_partime(Args(arguments, 1).text(0));
    if (name == "HasOmega") {  // 00335350 on element 0 with row 0 of list 4
        Args(arguments, 0);
        if (context.registries.awards[omega_list].empty()) fail("HasOmega reads row 0 of an empty omega registry");
        return n.step_and_flag(omega_list, 0) ? 1 : 0;
    }
    if (name == "HasRating") {
        const auto id = award_intern(context.pool, Args(arguments, 1).text(0));
        for (const auto& r : context.registries.ratings)
            if (r.key == id) return n.stat_word(r.stat) < r.threshold ? 0 : 1;
        return 0;
    }
    if (name == "HasAllRatings") {
        Args(arguments, 0);
        for (const auto& r : context.registries.ratings)
            if (n.stat_word(r.stat) < r.threshold) return 0;
        return 1;
    }
    if (name == "HasAllSpecialRatings") {  // the flags of elements 0 .. registry count - 1
        Args(arguments, 0);
        for (std::size_t j = 0; j < context.registries.awards[special_rating].size(); ++j)
            if (n.element(special_rating, j).flag == 0) return 0;
        return 1;
    }
    if (name == "UnlockLevel" || name == "UnlockWeapon") {
        const auto id = n.key16(Args(arguments, 1).text(0));
        const auto bit = name == "UnlockLevel"
            ? Natives::first_bit(context.registries.levels, id, &AwardLevelEntry::bit)
            : Natives::first_bit(context.registries.unlocks, id, &AwardUnlockEntry::bit);
        if (bit >= 0) n.set_unlock_bit(bit);
        return std::nullopt;
    }
    if (name == "UnlockSpecial") {
        n.set_unlock_bit(static_cast<std::uint8_t>(Args(arguments, 1).word(0)));
        return std::nullopt;
    }
    if (name == "UnlockZeus" || name == "LockZeus") {
        const Args a(arguments, 3);
        if (name == "UnlockZeus") n.unlock_zeus(a.word(0), a.word(1), a.word(2));
        else n.lock_zeus(a.word(0), a.word(1), a.word(2));
        return std::nullopt;
    }
    if (name == "UnlockPart") {
        n.unlock_part(Args(arguments, 1).text(0));
        return std::nullopt;
    }
    if (const auto list = requirement_list(name, "Unlock")) {
        n.unlock_requirement(*list, Args(arguments, 1).text(0));
        return std::nullopt;
    }
    if (const auto list = requirement_list(name, "Complete")) {
        n.complete_requirement(*list, Args(arguments, 1).text(0));
        return std::nullopt;
    }
    fail("AgentData native " + std::string(name) + " is not hosted");
}

std::optional<Value> AwardsAgentHost::call(const Script& script, const Function& function,
    const std::vector<Value>& arguments) {
    if (!function.external) fail("a call to a defined function reached the host");
    const auto it = std::find_if(signatures().begin(), signatures().end(),
        [&](const Signature& s) { return s.name == function.name; });
    if (it == signatures().end()) fail(function.name + " is not hosted");
    if (function.parameters.size() != it->parameters.size() + 1
        || script.type_key(function.parameters[0]) != "object:AgentData"
        || script.type_key(function.return_type) != it->result)
        fail(function.name + " does not have the bound signature");
    for (std::size_t i = 0; i < it->parameters.size(); ++i)
        if (script.type_key(function.parameters[i + 1]) != it->parameters[i])
            fail(function.name + " does not have the bound signature");
    if (arguments.empty() || arguments[0] != Value{token_}) fail(function.name + " is not called on the agent");
    const auto result = call_awards_agent_native(context_, function.name,
        std::vector<Value>(arguments.begin() + 1, arguments.end()));
    if (it->result == "primitive:4") return std::nullopt;
    return Value{*result};
}

void run_set_awards_and_bonuses(const Script& awards, AwardsAgentContext& context) {
    constexpr Word token = 0x5a6e7401;  // an opaque agent handle; only compared
    AwardsAgentHost host(context, token);
    Vm vm(awards, host, 20'000'000);
    vm.initialize();
    vm.invoke("awards::SetAwardsAndBonuses", {Value{token}});
}

}  // namespace omega
