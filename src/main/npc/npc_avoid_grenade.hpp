#pragma once
// Checkpoint 132fx1: AI mode 10 AvoidGrenade, the handler 001535e0 of the
// mode jump table 0049b6a0 (cNPC::AvoidGrenade, npc.cc:3485; 0x27c bytes,
// unmatched in the decomp: ported from its asm). Requested at priority 50 by
// the hearing of a landed projectile (00152fd0 cause 5, npc_alert.hpp
// npc_hear), which stores the projectile's ID at +f4.
//
// AI message (004ee550) event 0 (enter): the projectile 0040c540(&+f4); when
// it exists, its virtual +bc point (NpcProjectileView::point_bc) and, unless
// the route manager's head request is this actor's (00163fd0), the node
// search 0015ee00(manager, actor, query 0015e0f0(0, near 0.0, far -10000.0),
// self = actor virtual +f4 row 3, radius 10000.0, target = that point,
// start 001612d0(+1b0), mask 6, kind 2, exclude -1) and +13f = 2 (the cover
// search's result path). Then, existing or not, Run (virtual +148(3)) at
// priority 10 (+64 < 11: +60, +64 = 10, +141 bit 6 clear), +e8 = 4; 1.
// Event 1 (exit): +64 = 10 when below 11; 1. Event 3 (the +e8 timer): the
// projectile 0040c540(&+f4) present (+2d, +2c, !+2e) keeps the mode; gone or
// not present: +4c = 0 and the mode request 0016e170(+44, 1, 0) (Alert);
// either way +e8 = 4 and 0. Other AI events and other messages: 0.
#include "npc_ai.hpp"

namespace omega {
bool npc_avoid_grenade_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
}
