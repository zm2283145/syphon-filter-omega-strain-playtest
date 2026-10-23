#pragma once
// Script-driven movement of the AI component (NPC_COMBAT.md sections 3 and
// 5.7, milestone N5a): MoveToNode (script 0014a5c0 -> virtual +100 =
// 0014be50), the three-argument SetMode (script 0014a4a0 -> +dc = 0014bcf0
// with a node), mode 7 INTERNAL_ScriptControl (00154660), the dispatcher's
// path event 2 (route ready, 00152108..0015215c) and Activate's replay of a
// pending MoveToNode (00157f88..00157fbc). Routes go through the route
// manager (npc_route.hpp) the host links in NpcAiHost::routes.
#include "npc_ai.hpp"
#include "npc_route.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {
// 0014be50(c, node): mode 7 at priority 100 (0016e170), then +138 = the
// node's index and +f4 = -1 whether or not the request was taken. Returns 1.
std::uint8_t npc_move_to_node(NpcAiComponent& c,std::int32_t node);
// 0014be10(c, object), the go-to-object virtual +104 (Checkpoint 132ob8):
// +f4 = the object's ID (+c), +138 = -1, then mode 7 at priority 100
// (0016e170). Returns 1.
std::uint8_t npc_move_to_object(NpcAiComponent& c,std::int32_t object_id);
// 0014bd60(c, body, node), the carry-body virtual +108 (Checkpoint 132zf9,
// BELARUS1 CarryBodyToNode 0014a520): when the body's virtual +108 & 0xff is
// 2 or 3, its +58 controller is set and the mode +44 is not 8: mode 8 at
// priority 100 (0016e170), +f0 = the node's +78 (its index) and +f4 = the
// body's +c (ID). Returns 0.
std::uint8_t npc_carry_body_to_node(NpcAiComponent& c,std::uint8_t body_life,bool body_controller,std::int32_t node,
    std::int32_t body_id);
// Checkpoint 132zf11: Follow(NPC, GOBJ, float, float) 00148880 -> virtual
// +fc = 001488f0(c, target, f12, f13). With a target: +f4 = its +c, +100 =
// f12, +104 = f13, +108 = +10c = 0, then mode 9 at priority 100 (0016e170);
// a null target does nothing. The multiplayer send 002d7290 (005721c8) is
// not taken. Mode 9's handler 00153870 is npc_follow_mode (npc_follow.hpp).
void npc_follow(NpcAiComponent& c,std::optional<std::int32_t> target_id,float a,float b);
// 0014bcf0(c, mode, node) with a node (the three-argument script SetMode):
// for mode & 0xff below 7 a request at priority 100, +13f = the mode and the
// route to the node (0015fb20); modes 7 and above do nothing.
void npc_set_mode_at_node(NpcAiComponent& c,std::uint32_t mode,std::int32_t node,NpcAiHost& host);
// 00154660 (mode 7) through the mode dispatch. AI event 0 (enter): the
// actor's queued requests are released (00163c50); with +138 >= 0 the route
// to it (0015fb20); otherwise with an object +f4 (0014be10) resolved by
// 0040c540 (NpcAiHost::object_by_id): a live one (virtual +108 != 3, read
// through NpcAiHost::sense) or none skips to the Jog speed with no route; a
// dead one (virtual +108 & 0xff == 3, go to a body; Checkpoint 132ms4,
// 00154728..00154808) routes toward it: the region is, by its +58
// controller, a player's (+58 virtual +54) 004ed9c0[003cc950], else an AI's
// (+58 virtual +58) path follower (+58 +1b0) +5c, else (no +58, or neither)
// 0015d790(the body's virtual +e8 row 3, this actor's +1b0 +5c); then
// 0015fa70(+1b0, the body's virtual +f4 row 3, region) (its byte not read)
// and +e8 = -1; then the Jog
// speed (virtual +148(2)) when the speed priority +64 is at most 10 (+60,
// +64 = 10, +141 bit 6 clear). AI event 1 (exit): +64 = 10 when at most 10.
// Both return "handled". Path event 2: ResumeMovement (+158), not handled.
// Everything else: not handled.
// Interpreter goldens: tests/npc_mode7_body_goldens.hpp
// (tools/ee/gen_npc_mode7_body.py).
struct NpcGoToBodyHost {
    // Virtual +f4 (the 'flor' root world matrix) of the body; 00154800 reads row 3.
    std::function<std::array<RootVector,4>(std::uint64_t object)> world_f4;
    // 0015d790(point, previous) at 001547d4. 00154660 does not write f20, so
    // the lookup compares later groups with the f20 of the message's sender
    // (npc_region_at's caller_f20): NpcAiHost::sender_f20, the base update's
    // dt for its mode enter (0016f2ec; checkpoint 132uk6d, interpreter
    // goldens tests/npc_mode7_enter_f20_goldens.hpp); nullopt: such a
    // lookup throws.
    std::function<std::int32_t(const RootVector& point,std::int32_t previous,std::optional<float> caller_f20)> region_at;
    // 0015fa70(+1b0, point, region); its byte is not read.
    std::function<std::optional<std::uint8_t>(const RootVector& point,std::int32_t region)> route_to_point;
};
bool npc_script_control_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// The dispatcher's path event 2 when the mode did not handle it: +140 = +13f,
// +13f = 0, and when the route start 00160ce0 leaves path: movement state 1
// at priority 20 (0014d510) and the travel speed for the purpose +140
// (virtual +d4, 0014d730).
void npc_route_ready(NpcAiComponent& c,NpcAiHost& host);
// The route manager and this NPC's request handle from the host (throws
// without them).
NpcRouteManager& npc_route_manager(NpcAiHost& host);
}
