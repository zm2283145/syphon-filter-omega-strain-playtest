#pragma once
// Port-only (not in the original game; checkpoint 132pc2): the CPU partner's
// traversal breadcrumbs (omega_play --coop-partner; off with --no-coop-breadcrumbs).
//
// The original's second agent is a human, who follows the other player through a
// pipe, over a wall or up a ladder with the pad. The CPU partner (coop_partner.hpp)
// moves with the original NPC navigation only, so it can cross a traversal only
// where a NOD link (or the hand-made co-op link overlay) says how. TORONTO1's NOD
// has one climb link and a few crawls; every other climbable wall, ledge, pipe and
// ladder the player uses is a dead end for the partner, which waited there until
// the catch-up rule moved it.
//
// The breadcrumbs record each traversal the player makes (where its standing
// position was before, the crouched line of a crawl, where it stood again) and
// turn it into nodes and links of the partner's Nav overlay at run time, of the
// link types the AI component already answers with the player's own actor
// actions (00151d30 path event 7, 00151a90 controller code 8, 00152168):
//   climb       type 4   action 0x0f (the Triangle's 0024a1c0 sends the same)
//   climb down  type 0xb action 0x10 (the Cross's 00249f40)
//   crawl       type 8   action 9 (0014d900(4); the crouch toggle 0024a7a0)
//   ladder      type 3   action 0x1a (the ladder grab)
//   drop        type 2   a walk off the edge (the fall is the actor's own)
// The actor then runs the same detection (001dfb80), CAS_ACTION 0x2a (001930a0),
// climb, hang (the AI's answer to code 6 on a type-4 link is the pull-up 0x11),
// crawl and ladder code as for the player: no new movement or physics.
// The partner is sent along them in order (the queue below): while one is pending
// the follow driver's goal is that crumb's last node, consumed on arrival.
// The catch-up teleport stays the last resort it was.
#include "animation_root.hpp"
#include "npc_path_follower.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omega {
// walk (132pw5fix2): not a traversal of the player's but a walk bridge (install_coop_walk_bridge below).
enum class CoopTraversalKind : std::uint8_t { none, climb, climb_down, crawl, ladder, drop, buddy, jump, walk };
const char* coop_traversal_kind_name(CoopTraversalKind kind);
// The NOD link type the AI takes for a kind (above); -1 for none, buddy and jump
// (the buddy climb is the co-op box driver's; the AI's jump links 5 / 0xc / 0xd
// are not native, npc_ai.cpp).
std::int32_t coop_traversal_link_type(CoopTraversalKind kind);

// One frame of the player (actor 0).
struct CoopPlayerSample {
    std::uint32_t frame{};
    RootVector floor{};          // the flor root's world position
    std::uint32_t actions{};     // the active actions (1 climb, 7 / 8 hang, 10 / 11 fall and landing, 12-15 climb and hang motions, 16 ladder, 17-19 buddy climb)
    bool airborne{};             // physical +de
    bool crouched{};             // +3248 bit 1 (action 9)
    bool crawl_zone{};           // the input's +0x24 (a code-9 hotbox)
};
// The actions the recorder takes as a traversal (the catch-up rule's "climbing" set).
inline constexpr std::uint32_t coop_traversal_actions = (1U << 1) | (1U << 7) | (1U << 8) | (1U << 12) | (1U << 13) |
                                                        (1U << 14) | (1U << 15) | (1U << 16) | (1U << 17) | (1U << 18) |
                                                        (1U << 19);
inline constexpr std::uint32_t coop_buddy_actions = (1U << 17) | (1U << 18) | (1U << 19);
// The fall (action 10, 001a1590: "already falling") and the landing after it (11): a walk off an edge falls without
// the physical airborne byte (TORONTO1's dumpster at (-48, 109.3): from z 5.8 to 0.9 in frames 3600-3623).
inline constexpr std::uint32_t coop_fall_actions = (1U << 10) | (1U << 11);
// The player traverses: a traversal action, a fall, airborne, or crouched in a crawl zone.
bool coop_player_traversing(const CoopPlayerSample& s);

struct CoopBreadcrumbConfig {
    float spacing{2.2F};           // node spacing; every link must be longer than 2.0 (00160720)
    float min_rise{0.8F};          // a rise or fall at least this is a climb or drop
    std::uint32_t settle_frames{6}; // standing this many frames ends a traversal (the exit: where it stands then)
    std::uint32_t after_wait_frames{30}; // a crumb waits this long for the after place, then goes without it
    float jump{5.0F};              // a move this far in one frame is a teleport: the trail restarts
    std::size_t trail_size{96};    // standing places kept (spacing apart; 48 before 132pw5fix2: a crumb's approach uses max_approach of them)
    float join_radius{10.0F};      // a join to a node within this horizontally
    float join_max_dz{1.5F};       // and this in height
    std::size_t max_joins{2};
    std::size_t max_approach{24};  // trail nodes before the traversal at most
    float approach_step{6.0F};     // trail places farther apart than this are not joined (a teleport between)
    float covered_radius{3.0F};    // an existing link of the type within this of both ends covers the traversal
    float reuse_reach{0.6F};       // a climb's or ladder's entry reuses a node only this near (132ob6's ladder foot)
    float bridge_reach{0.6F};      // else its entry moves at most this past the player's place to a link's length
    float routed_factor{3.0F};     // the level's links already lead from the entry to the exit within
    float routed_extra{15.0F};     // routed_factor * the straight distance + routed_extra: no crumb
    float arrive_distance{1.5F};   // the partner within this of a crumb's goal horizontally
    float arrive_dz{1.0F};         // and this in height has consumed it (not while busy: a climb still under way)
    std::uint32_t patience_frames{1200}; // a crumb not consumed in 40 s (not counting busy frames) is dropped (and its links disabled)
    std::uint32_t stall_frames{150};     // the partner moved less than stall_move in this many frames: the MoveToNode again
    float stall_move{0.5F};
    std::uint32_t max_resends{2};        // then the crumb is dropped
    std::size_t max_pending{8};
    std::size_t max_tail{16};            // the player's standing places after a crumb's exit added as its tail
    std::size_t node_capacity{4096};     // nodes the graph may hold past the level's (reserved: no reallocation; 1024 before 132pw5fix2)
    // 132pw5fix2 walk bridges (install_coop_walk_bridge).
    float bridge_step{8.0F};             // a bridge link joins trail places at most this far apart (on a clear line)
    float bridge_step_dz{2.5F};          // and this in height
    std::size_t bridge_max_nodes{40};    // a bridge's nodes at most (the route buffer holds 100 nodes, 00161930 +78)
    std::size_t bridge_reserve{256};     // graph room kept for the crumbs and tails (which throw when it is full)
    float bridge_stuck_seconds{3.0F};    // trigger (c): stuck this long (the catch-up count) with a wall between it and the player
    std::uint32_t bridge_stuck_gap{300}; // and at least this many frames after the last bridge tried
};

struct CoopBreadcrumb {
    CoopTraversalKind kind{CoopTraversalKind::none};
    std::vector<RootVector> before;   // the standing trail before the traversal, oldest first
    RootVector entry{};               // where the player stood when it began
    std::vector<RootVector> line;     // a crawl's crouched places (spacing apart)
    RootVector exit{};                // where it stood again
    std::optional<RootVector> after;  // the first standing place spacing past the exit
    std::uint32_t start_frame{}, end_frame{};
    float rise{};                     // exit z - entry z
};

struct CoopBreadcrumbRecorder {
    std::vector<RootVector> trail;
    std::optional<RootVector> last;   // the previous frame's floor
    std::optional<RootVector> last_standing;
    bool episode{};
    RootVector start{};
    std::uint32_t start_frame{};
    std::vector<RootVector> before;
    std::optional<RootVector> settle;
    std::uint32_t settle_frame{}, quiet{};
    bool ladder{}, crawl{}, climb{}, air{}, buddy{};
    std::vector<RootVector> line;
    std::optional<CoopBreadcrumb> finishing;
    std::uint32_t restarts{};         // trail restarts after a teleport
};
// Feeds one frame. Returns the traversals that finished (classified; kind none
// for a traversal that is not one, e.g. a jump that lands level). A crumb is
// returned once the player stands spacing past its exit (after), or without
// `after` when the next traversal starts first or after_wait_frames pass.
std::vector<CoopBreadcrumb> coop_breadcrumb_record(CoopBreadcrumbRecorder& r, const CoopBreadcrumbConfig& c,
                                                   const CoopPlayerSample& s);
// The kind of a finished traversal from what the player did: buddy, ladder, crawl, then a climb
// (climb down when it fell min_rise or more, else climb, also when it ends level: a climb over), then
// in the air a drop (fell min_rise or more) or a jump.
CoopTraversalKind coop_breadcrumb_classify(bool ladder, bool crawl, bool climb, bool air, bool buddy, float rise,
                                           float min_rise);

struct CoopBreadcrumbHost {
    std::function<bool(const RootVector&, const RootVector&)> clear;  // a clear line between two floor places
    std::function<std::int32_t(const RootVector&)> region;            // the navigation region (0015d790) at a place, or -1
};
struct CoopBreadcrumbNodes {
    std::vector<std::int32_t> nodes;   // the installed nodes, the farthest approach node first
    std::int32_t entry{-1};            // the node the typed link starts at
    std::int32_t goal{-1};             // the last node (after, else exit)
    std::vector<std::int32_t> joins;   // nodes joined (in and out)
    std::size_t links{};
    bool joined_out{};                 // the goal is joined to the graph (no tail needed)
};
// An existing link of the kind's type (a NOD link, the static overlay's, an earlier
// crumb's) from within covered_radius of the entry to within it of the exit or after:
// the partner already crosses here (TORONTO1's low wall, ladder and C4 crawl).
bool coop_breadcrumb_covered(const NpcNodeGraph& graph, const CoopBreadcrumb& crumb, const CoopBreadcrumbConfig& c);
// The partner's own navigation already crosses here: from a join of the entry
// (a Nav node with links and a region within join_radius / join_max_dz on a
// clear line, as install_coop_breadcrumb joins) the graph's links (the NOD's,
// the static overlay's and earlier crumbs'; nodes with the Path or Nav bit, as
// the route search 00162080 expands them) reach a join of the exit (or the
// after place) in at most routed_factor * the straight distance + routed_extra.
// Such a traversal is left to the original navigation (TORONTO1's drop from
// the street wall at (-2, 42.6) beside the NOD's own walk down to 136008).
bool coop_breadcrumb_routed(const NpcNodeGraph& graph, const CoopBreadcrumb& crumb, const CoopBreadcrumbConfig& c,
                            const CoopBreadcrumbHost& host);
// Appends the crumb's nodes and links to the graph. nod_count: the level's own
// nodes (their links stay untouched; links from them go to graph.port_overlay);
// join candidates are the nodes before first_crumb (the NOD and the static overlay)
// with the Nav bit, links and a region, plus `goals` (earlier crumbs' standing nodes). When such a node lies
// nearer than a link to the chain's first place (a ladder from where a climb ended) the chain starts on it;
// one nearer than a link to its last place (the static overlay's wall-top node) ends it. A climb's or a
// ladder's entry reuses only a node within reuse_reach; one farther (but nearer than a link) is joined by a
// walking link to the entry moved a link's length from it (at most bridge_reach past the player's place).
// Node IDs from next_id. Returns nullopt with `why` when the crumb cannot be joined
// to the graph or its links would be too short.
std::optional<CoopBreadcrumbNodes> install_coop_breadcrumb(NpcNodeGraph& graph, std::size_t nod_count,
                                                           std::size_t first_crumb, const CoopBreadcrumb& crumb,
                                                           const CoopBreadcrumbConfig& c, const CoopBreadcrumbHost& host,
                                                           std::uint32_t& next_id, const std::vector<std::int32_t>& goals,
                                                           std::string& why);
inline constexpr std::uint32_t coop_breadcrumb_first_id = 0x80010001U;
// Disables the crumb's typed links (target -1, which the route search skips,
// 00162080): a traversal the partner failed is not routed through again.
std::size_t disable_coop_breadcrumb(NpcNodeGraph& graph, const CoopBreadcrumbNodes& nodes, CoopTraversalKind kind);

struct CoopBreadcrumbTrack {
    CoopTraversalKind kind{};
    CoopBreadcrumbNodes nodes;
    std::uint32_t created{};
    std::optional<std::uint32_t> sent;  // the frame of the MoveToNode
    std::uint32_t waited{};             // frames since it, not counting busy ones (the patience)
    std::uint32_t resends{};
    RootVector stall_anchor{};
    std::uint32_t stall_frame{};
};
struct CoopBreadcrumbQueue {
    std::deque<CoopBreadcrumbTrack> pending;
    std::vector<std::int32_t> goals;    // join candidates: every installed crumb's standing nodes (a crawl's ends) and tail nodes
    std::uint32_t installed{}, consumed{}, dropped{}, cleared{};
};
struct CoopBreadcrumbStep {
    bool active{};                               // a crumb is pending: the follow driver yields
    std::optional<std::int32_t> move_to;         // MoveToNode for the partner
    std::vector<CoopBreadcrumbTrack> consumed;   // reached
    std::vector<CoopBreadcrumbTrack> dropped;    // given up (patience or stalls)
    std::size_t cleared{};                       // dropped because the partner is with the player
};
// One step. with_player: the partner is with the player (the caller's test), so
// the pending crumbs are no longer needed. busy: the partner is in a box, an
// errand, a fight or a traversal: nothing is sent and the patience and stall
// counts wait. traversing: the partner is in a traversal (a climb still under
// way): it has not reached a goal yet.
CoopBreadcrumbStep coop_breadcrumb_follow(CoopBreadcrumbQueue& q, const CoopBreadcrumbConfig& c, const NpcNodeGraph& graph,
                                          const RootVector& partner, std::uint32_t frame, bool with_player, bool busy,
                                          bool traversing);
// The head crumb's route answered path event 3 (no route): it is dropped.
std::optional<CoopBreadcrumbTrack> coop_breadcrumb_route_failed(CoopBreadcrumbQueue& q);
// A queued crumb (oldest dropped past max_pending).
void coop_breadcrumb_push(CoopBreadcrumbQueue& q, const CoopBreadcrumbConfig& c, CoopBreadcrumbTrack track);
// After a catch-up, respawn or test teleport: nothing pending (the links stay).
std::size_t coop_breadcrumb_clear(CoopBreadcrumbQueue& q);
// The tail: where the player walks on from a crumb whose goal joins nothing
// (TORONTO1's mall low wall E1: the far side has no Nav node), its standing
// places spacing apart become nodes linked by walking links (type 2) from the
// goal, until one joins the graph (as install_coop_breadcrumb joins), the
// player starts another traversal, moves more than approach_step in one place
// (a teleport), or max_tail places are added. The follow driver's goal nodes
// then lead the partner down the player's own way.
struct CoopBreadcrumbTail {
    bool open{};
    std::int32_t last{-1};              // the last node (the crumb's goal, then each tail node)
    std::size_t count{};
    std::vector<std::int32_t> own;      // the crumb's and the tail's nodes (not joins)
};
// Opens the tail of an installed crumb (only when its goal joined nothing).
void coop_breadcrumb_open_tail(CoopBreadcrumbTail& tail, const CoopBreadcrumbNodes& nodes);
struct CoopBreadcrumbTailStep {
    std::optional<std::int32_t> node;   // the node added
    std::vector<std::int32_t> joins;    // joined (the tail is closed)
    bool closed{};
};
// One frame of the player standing at `player` (call it only while the player
// is not traversing; close the tail when it is).
CoopBreadcrumbTailStep coop_breadcrumb_extend_tail(CoopBreadcrumbTail& tail, NpcNodeGraph& graph, std::size_t nod_count,
                                                   std::size_t first_crumb, const RootVector& player,
                                                   const CoopBreadcrumbConfig& c, const CoopBreadcrumbHost& host,
                                                   std::uint32_t& next_id, const std::vector<std::int32_t>& goals);
// A pending crumb whose goal is `from` takes `to` (its tail grew); a crumb
// already sent is sent again (not counted as a resend). The goals list too.
bool coop_breadcrumb_retarget(CoopBreadcrumbQueue& q, std::int32_t from, std::int32_t to);

// Port-only (checkpoint 132pw5fix2, research/COOP.md "Follow through doors"): the walk bridge. The level's
// Nav graph has gaps the player walks through but the partner's route search cannot: TORONTO1's autopsy tent
// (its interior nodes 136673 .. 136685 have no link to the street's, the player walks in through the decon
// entrance) and the subway stairs (from the street to the platform at z -50 no link). There MoveToNode
// answers path event 3 and the partner stood until the catch-up teleported it. A bridge turns the player's
// own standing trail (CoopBreadcrumbRecorder::trail: where it walked, spacing apart, since its last
// traversal or teleport) from the newest place the partner reaches on a clear line to the player into
// nodes joined by walking links (type 2, both ways; places up to bridge_step apart on a clear line make one
// link), joined at its start to the partner's route start node (start_node: the follower's held or current
// node, which the route search opens, npc_route_to_node) and the level's nodes there, and at its end to the
// level's nodes by the player. The partner then walks the player's way with the original route search
// and path following (MoveToNode); the catch-up teleport stays the last resort.
// Returns nullopt with `why` when no trail place is in reach of the partner, nothing joins its start, the
// bridge would be shorter than a link, or the graph lacks room (bridge_reserve kept for the crumbs).
std::optional<CoopBreadcrumbNodes> install_coop_walk_bridge(NpcNodeGraph& graph, std::size_t nod_count,
                                                            std::size_t first_crumb, const std::vector<RootVector>& trail,
                                                            const RootVector& partner, const RootVector& player,
                                                            std::int32_t start_node, const CoopBreadcrumbConfig& c,
                                                            const CoopBreadcrumbHost& host, std::uint32_t& next_id,
                                                            const std::vector<std::int32_t>& goals, std::string& why);
// The partner's walk bridge bookkeeping (per partner).
struct CoopWalkBridgeState {
    std::optional<std::uint32_t> last_frame;   // the last bridge tried (installed or not)
    std::int32_t failed_start{-1};             // the start node whose bridge route failed (not bridged from again)
    std::optional<RootVector> last_goal;       // the last bridge's goal place (a bridge to the same place is not repeated)
    std::uint32_t engaged_frame{};             // the last frame engaged in the player's fight (trigger (c) counts from it)
    std::uint32_t installed{}, refused{};
};

}
