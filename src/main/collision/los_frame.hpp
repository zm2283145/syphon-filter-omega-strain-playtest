#pragma once
#include "animation_root.hpp"

namespace omega {
struct LosCorridorFrame {
    std::array<float,16> world_to_corridor;
    RootVector start,end;
};
// 003c2210's nonzero, affine segment branch. The original immediate LOS caller
// uses radius .01f. This is the oriented/scaled discovery frame, not a hit test.
// Points use W1 and displacement W0; degenerate/unsupported arithmetic rejects.
LosCorridorFrame prepare_los_corridor(const RootVector& origin,
    const RootVector& displacement,float radius=.01F);
}
