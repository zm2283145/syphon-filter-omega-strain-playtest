#pragma once
// The world line-of-sight service (the LOS manager 004ea0b0, virtual +0c =
// 00138a10 -> 0013a480) as the native port hosts it over a SoloCollisionWorld
// (research/LOS_QUERY_NATIVE.md, COMBAT.md "Checkpoint 54").
//
// 0013a480 (static) answers a request with mode byte +5c = 0 at once: it
// copies origin +30 / displacement +40, uses the caller's result storage +2c,
// sets up the provider (*004ea0b8), clears its exclusions and adds the
// request's objects +50 / +54 (at most two; they gate only an object's direct
// geometry), then runs 003c26d0 with the required mask 0, the excluded mask =
// the low byte of +58, no water list and the stack controls (0, 1) that select
// the executor 003c37a0; a zero displacement returns without a hit. A nonzero
// +5c queues the request (not hosted, throws), and a nonzero +24 posts the
// result to that receiver (not hosted, throws).
//
// The native host differs from the original in the gathering only: the
// candidates are the world's COL triangles whose bounds meet the segment's box
// (SoloCollisionWorld::expand, load order) instead of the provider's region
// collection (0x002245f0) and object traversal (0x003c4f00) in their order;
// the triangle test and the masks are the native kernel trace_los_triangles
// (003c37a0 / 003c45b0). Exactly equal hit distances can therefore resolve to
// a different triangle. The request's objects +50 / +54 exclude the resources
// whose game object they are (SoloCollisionWorld::game_objects: a placed
// object's model COL, GOB +b8 0021bd90); the hit object is the COL geometry
// handle (index + 1).
#include "bullet_pool.hpp"
#include "solo_actor_simulation.hpp"
#include "target_visibility.hpp"
#include <functional>
#include <optional>

namespace omega {

struct WorldLosQuery {
    RootVector origin{},displacement{};
    std::uint8_t required{},excluded{};
    // The provider's excluded objects (0013ab10: request +50 / +54, nonnull);
    // 003c4f00 drops their direct +b8 geometry (00183070 finds them).
    std::array<std::uint64_t,2> excluded_objects{};
};
// 0013ab10 adds a request's +50 then +54 to the provider's exclusion list (a null
// object skipped); 003c4f00 skips an object's direct +b8 geometry when 00183070
// finds it there (not 00183050's end). True when object is listed.
bool los_object_excluded(const std::array<std::uint64_t,2>& excluded,std::uint64_t object);
struct WorldLosHit {
    RootVector point{},normal{};
    CollisionTriangleSource source{};
};
// 003c26d0 -> 003c37a0 over the world (see above). Origin W is forced to 1 and
// displacement W to 0 as the callers do.
std::optional<WorldLosHit> trace_world_los(const SoloCollisionWorld& world,const WorldLosQuery& query);
// 0013a480 for a LosServiceRequest (the visibility cache's request, mask +58 = 4).
LosServiceResult answer_los_request(const SoloCollisionWorld& world,const LosServiceRequest& request);

// The bullet request's query (00277e20 -> 00148290 -> 003c26d0 with required
// mask 0, excluded mask 0x10 and the list 00504030 that selects the executor
// 003c3a60): the first pass is the kernel with 0x10 | 0x80 excluded; the
// second pass 003c4a80 (trace_los_pass_triangles) reports each water (0x80,
// not 0x10) triangle the first-pass-shortened segment crosses to the list
// object's callback 00278160 (vtable 004dcd88 +8): water gets the crossing
// point and the triangle's source, in candidate order; empty, a crossing throws.
// The excluded object (the shooter) gates only its own direct +b8 geometry;
// a shooter is an actor, which owns no COL in the world. The hit object is the COL source record's
// owner (00278060: *(result +64) +0): the population object (GOB) whose model COL it is
// (SoloCollisionWorld::game_object), else the terrain piece (geometry handle = owner = index + 1); the
// source words are the native candidate's (geometry, triangle, object, 0).
std::optional<BulletStaticHit> bullet_static_los(const SoloCollisionWorld& world,const RootVector& start,const RootVector& displacement,
    const std::function<void(const RootVector& point,const CollisionTriangleSource& source)>& water={});

}
