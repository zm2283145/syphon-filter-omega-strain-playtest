#pragma once
// Mode 5 Search (00154dd0) and the look-around it and LookAround (0014dd50,
// virtual +134) run (NPC_COMBAT.md sections 3 and 5.9, milestone N5c). Static: read
// from the disassembly, no interpreter goldens.
//
// A look-around fills the list of the component's look slot (+1b8, the
// shared slots 004ea1d0) with node objects, then the look-at selection
// 0014e0c0 (npc_attack.hpp) points look entry +164 at one listed node after
// another while +166 is set. Node objects are the hosts' handles
// npc_node_object(index) (npc_path_follower.hpp).
#include "npc_ai.hpp"
#include <cstdint>
#include <optional>

namespace omega {

// 0014f2e0(c, index, nearest, priority): with priority >= +160 the slot's
// list is emptied (when +1b8 is set), +15c = 0, +164 = index, +165 = nearest,
// +166 = 0, +160 = priority, +168 = 0.
void npc_look_begin(NpcAiComponent& c,std::uint8_t index,std::uint8_t nearest,std::int32_t priority,NpcAiHost& host);
// 0014f240(c, node object, priority, duration): with priority >= +160 the
// point {node, its ID, duration, count 0, unused} is appended to the slot's
// list (when +1b8 is set) and +166 = 1.
void npc_look_add_point(NpcAiComponent& c,std::int32_t node,std::int32_t priority,float duration,NpcAiHost& host);
// 0014ef80(c, region, node, exclude, duration): node >= 0: every link target
// of the node (stored order) other than exclude whose region +7c is region
// (any for region < 0); node < 0 and region >= 0: every node of the region
// group (004ffbe8). Each through 0014f240 at the current +160.
void npc_look_add_nodes(NpcAiComponent& c,std::int32_t region,std::int32_t node,std::int32_t exclude,float duration,NpcAiHost& host);
// 0014ee90(c): the first point (list order) with the largest count above 0,
// as its node when it resolves; nullopt for none.
std::optional<std::int32_t> npc_look_facing_node(NpcAiComponent& c,NpcAiHost& host);
// StopMovement (virtual +154 = 0014d4d0): 0014d510(c, 0, priority), +7c =
// duration, +168 = 0.
void npc_stop_movement(NpcAiComponent& c,std::int32_t priority,float duration,NpcAiHost& host);
// LookAround (virtual +134 = 0014dd50, priority): for a present actor,
// 0014f2e0(c, 0, 1, priority), then with the hold node pf+4c the links of
// that node, else the nodes of the destination region pf+5c, excluding the
// previous node pf+54 while moving (+13c == 1), for 1.0 each.
void npc_look_around(NpcAiComponent& c,std::int32_t priority,NpcAiHost& host);
// StopAndLookAround (virtual +138 = 0014dca0, priority; 132sx10): for a
// present actor (+2d, +2c, !+2e), +13c read first (lb), then +134(c,
// priority), +154(c, priority, 10000.0) and, when the read +13c was
// nonzero, +168 = 1.
void npc_stop_and_look_around(NpcAiComponent& c,std::int32_t priority,NpcAiHost& host);
// 0014e0f8..0014e5e4, the look-at selection's look-around branch (+166 set):
// +15c -= dt; when it ran out, or entry +164 is inactive or below +160, the
// next unused point is picked (the nearest with +165, else the first) among
// those farther than 3.0 from the eye, with a normalised z below 0.9 and not
// in sight (0016c260 with the head, stealth 0); the others are marked used
// and count against the best so far. A pick takes entry +164 (when its
// priority is at most +160) for +15c = its duration and sets +167; no pick
// releases entry +164 and ends the look-around (+166, +160 = 0, ResumeMovement
// with +168). Entry 2 (+194..) is not modelled: throws.
void npc_look_around_select(NpcAiComponent& c,float dt,NpcAiHost& host);
// Mode 5 Search (00154dd0) through the mode dispatch; returns "handled" (only
// AI event 0).
bool npc_search_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
}
