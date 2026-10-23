#pragma once
// The player's takedown camera 00131540(G = 004ffb50, actor, target) (COMBAT.md,
// "Checkpoint 120 takedown camera and baton/torch hits"). 00196480 calls it with
// target 0 after a takedown by the local player; it builds a view matrix and
// gives it to the takedown camera object G +d4 (a plain base camera, vtable
// 004da580, fov 0x3f91361e = 65 degrees, built at 00132d28), then pushes that
// object on the world stack (00131320(G, G +d4)) and sets G +d8 = 1 (the
// pending removal 00131510 that the attacker's tokens 0x96 / 0x97 run).
// Checkpoint 132ia1c: the target branch the air taser's attach takes
// (00131540(G, victim, taser owner)). Interpreter-verified
// (tools/ee/gen_takedown_camera.py).
#include "player_targeting.hpp"
#include "target_visibility.hpp"
#include <bit>
#include <cstdint>
#include <functional>

namespace omega {
inline constexpr int takedown_camera_head_node=14;                                   // lb *0049d024
inline constexpr float takedown_camera_fov=std::bit_cast<float>(0x3f91361eU);        // 0017f9f0's fov at 00132d28 (65 degrees)
struct TakedownCamera {
    RootVector head{};               // 00132910(00132a30(actor), node 14): the LOS origin
    RootVector position{};           // sp+2d0: the camera point (after the LOS pull-in)
    LosServiceRequest los{};         // the request (class 005435a0, vtable 004d9180) given to 003caa50's virtual +c
    LosServiceResult result{};       // its answer (result storage sp+1e0)
    TargetingMatrix view{};          // the matrix given to (G +d4) +50's virtual +c: rows right, forward, up, position
};
// The target branch 00131590..001316a8 (the air taser's attach 00264cb0
// calls 00131540(G, victim, taser owner)): what it reads of the target.
struct TakedownCameraTarget {
    RootVector target_e4{};          // the target's virtual +e4 (sp+330)
    float relative_z{};              // 0022ab20(actor, target, out) out +8: target_angles over the actor's +f4 /
                                     // +c0(-1) and the target's +c0(-1) (melee_relative_angles)
    RootVector actor_e4{};           // the actor's virtual +e4 (sp+160)
};
// 00131540. head: 00132910 on the actor's model for node 14; matrix_f4:
// actor virtual +f4 (its translation row is replaced by the head); actor:
// the request's +50. los answers the request (003caa50(004ffb50, 004ea0b0)
// -> virtual +c). A zero vector normalizes to zero (the VU's division by
// zero gives Q = FLT_MAX), as in the original. target: null for a2 = 0.
TakedownCamera player_takedown_camera(std::uint64_t actor,const RootVector& head,const TargetingMatrix& matrix_f4,
    const std::function<LosServiceResult(const LosServiceRequest&)>& los,const TakedownCameraTarget* target=nullptr);
}
