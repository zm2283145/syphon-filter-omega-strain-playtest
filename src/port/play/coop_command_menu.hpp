#pragma once
// Port-only (not in the original game; checkpoint 132pw3, research/COOP.md "Partner command menu"): the CPU
// partners' command menu, offline only, on the game's quick-chat menu.
//
// The original's quick chat (GuiQuickChat, vtable 004dde60; GAMEGUI "quickchat" with the four GuiTextWidget
// children dpad0..dpad3, init 002b1b30) is multiplayer-only: 00242740 shows it (0045dd90(screen, 1)) from
// 00464f64 only in a network game (byte 005721c8). Its input 002b1350 maps the D-pad 0x1000 / 0x4000 / 0x8000 /
// 0x2000 to entries 0..3 (Up, Down, Left, Right) of the current menu +48 in the table 0048bd10 (20 bytes per
// menu: a submenu byte, then four ids). A submenu entry makes the id the current menu and relabels the four
// children from "QC_MENU_%d" (004abee8) or "QC_ITEM_%d" (004abef8) through GetString 003fdb50 and 0041beb0; an
// item posts its text to the HUD ticker (003fa3f0(hud + 100, text, 00587c90, 1, 0)) and, in a network game,
// sends it. The original table (menu 0: Need / Respond / Info / Command, 4 = Command: Fall back, Flank, Follow,
// Stay) has no partner orders, so this table is the port's own: the same four-way D-pad pages, with the
// original's strings where they say the same thing (QC_MENU_4 "Command", QC_ITEM_17 "Follow",
// QC_ITEM_18 "Stay") and the port's English text for the rest.
//
//   menu 0 (who):       Up Partner 1, Down Partner 2, Left Partner 3, Right All   (skipped with one partner)
//   menu 1 (kind):      Up Command (QC_MENU_4), Down Fire, Left Tasks (132pw4)
//   menu 2 (movement):  Up Follow (QC_ITEM_17), Down Stay (QC_ITEM_18: hold), Left Go there, Right Regroup
//   menu 3 (fire):      Up Weapons free, Down Hold fire, Left Attack my target
//   menu 4 (tasks):     Up Level tasks, Down Hold here, Left Cover me, Right Follow me   (roles, 132pw4)
//
// The menu is a pure state machine; the play host opens it, feeds it D-pad presses and carries out the order.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace omega {

enum class CoopOrder : std::uint8_t { follow, hold, go_there, regroup, weapons_free, hold_fire, attack_target };
const char* coop_order_name(CoopOrder order);

// Checkpoint 132pw4 (port-only, research/COOP.md "Partner roles and level tasks"): a partner's standing role, set by
// the level's role table at the level start and by the menu's Tasks page.
//   tasks:  the level's tasks: the lowest-numbered partner with this role takes the level's partner commands
//           (CoopCommandRule, 132pc1) and the co-op boxes (the second agent's type-12 boxes and boosts); it follows
//           and makes pickup trips otherwise.
//   cover:  follows the player with weapons free (whatever the alert rule) and makes no pickup trips.
//   hold:   holds where it is when the role is given (Stay), and again after a respawn.
//   follow: follows (fire by the alert rule, pickup trips) and takes no level tasks.
enum class CoopRole : std::uint8_t { tasks, cover, hold, follow };
const char* coop_role_name(CoopRole role);
std::optional<CoopRole> parse_coop_role(const std::string& name);
// The level's role table: partner k's role at the level start (partners 1..3). With one partner: tasks (as before
// 132pw4: partner 1 takes the commands and the boxes).
std::array<CoopRole, 3> coop_level_roles(const std::string& mission, std::size_t partners);
// What the level's table gives the partners their roles for (a log line), e.g. TORONTO1's water main.
std::string coop_level_roles_note(const std::string& mission);

// The partner order a menu target names: 0..2 one partner, 3 all of them.
inline constexpr int coop_order_all = 3;

// A label: a STRINGS.DAT key the host resolves (GetString 003fdb50), else the port's text.
struct CoopMenuLabel {
    const char* key{};    // "QC_ITEM_17", or nullptr
    const char* text{};   // the port's English text (used without a key, or when the key is missing)
    bool operator==(const CoopMenuLabel&) const = default;
};

struct CoopCommandMenu {
    bool open{};
    int menu{};                 // the current menu (+48)
    int target{coop_order_all}; // the partner chosen on menu 0
    std::size_t partners{1};    // CPU partners present (1..3)
};

struct CoopMenuPress {
    bool consumed{};   // the press was the menu's (masked from the game's input)
    bool closed{};     // the menu closed with this press
    std::optional<CoopOrder> order;
    std::optional<CoopRole> role;   // a Tasks page entry (132pw4): the role, carried out with coop_role_assign's order
    int target{-1};    // 0..2, or coop_order_all
};

// Opens the menu: menu 0 with two or three partners, else menu 1 for partner 1. Throws for 0 or more than 3.
void coop_menu_open(CoopCommandMenu& menu, std::size_t partners);
// A D-pad press (0 Up, 1 Down, 2 Left, 3 Right) while open: a submenu entry moves on, an order closes the menu,
// an empty entry or a missing partner is ignored (consumed). Closed: not consumed.
CoopMenuPress coop_menu_press(CoopCommandMenu& menu, int direction);
// Triangle (or the opening button again) while open closes it without an order.
CoopMenuPress coop_menu_cancel(CoopCommandMenu& menu);
// The four children's labels (dpad0..dpad3: Up, Down, Left, Right) of the current menu; an empty entry has
// neither key nor text.
std::array<CoopMenuLabel, 4> coop_menu_labels(const CoopCommandMenu& menu);
CoopMenuLabel coop_order_label(CoopOrder order);
CoopMenuLabel coop_role_label(CoopRole role);
// The partners (0-based) an order's target names.
std::vector<std::size_t> coop_order_partners(int target, std::size_t partners);
std::string coop_order_target_name(int target);

// Port-only per-partner order state the play host keeps (swapped in with the partner's other state).
enum class CoopMoveOrder : std::uint8_t { follow, hold, go_there };
enum class CoopFireOrder : std::uint8_t { automatic, weapons_free, hold_fire };
struct CoopOrderState {
    CoopMoveOrder move{CoopMoveOrder::follow};
    CoopFireOrder fire{CoopFireOrder::automatic};
    std::uint64_t attack{};   // the player's target handle (attack my target), 0 none
    std::int32_t go_node{-1}; // go there: the Nav node sent to
    bool go_arrived{};
    // 132pw5fix2 (port-only): go there walks on from its node to the aimed place (coop_go_*): the place, the leg under way
    // and the progress the stall timeout reads.
    struct Go {
        std::optional<std::array<float, 4>> point;
        enum class Leg : std::uint8_t { node, settle, point, done } leg{Leg::node};
        std::uint32_t leg_at{}, best_at{};
        float best{};
        bool bridged{};   // the point route found no way: a port-only node at the place (install_coop_walk_bridge) is the goal
    } go;
    bool regrouping{};        // regroup: out of the fight until the partner is back beside the player (the host clears it)
    std::uint32_t orders{};   // orders taken (diagnostic)
    CoopRole role{CoopRole::tasks};   // 132pw4: the standing role
    // 132pw5fix2: the tasks role was given from the Tasks page: the level tasks partner seeks the level's partner tasks
    // (coop_command_rules) itself; `idle_reported` once "nothing to do here" is on the ticker. A move order ends it.
    bool seek_tasks{};
    bool idle_reported{};
    // 132pw5: a team task's post (CoopTeamTask): with a hold or go-there order the partner still joins the player's
    // fight (coop_order_may_engage) and goes back to its post after it; the host keeps a go-there guard engaged for the
    // whole task, so it also fights enemies that ignore the player (TORONTO1's saboteurs). Every move order clears it.
    bool guard{};
    // 132pw4: a regroup ordered from beyond the rejoin distance (30 m): the partner walks back by the follow driver's
    // MoveToNode routes (coop_regroup_walk_step); the out-of-view rejoin placement is the fallback.
    struct Walk {
        bool active{};
        std::uint32_t started{}, best_at{};
        float from{}, best{};
        std::uint32_t no_routes{};
    } walk;
};
// Applies an order's lasting part to the state (the host does the native calls). attack is the player's pick
// (0: none; attack my target is then refused, false).
bool coop_order_apply(CoopOrderState& state, CoopOrder order, std::uint64_t attack = 0, std::int32_t go_node = -1);
// The fire rule: weapons free fires, hold fire does not, otherwise the default rule (coop_partner_may_fire);
// an attack order fires.
bool coop_order_may_fire(const CoopOrderState& state, bool automatic);
// The follow driver and catch-up run only while following.
bool coop_order_follows(const CoopOrderState& state);
// Whether the partner may join a fight (the 132ob7 rule decides when): with an attack order, or following and not
// regrouping, or guarding a team task post (132pw5). A hold or go-there order keeps it out of fights it is not
// ordered into.
bool coop_order_may_engage(const CoopOrderState& state);

// 132pw4: gives the partner a role: sets the role and its fire order (cover: weapons free; the others: automatic,
// unless a hold fire order stands) and returns the move order the host carries out (hold: hold, else follow).
CoopOrder coop_role_assign(CoopOrderState& state, CoopRole role);
// The first partner (0-based) of `partners` whose role is tasks, if any: it takes the level's commands and boxes.
std::optional<std::size_t> coop_tasks_partner(const std::vector<CoopRole>& roles);
// Whether the role makes pickup trips (not cover).
bool coop_role_picks_up(CoopRole role);

// 132pw4 (port-only): the regroup walk's rule. The walk starts at a regroup order from beyond the rejoin
// distance; it ends when the partner is back (the host's "regrouped", regrouping cleared). It falls back to the
// rejoin placement when the follow routes answer no route (path event 3) max_no_routes times in a row, or the
// partner gets no closer by `progress` within `patience` frames.
struct CoopRegroupConfig {
    float progress{1.0F};
    std::uint32_t patience{300};       // 10 s at 30 fps
    std::uint32_t max_no_routes{2};
};
enum class CoopRegroupVerdict : std::uint8_t { idle, walking, fallback };
void coop_regroup_walk_start(CoopOrderState& state, float apart, std::uint32_t frame);
// One frame of the walk: apart is the partner's horizontal distance from the player; no_route a path event 3 on
// the partner's follow route this frame, route_found a path event 2.
CoopRegroupVerdict coop_regroup_walk_step(CoopOrderState& state, const CoopRegroupConfig& config, float apart, std::uint32_t frame,
                                          bool no_route, bool route_found);

// Checkpoint 132pw5fix2 (port-only, play-test 2026-10-21 "if i aim somewhere and tell it move there it doesnt go"): the
// go-there walk. The host sends the partner to the reachable Nav node nearest the aimed place (MoveToNode), then, once
// there (arrive, or the MoveToNode's destination event 6 / `settle` frames later, as the errand's point route waits),
// routes it to the place itself (port_route_to_point) when that lies over point_arrive from the node. The walk is over
// at the place; it stalls (the host holds the partner where it is and says so) when the distance to the current leg's
// goal has not dropped by `progress` in `patience` frames.
struct CoopGoConfig {
    float arrive{1.5F};
    float point_arrive{1.0F};
    float progress{0.5F};
    std::uint32_t patience{600};   // 20 s at 30 fps
    std::uint32_t settle{60};
};
void coop_go_start(CoopOrderState& state, const std::array<float, 4>& point, bool at_node, float distance, std::uint32_t frame);
// A new leg (its goal `distance` away) starts the stall clock again.
void coop_go_leg(CoopOrderState& state, CoopOrderState::Go::Leg leg, float distance, std::uint32_t frame);
// Whether the walk has stalled at this frame (distance: to the current leg's goal); a drop of `progress` restarts the clock.
bool coop_go_stalled(CoopOrderState& state, const CoopGoConfig& config, float distance, std::uint32_t frame);
// Checkpoint 132pw5 (port-only, research/COOP.md "Level sections and timed team tasks"): with two or three partners
// the level changes the partners' roles at its sections, and some sections are timed team tasks. Neither exists with
// one partner, so one-partner play is as before.
//
// A section starts at a display label (SetDisplayLabel 00225880, the key the level's script shows on the HUD ticker
// 00242b00, e.g. TORONTO2's "obj7_add", "Provide Imani covering fire"): from there partner k takes roles[k] (no entry:
// unchanged; a partner beyond the partners present: skipped). --coop-roles turns the sections off.
struct CoopRoleSection {
    std::string label;
    std::array<std::optional<CoopRole>, 3> roles{};
    std::string note;
};
std::optional<CoopRoleSection> coop_level_section(const std::string& mission, std::size_t partners, const std::string& label);

// A timed team task: at its start label each post (a Nav node by id) is given to a partner: first to the partner whose
// role is the post's preferred role, then the remaining posts in order to the lowest-numbered partners left. A posted
// partner goes there (MoveToNode at Run), holds there with weapons free and guards it (CoopOrderState::guard). The
// task waits until every posted partner is in position (or `patience` frames have passed) before its clock starts; it
// ends when the end objective succeeds or fails, or `time_limit` frames after the start; then the partners regroup.
struct CoopTeamPost {
    std::uint32_t node{};       // the Nav node id
    float x{}, y{};             // its position (checked against the level's graph by the host)
    CoopRole prefer{CoopRole::follow};
    std::string name;
};
struct CoopTeamTask {
    std::string name, start_label;
    std::int32_t end_objective{-1};   // the objective whose outcome ends it (-1: the time limit only)
    std::uint32_t patience{};         // frames to get in position
    std::uint32_t time_limit{};       // frames from the start (0: none)
    std::vector<CoopTeamPost> posts;
    std::string note;
};
std::optional<CoopTeamTask> coop_team_task(const std::string& mission, std::size_t partners, const std::string& label);
// Partner k's post (an index into task.posts), by role, then by partner number.
std::array<std::optional<std::size_t>, 3> coop_team_assign(const CoopTeamTask& task, const std::vector<CoopRole>& roles);

enum class CoopTeamPhase : std::uint8_t { idle, moving, running, done };
enum class CoopTeamEvent : std::uint8_t { none, in_position, started_late, objective_done, time_up };
struct CoopTeamState {
    CoopTeamPhase phase{CoopTeamPhase::idle};
    std::uint32_t started{}, running_at{}, ended_at{};
    CoopTeamEvent ended_by{CoopTeamEvent::none};
};
void coop_team_start(CoopTeamState& state, std::uint32_t frame);
// One frame: in_position is every posted partner at its post, objective_resolved the end objective's outcome.
CoopTeamEvent coop_team_step(CoopTeamState& state, const CoopTeamTask& task, std::uint32_t frame, bool in_position, bool objective_resolved);
const char* coop_team_event_name(CoopTeamEvent event);

}
