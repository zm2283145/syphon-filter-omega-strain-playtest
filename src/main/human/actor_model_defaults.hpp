#pragma once
// Model-side defaults a human actor receives before its first world update
// (EMULATOR_COMPARISON.md, "Level start: cause"):
//  - 0018caf0, the default animation channels, applied by model initialization
//    001b3460 (at 001b3924, after the startup motion install 003ad500) and by
//    the revive 001898f0;
//  - the centroid weights of the human skeleton nodes (table 001b0740, applied
//    by 001af190 at activation 001b53d0 / 001b4f40) and the weighted centroid
//    003a9090 that the world entry 00392aa0 stores at model context +50 after
//    the physical step. Physical +8 +50 is that centroid: 001e9380 and 001e5880
//    place it through the root pose and the model component.
#include "actor_animation.hpp"
#include "animation_root.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace omega {
// 0018caf0(actor): 23 set commands to the channels of actor +2ec0 +10
// (0018d0f0), in order 0..22: 0.5 for channels 0, 1, 5, 6, 9, 14, 15, 16, 17
// and 22, 1.0 for 21, 0.0 for the others (interpreter, ACTOR_TELEPORT.md).
std::array<ActorChannelCommand,23> actor_default_channel_commands();

struct HumanCentroidWeight {
    std::uint32_t node{};
    float weight{};
};
// The ten {node, weight} entries 001b0740 builds (004eea90.., pushed in this
// order into the vector at 004ee9d0).
std::span<const HumanCentroidWeight> human_centroid_table();
// 001af190 with context +14 set: every node weight (+170) of the collection is
// zeroed, then each table entry stores its weight on its node. The original
// indexes the collection without a range check (001af330); a table node
// outside the collection throws here.
std::vector<float> human_centroid_weights(std::size_t node_count);
// 003a9090 over skeleton-space node matrices (column-major, file order): the
// weighted sum of the translation columns (all four lanes, traversal order),
// scaled by the reciprocal of the weight total when it is nonzero, W forced
// to 1. This is the centroid part of update_actor_model_context.
RootVector human_model_centroid(std::span<const std::array<float,16>> nodes,std::span<const float> weights);
}
