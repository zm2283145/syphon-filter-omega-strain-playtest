#include "coop_command_menu.hpp"

#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {
// The port's menu table, in the shape of 0048bd10: a submenu flag and four entries (Up, Down, Left, Right).
// A submenu entry is the next menu; an order entry is 1 + the CoopOrder (1 + the CoopRole on a roles page); 0 is empty.
struct MenuRow {
    bool submenu;
    bool roles;   // 132pw4: the Tasks page
    std::array<int, 4> ids;
};
constexpr std::array<MenuRow, 5> menus{{
    {true, false, {1, 1, 1, 1}},   // who: Partner 1, 2, 3, All (the target is the entry)
    {true, false, {2, 3, 4, 0}},   // kind: Command, Fire, Tasks
    {false, false, {1 + int(CoopOrder::follow), 1 + int(CoopOrder::hold), 1 + int(CoopOrder::go_there), 1 + int(CoopOrder::regroup)}},
    {false, false, {1 + int(CoopOrder::weapons_free), 1 + int(CoopOrder::hold_fire), 1 + int(CoopOrder::attack_target), 0}},
    {false, true, {1 + int(CoopRole::tasks), 1 + int(CoopRole::hold), 1 + int(CoopRole::cover), 1 + int(CoopRole::follow)}},
}};
}

const char* coop_role_name(CoopRole role) {
    switch (role) {
    case CoopRole::tasks: return "tasks";
    case CoopRole::cover: return "cover";
    case CoopRole::hold: return "hold";
    case CoopRole::follow: return "follow";
    }
    throw std::runtime_error("Coop command menu: unknown role");
}

std::optional<CoopRole> parse_coop_role(const std::string& name) {
    for (const auto r : {CoopRole::tasks, CoopRole::cover, CoopRole::hold, CoopRole::follow})
        if (name == coop_role_name(r)) return r;
    return std::nullopt;
}

CoopMenuLabel coop_role_label(CoopRole role) {
    switch (role) {
    case CoopRole::tasks: return {nullptr, "Level tasks"};
    case CoopRole::cover: return {nullptr, "Cover me"};
    case CoopRole::hold: return {nullptr, "Hold here"};
    case CoopRole::follow: return {nullptr, "Follow me"};
    }
    throw std::runtime_error("Coop command menu: unknown role");
}

std::array<CoopRole, 3> coop_level_roles(const std::string& mission, std::size_t partners) {
    if (partners < 1 || partners > 3) throw std::runtime_error("Coop roles: 1 to 3 partners");
    // TORONTO1: partner 1 turns the water main (the level's command), partner 2 covers the player at the laptop,
    // partner 3 follows. Elsewhere partner 1 takes the level's boxes and partners 2 and 3 follow.
    if (mission == "TORONTO1") return {CoopRole::tasks, CoopRole::cover, CoopRole::follow};
    // BELARUS2 (132ms5): partners 1 and 2 each carry a C-4 charge to the bridge (coop_command_rules' slots 1 and 2).
    if (mission == "BELARUS2") return {CoopRole::tasks, CoopRole::tasks, CoopRole::follow};
    // UKRAINE (132uk6c): the launch override's buddy climbs and switches (coop_command_rules' slots 1 to 3): partner 1
    // climbs at B and presses switch B, partner 2 boosts at B and then at A, partner 3 climbs at A and presses switch A.
    if (mission == "UKRAINE") return {CoopRole::tasks, CoopRole::tasks, CoopRole::tasks};
    return {CoopRole::tasks, CoopRole::follow, CoopRole::follow};
}

std::string coop_level_roles_note(const std::string& mission) {
    if (mission == "TORONTO1") return "TORONTO1: partner 1 the water main (level tasks), partner 2 covers the player at the laptop, partner 3 follows";
    if (mission == "BELARUS2") return "BELARUS2: partners 1 and 2 take C-4 at the cache and plant the bridge charges (level tasks), partner 3 follows";
    if (mission == "UKRAINE") return "UKRAINE: partner 1 climbs at B and presses switch B, partner 2 boosts at B and at A, partner 3 climbs at A and presses switch A (level tasks)";
    return mission + ": partner 1 the level's co-op boxes and commands (level tasks), the others follow";
}

const char* coop_order_name(CoopOrder order) {
    switch (order) {
    case CoopOrder::follow: return "follow";
    case CoopOrder::hold: return "hold";
    case CoopOrder::go_there: return "go there";
    case CoopOrder::regroup: return "regroup";
    case CoopOrder::weapons_free: return "weapons free";
    case CoopOrder::hold_fire: return "hold fire";
    case CoopOrder::attack_target: return "attack my target";
    }
    throw std::runtime_error("Coop command menu: unknown order");
}

CoopMenuLabel coop_order_label(CoopOrder order) {
    switch (order) {
    case CoopOrder::follow: return {"QC_ITEM_17", "Follow"};
    case CoopOrder::hold: return {"QC_ITEM_18", "Stay"};
    case CoopOrder::go_there: return {nullptr, "Go there"};
    case CoopOrder::regroup: return {nullptr, "Regroup"};
    case CoopOrder::weapons_free: return {nullptr, "Weapons free"};
    case CoopOrder::hold_fire: return {nullptr, "Hold fire"};
    case CoopOrder::attack_target: return {nullptr, "Attack my target"};
    }
    throw std::runtime_error("Coop command menu: unknown order");
}

CoopMenuLabel coop_partner_label(std::size_t k) {
    switch (k) {
    case 0: return {"PlayerName1", "Partner 1"};
    case 1: return {"PlayerName2", "Partner 2"};
    case 2: return {"PlayerName3", "Partner 3"};
    }
    throw std::runtime_error("Coop command menu: no partner " + std::to_string(k + 1));
}

std::string coop_order_target_name(int target) {
    if (target == coop_order_all) return "all partners";
    if (target < 0 || target > 2) throw std::runtime_error("Coop command menu: target out of range");
    return "partner " + std::to_string(target + 1);
}

void coop_menu_open(CoopCommandMenu& menu, std::size_t partners, std::vector<CoopTaskEntry> tasks) {
    if (partners < 1 || partners > 3) throw std::runtime_error("Coop command menu: 1 to 3 partners");
    menu.open = true;
    menu.partners = partners;
    menu.menu = partners > 1 ? 0 : 1;
    menu.target = partners > 1 ? coop_order_all : 0;
    menu.tasks = std::move(tasks);
    menu.page = 0;
    menu.chosen = 0;
}

std::size_t coop_menu_task_pages(const CoopCommandMenu& menu) {
    if (menu.tasks.size() <= 4) return 1;
    return (menu.tasks.size() + 2) / 3;
}

std::optional<std::size_t> coop_menu_task_at(const CoopCommandMenu& menu, int direction) {
    if (direction < 0 || direction > 3) throw std::runtime_error("Coop command menu: D-pad direction out of range");
    std::size_t index = 0;
    if (menu.tasks.size() <= 4) {
        index = static_cast<std::size_t>(direction);
    } else {
        // Up, Left, Right hold the page's three tasks; Down turns the page.
        static constexpr std::array<int, 4> slot{0, -1, 1, 2};
        if (slot[static_cast<std::size_t>(direction)] < 0) return std::nullopt;
        index = menu.page * 3 + static_cast<std::size_t>(slot[static_cast<std::size_t>(direction)]);
    }
    if (index >= menu.tasks.size()) return std::nullopt;
    return index;
}

CoopMenuPress coop_menu_cancel(CoopCommandMenu& menu) {
    CoopMenuPress r;
    if (!menu.open) return r;
    menu.open = false;
    r.consumed = r.closed = true;
    return r;
}

CoopMenuPress coop_menu_press(CoopCommandMenu& menu, int direction) {
    CoopMenuPress r;
    if (!menu.open) return r;
    if (direction < 0 || direction > 3) throw std::runtime_error("Coop command menu: D-pad direction out of range");
    r.consumed = true;
    // 132pt1 (port-only): Level tasks opens the list of the level's open tasks; a task chosen there goes to the partner
    // chosen next (with one partner, to partner 1 at once).
    const auto give = [&](int partner) {
        menu.open = false;
        r.closed = true;
        r.task = menu.tasks.at(menu.chosen).objective;
        r.target = partner;
        return r;
    };
    if (menu.menu == 4 && direction == 0) {
        if (menu.tasks.empty()) {
            menu.open = false;
            r.closed = true;
            r.no_tasks = true;
            r.target = menu.target;
            return r;
        }
        menu.menu = coop_menu_task_list;
        menu.page = 0;
        return r;
    }
    if (menu.menu == coop_menu_task_list) {
        if (menu.tasks.size() > 4 && direction == 1) {
            menu.page = (menu.page + 1) % coop_menu_task_pages(menu);
            return r;
        }
        const auto task = coop_menu_task_at(menu, direction);
        if (!task) return r;
        menu.chosen = *task;
        if (menu.partners == 1) return give(0);
        menu.menu = coop_menu_task_partner;
        return r;
    }
    if (menu.menu == coop_menu_task_partner) {
        if (direction == 3 || static_cast<std::size_t>(direction) >= menu.partners) return r;
        return give(direction);
    }
    if (menu.menu < 0 || static_cast<std::size_t>(menu.menu) >= menus.size()) throw std::runtime_error("Coop command menu: bad menu");
    const auto& row = menus[static_cast<std::size_t>(menu.menu)];
    const int id = row.ids[static_cast<std::size_t>(direction)];
    if (id == 0) return r;
    if (row.submenu) {
        if (menu.menu == 0) {
            if (direction != coop_order_all && static_cast<std::size_t>(direction) >= menu.partners) return r;
            menu.target = direction;
        }
        menu.menu = id;
        return r;
    }
    menu.open = false;
    r.closed = true;
    if (row.roles) r.role = static_cast<CoopRole>(id - 1);
    else r.order = static_cast<CoopOrder>(id - 1);
    r.target = menu.target;
    return r;
}

std::array<CoopMenuLabel, 4> coop_menu_labels(const CoopCommandMenu& menu) {
    std::array<CoopMenuLabel, 4> out{};
    switch (menu.menu) {
    case 0:
        // 132pt1: the partners by their codenames (the agent slot's name, GetString("PlayerName<slot>"), as the co-op HUD's
        // callouts show them); the port's text only if the string is missing.
        for (std::size_t k = 0; k < menu.partners; ++k) out[k] = coop_partner_label(k);
        out[3] = {nullptr, "All"};
        return out;
    case coop_menu_task_list: {
        if (menu.tasks.size() > 4) out[1] = {nullptr, "Next page"};
        for (int d = 0; d < 4; ++d)
            if (const auto t = coop_menu_task_at(menu, d)) out[static_cast<std::size_t>(d)] = {nullptr, menu.tasks[*t].text.c_str()};
        return out;
    }
    case coop_menu_task_partner:
        for (std::size_t k = 0; k < menu.partners; ++k) out[k] = coop_partner_label(k);
        return out;
    case 1:
        out[0] = {"QC_MENU_4", "Command"};
        out[1] = {nullptr, "Fire"};
        out[2] = {nullptr, "Tasks"};
        return out;
    case 2:
    case 3:
        for (std::size_t k = 0; k < 4; ++k)
            if (const int id = menus[static_cast<std::size_t>(menu.menu)].ids[k]) out[k] = coop_order_label(static_cast<CoopOrder>(id - 1));
        return out;
    case 4:
        for (std::size_t k = 0; k < 4; ++k) out[k] = coop_role_label(static_cast<CoopRole>(menus[4].ids[k] - 1));
        return out;
    }
    throw std::runtime_error("Coop command menu: bad menu");
}

std::vector<std::size_t> coop_order_partners(int target, std::size_t partners) {
    std::vector<std::size_t> out;
    if (target == coop_order_all) {
        for (std::size_t k = 0; k < partners; ++k) out.push_back(k);
    } else if (target >= 0 && static_cast<std::size_t>(target) < partners) {
        out.push_back(static_cast<std::size_t>(target));
    } else {
        throw std::runtime_error("Coop command menu: no partner " + std::to_string(target + 1));
    }
    return out;
}

bool coop_order_apply(CoopOrderState& state, CoopOrder order, std::uint64_t attack, std::int32_t go_node) {
    if (order == CoopOrder::go_there && go_node < 0) return false;
    if (order == CoopOrder::follow || order == CoopOrder::regroup || order == CoopOrder::hold || order == CoopOrder::go_there)
        state.guard = false;   // 132pw5: a move order ends a team task post
    switch (order) {
    case CoopOrder::follow:
    case CoopOrder::regroup:
        state.move = CoopMoveOrder::follow;
        state.attack = 0;
        state.go_node = -1;
        state.go_arrived = false;
        state.go = {};
        state.regrouping = order == CoopOrder::regroup;
        state.walk = {};
        break;
    case CoopOrder::hold:
        state.walk = {};
        state.move = CoopMoveOrder::hold;
        state.go_node = -1;
        state.go_arrived = false;
        state.go = {};
        state.regrouping = false;
        break;
    case CoopOrder::go_there:
        if (go_node < 0) return false;
        state.walk = {};
        state.move = CoopMoveOrder::go_there;
        state.go_node = go_node;
        state.go_arrived = false;
        state.go = {};
        state.regrouping = false;
        break;
    case CoopOrder::weapons_free:
        state.fire = CoopFireOrder::weapons_free;
        break;
    case CoopOrder::hold_fire:
        state.fire = CoopFireOrder::hold_fire;
        state.attack = 0;
        break;
    case CoopOrder::attack_target:
        if (attack == 0) return false;
        state.attack = attack;
        state.regrouping = false;
        state.walk = {};
        if (state.fire == CoopFireOrder::hold_fire) state.fire = CoopFireOrder::automatic;
        break;
    }
    ++state.orders;
    return true;
}

bool coop_order_may_fire(const CoopOrderState& state, bool automatic) {
    if (state.attack != 0) return true;
    switch (state.fire) {
    case CoopFireOrder::weapons_free: return true;
    case CoopFireOrder::hold_fire: return false;
    case CoopFireOrder::automatic: return automatic;
    }
    return automatic;
}

bool coop_order_follows(const CoopOrderState& state) { return state.move == CoopMoveOrder::follow; }

bool coop_order_may_engage(const CoopOrderState& state) {
    return state.attack != 0 || (state.move == CoopMoveOrder::follow && !state.regrouping)
        || (state.guard && state.move != CoopMoveOrder::follow);
}

CoopOrder coop_role_assign(CoopOrderState& state, CoopRole role) {
    state.role = role;
    if (role == CoopRole::cover) state.fire = CoopFireOrder::weapons_free;
    else if (state.fire == CoopFireOrder::weapons_free) state.fire = CoopFireOrder::automatic;
    return role == CoopRole::hold ? CoopOrder::hold : CoopOrder::follow;
}

std::optional<std::size_t> coop_tasks_partner(const std::vector<CoopRole>& roles) {
    for (std::size_t k = 0; k < roles.size(); ++k)
        if (roles[k] == CoopRole::tasks) return k;
    return std::nullopt;
}

bool coop_role_picks_up(CoopRole role) { return role != CoopRole::cover; }

void coop_regroup_walk_start(CoopOrderState& state, float apart, std::uint32_t frame) {
    state.walk = {};
    state.walk.active = true;
    state.walk.started = state.walk.best_at = frame;
    state.walk.from = state.walk.best = apart;
}

CoopRegroupVerdict coop_regroup_walk_step(CoopOrderState& state, const CoopRegroupConfig& config, float apart, std::uint32_t frame,
                                          bool no_route, bool route_found) {
    auto& w = state.walk;
    if (!w.active) return CoopRegroupVerdict::idle;
    if (!state.regrouping || state.move != CoopMoveOrder::follow) {
        w.active = false;
        return CoopRegroupVerdict::idle;
    }
    if (no_route) ++w.no_routes;
    else if (route_found) w.no_routes = 0;
    if (apart <= w.best - config.progress) {
        w.best = apart;
        w.best_at = frame;
    }
    if (w.no_routes >= config.max_no_routes || frame - w.best_at >= config.patience) {
        w.active = false;
        return CoopRegroupVerdict::fallback;
    }
    return CoopRegroupVerdict::walking;
}

void coop_go_start(CoopOrderState& state, const std::array<float, 4>& point, bool at_node, float distance, std::uint32_t frame) {
    state.go = {};
    state.go.point = point;
    coop_go_leg(state, at_node ? CoopOrderState::Go::Leg::point : CoopOrderState::Go::Leg::node, distance, frame);
}

void coop_go_leg(CoopOrderState& state, CoopOrderState::Go::Leg leg, float distance, std::uint32_t frame) {
    state.go.leg = leg;
    state.go.leg_at = state.go.best_at = frame;
    state.go.best = distance;
}

bool coop_go_stalled(CoopOrderState& state, const CoopGoConfig& config, float distance, std::uint32_t frame) {
    auto& g = state.go;
    if (distance <= g.best - config.progress) {
        g.best = distance;
        g.best_at = frame;
    }
    return frame - g.best_at >= config.patience;
}

// 132pw5: the level sections. TORONTO2: partner 2 covers the player against the death squad (obj3_add), and partners 2
// and 3 cover the player on the sniper section with Imani (obj7_add, "Provide Imani covering fire"). TORONTO3: partner
// 2 covers the player at the mall entrances (obj2_add, the SWAT fight), and partners 2 and 3 while the player disarms
// the viral explosives (obj3_add).
std::optional<CoopRoleSection> coop_level_section(const std::string& mission, std::size_t partners, const std::string& label) {
    if (partners < 2 || partners > 3) return std::nullopt;
    CoopRoleSection s;
    s.label = label;
    if (mission == "TORONTO2" && label == "obj3_add") {
        s.roles[1] = CoopRole::cover;
        s.note = "TORONTO2's death squad: partner 2 covers the player";
    } else if (mission == "TORONTO2" && label == "obj7_add") {
        s.roles[1] = s.roles[2] = CoopRole::cover;
        s.note = "TORONTO2's covering fire for Imani (the sniper section): partners 2 and 3 cover the player";
    } else if (mission == "TORONTO3" && label == "obj2_add") {
        s.roles[1] = CoopRole::cover;
        s.note = "TORONTO3's mall entrances (the SWAT fight): partner 2 covers the player";
    } else if (mission == "TORONTO3" && label == "obj3_add") {
        s.roles[1] = s.roles[2] = CoopRole::cover;
        s.note = "TORONTO3's viral explosives: partners 2 and 3 cover the player while the player disarms them";
    } else {
        return std::nullopt;
    }
    for (std::size_t k = partners; k < s.roles.size(); ++k) s.roles[k].reset();
    return s;
}

// 132pw5: the team tasks. TORONTO1's scan defence: from "ProtectPower" ("Tissue Scanner Operating") saboteurs from the
// generators 142518 / 142519 come down to shoot the power supply 140518 while the scanner uploads (objective 5,
// "Samples scanned and data uploaded"). The player defends it from beside it (about (-617.4, -5.5)); the partners hold
// the ground floor's east and west sides (Nav nodes 136726, 136733, 136727), out of the player's lines of fire to the
// supply's south-east and the north spawns, until objective 5 is decided (150 s at most).
std::optional<CoopTeamTask> coop_team_task(const std::string& mission, std::size_t partners, const std::string& label) {
    if (partners < 2 || partners > 3) return std::nullopt;
    if (mission == "TORONTO1" && label == "ProtectPower") {
        CoopTeamTask t;
        t.name = "defend the power supply";
        t.start_label = label;
        t.end_objective = 5;
        t.patience = 450;
        t.time_limit = 4500;
        t.posts = {{136726, -603.75F, -5.07F, CoopRole::hold, "east of the power supply"},
                   {136733, -627.14F, -5.06F, CoopRole::cover, "west of the power supply"},
                   {136727, -603.79F, -13.15F, CoopRole::follow, "south-east, under the saboteurs' walkway"}};
        t.note = "TORONTO1's scan defence: the partners hold the power supply 140518's east and west sides while objective 5 uploads";
        return t;
    }
    return std::nullopt;
}

std::array<std::optional<std::size_t>, 3> coop_team_assign(const CoopTeamTask& task, const std::vector<CoopRole>& roles) {
    std::array<std::optional<std::size_t>, 3> post_of{};
    std::vector<bool> filled(task.posts.size());
    const auto n = std::min<std::size_t>(roles.size(), post_of.size());
    for (std::size_t p = 0; p < task.posts.size(); ++p)
        for (std::size_t k = 0; k < n; ++k)
            if (!post_of[k] && roles[k] == task.posts[p].prefer) {
                post_of[k] = p;
                filled[p] = true;
                break;
            }
    for (std::size_t p = 0; p < task.posts.size(); ++p) {
        if (filled[p]) continue;
        for (std::size_t k = 0; k < n; ++k)
            if (!post_of[k]) {
                post_of[k] = p;
                filled[p] = true;
                break;
            }
    }
    return post_of;
}

void coop_team_start(CoopTeamState& state, std::uint32_t frame) {
    state = {};
    state.phase = CoopTeamPhase::moving;
    state.started = frame;
}

CoopTeamEvent coop_team_step(CoopTeamState& state, const CoopTeamTask& task, std::uint32_t frame, bool in_position, bool objective_resolved) {
    if (state.phase == CoopTeamPhase::idle || state.phase == CoopTeamPhase::done) return CoopTeamEvent::none;
    const auto end = [&](CoopTeamEvent e) {
        state.phase = CoopTeamPhase::done;
        state.ended_at = frame;
        state.ended_by = e;
        return e;
    };
    if (objective_resolved) return end(CoopTeamEvent::objective_done);
    if (state.phase == CoopTeamPhase::moving) {
        if (in_position || frame - state.started >= task.patience) {
            state.phase = CoopTeamPhase::running;
            state.running_at = frame;
            return in_position ? CoopTeamEvent::in_position : CoopTeamEvent::started_late;
        }
        return CoopTeamEvent::none;
    }
    if (task.time_limit && frame - state.running_at >= task.time_limit) return end(CoopTeamEvent::time_up);
    return CoopTeamEvent::none;
}

const char* coop_team_event_name(CoopTeamEvent event) {
    switch (event) {
    case CoopTeamEvent::none: return "none";
    case CoopTeamEvent::in_position: return "in position";
    case CoopTeamEvent::started_late: return "started without everyone in position";
    case CoopTeamEvent::objective_done: return "its objective decided";
    case CoopTeamEvent::time_up: return "time up";
    }
    throw std::runtime_error("Coop team task: unknown event");
}

}
