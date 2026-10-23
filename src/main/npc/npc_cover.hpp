#pragma once
// Cover and firing positions of the AI component (NPC_COMBAT.md sections 3,
// 4 and 5.4/5.5, milestone N5c): the firing-position search 0014fec0, the
// cover search 00150150, the approach search 0014fd30 (node searches through
// npc_node_search.hpp), the LOS-check request 00151790 (a deferred request to
// the LOS manager answered to the component), mode 3 AttackFromCover
// 00155ba0, the dispatcher's path event 0 (the search result) and LOS-check
// result 005435a8, and the allowed-region virtuals +1f4 / +1f8 / +1fc
// (0014bea0 / 0014bf60 / 0014bf00). Single player.
#include "npc_ai.hpp"
#include <cstdint>

namespace omega {
// The inline allowed-region test of 0014fec0, 00150150 and the dispatcher:
// word (region >> 5, rounded toward zero) of +bc..+dc, bit region & 31; a
// node without a region (-1) tests bit 31 of the first word.
bool npc_region_bit(const NpcAiComponent& c,std::int32_t region);
// 0014fec0(c): a cached firing position +e0 whose region is allowed: routed
// to (0015fb20) unless this actor's request is the pending queue head
// (00163fd0), +141 bit 0 = 1, +13f = 3, returns 1. Otherwise, unless the
// queue head is pending, the node search (query (+70, +74, 1), the actor's
// +f4 translation, the perception's last known position +00, radius 15 +
// max(distance, +74), start region 001612d0, Action nodes 0x40, kind 1,
// nothing excluded) and +13f = 3; returns its result (0 when pending).
std::uint8_t npc_firing_position_search(NpcAiComponent& c,NpcAiHost& host);
// 00150150(c): a cached cover node +e4 whose region is allowed: the LOS check
// 00151790 and +13f = 10, returns 1. Otherwise as 0014fec0 with
// StandCover|CrouchCover (6), kind 2, +13f = 2.
std::uint8_t npc_cover_search(NpcAiComponent& c,NpcAiHost& host);
// 0014fd30(c): unless the queue head is pending, the node search with the
// query byte 0, Nav nodes (0x80), kind 1, and +13f = 4.
std::uint8_t npc_approach_search(NpcAiComponent& c,NpcAiHost& host);
// 00151790(c): from the perception's last known position +00 to the route
// end (the route buffer's point, raised 4.5, lowered 3.0 when the follower's
// +58 node is CrouchCover), a deferred LOS request (mask 4, mode 2) answered
// to the component; nothing for a zero displacement (returns 0); accepted ->
// +68 bit 0. A missing route buffer or one no route was written to throws
// (the original reads address 0 / heap content).
std::uint8_t npc_los_check_request(NpcAiComponent& c,NpcAiHost& host);
// Mode 3 AttackFromCover (00155ba0) through the mode dispatch; always returns
// 0 (not handled). See NPC_COMBAT.md section 3. Its reload action 0x3c and the
// target's weapon rating need NpcAttackHost::reload_allowed /
// weapon_rating_of; LookAround (+134) throws (its look modes are not native).
bool npc_attack_from_cover_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// Mode 2 TakeCover (001564c0) through the mode dispatch (NPC_COMBAT.md
// section 3). AI event 2 (the tick, handled): on a moving route whose current
// node +50 is the follower's +58 node, within 3.0 of the route end the fire
// state 0 (virtual +120), else at the speed class's distance (table 00489e20
// {20000, 13, 13, 18.5}, remaining +40 in (T, T + 1)) with actor +320c != 2
// and the follower's target +00 in sight (0016c260) the dive (0014d900 2,
// after a crouch 1 at a CrouchCover node with +141 bit 0 clear); then unless
// visible the weapon switch (as AttackFromCover's, through virtual +1ac =
// NpcAttackHost::select_item) or, with +ec = 0, +ec = 1 when a reload is due
// (else 2); +ec = 1 and 00143560 -> the reload action 0x3c and +ec = 2.
// Events 3 (handled): +e8 = 30; stopped: armed and ready -> the
// firing-position search and +110/+112 = 2/3; visible -> +e4 = +e0 = -1.
// 4 (handled): visible -> fire state 1. 5: +e8 = 40. 6: +28 != 0, visible and
// moving -> stop (0014d510(0, 20)), +e8 = 30. 0 (handled): the cover search
// unless +13f / +140 is 2, +e8 = moving ? -1 : 40, +ec = 0. 1 (handled): fire
// state 1. Path events (not handled): 5 the LOS check 00151790; 6 a crouch at
// a CrouchCover node (+50 = 0, +141 bit 0 clear), fire state 1, +e8 = 40; 1
// with +112 = 2 -> mode 4; 2 with +110 = 2 -> mode +112; 3 +e8 = 30.
bool npc_take_cover_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// The dispatcher's path event 0 (00152788..00152870), when the mode did not
// handle it: nothing for a node < 0; +13f = 2 -> +e4 = node, +e0 = the second
// node (+2c), else the reverse; then, unless the queue head is pending, the
// route to the node (0015fb20) when its region is allowed; +141 bit 0 = 0.
void npc_dispatch_search_result(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// The dispatcher's LOS-check result (001528e0..00152a70, a living actor):
// +13f = 10 (a cached cover node being checked): blocked and +e4 >= 0 ->
// unless the queue head is pending, the route to +e4 when its region is
// allowed, +141 bit 0 = 0, +13f = 2; otherwise +e4 = -1 and a new cover
// search. Other purposes: when +141 bit 0 differs from "clear" it takes it
// and AI event 6 (+28 = clear) goes to the component (transport 0). Then
// +68 bit 0 = 0.
void npc_dispatch_los_result(NpcAiComponent& c,std::uint8_t hit,NpcAiHost& host);
// +1f4 = 0014bea0(c, node): every allowed-region bit cleared, then with a
// node +1f8(node). +1f8 = 0014bf60(c, node): the node's region bit set.
// +1fc = 0014bf00(c, node): the node's region bit cleared.
void npc_clear_allowed_regions(NpcAiComponent& c,const NpcNodeGraph& graph,std::int32_t node);
void npc_allow_region(NpcAiComponent& c,const NpcNodeGraph& graph,std::int32_t node);
void npc_forbid_region(NpcAiComponent& c,const NpcNodeGraph& graph,std::int32_t node);
}
