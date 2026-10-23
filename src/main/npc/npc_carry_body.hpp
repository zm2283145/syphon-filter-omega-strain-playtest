#pragma once
// Checkpoint 132uk5: AI mode 8 INTERNAL_CarryBody, the handler 00153150 of
// the mode jump table 0049b6a0 (0x478 bytes), entered by the script's
// CarryBodyToNode(NPC, GOBJ, Node) 0014a520 -> virtual +108 = 0014bd60
// (npc_carry_body_to_node, npc_script_control.hpp: +f0 = the node, +f4 = the
// body's ID). UKRAINE's SaveHerStone: Stone carries Haddad.
// Interpreter goldens: tests/npc_carry_body_goldens.hpp
// (tools/ee/gen_npc_carry_body.py).
//
// +ec is the carry state: -1 entered, 0 walking to the body, 1 picking it
// up, 2 carrying it to the node.
// AI message (004ee550) event 0 (enter): +e8 = 3, +ec = -1, Walk (virtual
// +148(1) = 0.1) at priority 10 (+64 < 11: +60, +64 = 10, +141 bit 6
// clear); 1. Event 1 (exit): +64 = 10 when below 11; 1. Event 3 (the +e8
// timer): with +ec == 1: +ec = 2, r = 0015fb20(+1b0, +f0) (to the node),
// +e8 = -1. Otherwise the body 0040c540(&+f4); none: r = 0. Else +e8 = 3,
// r = 1, and when the body's virtual +108 is 3: its region and node from
// its +58 controller: a player (virtual +54) the region 004ed9c0[003cc950]
// and no node; otherwise (+58 virtual +58 AI, or +5c) its path follower
// +1b0 +5c and the node of +18c (0014af90: the hold node +4c, none when
// negative) +78; the body's skeleton node *0049d024 (14) world matrix
// (00132a30, 00133ed0) row 3 is the point; +ec = 0; a node: 0015fb20(+1b0,
// node) (its byte not read, r stays 1), else r = 0015fa70(+1b0, point,
// region); +e8 = -1. r == 0: ResumeAIMode (virtual +e0 = 0014b570). The
// result is r. Other AI events: 0.
// Path message (004ed9b8) event 6 (arrived): +ec 0: +ec = 1, +e8 = 1 and
// the actor action 0x2b (pick up the body) kind 5, +2c = +f4 (003c9e30
// with 0055d480, vtable 004e01a0, +20 = 1) to the actor with transport 0;
// +ec 2: the action 0x2c (lay the body down) kind 5, +2c = +f4, then
// ResumeAIMode. Events 1 / 3 (no route) with +ec 0 or 2: ResumeAIMode.
// Every path event returns 0; any other message: 0.
// A +58 null controller (the original calls its virtual +58 through null)
// throws, as does a body the host has no view of.
#include "npc_ai.hpp"
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {
struct NpcCarryHost {
    // An AI body's path follower +4c, 0014af90's node index (+18c, +78); -1: none.
    std::function<std::int32_t(std::uint64_t body)> hold_4c;
    // 00133ed0(00132a30(body), node *0049d024 = 14): the world matrix row 3.
    std::function<RootVector(std::uint64_t body)> head_row3;
    // 0015fb20(+1b0, node) / 0015fa70(+1b0, point, region); nullopt: the
    // original returns an unset stack byte (npc_route_to_node / _point).
    std::function<std::optional<std::uint8_t>(std::int32_t node)> route_to_node;
    std::function<std::optional<std::uint8_t>(const RootVector& point,std::int32_t region)> route_to_point;
};
// The actor actions of mode 8 (kind 5, +2c the body's ID).
inline constexpr std::int32_t npc_carry_pickup_action=0x2b,npc_carry_drop_action=0x2c;
// 00153150 (see above). Needs NpcAiHost::carry, object_by_id and sense.
bool npc_carry_body_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
}
