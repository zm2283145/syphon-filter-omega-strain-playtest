#pragma once
// Route following for the AI (NPC_COMBAT.md section 5.7, milestone N5a): the
// regions (GRP: node groups 001c0ee0, the boxes 0015da70, the region lookup
// 0015d790, the nearest node of a group 0015d510), the route manager (key
// 004ed9b0, vtable 004d96c0, 0x7c bytes; 00164620 / 1644b0) with its 16
// requests (0x610 bytes), the route request 0015efb0 behind 0015fb20 (to a
// node) and 0015fa70 (to a point in a region), the route search 00162080
// (an A* over the NOD links with the entry pool 004ea7b0, 400 entries of
// 0x20 bytes, counter 004ea798), the manager's frame update 00163ad0 (one
// request per frame, and the per-player region table 004ed9c0), the queue
// helpers 00163e20 / 00163c50 / 00163fd0, and the path follower's side: the
// route buffer +74 (allocated by 00160f60), the route start 00160ce0, the
// route step 00160a00, the release 00160e60 and the distance 00160130.
//
// Single player. N5c (npc_node_search.hpp): node-search requests (kind 2,
// 0015ebc0) are run by the frame update through 00161950 and answered by the
// LOS-check results (005435a8 -> npc_route_los_result). The route search's
// states 0-2 (a route request without a start node, or to a region) run the
// global node search object 004ed9f0 and the LOS manager the same way, their
// LOS results going through 00163580.
#include "npc_node_search_state.hpp"
#include "npc_path_follower.hpp"
#include "population.hpp"
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace omega {
// GRP: (001c0ee0): a count, then per group a count and that many node
// indices. Group r is region r (node +7c); 004ffbe4 = the group count,
// 004ffbe8 +8 = the 16-byte entries (+4 count, +8 node indices).
std::vector<std::vector<std::int32_t>> decode_population_groups(std::span<const std::uint8_t> bytes,const Population& population);

struct NpcRegionBox {RootVector minimum{},maximum{};};
struct NpcRegions {
    std::vector<std::vector<std::int32_t>> groups;
    // 004ee118: built on the first lookup (0015da70) while the group count is
    // positive; null (unbuilt) otherwise.
    std::optional<std::vector<NpcRegionBox>> boxes;
};
// 0015da70: per group min / max (vmini.xyz / vmax.xyz from FLT_MAX /
// -FLT_MAX, w 1.0) over its nodes' positions, then min - (2, 2, 2) and max +
// (2, 2, 2) (vadd.xyz). Nothing when built already or there are no groups.
void build_npc_region_boxes(NpcRegions& regions,const NpcNodeGraph& graph);
// 0015d510(region, point, mask, exclude): the group's node (in stored order)
// other than `exclude` with (mask & node +64) != 0 whose squared distance
// ((dx^2 + dy^2) + dz^2, ACC order) is strictly the smallest, starting from
// FLT_MAX; node -1 when none (or region < 0).
struct NpcNearestNode {std::int32_t node{-1};float distance_squared{};};
NpcNearestNode npc_nearest_region_node(const NpcRegions& regions,const NpcNodeGraph& graph,std::int32_t region,
    const RootVector& point,std::uint32_t mask,std::int32_t exclude);
// 0015d790(point, previous): builds the boxes, returns `previous` when its box
// holds the point (no sign flag of point - min nor of max - point, xyz);
// otherwise over all groups in order: the first whose box holds the point,
// and among later ones holding it the one whose nearest Nav|Path (0x88) node
// is strictly closer (0015d510; a group without such a node is skipped). -1
// when no box holds it. f20 is written (the first group's squared distance,
// 0015d958 / 0015d984) only when the first holding group has such a node;
// otherwise the later groups are compared (0015da0c c.olt.s) with the f20
// the caller left in the register (0015d7bc saves it, 0015da50 restores it).
// Checkpoint 132uk3: `caller_f20` is that value for the call sites that host
// it (interpreter goldens tools/ee/gen_region_f20.py):
//   00163c1c (the manager's per-player regions, 00163ad0): the main update's
//     frame delta (00168764 div.s f20; nothing on the chain 003c9420 ->
//     00136a90 -> 003caca0 -> 00164180 -> 00163ad0 writes f20);
//   00153ee0 (mode 9, 00153870): the actor's own speed (00153bd0 / 00153bd8
//     or 00153cc8);
//   0016c15c (0016c030) from 0016cf10: 0016b580's dt (0016b5c8 mov.s f20,
//     f12) until a record reaches 0016d7d4, then max.s(+10, 1.0) (0016d7e0);
//     from 0016ddb0 called at 0016b94c: the score of the last record that
//     reached 0016b7d0 (0016b7d0 / 0016b894 / 0016b8dc), else that dt;
//   001547d4 (mode 7's enter toward a dead object, 00154660) and 00153a5c
//     (mode 9's target that is neither a player nor an AI): the f20 of the
//     message's sender (NpcAiHost::sender_f20); the base update's enter
//     (0016f2d0, from 00159590) leaves its dt (0016f2ec mov.s f20, f12;
//     checkpoint 132uk6d, interpreter goldens tools/ee/gen_mode7_enter_f20.py:
//     003c9420 -> 00151d30 -> 001519a0 -> 00154660 do not write f20).
// Without it (nullopt) such a lookup throws naming `site`.
std::int32_t npc_region_at(NpcRegions& regions,const NpcNodeGraph& graph,const RootVector& point,std::int32_t previous,
    std::optional<float> caller_f20=std::nullopt,const char* site=nullptr);

// The path follower's route buffer (+74, 0x30 bytes; 00160f60 allocates it
// with +24 = the follower's +78 and the node array of +24 words).
struct NpcRouteBuffer {
    // +00: the destination point. 00160f60 does not write it, so until the
    // first route it is the heap block's previous content (nullopt; reading
    // it, by the route step or 00160130, throws).
    std::optional<RootVector> point_00;
    std::int32_t destination_10{-1};        // the destination node (-1: a point)
    // +14 / +18: written by 00160e60 (the route's first and last node on
    // deactivation; -1 / kept without a route); 00160f60 reads +14 >= 0 and
    // rebuilds the route from +14 to +18 (0015f3d0, npc_route_rebuild).
    std::int32_t start_14{-1},end_18{-1};
    std::int32_t capacity_24{};
    bool array_20{};                        // the node array is allocated
    std::vector<std::int32_t> nodes;        // +20 / count +28: the destination first
};

// The requesting actor as a request holds it (+0 object, +4 / +8 its ID).
struct NpcRouteClient {
    std::uint32_t id{};                                       // actor +c
    std::function<bool()> valid;                              // the handle resolves (null: always)
    std::uint8_t local_14{1};                                 // actor +14
    std::function<RootVector()> translation;                  // virtual +f4 row 3
    // 003c9420(path message 004ed9b8, actor +58 component, transport 1):
    // delivered at once to the component's dispatcher.
    std::function<void(const NpcPathEvent&)> deliver;
    // N5c (0015e110): with an AI controller (actor +58 and its virtual +58),
    // 0014bfb0(component, region): the allowed-region bit +bc..+dc (a
    // negative region is allowed). Null: no AI controller (no filtering).
    std::function<bool(std::int32_t region)> allowed_region;
    // N5c: the LOS-check result 005435a8 (its byte +30) to the actor's +58
    // component (00151790 names it the receiver), transport 1.
    std::function<void(std::uint8_t hit)> deliver_los_result;
    // N5c (00162080 state 0, 00162360..00162514): with an actor +58
    // component whose virtual +88 is 1 or 2 (an AI controller), its path
    // follower's +50 / +54. Null: no such component.
    std::function<std::array<std::int32_t,2>()> path_nodes_50_54;
    // Port-only (checkpoint 132ob5): the route search also expands the co-op
    // link overlay (NpcNodeGraph::port_overlay). Set for the --coop-partner
    // only; every original client leaves it clear.
    bool port_overlay_links{};
};

// A request (0x610 bytes). Kind +c 1 = route (node searches, N5c, use other
// kinds). Fields a request does not rewrite keep their previous values.
struct NpcRouteRequest {
    std::shared_ptr<NpcRouteClient> client;                   // +0
    std::uint32_t id_04{},id_08{};
    std::uint8_t kind_0c{},first_0d{},byte_0e{},state_10{},found_11{},node_destination_12{};
    std::shared_ptr<NpcRouteBuffer> buffer_14;
    RootVector destination_20{},position_30{};
    std::int32_t start_40{},previous_44{},region_48{},destination_node_4c{},destination_region_50{};
    std::int32_t pool_base_54{};
    // The open (+58: first +60, end +68, count +74) and closed (+78: first
    // +80, end +88, count +94) lists of this request's search, as pool
    // indices in list order (both sorted by f). Per request: 00164780 and the
    // 00162e10 / 00162e50 scans address them off the request, so a search run
    // directly by npc_route_request leaves a queued request's lists alone.
    std::vector<std::int32_t> open,closed;
    std::int32_t open_count_74{},closed_count_94{};
    NpcNodeSearch search;                                     // +10..+604 of a node search (kind 2)
};
// An entry of the search pool 004ea7b0 (0x20 bytes): +c f = g + h, +10 the
// parent entry, +14 the node, +18 g, +1c 1 open / 2 closed.
struct NpcRouteEntry {
    float f_0c{};std::int32_t parent_10{-1};std::int32_t node_14{};float g_18{};std::uint8_t list_1c{};
};
inline constexpr std::size_t npc_route_request_count=16,npc_route_pool_size=400;
struct NpcLosService;

struct NpcRouteManager {
    const NpcNodeGraph* graph{};
    NpcRegions* regions{};
    // 1644b0: 16 requests, all on the free list +2c in order; the queue +20
    // empty; 004ed9d0 = 0; 004ed9c0..cc = -1; 004ea798 = 0.
    std::array<NpcRouteRequest,npc_route_request_count> requests{};
    std::deque<std::size_t> free_2c,queue_20;
    std::array<NpcRouteEntry,npc_route_pool_size> pool{};
    std::int32_t pool_counter_798{};
    std::uint8_t los_pending_9d0{};
    std::array<std::int32_t,4> player_regions_9c0{-1,-1,-1,-1};
    // The global node search object 004ed9f0: +e (004ed9fe), set to 1 by a
    // finished route search (0 when a search without a start node or to a
    // region sets the object up).
    std::uint8_t search_done_9fe{};
    // N5c: the rest of the global node search object 004ed9f0 (0x610 bytes,
    // the layout of a request; its +e is search_done_9fe above, its +d
    // 004ed9fd the first-call flag of 00161950, also the "LOS check pending"
    // flag of state 2, and its +478 004ede68 the state-2 answer), the region
    // destination's accepted nodes 004ee010 (count) / 004ee014 (64 words),
    // and the far distance 0053834c (the global search's radius, the same
    // global as NpcAiSystem::far_distance: link_npc_node_searches points this
    // at it; null before the link, and a route search's states 0-2 throw).
    NpcRouteRequest global_9f0;
    std::vector<std::int32_t> region_candidates_ee010;
    const float* far_distance_34c{};
    // N5c (npc_node_search.hpp): the LOS manager 004ea0b0 the node searches
    // check their candidates with, and 0014fa00 (the actor of the first
    // thinker whose firing-position / cover node or route destination is the
    // node, by its ID; nullopt: none). Null: a node search throws.
    NpcLosService* los{};
    std::function<std::optional<std::uint32_t>(std::int32_t node)> node_reserved_by;
    // Port-only diagnostic (checkpoint 132uk6e, omega_play --npc-trace): told
    // which site sends a route search's path event 3 (00162080 state 0 / 1,
    // 00162f70, 001631a8, 00163298), and when 00163ad0 or 00163c50 frees a
    // request whose search has started (its pool entries above +54 are not
    // returned: only a finished 00162080 stores +54 back into 004ea798);
    // no behaviour reads it.
    std::function<void(const NpcRouteRequest&,const char* site)> port_route_trace;
    NpcRouteManager(const NpcNodeGraph& g,NpcRegions& r);
};
// 0015efb0(manager, actor, start, previous, region, destination node,
// destination region, point, buffer). Returns 0 without a buffer; 1 after the
// trivial route (start == destination, sent at once) or after queueing; the
// search's result when it searched at once (start >= 0, a destination node,
// another region than the current one). When the actor already has a queued
// route that is not being processed (00163e20, not the queue head) that
// request is updated in place and the original returns an unset stack byte:
// nullopt.
std::optional<std::uint8_t> npc_route_request(NpcRouteManager& m,const std::shared_ptr<NpcRouteClient>& client,std::int32_t start,
    std::int32_t previous,std::int32_t region,std::int32_t destination_node,std::int32_t destination_region,const RootVector* point,
    const std::shared_ptr<NpcRouteBuffer>& buffer);
// 0015fb20(pf, node) / 0015fa70(pf, point, region): start = the hold node, or
// (with +60 bit 1 or bit 2 and no hold) the current and previous nodes;
// region = +5c; the buffer +74.
std::optional<std::uint8_t> npc_route_to_node(NpcRouteManager& m,const NpcPathFollower& pf,const std::shared_ptr<NpcRouteClient>& client,
    std::int32_t node);
std::optional<std::uint8_t> npc_route_to_point(NpcRouteManager& m,const NpcPathFollower& pf,const std::shared_ptr<NpcRouteClient>& client,
    const RootVector& point,std::int32_t region);
// 00162080(request): true when done. The first call (+d) sets the lists up
// and opens the start node (and the previous one, which then clears +40)
// with 00163410: state 3 with a destination node (run to its end in this
// call), else 1. Without a start node: state 2 when the current region +48
// is the destination region +50 and the heights +38 / +28 differ by less
// than 2.6 (an LOS request from the destination raised by 2.6 to the actor,
// answered to the manager, 004ed9d0 = 1), else state 0 (an AI actor's path
// follower +50 / +54 Nav nodes are opened). States 0-2 set the global
// search object 004ed9f0 up (Nav nodes, kind 1, +481 / +482 set, the far
// distance as the radius, around the actor's position in its region).
// State 0: 00161950 on the global object, whose LOS-clear nodes 00163580
// opens; when it ends, path event 3 without an open node, else state 3 (a
// destination node) or 1. State 1: the global object around the
// destination point in the destination region (+ Path nodes with a previous
// node); its LOS-clear nodes are accepted as the destination (004ee014);
// none: path event 3, else state 3. State 2 waits for its LOS answer: clear
// -> the route is the destination alone (path event 2), blocked -> state 0.
// 004ea798 is restored when the search is done.
bool npc_route_search(NpcRouteManager& m,NpcRouteRequest& r);
// 00163ad0 (the manager's frame update, from the world's frame-update
// broadcast before the object loop): with a queued request and no LOS check
// pending, the head request is searched (a route with a buffer and node
// array; a node search through 00161950, npc_node_search.hpp) and removed
// when done (also when its actor is gone or a route has no buffer); then the
// player slot (frame & 3): its +f4 translation's region (0015d790 from the
// slot's previous one) when >= 0.
struct NpcRouteFrameHost {
    std::uint32_t frame{};                                                // 005383c8
    // Checkpoint 132uk3: the caller's f20 at 00163c1c, the main update's frame delta 00168764 (nullopt: not
    // hosted; a player point whose first region has no Nav / Path node then throws).
    std::optional<float> frame_delta_f20;
    std::function<std::optional<RootVector>(std::uint32_t slot)> player_translation;   // 003fb8d0(slot) +f4 row 3
};
void update_npc_route_manager(NpcRouteManager& m,const NpcRouteFrameHost& host);
// The manager's handler 00164180 for the LOS-check result 005435a8 (byte
// +30): with 004ed9d0 = 2 (the checking request was released) 004ed9d0 = 0
// and nothing else; otherwise, with a queued request, 004ed9d0 = 0 and the
// head request takes it: a route (+c = 1) through 00163580 (state 2: the
// answer 004ede68 = clear and 004ed9fd = 0; states 0 / 1: 00161db0 on the
// global object, then its found node is opened (state 0) or accepted as a
// destination (state 1, at most 64) and 004ede68 = -1; other states
// nothing), any other kind through 00161db0. Without a queued request
// nothing.
void npc_route_los_result(NpcRouteManager& m,std::uint8_t hit);
// 00163e20(manager, actor, kinds): the first queued request of this actor
// (by ID) whose kind has a bit of `kinds` (index into requests).
std::optional<std::size_t> npc_route_queued(const NpcRouteManager& m,std::uint32_t actor_id,std::uint8_t kinds);
// 00163c50(manager, actor): every queued request of this actor is freed (an
// LOS check pending for the head: 004ed9d0 1 -> 2).
void npc_route_release(NpcRouteManager& m,std::uint32_t actor_id);
// 00163fd0(manager, actor): the queue head belongs to this actor and its +e
// is clear.
bool npc_route_head_pending(const NpcRouteManager& m,std::uint32_t actor_id);

// 00160f60's tail: the buffer (+24 = +78, +10 = +14 = -1, count 0) and its
// node array when missing; with +14 >= 0 the route rebuild 0015f3d0(the
// route manager 003caa50(004ffb50, 004ed9b0), +14, +18, buffer) (every later
// activation rebuilds again: +14 is not reset). A null manager throws.
void allocate_npc_route_buffer(NpcPathFollower& pf,NpcRouteManager* m=nullptr);
// 0015f3d0(manager, start, end, buffer): the free list's first request
// (001642e0; none: throws, the original writes through null) gets +d = 1,
// +e = 0, +c = 1, a null actor (+0 = 0, +4 = -1, +8 kept), the buffer, start
// +40, +44 = -1, end +4c, +12 = 1, the destination the buffer's point +0 (lq;
// a buffer no route wrote throws), +50 / +48 the end's / start's region +7c,
// +30 the start's position (w 1) and +54 = 0; 00162080 runs it at once (with
// a start and a destination node: state 3 to its end; a found route sends
// nothing to the null actor, a failed one sends path event 3 to +58 of null
// (address 0x58): throws), 001642b0 frees it and the buffer's +10 is
// restored. Returns 00162080's answer.
std::uint8_t npc_route_rebuild(NpcRouteManager& m,std::int32_t start,std::int32_t end,const std::shared_ptr<NpcRouteBuffer>& buffer);
// 00160ce0(pf): leaves the patrol (bit 0, hold dropped), +48 = the route's
// count (minus one when its last two entries are the previous and current
// nodes), +50 = -1, +58 = the destination node, then the route step; bit 3
// = no segment. True when there is path left.
bool start_npc_route(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host);
// 0015fe80(pf, node) (the AI's +84, 0014f3f0, with the hold node's type-7
// link target): without a node array (+74 +20) false. Otherwise the route is
// the one node (count +28 = 1), +00 = its position (0015ddb0 -> 0015c2c0,
// w = 1), +10 = the node, then path event 2 (+28 = -1; message 004ed9b8 by
// 003c9e30) to the actor's +58 component with transport 1 (deliver), true.
// The original reads +74 without a check: a follower without a route buffer
// throws.
bool npc_route_direct_to_node(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,
    const std::function<void(const NpcPathEvent&)>& deliver);
// The AI's virtual +84 (0014f3f0, static) on its follower (+1b0): with a hold
// node (+4c >= 0) whose first type-7 link exists (0015d280), 0015fe80 with the
// link's target (node +78) and, when it returns true, the death code 2
// (dataDeathFallOffLedge); otherwise `code` unchanged.
std::uint8_t npc_death_code_84(NpcPathFollower& pf,const NpcNodeGraph& graph,std::uint8_t code,
    const std::function<void(const NpcPathEvent&)>& deliver);
// 00160a00(pf): see NPC_COMBAT.md section 5.7.
bool step_npc_route(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host);
// 0015ff90(pf, node) (Teleport 0014afe0 with the node's +78): 0015ffd0 then
// 00160c40(pf, node). 0015ffd0: unless bit 3 (path ended) is set or the node
// is +50 or +4c, the first entry of the route's node array (+74 +20, count
// +28) equal to the node gives +48 = its index, +50 = node, the route step
// 00160a00 and bit 3 = no segment; then 00160c40(pf, node). The original
// reads +74 +28 without a check: no route buffer there throws.
void relocate_npc_path_follower(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,const NpcPathHost& host);
// 0015ffd0 alone (one 00160c40 hold; the remote NPC controller 002d5160's relocations).
void relocate_npc_path_follower_15ffd0(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,const NpcPathHost& host);
// 001600a0(pf, node): the first index t of the node in the route's node array (+74 +20, count +28): not found 0, t
// == +48 + 1 1, t > +48 + 1 3, else 2. The original reads +74 without a check: no route buffer throws.
std::uint8_t npc_route_order_1600a0(const NpcPathFollower& pf,std::int32_t node);
// 00160e60(pf) (Deactivate): with a buffer: +14 = the route's last entry (or
// -1 without one), +18 = its first, and the node array is released.
void release_npc_route_buffer(NpcPathFollower& pf);
// 00160130(pf): |the actor's +f4 translation - buffer +0| (VU), 0 without a
// buffer; a buffer no route was written to throws (heap content).
float npc_route_distance(const NpcPathFollower& pf,const RootVector& actor_translation);
// 00160130 on a route buffer no route has written (+28 count 0): the point
// +0 is the heap block's earlier content. Measured in PCSX2 (2026-10-12,
// NPC_COMBAT.md section 5.6) for TORONTO1 records 12 and 9 at their alert
// after activation: x, y and z held zeros or pointer-sized denormals (VU
// zero), so D = |the actor's translation| (bit-exact: 431d87c6 for record
// 12). This is that model, the point taken as the origin; it is measured
// only for actors loaded with the level (a generator-spawned actor's block
// held arbitrary floats, different between runs).
float npc_unwritten_route_distance(const RootVector& actor_translation);
}
