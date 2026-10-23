#pragma once
// The AI's node searches (NPC_COMBAT.md section 5.4, milestone N5c): the
// request 0015ebc0 with the cached lookup 0015f780, the manager's step
// 00161950 with the candidate collection 0015e110 (regions 0015e830, the
// score 0015ddf0, the reservations 0014fa00), the LOS checks 00161bc0 through
// the LOS manager's deferred requests (npc_los_service.hpp) and their results
// 00161db0 (00162010 / 00161fa0), and the start region 001612d0. A request
// lives in the route manager's queue (npc_route.hpp, kind +c = 2) and is
// answered by path event 0 (node, second node) or 1 (none) to the requesting
// actor's component, transport 1.
#include "npc_ai.hpp"
#include "npc_los_service.hpp"
#include "npc_route.hpp"
#include <cstdint>
#include <memory>
#include <optional>

namespace omega {
// 0015e0f0(out, near, far, byte): the query (+430, +434, +438).
struct NpcNodeSearchQuery {float near_0{},far_4{};std::uint8_t byte_8{};};

// 0015ebc0(manager, actor, radius, query, self position, target position,
// start region, type mask, kind, excluded node). Start region < 0: path
// event 1 at once, returns 0. The actor's queued node search (00163e20 kind
// 2, the head included) is rewritten in place (+430..+470, +480..+482 = 0),
// returns 0. A finished request of this actor in the free list with the same
// mask, positions (all four lanes, VU zero flags) and query byte and a query
// without NaN (0015f780) answers at once (path event 0 with its +478 / +47c,
// or 1 when +478 < 0), returns 1. Otherwise the free list's first request
// (+c = 2, +d = 1, +e = 0, +0 / +4 / +8 the actor) is filled and queued,
// returns 1.
std::uint8_t npc_node_search_request(NpcRouteManager& m,const std::shared_ptr<NpcRouteClient>& client,float radius,
    const NpcNodeSearchQuery& query,const RootVector& self,const RootVector& target,std::int32_t start_region,std::uint32_t mask,
    std::int8_t kind,std::int32_t exclude);
// 00161950(request): see NPC_COMBAT.md section 5.4. True when done (the
// manager then frees the request).
bool npc_node_search_step(NpcRouteManager& m,NpcRouteRequest& r);
// 00161db0(request, result).
void npc_node_search_los_result(NpcRouteManager& m,NpcRouteRequest& r,std::uint8_t hit);
// 0015ddf0(query, node, a2, self, target): the candidate score (smaller is
// better; 10000 for a node on the far side of the target).
float npc_node_search_score(const NpcNodeSearchQuery& query,const RootVector& node_position,std::int32_t a2,const RootVector& self,
    const RootVector& target);
// 001612d0(pf): the follower's region +5c; without one, for a patrolling
// follower, the first nonzero region (node +7c) along the patrol path from
// the hold node (0015d680), stopping at the hold node again or at a node
// without a next one; else -1.
std::int32_t npc_search_start_region(const NpcPathFollower& pf,const NpcNodeGraph& graph);
// 0014fa00(node): the actor (ID) of the first think-list entry whose
// component is an AI (virtual +88 = 1, every native thinker), whose actor is
// not dead (life 3) and whose +e4, +e0 or path follower +58 is the node.
std::optional<std::uint32_t> npc_node_reserved_by(const NpcAiSystem& system,std::int32_t node);
// The play host's links: the manager's LOS manager, reservations and far
// distance 0053834c (the system's, the route searches' global radius).
void link_npc_node_searches(NpcRouteManager& m,NpcAiSystem& system,NpcLosService& los);
}
