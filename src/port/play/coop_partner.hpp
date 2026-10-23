#pragma once
// Port-only (not in the original game; checkpoint 132ob2): the agent slots
// and the CPU co-op partner (omega_play --coop-partner, off by default).
//
// The original keeps four player slots 003fb8d0(0..3): in single player only
// slot 0 (the local player *004ffc0c) is filled, and the multiplayer session
// fills the others with the remote agents. AgentSlots is the port's table of
// who drives each agent slot: the local pad, the CPU co-op partner, or (for a
// later LAN feature) a remote player. It does not feed the NPC perception's
// player slots (h.player_slots, 0016e1a0): the multiplayer loops that read
// them are not modelled.
//
// The CPU partner is a role-2 population record with the player's manifest
// (TORONTO1 record 24, 329729 PC_MALE: the record nothing activates in solo,
// NPC_ACTIVATION_AND_RESPAWN.md). It is an ordinary AI actor of the actor
// world; the follow driver below only chooses NOD nodes and travel speeds and
// hands them to the original's script natives SetTravelSpeed (00149db0 ->
// 0014d6f0) and MoveToNode (0014a5c0 -> 0014be50: mode 7, priority 100, the
// route manager 004ed9b0), so every step of its
// movement is the original NPC navigation. With partner_stealth (default on)
// the partner is marked in port_cheats.hpp, so the enemy perception ignores it.
#include "animation_root.hpp"
#include "hotbox_world.hpp"
#include "npc_path_follower.hpp"
#include "solo_actor_simulation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace omega {
struct NpcPerception;
enum class AgentDriver : std::uint8_t { empty, local_pad, cpu_partner, remote };

struct AgentSlot {
    AgentDriver driver{AgentDriver::empty};
    std::size_t actor{};       // the actor world index
    std::uint32_t object_id{}; // the agent's object ID (+c)
};

class AgentSlots {
public:
    static constexpr std::size_t count = 4; // 003fb8d0(0..3)
    // Throws for a slot out of range, an occupied slot, an empty driver or an
    // actor that already has a slot.
    void assign(std::size_t slot, AgentDriver driver, std::size_t actor, std::uint32_t object_id);
    void release(std::size_t slot);
    const AgentSlot& slot(std::size_t index) const;
    std::optional<std::size_t> slot_of_actor(std::size_t actor) const;
    std::optional<std::size_t> first_free() const;
    std::size_t occupied() const;

private:
    std::array<AgentSlot, count> slots_{};
};

const char* agent_driver_name(AgentDriver driver);

struct CoopFollowConfig {
    std::uint32_t interval_frames{30}; // how often the driver re-plans
    float follow_distance{6.0F};       // horizontal distance to the player before it moves
    float max_dz{3.0F};                // a goal node within this height of the player
    float min_player_distance{0.0F};   // the start node keeps at least this distance from the player
    float run_distance{12.0F};         // farther than this the partner runs, otherwise it jogs
    float point_distance{3.5F};        // a route to a point stops this far from the player (past the 2.8 separation)
    float far_node_distance{40.0F};    // 132pw3 (port-only): beyond this the follow node yields to one in the band below
    // 132pw3 (port-only, up to 3 CPU partners): the goal nodes and places of the lower-numbered partners. A follow goal
    // within avoid_radius (horizontally) of one is skipped while another node qualifies, so the partners spread out.
    // Empty for partner 1 (its rule is unchanged).
    std::vector<RootVector> avoid;
    float avoid_radius{2.5F};
};

struct CoopFollowState {
    std::int32_t goal{-1};       // the node of the last MoveToNode (-1: none)
    std::uint32_t next_frame{0}; // the next frame the driver plans
    std::optional<RootVector> point; // the point of the last route to a point (checkpoint 132ob4)
    std::optional<RootVector> last_partner; // the partner at the last planning frame (checkpoint 132ob4)
};

// Checkpoint 132ob4: a planning step's request, a MoveToNode node or a route
// to a point (NpcAiController::port_route_to_point).
struct CoopFollowStep {
    std::optional<std::int32_t> node;
    std::optional<RootVector> point;
};

// The nearest node to `point` (horizontal distance) with the Nav bit 0x80 and
// at least one link, within max_dz in height and at least min_distance away;
// nullopt when there is none.
std::optional<std::int32_t> coop_partner_nearest_node(const NpcNodeGraph& graph, const RootVector& point, float max_dz,
                                                      float min_distance = 0.0F);

// Checkpoint 132ob9 (port-only follow rule): the follower's goal node. The
// nearest node (as coop_partner_nearest_node) on the partner's side of the
// player (dot(node - player, partner - player) >= 0, horizontally) when it is
// within follow_distance of the player, else the nearest node on any side.
// A goal kept behind the player does not stand in front of a door or a
// passage the player is heading for. Checkpoint 132ob11: a crawl's interior
// node (coop_partner_crawl_interior) is never a goal, on either side, so the
// partner crosses a crawl in one route. Checkpoint 132pw3 (port-only): when
// that nearest node lies farther than far_distance (horizontally), the node
// nearest the player in 3D within far_distance whose height lies between
// player z - 2 max_dz and player z + max_dz (Nav, linked, not a crawl
// interior) is taken instead; a player on a raised spot without nodes
// (TORONTO1's bank steps) gets the street node below it, not a node at its
// height across the level.
std::optional<std::int32_t> coop_partner_follow_node(const NpcNodeGraph& graph, const RootVector& player,
                                                     const RootVector& partner, float follow_distance, float max_dz,
                                                     float far_distance = std::numeric_limits<float>::infinity());

// Checkpoint 132pw3 (port-only, up to 3 CPU partners): coop_partner_follow_node, skipping the goal nodes within
// avoid_radius (horizontally) of an avoid point (the other partners' goals and places); with no such node left,
// the rule's own node. With no avoid points it is coop_partner_follow_node.
std::optional<std::int32_t> coop_partner_follow_node_spaced(const NpcNodeGraph& graph, const RootVector& player,
                                                            const RootVector& partner, float follow_distance, float max_dz,
                                                            float far_distance, const std::vector<RootVector>& avoid,
                                                            float avoid_radius);

// Checkpoint 132pw3 (port-only, up to 3 CPU partners): `count` start nodes near `start`. The first is
// coop_partner_nearest_node(graph, start, max_dz, min_distance) (partner 1's start, unchanged); each next one is
// the nearest such node at least `spacing` (horizontally) from every one before, else the first again. Empty
// when there is no first node. Since 132pw5 the next ones are never a crawl's interior node.
std::vector<std::int32_t> coop_partner_start_nodes(const NpcNodeGraph& graph, const RootVector& start, float max_dz,
                                                   float min_distance, std::size_t count, float spacing);

// Checkpoint 132pw5fix2 (port-only, the go-there order): the nodes a route search (npc_route_search) from node `start`
// can reach: the start, then breadth first over the links of each node with flags 0x88 (the search's expansion rule:
// no type-7 link, no negative target; with `overlay` the co-op overlay's entry links too). The search's pool limit
// is not modelled. Empty when start is outside the graph.
std::vector<bool> coop_partner_reachable(const NpcNodeGraph& graph, std::int32_t start, bool overlay);
// As coop_partner_start_nodes, among the nodes `reachable` marks with an index below `limit` (the crumbs, which the
// partner's own walks add, are routes, not destinations) and no crawl interior; empty when none lies within max_dz.
std::vector<std::int32_t> coop_partner_reachable_nodes(const NpcNodeGraph& graph, const std::vector<bool>& reachable,
                                                       std::size_t limit, const RootVector& point, float max_dz,
                                                       std::size_t count, float spacing);

// Checkpoint 132ob11: a crawl's interior node, every link (NOD and
// port_overlay) of type 8, as TORONTO1's 136789 ... 136795.
bool coop_partner_crawl_interior(const NpcNodeGraph& graph, std::size_t node);

// Checkpoint 132ob11 (port-only follow rule): the player is at or in a crawl,
// its nearest Nav node a crawl interior node within 3 m. The planning step
// then keeps the partner where it is until the player is out of the crawl.
bool coop_partner_player_at_crawl(const NpcNodeGraph& graph, const RootVector& player, float max_dz);

// One planning step at `frame`: nullopt, or the node the partner should be
// sent to with MoveToNode. Plans every interval_frames; nothing while the
// partner is within follow_distance of the player (the goal is cleared, so it
// is re-sent later; checkpoint 132ob6: horizontally, and within max_dz in
// height, so a partner on a landing under a player on the roof still moves),
// when the goal node (coop_partner_follow_node, checkpoint 132ob9) is the current goal, when
// that node is no closer to the player than the partner, or when the partner
// already stands on it (1.0).
std::optional<std::int32_t> coop_partner_follow(CoopFollowState& state, const CoopFollowConfig& config,
                                                const NpcNodeGraph& graph, const RootVector& partner,
                                                const RootVector& player, std::uint32_t frame);

// Checkpoint 132ob4: coop_partner_follow, and where it sends no node while the
// player is still farther than follow_distance and no Nav node lies within
// follow_distance of the player (the player stands away from the NOD graph),
// a route to a point (point): the point point_distance from the player
// toward the partner, else the player's own point, else the points on from
// there toward the partner in 1.0 steps (to 2.0 short of the partner), the
// first that `routable` accepts (a navigation region holds it, 0015d790).
// Not sent while the partner is on the way to its goal node (it moved more
// than 0.5 since the last planning frame, or there was none), nor again
// within 2.0 of the last point.
// Checkpoint 132ob6: while the goal node is a ladder foot (it has a type 3
// link) and the partner, still moving (more than 0.25 since the last planning
// frame), is more than 1.0 from it, nothing new is sent. This lets the next
// route start on the ladder link at the foot.
// Checkpoint 132pw5 (partners 2 and 3, config.avoid set): when the spaced node
// is no closer to the player than the partner, the unspaced rule's node.
CoopFollowStep coop_partner_follow_step(CoopFollowState& state, const CoopFollowConfig& config,
                                        const NpcNodeGraph& graph, const RootVector& partner, const RootVector& player,
                                        std::uint32_t frame, const std::function<bool(const RootVector&)>& routable);

// The eTravelSpeed (table 00489dd0) the driver hands to SetTravelSpeed (00149db0 -> 0014d6f0, priority 100)
// with each MoveToNode: Run (3) farther than run_distance from the player, otherwise Jog (2), the speed mode 7
// itself installs.
std::uint8_t coop_partner_travel_speed(const CoopFollowConfig& config, float apart);

// Checkpoint 132ob5 (port-only, --coop-partner): the catch-up rule. The
// original has no CPU partner, so nothing in it brings a lost follower back;
// this rule stands in for a remote player who would walk round on their own.
// - Stuck: the partner is not with the player (farther than near_distance
//   horizontally, or more than max_dz apart in height) and for stuck_seconds
//   (N) has neither come `progress` nearer the player than its best since the
//   count began nor moved `progress` from where it stood when the count
//   (re)started (a partner still running its route behind a faster player is
//   not stuck). The count is cleared while the partner is busy (a co-op box, a
//   buddy climb, a ladder) or with the player.
// - View: the host catches up only while the original's scene frame culls the
//   partner (003c8990 code 0x80, the actor's +38) and only to a point the
//   same cull puts outside the view, so the player never sees the move.
// - Point: a Nav node with links within max_dz of the player's height,
//   target_min..target_max from the player and in the player's navigation
//   region (0015d790), nearest the player first; else a place the player
//   stood on (the trail: the player's floor root every trail_interval frames
//   while it stands on the ground), within max_dz of its height and in the
//   same distance band, newest first.
// - Move: the Teleport Message 001bc4b0 mode 2 facing the player, then
//   StopMovement until the follow driver next sends it on.
struct CoopCatchUpConfig {
    float stuck_seconds{10.0F};        // N
    float near_distance{12.0F};        // with the player within this horizontally (and max_dz in height)
    float progress{2.0F};              // coming this much nearer than its best, or moving this far, restarts the count
    float target_min{4.0F};            // the point lies at least this far from the player (past the 2.8 separation)
    float target_max{12.0F};           // and at most this far (near_distance: the caught-up partner is with the player)
    std::uint32_t trail_interval{10};  // frames between trail places
    std::size_t trail_size{192};       // trail places kept
};

struct CoopCatchUpState {
    bool counting{};
    float stuck{};                    // seconds counted
    float best{};                     // the nearest (3-D) the partner has come during the count
    RootVector anchor{};              // where the partner stood when the count (re)started
    std::vector<RootVector> trail;    // the player's recent standing places, oldest first
    std::uint32_t next_trail{};
    std::uint32_t count{};            // catch-ups made
};

// Adds the player's place to the trail every trail_interval frames while
// `standing` (on the ground, not climbing or hanging, not in a crawl zone since checkpoint 132ob11).
void coop_partner_record_trail(CoopCatchUpState& state, const CoopCatchUpConfig& config, const RootVector& player,
                               std::uint32_t frame, bool standing);

// Counts the stuck time by `dt` seconds; true once it reaches stuck_seconds.
bool coop_partner_catch_up_due(CoopCatchUpState& state, const CoopCatchUpConfig& config, const RootVector& partner,
                               const RootVector& player, float max_dz, float dt, bool busy);

// The catch-up points in order of preference (see CoopCatchUpConfig);
// `same_region` says whether a node position lies in the player's navigation
// region.
std::vector<RootVector> coop_partner_catch_up_points(const CoopCatchUpState& state, const CoopCatchUpConfig& config,
                                                     const NpcNodeGraph& graph, const RootVector& player, float max_dz,
                                                     const std::function<bool(const RootVector&)>& same_region);

// Clears the count after a catch-up (the trail is kept).
void coop_partner_caught_up(CoopCatchUpState& state);

// Checkpoint 132ob6 (port-only rule over the original ray): the catch-up's wall test. A point is hidden
// from the viewer when the target manager's pair request (0022cb10: viewer -> 250 * the unit direction,
// mask 4; pair_los_request) answered by the world LOS service (0013a480, answer_los_request over the
// level collision) hits before the point (0022d1f0's reading, pair_los_blocks). The visibility cache
// 004f77c0 is not read or written. A body is hidden behind walls when all of its points are: the
// partner's feet + 0.3, + 1.0 and + 1.7 (coop_partner_body_points; the culler box is 0..1.9 high).
bool coop_partner_wall_hides(const SoloCollisionWorld& world, const RootVector& viewer, const RootVector& point);
std::array<RootVector, 3> coop_partner_body_points(const RootVector& feet);
bool coop_partner_walls_hide_body(const SoloCollisionWorld& world, const RootVector& viewer, const RootVector& feet);

// Port-only (checkpoint 132ob6): partner combat. The rule only decides *when*
// the partner fights; the fighting is the original allied NPC's: its team-0
// perception (the think round robin 0016e1a0 over 004ea190, scored and
// targeted through IsEnemy 0022f340 on the team matrix *004ee620), then
// ResumeAIMode 0014b570 (priority 0, mode 1 with a target), and from there
// the AI modes (1 decide 00156d00, 2-5) with the mode-independent firing gate
// 00150340 and the discharge 0013ff40.
// - Combat signals of a frame (from original state): the danger meter's
//   input (00273b40: the largest aim progress of an enemy aiming at the local
//   player, CombatPlayFrame::hud_meter) above 0; the player's own discharge
//   (a bullet, 00277e20); an NPC that treats the player as an enemy holding
//   the player as its perception target (+40).
// - The player is in combat from a signal until hold_seconds pass without one.
// - Engaged: in combat, and the partner within leash of the player (once
//   engaged, within release: a fighting partner does not follow, so the
//   player may move away from it before it is called back). Engaged,
//   the partner keeps its perception (port_set_perception_engaged), so it
//   picks a target; once it holds one the follow driver yields (ResumeAIMode
//   once, no MoveToNode). Not engaged, the follow driver's MoveToNode (mode 7,
//   priority 100) takes it back.
// - Stealth (partner_stealth on): enemies keep ignoring the partner until it
//   fires (its discharge clears the mark). It is marked again once the
//   player is out of combat and no NPC holds the partner as its target.
//   Disengaged while still marked, it forgets the NPCs it saw
//   (coop_partner_forget_npcs), so it does not fire while it follows.
// - Fire discipline (checkpoint 132ob7): the partner never shoots while the
//   player is in stealth. Stealth is broken (alerted) by the danger meter's
//   input above 0 or a hostile NPC holding the player (or the partner) as its
//   perception target; the player's own discharge alone does not break it (a
//   silenced kill stays stealthy). Alerted lasts until hold_seconds pass
//   without one of these. Not alerted, the partner's firing gate 00150340
//   holds every shot it decides (NpcAiController::port_hold_fire), engaged
//   or not, with partner_stealth on or off.
struct CoopCombatConfig {
    float hold_seconds{10.0F};   // out of combat this long after the last signal
    float leash{24.0F};          // engages only within this (3-D) distance of the player
    float release{40.0F};        // once engaged, farther than this the partner follows instead of fighting
};
struct CoopCombatSignals {
    float danger{};              // CombatPlayFrame::hud_meter (00273b40's result)
    bool player_fired{};         // the local player's discharge made a bullet this frame
    bool player_targeted{};      // an NPC hostile to the player holds it as its perception target
    bool partner_targeted{};     // an NPC holds the partner as its perception target (132ob7: alert only)
};
struct CoopCombatState {
    bool in_combat{};
    float quiet{};               // seconds since the last signal
    bool engaged{};
    bool yielded{};              // ResumeAIMode sent for this engagement
    bool revealed{};             // the partner has fired: the stealth mark is cleared
    std::uint32_t engagements{};
    bool alerted{};              // 132ob7: stealth broken (danger or a hostile target), the partner may fire
    float calm{};                // seconds since the last alert signal
};
// Folds one frame's signals (dt seconds); returns whether the player is in combat.
bool coop_partner_combat_update(CoopCombatState& state, const CoopCombatConfig& config, const CoopCombatSignals& signals,
                                float dt);
// Checkpoint 132ob7: whether the partner's firing gate may shoot (the player is
// out of stealth by the fire discipline above; state.alerted after the update).
bool coop_partner_may_fire(const CoopCombatState& state);
// Whether the partner should be engaged (in combat and within the leash, or
// the release distance while state.engaged).
bool coop_partner_combat_engaged(const CoopCombatState& state, const CoopCombatConfig& config, const RootVector& partner,
                                 const RootVector& player);
// Disengaged while still marked, the partner is a blind observer again (an
// empty think list, port_cheats.hpp) and must not keep the NPC it fought:
// its records are freed as 0016dc60 frees them, and its last visibility
// candidate +48 / +4c goes back to the constructor's 0 / -2 (0016e0a0), since
// the candidate walk 0016e1a0 hands back the previous candidate when the think
// list is empty. The next think (0016b580) then drops the target through its
// own no-record branch (0016ddb0 with 0), so the firing gate 00150340 has
// nothing to shoot at. Returns the number of records freed.
std::size_t coop_partner_forget_npcs(NpcPerception& perception);

// Port-only (checkpoint 132ob3): the partner's part of a co-op interaction
// box (type 12, the buddy climb of TORONTO1: its link is an InteractGOBJ that
// pairs two agents, 00216230). In the original the second agent is a remote
// player who walks into the box and presses Triangle (001714c0 -> HotboxMsg
// action 3 -> 00171880). The driver stands in for that player only: once the
// player stands in an eligible type-12 box with links, the follow driver is
// suspended, the partner is sent with MoveToNode to the node nearest the
// box's horizontal centre at the player's height. No Nav node need lie inside
// the box (TORONTO1's buddy box 139996 has none, and most of its floor lies
// in no navigation region), so on reaching that node the step asks for a
// route into the box (route_to; the host makes it with
// NpcAiController::port_route_to_point, the route request of mode 7's
// MoveToGobj). The candidate points are the box floor points of
// hotbox_entry_points that the caller's `routable` accepts (a navigation
// region holds them, 0015d790), nearest the partner first; the host reports
// each route's path event (coop_partner_route_answer): event 3 (the route
// search found no node with a clear line to the point) tries the next one.
// When the partner stands in the box the step asks for the
// Triangle (trigger_box, once). The host queues the trigger 00171880 makes
// (hotbox_multiplayer_interaction_trigger) to the partner's actor; everything
// after it is the original's pairing and action code. Leaving the box before
// the Triangle disengages; leaving it after reports the box as released (the
// host presses the partner's Triangle again when it still waits there alone,
// 00216230's leave, 0x32), and the follow driver resumes.
// The most route points into a box the partner tries (each failed search
// costs the route manager a few frames of LOS checks).
inline constexpr std::size_t coop_route_tries = 32;
// Checkpoint 132ob5: route points within this horizontal distance of the partner
// are skipped. The final route segment to a point no more than 2.0 away is no
// segment (00160720, set_npc_path_segment_to: c.ole.s +40, 2.0), so the partner
// would answer "route found" and never step into the box (box 2:8922).
inline constexpr float coop_route_min_segment = 2.0F;
// Checkpoint 132ms5: CoopBoostState::goal when no Nav node is near the box: the route points are asked for at once.
inline constexpr std::int32_t coop_route_goal_direct = -2;
// Checkpoint 132ms5: a command's box goes by a Nav node only when one lies this near it (horizontally); else the
// partner routes to the box's points directly (BELARUS2's bridge: the nearest node at its height is 241 m off).
inline constexpr float coop_command_node_reach = 30.0F;
// Port-only (132uk6c, CoopCommandRule::retry_stalled): a routing partner that moved under this far (m, horizontal)
// in this many frames (5 s at 60 Hz) has its route asked again.
inline constexpr float coop_command_stall_reach = 1.0F;
inline constexpr std::uint32_t coop_command_stall_frames = 300U;
struct CoopBoostState {
    std::optional<std::size_t> box; // the engaged box (index into the hotbox world)
    std::int32_t goal{-1};          // the MoveToNode node sent for it (-1: none; coop_route_goal_direct: no node near)
    std::vector<RootVector> entries; // the route points into the box still to try (routable, nearest first)
    std::size_t entry{};             // the next one to ask for
    bool route_pending{};            // a route was asked for and its path event has not come
    bool routed{};                   // a route into the box was found (path event 2)
    bool unreachable{};              // every point failed: the partner waits at the goal node
    bool triggered{};                // the partner's Triangle was asked for
    // Port-only (checkpoint 132ob4): the box whose climb the partner finished
    // (coop_partner_boost_done); not engaged again until the player leaves it.
    std::optional<std::size_t> done_box;
};
struct CoopBoostStep {
    std::optional<std::int32_t> move_to;   // MoveToNode for the partner
    std::optional<RootVector> route_to;     // a route to this point in the box (after the MoveToNode node is reached)
    std::optional<std::size_t> trigger_box; // the partner presses Triangle in this box
    std::optional<std::size_t> released;    // the player left the box after the partner's Triangle
    std::optional<std::size_t> unreachable; // no route into this box (reported once)
};
// The points of the box's floor (local minimum z) on a grid of `spacing`
// metres, `inset` inside the sides, nearest `from` (horizontally) first.
std::vector<RootVector> hotbox_entry_points(const Hotbox& box, const RootVector& from, float inset = 0.3F,
                                            float spacing = 0.5F);
// Port-only (checkpoint 132ms5): where a command's partner is stood in the box when the NPC navigation cannot
// take it there (no Nav node within coop_command_node_reach and no point of the box in a navigation region:
// BELARUS2's bridge, where the original has neither). The box's entry points nearest the partner first, at the
// partner's height clamped into the box, the first with a clear line from the partner (`blocked(a, b)` at 0.3
// and 1.0 above both) and a floor under it (blocked from 1.0 above to 0.5 below); nullopt: none.
std::optional<RootVector> coop_box_place_point(const Hotbox& box, const RootVector& partner, float inset,
                                               const std::function<bool(const RootVector&, const RootVector&)>& blocked);
// partner: the point the box test reads for the partner (actor +50, as the
// tracker 00171600 reads it; also its horizontal position for navigation);
// player: the player's floor placement; player_inside: the player's tracker
// list (+28), so "the player stands in the box" is the original's own state;
// routable: whether a route may be asked for to a point (a navigation region
// holds it).
CoopBoostStep coop_partner_boost(CoopBoostState& state, const HotboxWorld& world, const NpcNodeGraph& graph,
                                 const RootVector& partner, const RootVector& player,
                                 const std::vector<std::size_t>& player_inside, float max_dz,
                                 const std::function<bool(const RootVector&)>& routable);
// The path event (2 found, 3 not found) of the route route_to asked for.
void coop_partner_route_answer(CoopBoostState& state, std::uint8_t event);
// Port-only (checkpoint 132ob4): the partner has finished the follower half
// of the box's buddy climb (buddyclimb_secondguyup, the hang and the pull-up)
// and stands up top with the player. The box is disengaged (the follow driver
// resumes) and marked done, so it is not engaged again while the player stays
// in it (TORONTO1's box 139996 runs up to the roof). Nothing without an
// engaged, triggered box.
void coop_partner_boost_done(CoopBoostState& state);

// Port-only (checkpoint 132pc1, replaces 132ob8's automatic errand): partner
// commands. With the CPU partner the multiplayer objectives are created
// (mission_script_host.hpp partner_objective_multiplayer_site); some need the
// agents at two places at once. TORONTO1's laptop (box 139948 -> 140511,
// STREETS handler 839) must be taken within 9 s of the water main's turn (box
// 139949 -> InteractGOBJ 140471, handler 838: AddLaptop, then LaptopCheck
// scheduled at 9.0 s fails it), and the two are ~200 m apart; the laptop box
// is inside the bank, behind fire zone 144361 until the water main turns. The
// original's second agent is a human; the CPU partner is told what to do:
//   * a command point of the mission's table (an area: within `radius`
//     horizontally of `point` and `max_dz` vertically of its floor) where the
//     player can give the order while one of `targets` is open (its box
//     eligible: the script enables it while the objective needs it);
//   * while the player stands there and the partner is free, the game's own
//     interact prompt shows `prompt` (game_screen_interact_show, the script
//     prompt's client branch 0045d070(gs, text, button, 1));
//   * a fresh Triangle press there sends the partner (`partner_slot`, an
//     AgentSlots index: later rules may name other partners): it routes into
//     the target box with the original NPC navigation (coop_route_into_box,
//     the MoveToNode / mode 7 MoveToGobj route request, as for a boost box)
//     and presses Triangle in it: the box's base +0x114 (00171c90) Interact
//     triggers (hotbox_interaction_triggers) with the partner as the actor,
//     i.e. the script's Who, so the handler runs as for a second player;
//   * the order is kept when the player walks off (the player's own half is
//     elsewhere) and is dropped only when the target box stops being open.
// The table is general: several command points per mission, several target
// boxes per command (the first open one is taken) and a partner slot each.
// Checkpoint 132ms5 (BELARUS2's bridge C-4, research/COOP.md "BELARUS2 bridge"):
//   * `partner_slot` n is the n-th partner with the level tasks role (CoopRole::tasks; 1: the first, the one
//     that ran every rule before); a partner runs only the rules of its own slot (CoopCommandInput::slot);
//   * `automatic`: no prompt and no press: the order goes as soon as the player and the partner are in the area,
//     the partner is free and a target is open (the C-4 cache and the bridge: the partners take and plant their own C-4);
//   * `requires_item` / `unless_item`: the order goes only while the partner holds that item (its +3528 list,
//     PlayerGetItemCount's: the C-4 the script's PlayerAddItem gave it as Who) / holds none of it;
//   * `together`: in its box the partner waits (no Triangle) until the team is in position
//     (CoopCommandInput::team_ready: every agent holding the item stands in an open target box), so the
//     charges go together.
//   * `while_open`: the order goes only while one of these boxes is still open (the cache: while a plant spot is
//     unplanted, so a partner back at the cache after the plant takes no spare charge).
// Checkpoint 132uk6c (UKRAINE's launch override, research/UKRAINE_COOP.md "Checkpoint 132uk6c"): the two switches
// stand on platforms only a buddy climb reaches, so the partners climb for each other:
//   * `buddy`: the target is a type-12 co-op box, and the partner's Triangle there is the remote agent's
//     (hotbox_multiplayer_interaction_trigger, action 7 to the InteractGOBJ, 00216230's pairing). `boost`: it presses
//     on arrival, first, so (below the node by 5 m or more) it kneels (0x31); paired, it is the follower (0x33,
//     buddyclimb_secondguyup) and drops from the hang (the AI's own 0x13, the climb mark not set) so it stays below.
//     `climb`: in the box it waits until another agent kneels there (CoopCommandInput::buddy_waits), then presses
//     second: the leader (0x34, buddyclimb_firstguyup) climbs to the platform;
//   * `once`: the rule is not ordered again after its Triangle (CoopCommandState::done); `after_rule`: it is ordered
//     only after that rule's Triangle; `after_objective`: only once that objective (ObjectiveStage id) has been
//     active (latched; UKRAINE: Obj_SaveGabe, id 5, which StartFinalFight activates once the four lieutenants are dead:
//     CreateObjectives deactivates all seven, and the silo's entry box 1122998 is already enabled at the level start);
//   * `cue_closed`: in the box the Triangle waits until one of these boxes is closed (the other switch pressed:
//     OnGOBJTriggerEventMsg disables its box); `cue_occupied`: ... until another agent stands in one of these boxes
//     (the second switch's partner is in place), so the second press comes well inside the 20 s window;
//   * `hold_after`: once its action is over the partner holds there (the command menu's hold order, coop_order_apply),
//     as a second player stays on the platform; the follow driver and the catch-up leave it;
//   * `direct`: no MoveToNode to the box's nearest node first; the route points are asked for from where the partner
//     stands (the platforms' switch nodes: from the climb's landing the NOD search answers no route, path event 3);
//   * `retry_stalled`: when the partner, routing (not in the box), has moved under coop_command_stall_reach for
//     coop_command_stall_frames, the route is asked again from where it stands (step.rerouted; the 325 m walk from
//     B to A: a partner whose MoveToNode stopped after the ledge climb at (26.7, -354.2, -170.1) stood there);
//   * `min_tasks` (132uk6e): the rule is ordered only while at least this many partners have the level tasks role
//     (CoopCommandInput::tasks). UKRAINE's override needs three (both buddy pairs); with one partner its climb at B had
//     no booster, and the partner walked off to box 1123038 at the lieutenants' deaths and stood there to the level end.
enum class CoopBuddyPart : std::uint8_t { none, boost, climb };
struct CoopCommandRule {
    RootVector point{};                       // the command point (its floor)
    float radius{};                           // horizontal reach of the area around `point`
    float max_dz{};                           // vertical reach of the area
    std::vector<std::uint32_t> targets;       // the boxes the partner is sent to (ids; type 8 or 0x12; first open)
    std::uint32_t partner_slot{1};            // the n-th level tasks partner goes (132ms5)
    const char* prompt{};                     // the interact prompt's text (PORT-ONLY string)
    const char* sent{};                       // HUD ticker when the partner leaves (PORT-ONLY string; none: no ticker)
    const char* done{};                       // HUD ticker when its Triangle there is done (PORT-ONLY string; none: no ticker)
    const char* cancelled{};                  // HUD ticker when the order is dropped (PORT-ONLY string; none: no ticker)
    const char* what{};                       // the log's description
    bool automatic{};                         // ordered without a prompt or press (132ms5)
    const char* requires_item{};              // the partner goes only while it holds this item key (132ms5)
    const char* unless_item{};                // the partner goes only while it holds none of this item key (132ms5)
    bool together{};                          // in the box, the Triangle waits for CoopCommandInput::team_ready (132ms5)
    float inset{0.3F};                        // the route points' inset inside the box sides (hotbox_entry_points; 132ms5)
    std::vector<std::uint32_t> while_open{};  // none, or the order goes only while one of these boxes is open (132ms5)
    CoopBuddyPart buddy{};                    // a type-12 box's buddy climb: boost or climb (132uk6c)
    bool once{};                              // not ordered again after its Triangle (132uk6c)
    std::optional<std::size_t> after_rule{};  // ordered only after this rule's Triangle (132uk6c)
    std::uint32_t after_objective{};          // 0, or ordered only once this objective has been active (latched; 132uk6c)
    std::vector<std::uint32_t> cue_closed{};  // the Triangle waits until one of these boxes is closed (132uk6c)
    std::vector<std::uint32_t> cue_occupied{};// the Triangle waits until another agent stands in one of these (132uk6c)
    bool hold_after{};                        // the partner holds there once its action is over (132uk6c)
    bool direct{};                            // no MoveToNode first: the route points from where it stands (132uk6c)
    bool retry_stalled{};                     // a stalled route is asked again (coop_command_stall_frames; 132uk6c)
    std::uint32_t min_tasks{};                // 0, or ordered only with at least this many level tasks partners (132uk6e)
};
// The command points of a mission (none for most).
const std::vector<CoopCommandRule>& coop_command_rules(const std::string& mission);
// The player's floor root in the rule's area.
bool coop_command_area_contains(const CoopCommandRule& rule, const RootVector& player);
// The first open target box of a rule (eligible, type 8 or 0x12; a buddy rule's: type 12), as an index into the box world.
std::optional<std::size_t> coop_command_open_target(const CoopCommandRule& rule, const HotboxWorld& world);
// The first open box (eligible, type 8 or 0x12) among `ids` (132ms5).
std::optional<std::size_t> coop_command_open_box(const std::vector<std::uint32_t>& ids, const HotboxWorld& world);
struct CoopCommandState {
    std::optional<std::size_t> rule;   // the order being carried out (index into the rules)
    CoopBoostState route;              // route.box: the target box
    std::optional<std::size_t> prompt; // the rule whose prompt is up
    bool triangle_held{};              // the player's Triangle last step (a fresh press orders)
    bool waiting{};                    // in a together rule's box, waiting for the team (132ms5; 132uk6c: or its cue)
    std::vector<std::size_t> done;     // the rules whose Triangle was given (132uk6c: once / after_rule)
    std::vector<std::uint32_t> seen_active; // the after_objective objectives seen active (latched; 132uk6c)
    CoopBuddyPart buddy{};             // the part of the buddy climb the partner's last Triangle took (132uk6c)
    std::optional<std::size_t> buddy_box;   // its box (132uk6c)
    RootVector stall_point{};          // where the routing partner last moved coop_command_stall_reach (132uk6c)
    std::uint32_t stall_frame{};       // and when (CoopCommandInput::frame)
    // 132pw5fix2: steps a seeking partner waits after its Triangle in a target box (its action starts and the box closes)
    std::uint32_t seek_wait{};
};
struct CoopCommandInput {
    RootVector player{};               // the player's floor root
    bool triangle{};                   // the player's Triangle this frame (gated pad input)
    bool partner_free{};               // the partner is on, alive and not in a boost, climb or action
    std::uint32_t slot{1};             // the partner's level tasks slot: it runs the rules with this partner_slot (132ms5)
    std::uint32_t tasks{1};            // how many partners have the level tasks role (min_tasks; 132uk6e)
    std::function<std::int32_t(const char*)> held;   // the partner's count of an item key (the item gates; 132ms5)
    bool team_ready{true};             // every agent holding the item stands in an open target box (together rules; 132ms5)
    // 132uk6c: another agent kneels at the box's InteractGOBJ (00216230's first +b4, below the node: not +94, no
    // second +b8, the pair not done +b0): a climb rule's partner may press second.
    std::function<bool(std::size_t)> buddy_waits;
    // 132uk6c: another agent (the player or another partner) stands in the box (cue_occupied).
    std::function<bool(std::size_t)> occupied;
    // 132uk6c: the objective (ObjectiveStage id) is active (after_objective).
    std::function<bool(std::uint32_t)> objective_active;
    std::uint32_t frame{};             // the frame (retry_stalled; 132uk6c)
    // Checkpoint 132pw5fix2: the level tasks role given from the command menu's Tasks page: with no order, the first
    // rule with an open target is ordered at once, wherever the player is and without the Triangle.
    bool seek{};
};
struct CoopCommandStep {
    CoopBoostStep route;                   // the partner's navigation and its Triangle (route.trigger_box)
    std::optional<std::size_t> show;       // the prompt of this rule goes up
    bool hide{};                           // the prompt goes down
    std::optional<std::size_t> ordered;    // the partner was sent (rule index)
    std::optional<std::size_t> arrived;    // the partner's Triangle in the target box (rule index)
    std::optional<std::size_t> cancelled;  // the order was dropped: its target box closed (rule index)
    std::optional<std::size_t> waiting;    // the partner reached a together rule's box and waits for the team (once; 132ms5)
    std::optional<std::size_t> cued;       // the partner reached its box and waits for the rule's cue (once; 132uk6c)
    std::optional<std::size_t> rerouted;   // a retry_stalled rule's route is asked again (132uk6c)
    bool nothing{};                        // seeking (132pw5fix2) and free, but no rule has an open target
};
// 132pw5fix2: after its Triangle in a target box a seeking partner orders nothing for this many steps (frames).
inline constexpr std::uint32_t coop_command_seek_wait = 90;
// Whether a rule applies to this partner now: its slot and its item gates (132ms5).
bool coop_command_rule_gates(const CoopCommandRule& rule, const CoopCommandInput& input);
// Whether rule r may be ordered given the partner's history (132uk6c): once, after_rule and after_objective.
bool coop_command_rule_sequence(const CoopCommandState& state, const std::vector<CoopCommandRule>& rules, std::size_t r);
// Whether the partner in rule r's box (target index `box`) may press now (132uk6c): a climb rule waits for a kneeling
// booster, cue_closed for one of its boxes to close, cue_occupied for another agent in one of its boxes.
bool coop_command_cue_ready(const CoopCommandRule& rule, std::size_t box, const CoopCommandInput& input,
                            const HotboxWorld& world);
// One step. With no order: the first rule (of the partner's slot, its item gates passed) whose area holds the
// player and which has an open target while the partner is free: an automatic rule orders at once, else its
// prompt goes up and a fresh Triangle press under it orders (step.ordered) and routes at once. With an
// order: the target box closed -> cancelled (route.released); the partner's point in the box -> its Triangle
// (route.trigger_box, step.arrived) and the order ends (a together rule first waits for input.team_ready);
// else the route into the box (coop_route_into_box). Seeking (input.seek, 132pw5fix2) with no order and the
// partner free, the first rule (of its slot, gates passed) with an open target is ordered without the area or
// the Triangle; with none, step.nothing.
CoopCommandStep coop_partner_command(CoopCommandState& state, const std::vector<CoopCommandRule>& rules,
                                     const CoopCommandInput& input, const HotboxWorld& world, const NpcNodeGraph& graph,
                                     const RootVector& partner, float max_dz,
                                     const std::function<bool(const RootVector&)>& routable);

// Port-only (checkpoint 132ob7): the partner's respawn beside the player (the
// player revived at its checkpoint, or the partner revived at the player's next
// checkpoint). Every driver state is cleared as a newly spawned partner's: no
// goal, point or box; the catch-up count and trail (the player's places before
// the respawn) are cleared, its catch-up total is kept; the combat state is out
// of combat, not engaged, not revealed and not alerted (fire held until a new
// alert signal), its engagement total is kept.
void coop_partner_respawn_reset(CoopFollowState& follow, CoopBoostState& boost, CoopCatchUpState& catch_up,
                                CoopCombatState& combat);

// Port-only (checkpoint 132ob7): where the partner respawns. The original's
// SetPlayersCheckpoint (0022de90) gives every player slot the same checkpoint
// matrix, so co-op players are revived on one point; the partner takes the
// player's checkpoint (`anchor`, facing `right` x forward from its matrix rows
// 0 and 1) and is stood beside it rather than inside the player, outside the
// 2.8 m actor-actor separation (001e7f40) so neither is pushed:
//   1. the nearest Nav node with links within `node_radius` (horizontal; at
//      least `min_distance` away, within max_dz) with a clear line from the
//      anchor (blocked(from, to): the level collision hits between them);
//   2. else `offset` to the right, left, behind or in front of the anchor when
//      the line from it is clear at + 0.3 and + 1.0 and a floor is under it (a
//      hit from + 1.0 down to - 0.5);
//   3. else the anchor itself (the original's shared checkpoint).
// Checkpoint 132pw5: `taken` holds the places the lower-numbered partners were
// just stood on; steps 1 and 2 skip a place within `min_distance` of one of
// them (before 132pw5 the player's checkpoint revive stood all three partners
// on one node, coop_t1_3p_end frame 5667). Empty for partner 1.
enum class CoopRespawnPlace { node, beside, shared };
struct CoopRespawnPoint {
    RootVector point{};
    CoopRespawnPlace place{CoopRespawnPlace::shared};
    std::optional<std::int32_t> node;
};
struct CoopRespawnConfig {
    float node_radius{6.0F};
    float min_distance{3.0F};   // past the 2.8 m actor-actor separation (001e7f40)
    float offset{3.5F};         // the follow goal's point_distance
};
CoopRespawnPoint coop_partner_respawn_point(const CoopRespawnConfig& config, const NpcNodeGraph& graph,
                                            const RootVector& anchor, const RootVector& right,
                                            const RootVector& forward, float max_dz,
                                            const std::function<bool(const RootVector&, const RootVector&)>& blocked,
                                            const std::vector<RootVector>& taken = {});

// Port-only (checkpoint 132ob5): the co-op link overlay. TORONTO1's z 18
// rooftop (the player's route to objective 4 climbs a low wall with Triangle,
// HangClimbUp, then the ladder at (-44, 109.6)) has no NOD link from the
// ground: the roof nodes 135889 / 135891 / 135892 link only to each other, and
// the NOD has no ladder link (type 3 / 0xb) anywhere. The overlay adds the
// links an NOD author would have drawn: for the low wall the NOD's own pattern
// for its one climb (TOR1_QLF_1's patrol: a type-4 link 136719 -> 136720 that
// ends where a Triangle catches the wall), for the ladder the AI's ladder link
// (type 3: action 0x1a, the ladder grab, then 0x1e when the route leaves it),
// a node before each, a link to a node past its top, type-2 walking links
// between, and an entry link from a NOD node. The partner then climbs with the
// original code paths only: the route search 00162080, the path event 7
// (0015217c: movement state 2, then action 0x0f / 0x1a), the edge detection
// and mount (001dfb80 / 001930a0), the ladder navigation 001a80e0 mode 16 and
// its 00476490 climb / exit request (with SoloActorSimulation::
// set_port_ladder_arrival_at_end for its arrival). The
// overlay nodes are appended past the NOD's (Nav bit only, so no patrol
// takes them; no region group lists them, so no node search or region box
// sees them) and the entry links sit in NpcNodeGraph::port_overlay, which
// only the partner's route client searches. The positions are the player's
// own route (toronto1_all_objectives.txt, frames 900-1900). Checkpoint 132ob11
// adds the A2 pocket and the crawl into the C4 room as type-8 links (the NOD's
// own crawl pattern, 135871 -> 136789 ... 136796), which no NOD link reaches.
struct CoopOverlayNode {
    std::uint32_t id{};           // port-only IDs from 0x80000001 (no NOD record uses them)
    std::array<float, 3> position{};
    std::int32_t region{-1};      // node +7c: the region of the NOD nodes around it
};
struct CoopOverlayLink {
    std::uint32_t from{}, to{};   // node IDs (NOD records or overlay nodes)
    std::int32_t type{};          // 2 walk, 3 ladder (0x1a), 4 climb (0x0f, Triangle), 8 crawl (action 9, 132ob11)
};
struct CoopLinkOverlay {
    std::vector<CoopOverlayNode> nodes;
    std::vector<CoopOverlayLink> links;
};
inline constexpr std::uint32_t coop_overlay_first_id = 0x80000001U;
// The overlay of a mission folder (TORONTO1; TORONTO2 since checkpoint 132ob10), or null.
const CoopLinkOverlay* coop_link_overlay(const std::string& mission);
// Appends the overlay nodes and links to the graph. Throws for an unknown ID,
// a duplicate overlay ID, an overlay link from an overlay node to a NOD node
// of no region, or a link no longer than 2.0 (00160720 skips such segments).
// Returns the index of the first overlay node.
std::int32_t install_coop_link_overlay(NpcNodeGraph& graph, const CoopLinkOverlay& overlay);
}
