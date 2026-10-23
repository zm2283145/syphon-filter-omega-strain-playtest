#include "objective_registration.hpp"

#include "objective_progression.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
void check(bool condition) {
    if (!condition) throw std::runtime_error("Unsupported AWARDS objective registration prefix");
}
void plain(const Function& f) {
    check(f.dispatch_fields == std::array<Word, 5>{0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU, 0});
}
class Reader {
public:
    const Script& script;
    std::size_t pc;
    void skip_source() {
        while (pc < script.code.size() && (script.code[pc] & 0xffffU) == 59) ++pc;
    }
    Word peek() { skip_source(); check(pc < script.code.size()); return script.code[pc] & 0xffffU; }
    Word take(Word opcode) {
        check(peek() == opcode);
        return script.code[pc++] >> 16;
    }
    std::int32_t integer() {
        check(take(24) == 0 && pc < script.code.size());
        const float value = word_float(script.code[pc++]);
        check(std::isfinite(value) && std::trunc(value) == value
            && static_cast<double>(value) >= std::numeric_limits<std::int32_t>::min()
            && static_cast<double>(value) <= std::numeric_limits<std::int32_t>::max());
        check(take(46) == 0);
        return static_cast<std::int32_t>(value);
    }
    const Function& call() {
        const auto index = take(18);
        check(index < script.functions.size());
        const auto& f = script.functions[index];
        check(f.external && f.return_type == 4);
        plain(f);
        return f;
    }
    std::string string(Word opcode) {
        const auto index = take(opcode);
        check(index < script.strings.size() && script.strings[index].has_value());
        return *script.strings[index];
    }
};
void quoted(std::ostream& out, const std::string& value) {
    out << '"';
    for (char c : value) {
        if (c == '"' || c == '\\') out << '\\';
        out << c;
    }
    out << '"';
}
void bits(std::ostream& out, const ObjectiveBits& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
}
void hex_bytes(std::ostream& out, std::span<const std::uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    out << '"';
    for (const auto value : bytes) out << digits[value >> 4] << digits[value & 15];
    out << '"';
}
}

ObjectiveRegistrationPrefix read_objective_registration_prefix(const Script& script) {
    const Function* entry = nullptr;
    for (const auto& f : script.functions) {
        if (!f.external && f.name == "awards::Init") {
            check(!entry);
            entry = &f;
        }
    }
    check(entry && entry->return_type == 4 && entry->parameters.empty());
    plain(*entry);
    Reader reader{script, entry->entry};
    check(reader.take(1) == 0);
    // A verified logging call precedes the literal table. Decode but do not send it.
    reader.string(52);
    const auto& print = reader.call();
    check(print.name == "client::Print" && print.parameters == std::vector<Word>{3});
    reader.skip_source();
    ObjectiveRegistrationPrefix result{static_cast<Word>(reader.pc), 0, {}};
    while (reader.peek() == 24) {
        const auto multi = reader.integer(), solo = reader.integer(), points = reader.integer();
        check((multi == 0 || multi == 1) && (solo == 0 || solo == 1));
        auto key = reader.string(67);
        check(!key.empty() && key.size() <= 15);
        for (const unsigned char c : key) check(c >= 32 && c <= 126);
        reader.skip_source();
        const auto call_word = static_cast<Word>(reader.pc);
        const auto& f = reader.call();
        check(f.name == "game::RegisterObjective" && f.parameters == std::vector<Word>{7, 1, 1, 1});
        check(result.rows.size() < 256);
        result.rows.push_back({static_cast<Word>(result.rows.size()), std::move(key), points,
                               solo != 0, multi != 0, call_word});
    }
    check(!result.rows.empty());
    result.end_word = static_cast<Word>(reader.pc);
    return result;
}

std::vector<LevelRegistration> read_level_registrations(const Script& awards) {
    struct Item {
        enum class Kind { real, integer, text, opaque } kind;
        float real{};
        std::int32_t integer{};
        std::string text;
    };
    const auto prefix = read_objective_registration_prefix(awards);
    const auto& code = awards.code;
    std::size_t pc = prefix.end_word;
    std::vector<Item> stack;
    std::vector<LevelRegistration> levels;
    auto pop = [&](Item::Kind kind) {
        check(!stack.empty() && stack.back().kind == kind);
        auto item = std::move(stack.back());
        stack.pop_back();
        return item;
    };
    while (pc < code.size()) {
        const Word opcode = code[pc] & 0xffffU, operand = code[pc] >> 16;
        if (opcode == 59) { ++pc; continue; }
        if (opcode == 24) {
            check(operand == 0 && pc + 1 < code.size());
            const float value = word_float(code[pc + 1]);
            check(std::isfinite(value));
            stack.push_back({Item::Kind::real, value});
            pc += 2;
        } else if (opcode == 46) {
            check(operand == 0);
            const float value = pop(Item::Kind::real).real;
            check(std::trunc(value) == value && static_cast<double>(value) >= std::numeric_limits<std::int32_t>::min()
                  && static_cast<double>(value) <= std::numeric_limits<std::int32_t>::max());
            stack.push_back({Item::Kind::integer, 0, static_cast<std::int32_t>(value)});
            ++pc;
        } else if (opcode == 31) {
            check(operand == 0);
            const float b = pop(Item::Kind::real).real, a = pop(Item::Kind::real).real;
            stack.push_back({Item::Kind::real, a * b});
            ++pc;
        } else if (opcode == 39) {
            check(operand == 0);
            const float value = pop(Item::Kind::real).real;
            stack.push_back({Item::Kind::real, -value});
            ++pc;
        } else if (opcode == 67) {
            check(operand < awards.strings.size() && awards.strings[operand].has_value());
            stack.push_back({Item::Kind::text, 0, 0, *awards.strings[operand]});
            ++pc;
        } else if (opcode == 58) {
            check(pc + 1 < code.size());
            stack.push_back({Item::Kind::opaque});
            pc += 2;
        } else if (opcode == 18) {
            check(operand < awards.functions.size());
            const auto& f = awards.functions[operand];
            check(f.external && f.return_type == 4);
            plain(f);
            check(stack.size() == f.parameters.size());
            if (f.name == "game::RegisterLevel") {
                check(f.parameters == std::vector<Word>{7, 1, 1, 1});
                auto name = pop(Item::Kind::text).text;
                const auto first = pop(Item::Kind::integer).integer;
                const auto second = pop(Item::Kind::integer).integer;
                const auto mission = pop(Item::Kind::integer).integer;
                check(!name.empty());
                for (const auto& level : levels) check(level.name != name);
                levels.push_back({std::move(name), mission, first, second, static_cast<Word>(pc)});
            }
            stack.clear();
            ++pc;
        } else break;
    }
    // The block ends at a statement boundary.
    check(stack.empty() && !levels.empty());
    return levels;
}

std::int32_t level_mission_number(const std::vector<LevelRegistration>& levels, std::string_view level) {
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    for (const auto& row : levels) {
        if (row.name.size() != level.size()) continue;
        bool same = true;
        for (std::size_t i = 0; i < level.size() && same; ++i)
            same = lower(static_cast<unsigned char>(row.name[i])) == lower(static_cast<unsigned char>(level[i]));
        if (same) return row.mission_number;
    }
    return 0x16;
}

void write_objective_registration_probe(std::ostream& out, const Script& script) {
    const auto prefix = read_objective_registration_prefix(script);
    ObjectiveBits solo{}, multi{};
    out << "{\"scope\":\"AWARDS registration prefix decoded; original intern-pool IDs unresolved\","
        << "\"start_word\":" << prefix.start_word << ",\"end_word\":" << prefix.end_word
        << ",\"registrations\":[";
    for (std::size_t i = 0; i < prefix.rows.size(); ++i) {
        const auto& row = prefix.rows[i];
        if (i) out << ',';
        const auto bit = Word{1} << (row.index % 32);
        if (row.solo_required) solo[row.index / 32] |= bit;
        if (row.multi_required) multi[row.index / 32] |= bit;
        out << "{\"index\":" << row.index << ",\"key\":";
        quoted(out, row.key);
        out << ",\"points\":" << row.points << ",\"solo_required\":"
            << (row.solo_required ? "true" : "false") << ",\"multi_required\":"
            << (row.multi_required ? "true" : "false") << ",\"call_word\":" << row.call_word << '}';
    }
    out << "],\"solo_required\":";
    bits(out, solo);
    out << ",\"multi_required\":";
    bits(out, multi);
    out << ",\"levels\":[";
    const auto levels = read_level_registrations(script);
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i) out << ',';
        out << "{\"name\":";
        quoted(out, levels[i].name);
        out << ",\"mission_number\":" << levels[i].mission_number << ",\"pair\":[" << levels[i].pair_first << ','
            << levels[i].pair_second << "],\"call_word\":" << levels[i].call_word << '}';
    }
    out << "]}\n";
}

void write_objective_registration_probe(std::ostream& out, const Script& script,
                                       const StringPool& english_base_pool) {
    const auto prefix = read_objective_registration_prefix(script);
    ObjectiveProgression table;
    for (const auto& row : prefix.rows) {
        const auto intern_id = english_base_pool.lookup(row.key);
        // This mapped diagnostic requires complete membership in the proved base pool.
        check(intern_id >= 0);
        check(table.register_objective(intern_id, row.points, row.solo_required,
                                       row.multi_required) == row.index);
    }
    out << "{\"scope\":\"Original English base-pool IDs and AWARDS prefix; diagnostic completion bits\","
        << "\"pool_count\":" << english_base_pool.entries().size()
        << ",\"start_word\":" << prefix.start_word << ",\"end_word\":" << prefix.end_word
        << ",\"pool_entries\":[";
    for (std::size_t i = 0; i < english_base_pool.entries().size(); ++i) {
        if (i) out << ',';
        const auto& entry = english_base_pool.entries()[i];
        out << "{\"key_hex\":";
        hex_bytes(out, entry.key);
        out << ",\"text_hex\":";
        hex_bytes(out, std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(entry.text.data()), entry.text.size()));
        out << '}';
    }
    out << "],\"registrations\":[";
    for (std::size_t i = 0; i < prefix.rows.size(); ++i) {
        if (i) out << ',';
        const auto& row = prefix.rows[i];
        out << "{\"index\":" << row.index << ",\"key\":";
        quoted(out, row.key);
        out << ",\"points\":" << row.points << ",\"solo_required\":"
            << (row.solo_required ? "true" : "false") << ",\"multi_required\":"
            << (row.multi_required ? "true" : "false") << ",\"call_word\":" << row.call_word
            << ",\"intern_id\":" << table.rows()[i].intern_id << '}';
    }
    out << "],\"solo_required\":";
    bits(out, table.solo_required());
    out << ",\"multi_required\":";
    bits(out, table.multi_required());
    out << ",\"operations\":[";
    AgentObjectiveBits agent;
    bool first = true;
    const auto snapshot = [&](const char* action, std::int32_t intern_id, bool clear) {
        if (!first) out << ',';
        first = false;
        out << "{\"action\":\"" << action << '"';
        if (clear) {
            agent.clear_mission_bits_only();
        } else {
            const bool before = agent.is_complete(table, intern_id);
            agent.add(table, intern_id);
            out << ",\"intern_id\":" << intern_id << ",\"index\":" << table.lookup(intern_id)
                << ",\"complete_before\":" << (before ? "true" : "false")
                << ",\"complete_after\":" << (agent.is_complete(table, intern_id) ? "true" : "false");
        }
        out << ",\"persistent\":";
        bits(out, agent.persistent());
        out << ",\"mission\":";
        bits(out, agent.mission());
        out << '}';
    };
    for (const auto& row : table.rows()) snapshot("add_registered", row.intern_id, false);
    const auto first_id = table.rows().front().intern_id;
    snapshot("repeat_first", first_id, false);
    snapshot("missing_intern", -1, false);
    snapshot("clear_mission_bits_only", -1, true);
    snapshot("add_first_after_clear", first_id, false);
    out << "]}\n";
}
}
