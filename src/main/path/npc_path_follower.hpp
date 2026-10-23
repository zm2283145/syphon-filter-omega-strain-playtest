#pragma once
// The AI path follower (component +1b0, 0x80 bytes; NPC_SPAWN_AND_AI.md,
// "Path follower") over the NOD node collection 004ffbe0: the constructor
// 00161840, the start 00160f60, the arrival input 00161370, the per-frame
// update 00160220 with the patrol next-node rule 0015d680, the segment set-up
// 00160990 -> 00160720, the hold 00160c40 and the path-height helpers
// 0015fce0 / 0015fdc0. Route following (the route buffer +74, the route step
// 00160a00, 0015fb20) is in npc_route.hpp (N5a). Path events go to the
// actor's +58 component with transport 0 (queued); the host records them in
// order.
#include "animation_root.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace omega {
// A NOD node (0015c390): position +80 (0015c2c0 returns it with w = 1), type
// bits +64 (Path 0x8, Nav 0x80), links +68 (target index, link type) in stored
// order, the record parameter +7c, and the eligibility of the object node +60
// resolves to (null when the node has none; read by the locomotion producer).
struct NpcNode {
    std::array<float,3> position{};
    std::uint32_t flags{};
    struct Link {std::int32_t target{},type{};};
    std::vector<Link> links;
    std::int32_t parameter_7c{-1};
    enum class Association : std::uint8_t {none,eligible,ineligible,unknown};
    Association association{Association::none};
    // The object 0040c480 resolves (record version >= 0x40, parent index >= 0):
    // (the node record's category << 16) | its parent index (0015c474..0015c490).
    std::uint32_t association_key{};
    std::uint32_t id{};                                          // the node object's +c (the population record's ID)
    float yaw_8c{};                                              // +8c (population_node_yaw; 0 for overlay nodes)
};
inline constexpr std::uint32_t npc_node_path=0x8,npc_node_nav=0x80;
// The node objects (vtable 004d95b0, class word *004ea770) as host object
// handles: 0xe0000000 + the node index (labelled native numbering; actors
// use 0x100000 + index, backpacks 0xb0000000, terrain 0xc0000001, throws
// 0xd0000000 upwards).
inline constexpr std::uint64_t npc_node_object_base=0xe0000000ULL;
inline std::uint64_t npc_node_object(std::int32_t index){return npc_node_object_base+static_cast<std::uint32_t>(index);}
struct NpcNodeGraph {
    std::vector<NpcNode> nodes;
    // Port-only (checkpoint 132ob5, --coop-partner; coop_partner.hpp): the
    // co-op link overlay's entry links, by node index (empty: none). They
    // lead from NOD nodes to the overlay nodes appended past the NOD's own
    // (which no region group lists); only a route client with
    // port_overlay_links (npc_route.hpp) searches them, and npc_link_type
    // reads them after the node's own links. No other reader sees them.
    std::vector<std::vector<NpcNode::Link>> port_overlay;
    // 0015ddb0 (no range check in the original; out of range throws here).
    const NpcNode& node(std::int32_t index) const;
    RootVector position(std::int32_t index) const;               // 0015c2c0
    // The node index of a node object handle within this collection, or nullopt.
    std::optional<std::int32_t> node_of_object(std::uint64_t object) const;
};
// 0015d680: the first link (stored order) whose target has the Path bit, or -1.
std::int32_t npc_next_patrol_node(const NpcNodeGraph& graph,std::int32_t index);
// 0015d3f0: the type of the first link of `from` that targets `to`, or -1.
std::int32_t npc_link_type(const NpcNodeGraph& graph,std::int32_t from,std::int32_t to);
// 0015d280(node, type) (static): the target of the first link (stored order)
// of `from` whose type is `type` (the original returns the node table entry
// *(004ffbe0 +8)[target], non-null for every node), or -1.
std::int32_t npc_first_link_of_type(const NpcNodeGraph& graph,std::int32_t from,std::int32_t type);

struct NpcRouteBuffer;
struct NpcRouteManager;
struct NpcPathFollower {
    RootVector target_00{};          // current target point
    RootVector direction_10{};       // unit segment direction (w kept from the difference)
    RootVector saved_20{};           // saved point (hold node position)
    float remaining_40{},segment_44{};
    std::int32_t index_48{};         // route index (counts down; not written by the constructor)
    std::int32_t hold_4c{-1},current_50{-1},previous_54{-1},field_58{-1},destination_5c{-1};
    // +60: bit 0 patrolling, 1 current node has the Path bit, 2 off support
    // (written by the locomotion producer), 3 path ended, 4 arrival edge, 5
    // arrival latch, 6 not yet started; bit 7 is kept.
    std::uint8_t flags_60{};
    // +74: the route buffer (npc_route.hpp), null until 00160f60 allocates it.
    std::shared_ptr<NpcRouteBuffer> route_74;
    std::int32_t capacity_78{};      // the constructor's 100 (route buffer size)
};
// 00161840(pf, actor, capacity, hold): +4c = hold, +78 = capacity, +74 = 0,
// +50/+54/+58/+5c = -1, flags bits 0..2 clear and 3..6 set (bit 7 kept),
// +10 = 0. +00, +20, +40, +44 and +48 are left as they were.
void construct_npc_path_follower(NpcPathFollower& pf,std::int32_t capacity,std::int32_t hold);
// Path message 004ed9b8 (vtable 004d9760): +24 event, +28 value, +2c the
// second node of a node-search result (path event 0, N5c; not written by
// the other senders: 0 here).
struct NpcPathEvent {std::uint8_t event{};std::int32_t value{};std::int32_t second_2c{};};
struct NpcPathHost {
    // Actor virtual +f4 (00132280): the 'flor' root world translation.
    std::function<RootVector()> actor_translation;
    // 003c9420 to the actor's +58 component with transport 0.
    std::function<void(const NpcPathEvent&)> send;
};
// 00160f60 (Activate): with bit 6, clear it; with a hold node, hold it
// (00160c40) and target its position; from a Path node set bits 0/1, take the
// next patrol node, set up the segment, bit 3 = no segment, bit 4 clear, and
// send event 4 (value -1) when the segment exists; then +5c from node +7c
// when >= 0. Without a hold node the target is the actor's translation. On
// every path it then allocates the route buffer +74 and its node array when
// missing (allocate_npc_route_buffer, npc_route.hpp), which rebuilds a route
// kept over a deactivation with the route manager (0015f3d0; null: throws).
void start_npc_path_follower(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host,NpcRouteManager* routes=nullptr);
// 00161370: bit 4 = rising edge of the navigation arrival byte, bit 5 latch.
void npc_path_arrival_input(NpcPathFollower& pf,std::uint8_t arrived);
// 00160220. With bit 3 nothing. Else +40 = distance to the target; the hold
// is released (+54 = hold, hold = -1) beyond squared distance 4.0; on the
// arrival edge while patrolling: next patrol node -> event 5 (current), new
// segment (event 7 from 00160720), bit 3 = no segment, +5c = node +7c; no
// next node -> hold the current node, bit 3, event 6, and drop the hold unless
// the node has the Nav bit. Not patrolling: the route step 00160a00
// (step_npc_route, npc_route.hpp), bit 3 = no segment. The multiplayer
// branch (005721c8) is not hosted.
void update_npc_path_follower(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host);
// 00160c40: +4c = node; with node >= 0: +20 = its position, +5c = its +7c
// when >= 0, bit 1 = its Path bit.
void hold_npc_path_node(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node);
// 00160990 / 00160720: the segment to `node` (from the current node's
// position, or the actor's translation without one); event 7 with the link
// type when the current node changes and a hold exists; +0 = point and true
// only when the segment is longer than 2.0.
bool set_npc_path_segment(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,const NpcPathHost& host);
bool set_npc_path_segment_to(NpcPathFollower& pf,const NpcNodeGraph& graph,const RootVector& point,std::int32_t node,const NpcPathHost& host);
// 0015fce0(pf, a1, a2): the path height the locomotion producer compares
// (a1: its +158 contact byte before the frame; a2: actor +3204 bit 7 with
// eligible node associations). With a hold and a2 clear: the hold node's z.
// With +54 and +50: min(+08, +28), or, with a1 and a2 clear and +40 <=
// 1 + +44, (+28 * +40 + +08 * (+44 - +40)) / +44. Otherwise +28.
float npc_path_height(const NpcPathFollower& pf,const NpcNodeGraph& graph,bool a1,bool a2);
// 0015fdc0: +08 = +28 = z.
void set_npc_path_height(NpcPathFollower& pf,float z);
// 001601e0: the node of the hold (or of +54 without one), or nullopt.
std::optional<std::int32_t> npc_path_reference_node(const NpcPathFollower& pf);
}
