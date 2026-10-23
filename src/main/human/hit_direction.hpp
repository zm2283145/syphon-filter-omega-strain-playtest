#pragma once
// The hit-direction classifier 00186950(actor, position, base) the kill
// 00186bf0 calls for damage types 1 (base 6), 2 and 0xd (base 4)
// (ACTOR_DAMAGE_AND_HEALTH.md), interpreter-verified
// (tests/hit_direction_goldens.hpp):
//   1. yaw = 0; with a position: 0022abc0 over the actor's virtual +f4
//      matrix whose translation row is replaced by the eye (virtual
//      +c0(actor, -1)), yaw = its third angle (atan2f(-x, y));
//   2. |yaw| widened to a double (0011bd70) and compared with 1.57 (00100260
//      answers nonzero for greater and for NaN): greater gives the code
//      `base` and turns yaw by pi (yaw <= 0: + 3.14, else - 3.14, the float
//      0x4048f5c3); otherwise the code is (base + 1) & 0xff;
//   3. unless |yaw| <= 0.3 (0x3e99999a): the heading h of the +f4 matrix
//      (row 0 z < row 1 z: atan2f(r0.y, r0.x), else atan2f(-r1.x, r1.y), the
//      game's atan2f) plus yaw goes into the heading history entry +3098
//      (00186be0) and the heading curve +3030 takes the last entry as its
//      target with duration 0.5 (00186b50).
#include "player_targeting.hpp"
#include <cstdint>
#include <optional>

namespace omega {

struct HitDirection {
    std::int32_t code{};
    // The angle written to history entry +3098, then 00186b50(+3030, 0.5).
    std::optional<float> heading;
};
HitDirection classify_hit_direction(const TargetingMatrix& frame_f4,const RootVector& eye_c0,
    const std::optional<RootVector>& position,std::int32_t base);
// The duration 00186950 passes to 00186b50.
inline constexpr float hit_direction_turn_duration=0.5F;

}
