#pragma once
// Checkpoint 132fx1: AI mode 6 Flee, the handler 001548f0 of the mode jump
// table 0049b6a0 (cNPC::Flee, npc.cc:3129; 0x4e0 bytes, unmatched in the
// decomp: ported from its asm).
//
// The node search of this mode: unless the route manager's head request is
// this actor's (00163fd0), 0015ee00(manager, actor, query 0015e0f0(0, near
// 0.0, far -10000.0), self = actor virtual +f4 row 3, radius 10000.0, target
// = the perception's last known point +1ac +00, start 001612d0(+1b0), mask 6,
// kind 2, exclude -1) and +13f = 2 (the cover search's result path).
// AI message (004ee550) event 0 (enter): +e8 = 60, the weapon-draw request
// virtual +bc(0, 20), +e0 = +e4 = -1, the search; 1. Event 1 (exit): virtual
// +bc(1, 20); 0. Event 3 (the +e8 timer): standing (virtual +a4 0014d3b0:
// +13c == 0) with the target visible (perception +20 bits 0 and 2): the
// search; then +e8 = 60; 0. Other AI events: 0.
// Path message (004ed9b8) event 7 (the next link's action) with +28 == 10:
// the perception's target +40 (valid while its +c is +44) that IsEnemy
// (virtual +70) and is not visible: the look request virtual +128(target,
// priority 0, 1.0); 0. Event 6 (arrived): +e8 = 1; 0. Anything else: 0.
#include "npc_ai.hpp"

namespace omega {
bool npc_flee_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
}
