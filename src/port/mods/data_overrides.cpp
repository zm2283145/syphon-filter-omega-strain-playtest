// Port-only (checkpoint 132md16): see data_overrides.hpp.
#include "data_overrides.hpp"

#include "population.hpp"
#include "weapon_aim.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace omega::mods {
namespace {

using T = DataFieldType;

// COMMON\WEAPONS.WDB record fields (weapon_database.hpp, COMBAT.md section 1's field map). The ranges are the port's
// (wide enough for any record on the disc; tight enough that the game's arithmetic stays sane). Not offered: the
// class, kind, damage type, projectile, hold class and ammo type (they pick code paths, models and inventory slots),
// the names and the runtime words.
constexpr DataField kWeaponFields[] = {
    {"range_near", 0x34, T::i32, 0, 10000, "the near band's bound (accuracy_near / damage_near within it)"},
    {"range_mid", 0x38, T::i32, 0, 10000, "the middle band's bound"},
    {"range_far", 0x3c, T::i32, 0, 10000, "the far band's bound; also the hitscan length"},
    {"accuracy_near", 0x40, T::i32, 0, 100, "percent, within range_near (0022f5b0)"},
    {"accuracy_mid", 0x44, T::i32, 0, 100, "percent, within range_mid"},
    {"accuracy_far", 0x4c, T::i32, 0, 100, "percent, within range_far"},
    {"aim_time_near", 0x50, T::f32, 0, 60, "seconds the aim takes to settle, near"},
    {"aim_time_far", 0x54, T::f32, 0, 60, "seconds the aim takes to settle, far"},
    {"burst", 0x5c, T::i32, 0, 100, "rounds a trigger pull fires"},
    {"damage_mid", 0x64, T::i32, 0, 100000, "damage a hit does in the middle band"},
    {"damage_near", 0x68, T::i32, 0, 100000, "damage a hit does in the near band"},
    {"damage_far", 0x6c, T::i32, 0, 100000, "damage a hit does in the far band"},
    {"clip", 0x70, T::i32, 1, 999, "rounds a clip holds (the HUD's first number)"},
    {"clips", 0x74, T::i32, 0, 99, "clips a fresh pickup / loadout carries (the reserve is clip x clips)"},
    {"full_auto", 0x78, T::u8, 0, 2, "0 single shot, 1 / 2 the automatic fire modes"},
    {"tracer_interval", 0xa0, T::i32, 0, 100, "a tracer every n rounds (0: none)"},
    {"rate", 0xa4, T::f32, 0, 60, "rounds a second"},
    {"noise_radius", 0xb8, T::f32, 0, 5000, "how far the AI hears a shot"},
    {"explosion_radius", 0xc4, T::f32, 0, 1000, "> 0: a blast at the end of travel (0021a200)"},
    {"explosion_outer", 0xc8, T::f32, 0, 1000, "the blast's outer radius (002774a0)"},
    {"penetration", 0xf8, T::i32, 0, 1000, "what a round passes through (bullet +10)"},
    {"impact_noise", 0xfc, T::f32, 0, 5000, "how far the AI hears a round's impact"},
    {"power", 0x104, T::i32, 0, 5000, "the round's speed; >= 200 enables the hit roll"},
};

// GAMEDATA\<LEVEL>\DATA.POP NPC: records' parameter words (offset: 0 health, 4 armor).
constexpr DataField kNpcFields[] = {
    {"health", 0, T::i32, 1, 100000, "the NPC's health ([npc <LEVEL> <id>] / [npc <LEVEL> player])"},
    {"armor", 4, T::i32, 0, 100000, "the NPC's armor (the player's is StartingArmor's, whatever the record says)"},
    {"health_percent", 0, T::i32, 1, 1000, "every AI NPC's health scaled ([npc <LEVEL> *] / [npc * *])"},
    {"armor_percent", 4, T::i32, 0, 1000, "every AI NPC's armor scaled ([npc <LEVEL> *] / [npc * *])"},
};

// game::ToHit (0048ac80, engine_globals.cpp to_hit_settings_defaults), in ToHitSettings' order.
constexpr DataField kToHitFields[] = {
    {"runAccFactor", 0, T::f32, 0, 10, "the shooter's accuracy while running"},
    {"runAimFactor", 4, T::f32, 0, 10, "the shooter's aim time while running"},
    {"kneelAccFactor", 8, T::f32, 0, 10, "the shooter's accuracy while kneeling"},
    {"kneelAimFactor", 12, T::f32, 0, 10, "the shooter's aim time while kneeling"},
    {"movingTargAccFactor", 16, T::f32, 0, 10, "accuracy against a moving target"},
    {"crouchTargAccFactor", 20, T::f32, 0, 10, "accuracy against a crouching target"},
    {"rollTargAccFactor", 24, T::f32, 0, 10, "accuracy against a rolling target"},
    {"maxVelocity", 28, T::f32, 0, 1000, "the target speed the moving factor saturates at"},
    {"npcExtraAimTime", 32, T::f32, 0, 60, "seconds an NPC's aim takes beyond the weapon's"},
    {"npcMovingTargVal", 36, T::f32, 0, 1, "an NPC's accuracy loss against a moving target"},
    {"npcRollingTargVal", 40, T::f32, 0, 1, "an NPC's accuracy loss against a rolling target"},
    {"reloadAimLoss", 44, T::f32, 0, 1, "the aim lost by a reload"},
    {"npcNearDist", 48, T::f32, 0, 1000, "the distance an NPC counts as near"},
    {"npcNearMul", 52, T::f32, 0, 100, "an NPC's accuracy multiplier when near"},
};

constexpr std::string_view kWdbKey = "GAMEDATA/COMMON/WEAPONS.WDB";
constexpr std::size_t kWdbHeader = 0x10, kWdbRecord = 0x140, kWdbName = 0x80, kWdbDamageType = 0xbc;

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::string lower_text(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool all_digits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool level_name(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8 | std::uint32_t(b[at + 2]) << 16 | std::uint32_t(b[at + 3]) << 24;
}

void put_le32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

std::string number_text(double v) {
    if (v == std::floor(v) && std::abs(v) < 1e15) return std::to_string(static_cast<long long>(v));
    char text[32];
    const auto [end, error] = std::to_chars(text, text + sizeof text, static_cast<float>(v));
    return error == std::errc{} ? std::string(text, end) : std::to_string(v);
}

double field_value(std::span<const std::uint8_t> b, std::size_t at, DataFieldType type) {
    switch (type) {
    case T::u8: return b[at];
    case T::f32: return std::bit_cast<float>(le32(b, at));
    default: return static_cast<std::int32_t>(le32(b, at));
    }
}

void set_field_value(std::vector<std::uint8_t>& b, std::size_t at, DataFieldType type, double v) {
    switch (type) {
    case T::u8: b[at] = static_cast<std::uint8_t>(v); break;
    case T::f32: put_le32(b, at, std::bit_cast<std::uint32_t>(static_cast<float>(v))); break;
    default: put_le32(b, at, static_cast<std::uint32_t>(static_cast<std::int32_t>(v))); break;
    }
}

bool damage_field(const DataField& f) { return f.offset >= 0x64 && f.offset <= 0x6c; }

std::size_t wdb_count(std::span<const std::uint8_t> wdb) {
    if (wdb.size() < kWdbHeader) throw std::runtime_error("WEAPONS.WDB is shorter than its header");
    if (le32(wdb, 0) != 4) throw std::runtime_error("WEAPONS.WDB is not version 4");
    const auto count = static_cast<std::int32_t>(le32(wdb, 4));
    if (count <= 0) return 0;
    if ((wdb.size() - kWdbHeader) / kWdbRecord < static_cast<std::size_t>(count))
        throw std::runtime_error("WEAPONS.WDB is shorter than its " + std::to_string(count) + " records");
    return static_cast<std::size_t>(count);
}

std::string record_key(std::span<const std::uint8_t> wdb, std::size_t record) {
    const auto at = kWdbHeader + record * kWdbRecord + kWdbName;
    std::string out;
    for (std::size_t i = 0; i < 32 && wdb[at + i]; ++i) out += static_cast<char>(wdb[at + i]);
    return out;
}

std::string summary(std::string_view file, const std::vector<DataChange>& changes) {
    std::set<std::string> mods;
    for (const auto& c : changes) mods.insert(c.mod);
    std::string out = std::string(file) + ": " + std::to_string(changes.size()) + " field(s) from";
    for (const auto& m : mods) out += " " + m;
    // The values that change first (a percentage leaves many zero armors as they are).
    std::vector<const DataChange*> order;
    for (const auto& c : changes)
        if (c.from != c.to) order.push_back(&c);
    for (const auto& c : changes)
        if (c.from == c.to) order.push_back(&c);
    out += " (";
    for (std::size_t i = 0; i < order.size() && i < 6; ++i)
        out += (i ? ", " : "") + order[i]->what + " " + number_text(order[i]->from) + " -> " + number_text(order[i]->to);
    if (order.size() > 6) out += ", ...";
    return out + ")";
}

}  // namespace

std::span<const DataField> data_fields(std::string_view table) {
    if (table == "weapon") return kWeaponFields;
    if (table == "npc") return kNpcFields;
    if (table == "tohit") return kToHitFields;
    return {};
}

const DataField* find_data_field(std::string_view table, std::string_view name) {
    const auto wanted = lower_text(name);
    for (const auto& f : data_fields(table))
        if (lower_text(f.name) == wanted) return &f;
    return nullptr;
}

std::string data_entry_key(const DataOverride& item) {
    return item.table + (item.selector.empty() ? std::string() : " " + item.selector) + " " + item.field;
}

bool is_data_file_key(std::string_view key) {
    return key == kWdbKey || (key.starts_with("GAMEDATA/") && key.ends_with("/DATA.POP") && std::count(key.begin(), key.end(), '/') == 2);
}

std::vector<DataOverride> parse_data_overrides(std::string_view text, std::string_view file) {
    std::vector<DataOverride> out;
    std::set<std::string> seen;
    std::string table, selector;
    bool in_section = false;
    std::size_t line_number = 0;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        ++line_number;
        auto line = trim(text.substr(begin, end - begin));
        begin = end + 1;
        if (line_number == 1 && line.starts_with("\xEF\xBB\xBF")) line = trim(line.substr(3));
        const auto where = std::string(file) + " line " + std::to_string(line_number) + ": ";
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[') {
            if (line.back() != ']') throw std::runtime_error(where + "a section is [table ...]");
            const auto inside = trim(std::string_view(line).substr(1, line.size() - 2));
            const auto space = inside.find_first_of(" \t");
            table = lower_text(inside.substr(0, space));
            const auto rest = space == std::string::npos ? std::string() : trim(std::string_view(inside).substr(space));
            if (table == "tohit") {
                if (!rest.empty()) throw std::runtime_error(where + "[tohit] takes no selector");
                selector.clear();
            } else if (table == "weapon") {
                if (rest.empty()) throw std::runtime_error(where + "[weapon] needs a record id or display key ([weapon 112], [weapon SocomSilenced])");
                if (all_digits(rest)) {
                    if (rest.size() > 6) throw std::runtime_error(where + "weapon record id " + rest + " is out of range");
                    selector = std::to_string(std::stoul(rest));
                } else {
                    if (rest.size() > 31) throw std::runtime_error(where + "weapon display keys are at most 31 characters");
                    selector = upper(rest);
                }
            } else if (table == "npc") {
                const auto split = rest.find_first_of(" \t");
                if (rest.empty() || split == std::string::npos)
                    throw std::runtime_error(where + "[npc] needs a level and an id ([npc TORONTO1 133140], [npc TORONTO1 player], [npc TORONTO1 *], [npc * *])");
                const auto level = upper(rest.substr(0, split));
                auto id = upper(trim(std::string_view(rest).substr(split)));
                if (level != "*" && !level_name(level)) throw std::runtime_error(where + "'" + level + "' is not a level folder name");
                if (id != "*" && id != "PLAYER") {
                    if (!all_digits(id) || id.size() > 10 || std::stoull(id) > 0xffffffffull)
                        throw std::runtime_error(where + "'" + id + "' is not an NPC id (decimal, as the play log gives it), player or *");
                    id = std::to_string(std::stoull(id));
                }
                if (level == "*" && id != "*")
                    throw std::runtime_error(where + "NPC ids are a level's: [npc * *] or [npc <LEVEL> " + id + "]");
                selector = level + " " + id;
            } else {
                throw std::runtime_error(where + "unknown table '" + table + "' (weapon, npc, tohit)");
            }
            in_section = true;
            continue;
        }
        if (!in_section) throw std::runtime_error(where + "a field before any [section]");
        const auto equals = line.find('=');
        if (equals == std::string::npos) throw std::runtime_error(where + "expected field = value");
        const auto name = trim(std::string_view(line).substr(0, equals));
        const auto value_text = trim(std::string_view(line).substr(equals + 1));
        const auto* field = find_data_field(table, name);
        if (!field) {
            std::string known;
            for (const auto& f : data_fields(table)) known += (known.empty() ? "" : ", ") + std::string(f.name);
            throw std::runtime_error(where + "unknown " + table + " field '" + name + "' (" + known + ")");
        }
        if (table == "npc") {
            const bool wildcard = selector.ends_with(" *");
            const bool percent = std::string_view(field->name).ends_with("_percent");
            if (wildcard != percent)
                throw std::runtime_error(where + std::string(field->name) + " is for " +
                                         (percent ? "[npc <LEVEL> *] / [npc * *]" : "[npc <LEVEL> <id>] / [npc <LEVEL> player]") +
                                         " (" + (wildcard ? "health_percent / armor_percent" : "health / armor") + " here)");
        }
        double value = 0;
        if (field->type == T::f32) {
            const auto [ptr, error] = std::from_chars(value_text.data(), value_text.data() + value_text.size(), value);
            if (value_text.empty() || error != std::errc{} || ptr != value_text.data() + value_text.size() || !std::isfinite(value))
                throw std::runtime_error(where + std::string(field->name) + " is a number ('" + value_text + "')");
        } else {
            long long whole = 0;
            const auto* first = value_text.data();
            if (!value_text.empty() && *first == '+') ++first;
            const auto [ptr, error] = std::from_chars(first, value_text.data() + value_text.size(), whole);
            if (value_text.empty() || error != std::errc{} || ptr != value_text.data() + value_text.size())
                throw std::runtime_error(where + std::string(field->name) + " is a whole number ('" + value_text + "')");
            value = static_cast<double>(whole);
        }
        if (value < field->min || value > field->max)
            throw std::runtime_error(where + std::string(field->name) + " " + value_text + " is out of range (" + number_text(field->min) +
                                     ".." + number_text(field->max) + ")");
        DataOverride item{table, selector, std::string(field->name), value, std::string(file), line_number};
        if (!seen.insert(data_entry_key(item)).second)
            throw std::runtime_error(where + std::string(field->name) + " is given twice for [" + table +
                                     (selector.empty() ? "" : " " + selector) + "]");
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<std::string> weapon_record_keys(std::span<const std::uint8_t> wdb) {
    std::vector<std::string> out;
    const auto count = wdb_count(wdb);
    for (std::size_t r = 0; r < count; ++r) out.push_back(record_key(wdb, r));
    return out;
}

std::optional<std::vector<std::uint8_t>> apply_data_overrides(const ModSet& set, std::string_view key,
                                                              std::span<const std::uint8_t> bytes, std::vector<DataChange>* changes) {
    if (!set.has_data() || !is_data_file_key(key)) return std::nullopt;
    std::vector<DataChange> local;
    auto& out_changes = changes ? *changes : local;
    out_changes.clear();
    std::vector<std::uint8_t> patched(bytes.begin(), bytes.end());
    if (key == kWdbKey) {
        const auto count = wdb_count(bytes);
        std::vector<std::string> keys;
        for (std::size_t r = 0; r < count; ++r) keys.push_back(upper(record_key(bytes, r)));
        // (record, field offset) -> the first value in resolution order (an id and a display key may both name it).
        std::map<std::pair<std::size_t, std::size_t>, const ModSet::DataSource*> chosen;
        for (const auto& d : set.data()) {
            if (d.item.table != "weapon") continue;
            const auto* field = find_data_field("weapon", d.item.field);
            if (!field) continue;
            for (std::size_t r = 0; r < count; ++r)
                if (all_digits(d.item.selector) ? std::stoul(d.item.selector) == r : keys[r] == d.item.selector)
                    chosen.emplace(std::pair{r, field->offset}, &d);
        }
        if (chosen.empty()) return std::nullopt;
        for (const auto& [where, d] : chosen) {
            const auto* field = find_data_field("weapon", d->item.field);
            const auto at = kWdbHeader + where.first * kWdbRecord + field->offset;
            // 00147400 divides the damage words of a damage-type-1 record by 5: store 5x what the game is to use.
            const bool scaled = damage_field(*field) && bytes[kWdbHeader + where.first * kWdbRecord + kWdbDamageType] == 1;
            const double from = field_value(bytes, at, field->type);
            set_field_value(patched, at, field->type, scaled ? d->item.value * 5 : d->item.value);
            out_changes.push_back({"WEAPONS.WDB record " + std::to_string(where.first) + " " + record_key(bytes, where.first) + " " +
                                       std::string(field->name) + (scaled ? " (stored x5)" : ""),
                                   scaled ? std::trunc(from / 5) : from, d->item.value, d->source.mod->folder});
        }
        note("data: " + summary("WEAPONS.WDB", out_changes));
        return patched;
    }
    // GAMEDATA/<LEVEL>/DATA.POP
    const auto level = std::string(key.substr(9, key.find('/', 9) - 9));
    const auto population = decode_population(bytes);
    const auto first = [&](const std::string& selector, std::string_view field) -> const ModSet::DataSource* {
        for (const auto& d : set.data())
            if (d.item.table == "npc" && d.item.selector == selector && d.item.field == field) return &d;
        return nullptr;
    };
    for (std::size_t i = 0; i < population.npcs.size(); ++i) {
        const auto& npc = population.npcs[i];
        const auto own = level + " " + (i == 0 ? std::string("PLAYER") : std::to_string(npc.id));
        const auto by_id = level + " " + std::to_string(npc.id);
        for (const auto& [name, percent_name, word] :
             {std::tuple{"health", "health_percent", std::size_t{0}}, std::tuple{"armor", "armor_percent", std::size_t{4}}}) {
            const auto* d = first(by_id, name);
            if (!d && i == 0) d = first(own, name);
            const auto at = npc.parameters_offset + word;
            if (at + 4 > patched.size()) throw std::runtime_error(std::string(key) + ": an NPC record lies past the file");
            const auto from = static_cast<double>(static_cast<std::int32_t>(le32(bytes, at)));
            double to = from;
            if (d) to = d->item.value;
            else if (i != 0) {
                d = first(level + " *", percent_name);
                if (!d) d = first("* *", percent_name);
                if (!d) continue;
                to = std::clamp(std::round(from * d->item.value / 100.0), word == 0 ? 1.0 : 0.0, 100000.0);
            } else continue;
            put_le32(patched, at, static_cast<std::uint32_t>(static_cast<std::int32_t>(to)));
            out_changes.push_back({level + " DATA.POP NPC " + std::to_string(npc.id) + (i == 0 ? " (player)" : "") + " " + name, from, to,
                                   d->source.mod->folder});
        }
    }
    if (out_changes.empty()) return std::nullopt;
    note("data: " + summary(level + " DATA.POP", out_changes));
    return patched;
}

std::vector<std::pair<std::string, float>> tohit_overrides(const ModSet& set) {
    std::vector<std::pair<std::string, float>> out;
    for (const auto& d : set.data())
        if (d.item.table == "tohit") out.emplace_back(d.item.field, static_cast<float>(d.item.value));
    return out;
}

std::size_t apply_tohit_overrides(const ModSet& set, ToHitSettings& s) {
    std::array<float*, 14> members{&s.run_acc_factor,      &s.run_aim_factor,        &s.kneel_acc_factor,      &s.kneel_aim_factor,
                                   &s.moving_targ_acc_factor, &s.crouch_targ_acc_factor, &s.roll_targ_acc_factor, &s.max_velocity,
                                   &s.npc_extra_aim_time,  &s.npc_moving_targ_val,   &s.npc_rolling_targ_val,  &s.reload_aim_loss,
                                   &s.npc_near_dist,       &s.npc_near_mul};
    std::size_t changed = 0;
    for (const auto& [name, value] : tohit_overrides(set))
        if (const auto* field = find_data_field("tohit", name)) {
            *members[field->offset / 4] = value;
            ++changed;
        }
    return changed;
}

}  // namespace omega::mods
