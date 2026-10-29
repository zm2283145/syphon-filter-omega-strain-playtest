#include "port_cheats.hpp"

#include "awards_agent_host.hpp"
#include "profile_awards.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace omega {
namespace {

AwardsAgentContext context_of(const ProfileAwards& awards, std::span<std::uint8_t, agent_record_size> agent,
    AgentLoadState& state) {
    return AwardsAgentContext{agent, state, awards.registries, awards.tables, awards.pool, awards.parts, {}};
}

void call(AwardsAgentContext& c, const char* native, Value argument) {
    call_awards_agent_native(c, native, std::vector<Value>{std::move(argument)});
}

// The text whose intern (003fdb40) is the pool id `id`: its key up to the NUL,
// when that key names the id (a later duplicate key interns to the first row).
std::optional<std::string> pool_name(const StringPool& pool, std::int32_t id) {
    if (id < 0 || static_cast<std::size_t>(id) >= pool.entries().size()) return std::nullopt;
    const auto& key = pool.entries()[static_cast<std::size_t>(id)].key;
    std::string s;
    for (const auto b : key) {
        if (b == 0) break;
        if (b < 32 || b > 126) return std::nullopt;
        s.push_back(static_cast<char>(b));
    }
    if (s.empty() || s.size() > 15 || pool.lookup(s) != id) return std::nullopt;
    return s;
}

// The store 00333050 / 003330c0 make for a found row (the word
// +b08 + 4 (bit >> 5), bit & 31).
void set_unlock_bit(std::span<std::uint8_t, agent_record_size> agent, std::int32_t bit) {
    const auto u = static_cast<std::uint32_t>(bit);
    if (u >= (0xb50 - 0xb08) * 8) throw std::runtime_error("port cheat: an unlock bit past +b4f");
    const std::size_t o = 0xb08 + 4 * (u >> 5);
    agent[o + ((u & 31) >> 3)] |= static_cast<std::uint8_t>(1u << (u & 7));
}

}  // namespace

void port_unlock_all_weapons(const ProfileAwards& awards, std::span<std::uint8_t, agent_record_size> agent,
    AgentLoadState& state) {
    auto c = context_of(awards, agent, state);
    for (const auto& row : awards.registries.unlocks)
        if (const auto name = pool_name(awards.pool, row.key)) call(c, "UnlockWeapon", Value{*name});
}

void port_unlock_all(const ProfileAwards& awards, std::span<std::uint8_t, agent_record_size> agent,
    AgentLoadState& state, std::uint8_t& level_restrict) {
    auto c = context_of(awards, agent, state);
    // Registry keys are pool ids (003fdb40); the natives re-intern the key text.
    for (const auto& row : awards.registries.levels)
        if (const auto name = pool_name(awards.pool, row.key)) call(c, "UnlockLevel", Value{*name});
        else set_unlock_bit(agent, row.bit);
    port_unlock_all_weapons(awards, agent, state);
    for (Word special = 0; special < 6; ++special) call(c, "UnlockSpecial", Value{special});
    for (const auto& row : awards.parts) call(c, "UnlockPart", Value{row.name});
    // The registry lists in AwardRegistries::awards order.
    static constexpr const char* requirement_natives[agent_award_lists] = {
        "UnlockSpecialRatingRequirement", "UnlockCommendationRequirement", "UnlockMedalRequirement",
        "UnlockBonusLevelRequirement", "UnlockOmegaRequirement"};
    for (std::size_t list = 0; list < agent_award_lists; ++list)
        for (const auto& row : awards.registries.awards[list])
            for (std::int32_t k = 0; k < row.count; ++k)
                if (const auto name = pool_name(awards.pool, row.requirement + k))
                    call(c, requirement_natives[list], Value{*name});
    level_restrict = 0;  // 0013d270: sb zero, 004ffbb5
}

namespace {
std::set<const SoloActorSimulation*>& ignored_actors() {
    static std::set<const SoloActorSimulation*> actors;
    return actors;
}
std::set<const SoloActorSimulation*>& engaged_actors() {
    static std::set<const SoloActorSimulation*> actors;
    return actors;
}

struct CheatState {
    std::array<bool, port_cheat_count> on{};
    bool menu = true;
    std::function<void(PortCheat, bool)> hook;
};
CheatState& cheats() {
    static CheatState s;
    return s;
}

std::uint32_t rd32(std::span<const std::uint8_t, agent_record_size> a, std::size_t o) {
    return static_cast<std::uint32_t>(a[o]) | static_cast<std::uint32_t>(a[o + 1]) << 8 |
           static_cast<std::uint32_t>(a[o + 2]) << 16 | static_cast<std::uint32_t>(a[o + 3]) << 24;
}
void wr32(std::span<std::uint8_t, agent_record_size> a, std::size_t o, std::uint32_t v) {
    for (std::size_t k = 0; k < 4; ++k) a[o + k] = static_cast<std::uint8_t>(v >> (8 * k));
}

const GuiNode* find_node(const GuiNode& n, const std::string& name) {
    if (n.name == name) return &n;
    for (const auto& c : n.children)
        if (const GuiNode* f = find_node(c, name)) return f;
    return nullptr;
}
GuiNode* find_node(GuiNode& n, const std::string& name) {
    return const_cast<GuiNode*>(find_node(static_cast<const GuiNode&>(n), name));
}
const GuiNode& need(const GuiNode& root, const std::string& name, const char* gui) {
    const GuiNode* n = find_node(root, name);
    if (!n) throw std::runtime_error(std::string("port cheats: ") + gui + " has no widget \"" + name + "\"");
    return *n;
}
// A copied widget keeps the decorator it had in its own file: an empty IE is
// that file's, an empty node its own name.
void pin_decorators(GuiNode& n, const std::string& ie) {
    if (n.decorator) {
        if (n.decorator->ie.empty()) n.decorator->ie = ie;
        if (n.decorator->node.empty()) n.decorator->node = n.name;
    }
    for (auto& c : n.children) pin_decorators(c, ie);
}
// Moves a widget and its children up the screen by dy (rect top and the
// decorator's vertical translation; negative is down).
void shift_y(GuiNode& n, float dy) {
    n.rect[1] += dy;
    if (n.decorator) n.decorator->matrix[11] += dy;
    for (auto& c : n.children) shift_y(c, dy);
}
GuiNode copy_from(const GuiNode& root, const std::string& name, const char* gui, const std::string& ie) {
    GuiNode n = need(root, name, gui);
    pin_decorators(n, ie);
    return n;
}
}  // namespace

bool port_perception_ignores(const SoloActorSimulation& actor) { return ignored_actors().count(&actor) != 0; }

void port_set_perception_ignored(const SoloActorSimulation& actor, bool ignored) {
    if (ignored) ignored_actors().insert(&actor);
    else ignored_actors().erase(&actor);
}

bool port_perception_engaged(const SoloActorSimulation& actor) { return engaged_actors().count(&actor) != 0; }

void port_set_perception_engaged(const SoloActorSimulation& actor, bool engaged) {
    if (engaged) engaged_actors().insert(&actor);
    else engaged_actors().erase(&actor);
}

bool port_perception_observer_blind(const SoloActorSimulation& actor) {
    return port_perception_ignores(actor) && !port_perception_engaged(actor);
}

void port_clear_perception_ignored() {
    ignored_actors().clear();
    engaged_actors().clear();
}

namespace {
std::map<std::uint64_t, PortPartnerShield>& partner_shields() {
    static std::map<std::uint64_t, PortPartnerShield> shields;
    return shields;
}
}  // namespace

void port_set_partner_shield(std::uint64_t actor, PortPartnerShield shield) {
    if (!shield.friendly) throw std::runtime_error("Port partner shield: no friendly-source predicate");
    partner_shields()[actor] = std::move(shield);
}

const PortPartnerShield* port_partner_shield(std::uint64_t actor) {
    const auto it = partner_shields().find(actor);
    return it == partner_shields().end() ? nullptr : &it->second;
}

void port_clear_partner_shields() { partner_shields().clear(); }

bool port_partner_friendly_source(const PortPartnerShield* shield, std::uint64_t actor, std::uint64_t source) {
    return shield && source != 0 && source != actor && shield->friendly(source);
}

bool port_cheat(PortCheat c) { return cheats().on.at(static_cast<std::size_t>(c)); }

void port_set_cheat(PortCheat c, bool on) {
    bool& v = cheats().on.at(static_cast<std::size_t>(c));
    if (v == on) return;
    v = on;
    if (cheats().hook) cheats().hook(c, on);
}

void port_clear_cheats() { cheats().on.fill(false); }

void port_on_cheat_changed(std::function<void(PortCheat, bool)> hook) { cheats().hook = std::move(hook); }

bool port_cheat_menu_enabled() { return cheats().menu; }

void port_set_cheat_menu_enabled(bool enabled) { cheats().menu = enabled; }

std::optional<std::string> port_cheat_string(const std::string& key) {
    if (key == "StartMenu7" || key == "PortCheatsTitle") return std::string("Cheats");
    if (key == "StartMenuOpt7") return std::string("Port-only cheats: toggle each On or Off");
    // Port-only (checkpoint 132win11): the pause menu's Port Settings entry.
    if (key == "StartMenu8") return std::string("Port Settings");
    if (key == "StartMenuOpt8") return std::string("Port-only settings");
    if (key.size() == 10 && key.compare(0, 9, "PortCheat") == 0 && key[9] >= '0' &&
        key[9] < static_cast<char>('0' + port_cheat_count))
        return std::string(port_cheat_names[static_cast<std::size_t>(key[9] - '0')]);
    return std::nullopt;
}

std::size_t port_unlock_all_weapons(std::span<std::uint8_t, agent_record_size> agent, const AwardTables& tables) {
    std::size_t set = 0;
    for (const auto& row : tables.unlocks) {
        if (std::find(tables.unpooled.begin(), tables.unpooled.end(), row.name) != tables.unpooled.end()) continue;
        // 003352c0's bound: the unlock words end at +b4f (the rank pointer +b50).
        const auto u = static_cast<std::uint32_t>(row.bit);
        if (row.bit < 0 || u >= (0xb50 - 0xb08) * 8)
            throw std::runtime_error("port Unlock All: unlock row " + row.name + " has bit " + std::to_string(row.bit) +
                                     " outside +b08..+b4f");
        const std::size_t o = 0xb08 + 4 * (u >> 5);
        const std::uint32_t mask = 1u << (u & 31);
        const std::uint32_t w = rd32(agent, o);
        if (w & mask) continue;
        wr32(agent, o, w | mask);
        ++set;
    }
    return set;
}

PortUnlockResult port_unlock_all(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state,
    const AwardTables& tables, std::span<const PartRow> parts) {
    PortUnlockResult r;
    r.weapon_bits = port_unlock_all_weapons(agent, tables);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto row = static_cast<std::uint32_t>(i);
        if (std::find(state.parts.begin(), state.parts.end(), row) != state.parts.end()) continue;
        // UnlockPart 00332610 -> 00330280 -> 00139200: one appended at the end.
        const std::uint32_t count = rd32(agent, 0xb5c);
        if (count != state.parts.size()) throw std::runtime_error("port Unlock All: +b5c differs from the parts vector");
        const std::uint32_t cap = rd32(agent, 0xb58);
        const std::uint32_t needed = count + 1;
        if (needed > agent_parts_limit) throw std::runtime_error("port Unlock All: the parts vector grows past 0x1800000");
        if (needed > cap) {
            std::uint32_t n = cap ? cap : 1;
            while (n < needed) n = n < 0xc00000 ? n * 2 : agent_parts_limit;
            wr32(agent, 0xb58, n);
        }
        state.parts.push_back(row);
        wr32(agent, 0xb5c, needed);
        ++r.parts;
    }
    return r;
}

PortAmmoGuard::PortAmmoGuard(WeaponInventory& inventory, bool active) : inv_(inventory), active_(active) {
    if (!active_) return;
    slots_ = inv_.slots;
    reserve_ = inv_.infinite_reserve_99;
    inv_.infinite_reserve_99 = true;
}

PortAmmoGuard::~PortAmmoGuard() {
    if (!active_) return;
    for (std::size_t k = 0; k < slots_.size(); ++k) {
        WeaponSlot& s = inv_.slots[k];
        if (s.item < 0 || s.item != slots_[k].item) continue;
        s.clip = std::max(s.clip, slots_[k].clip);
        s.spare = std::max(s.spare, slots_[k].spare);
    }
    inv_.infinite_reserve_99 = reserve_;
}

namespace {
// A rows screen built from GAMEOPT2's widgets (PORTCHEATS / PORTSETTINGS): a title copied from
// label_controlsettings and `count` rows of GAMEOPT2's control group (textbackground8 / label_freelookx /
// value_manualaim; label_manualaim's row at y 66.03, label_freelookx's at 26.45), `step` apart from y `top`.
// Rows are "<prefix>background%d" / "<prefix>label%d" / "<prefix>value%d"; the value buttons come last.
GuiNode options_rows_screen(const GuiNode& gameopt2, const std::string& name, const std::string& prefix,
    const std::string& title_text, const std::string& label_key, std::size_t count, float top, float step) {
    static constexpr const char* gui = "GAMEOPT2.GUI";
    const std::string ie = "GAMEOPT2";
    GuiNode root = gameopt2;
    root.children.clear();
    root.name = name;
    if (!root.decorator) throw std::runtime_error("port cheats: GAMEOPT2.GUI's root has no decorator");
    // The root's own node "<name>_root" is not in GAMEOPT2.IE: nothing is drawn for it (gameopt2_root
    // would draw the options screen's frame).
    if (root.decorator->ie.empty()) root.decorator->ie = ie;
    if (root.decorator->node.empty()) root.decorator->node = name + "_root";

    GuiNode title = copy_from(gameopt2, "label_controlsettings", gui, ie);
    title.name = prefix + "title";
    if (!title.text) throw std::runtime_error("port cheats: label_controlsettings is not a text widget");
    title.text->text = title_text;
    root.children.push_back(std::move(title));

    const GuiNode& background = need(gameopt2, "textbackground8", gui);
    const GuiNode& label = need(gameopt2, "label_freelookx", gui);
    const GuiNode& value = need(gameopt2, "value_manualaim", gui);
    const float background_row = need(gameopt2, "label_manualaim", gui).rect[1];
    if (!label.text || !value.text) throw std::runtime_error("port cheats: label_freelookx / value_manualaim carry no text");
    std::vector<GuiNode> values;
    for (std::size_t i = 0; i < count; ++i) {
        const float ry = top - step * static_cast<float>(i);
        GuiNode b = copy_from(gameopt2, background.name, gui, ie);
        b.name = prefix + "background" + std::to_string(i);
        shift_y(b, ry - background_row);
        root.children.push_back(std::move(b));
        GuiNode l = copy_from(gameopt2, label.name, gui, ie);
        l.name = prefix + "label" + std::to_string(i);
        l.text->text = label_key.empty() ? std::string() : label_key + std::to_string(i);
        shift_y(l, ry - label.rect[1]);
        root.children.push_back(std::move(l));
        GuiNode v = copy_from(gameopt2, value.name, gui, ie);
        v.name = prefix + "value" + std::to_string(i);
        v.text->text.clear();
        shift_y(v, ry - background_row);
        values.push_back(std::move(v));
    }
    for (auto& v : values) root.children.push_back(std::move(v));
    return root;
}

// Appends a copy of ingame_button_grp's last button one row below it, named `name` with text `text`; its
// textbackground<N> becomes textbackground<N+1>.
void append_ingame_button(GuiNode& group, const std::string& name, const std::string& text) {
    if (group.children.size() < 2) throw std::runtime_error("port cheats: INGAMENU.GUI's ingame_button_grp is too short");
    const GuiNode& last = group.children.back();
    const GuiNode& before = group.children[group.children.size() - 2];
    const float step = last.rect[1] - before.rect[1];  // 06 -> 07: -36.85
    GuiNode b = last;
    pin_decorators(b, std::string{});  // the copy draws the last button's nodes ("bracketbutton" / "textbackground")
    b.name = name;
    if (!b.text) throw std::runtime_error("port cheats: INGAMENU.GUI's " + last.name + " is not a button with text");
    b.text->text = text;
    for (auto& c : b.children) {
        if (c.name.rfind("textbackground", 0) != 0) continue;
        const std::string digits = c.name.substr(14);
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
        c.name = "textbackground" + std::to_string(std::stoi(digits) + 1);
    }
    shift_y(b, step);
    group.children.push_back(std::move(b));
    group.rect[3] -= step;
}
}  // namespace

GuiNode port_cheats_screen(const GuiNode& gameopt2) {
    return options_rows_screen(gameopt2, "portcheats", "pc_", "$PortCheatsTitle", "$PortCheat", port_cheat_count, 85.0F, 35.0F);
}

GuiNode port_settings_screen(const GuiNode& gameopt2, std::size_t rows) {
    // Nine rows 30 apart from y 90 end where the seven cheats 35 apart from y 85 do (-125) plus one row;
    // the title, labels and values are filled by PortGuiSettings at run time.
    return options_rows_screen(gameopt2, "portsettings", "ps_", std::string(), std::string(), rows, 90.0F, 30.0F);
}
void port_cheats_patch_ingamenu(GuiNode& ingamenu) {
    GuiNode* group = find_node(ingamenu, "ingame_button_grp");
    if (!group) throw std::runtime_error("port cheats: INGAMENU.GUI has no ingame_button_grp");
    if (group->children.size() != 7 || group->children.back().name != "07")
        throw std::runtime_error("port cheats: INGAMENU.GUI's ingame_button_grp is not the seven buttons 01..07");
    GuiNode* paused = find_node(ingamenu, "label_paused");
    if (!paused) throw std::runtime_error("port cheats: INGAMENU.GUI has no label_paused");
    append_ingame_button(*group, "08", "$StartMenu7");
    shift_y(*paused, -30.0F);
}

void port_settings_patch_ingamenu(GuiNode& ingamenu) {
    GuiNode* group = find_node(ingamenu, "ingame_button_grp");
    if (!group) throw std::runtime_error("port settings: INGAMENU.GUI has no ingame_button_grp");
    if (group->children.empty() || group->children.back().name == "09")
        throw std::runtime_error("port settings: INGAMENU.GUI's ingame_button_grp already has the Port Settings button");
    GuiNode* paused = find_node(ingamenu, "label_paused");
    if (!paused) throw std::runtime_error("port settings: INGAMENU.GUI has no label_paused");
    const std::size_t n = group->children.size();
    const float step = group->children[n - 1].rect[1] - group->children[n - 2].rect[1];
    const float height = group->rect[3];
    append_ingame_button(*group, "09", "$StartMenu8");
    if (n + 1 <= 8) {
        shift_y(*paused, -30.0F);  // no Cheats entry: the 132f5 layout (eight buttons, PAUSED one row lower)
        return;
    }
    // Nine buttons: spread over the eight-button span (7 steps of 36.85 -> 8 of 32.24) so PAUSED stays above the
    // objectives panel's bottom row.
    const float spread = step * 7.0F / 8.0F;
    for (std::size_t k = 1; k < group->children.size(); ++k)
        shift_y(group->children[k], static_cast<float>(k) * (spread - step));
    group->rect[3] = height;
}
}  // namespace omega
