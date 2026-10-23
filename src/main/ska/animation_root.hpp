#pragma once
#include <array>
#include <cstdint>
#include <iosfwd>
#include <optional>

namespace omega {
using RootVector = std::array<float, 4>;
struct RootParentTransform {
    RootVector orientation{0,0,0,1}, position{0,0,0,1};
};

// Local retained fields consumed/produced by the original root finalizer.
// A caller may supply the resolved parent placement separately.
struct RootAnimationState {
    RootVector orientation{0, 0, 0, 1};
    RootVector position{0, 0, 0, 1};
    RootVector motion_cache{};
    RootVector pose_orientation{0, 0, 0, 1};
    RootVector pose_position{0, 0, 0, 1};
    std::optional<RootVector> sign_reference;
    std::uint32_t normalization_counter{};
    bool dirty_transform{}, dirty_motion{}, dirty_pose{};
};

// Already weighted accumulators, supplied in their original bucket order.
// This is not a clip sampler or a graph-selected player input command.
struct RootFrame {
    float weight{};
    RootVector pose_quaternion{};
    RootVector pose_translation{};
    float angle{};
    std::array<RootVector, 4> motion_buckets{};
};

enum class RootConstraintKind { none, reject_axis, keep_axis };
struct RootMotionConstraint {
    RootConstraintKind kind{RootConstraintKind::none};
    RootVector axis{};
};
struct RootAnimationOutput {
    bool active{};
    std::optional<RootVector> weighted_orientation;
    std::optional<RootVector> weighted_position;
};

RootAnimationState make_initial_root_animation();
// Original unparented placement setter: no normalization, counter or cache reset.
void set_root_placement(RootAnimationState& state, const RootVector& orientation,
                        const RootVector& position);
// Original orientation setter consumes world rotation and conjugates the
// resolved parent rotation. No normalization or counter/pose-cache reset.
void set_root_world_orientation(RootAnimationState& state,const RootVector& orientation,
    const RootParentTransform* parent=nullptr);
RootParentTransform invert_root_placement(const RootParentTransform& placement);
void set_root_world_placement(RootAnimationState& state,const RootParentTransform& placement,
    const RootParentTransform* parent=nullptr);
// Position-only publication (001b26a0); leaves orientation, caches and counter
// untouched. The owner must invalidate descendant transform/motion/pose caches.
void set_root_world_position(RootAnimationState& state,const RootVector& position,
    const RootParentTransform* parent=nullptr);
// Finite nonnegative-weight domain. On failure, state remains unchanged.
// Zero weight only installs the identity sign reference and yields no outputs.
// sqrt/rsqrt follow the interpreter's VU SQRT/RSQRT model (vu_float.hpp);
// trigonometry is a documented portable approximation, not PS2
// execution-verified. Dirty flags are retained, not frame event flags.
RootAnimationOutput finalize_root_animation(RootAnimationState& state,
    const RootFrame& frame, const RootMotionConstraint& constraint = {},
    const RootParentTransform* parent = nullptr);
// Compose retained local placement into an explicitly resolved parent transform.
// Parent resolution/tree invalidation remains the owning actor's responsibility.
RootParentTransform root_world_placement(const RootAnimationState& state,
    const RootParentTransform* parent = nullptr);
// Weighted affine composition; W=0 represents a direction/normal.
RootVector transform_root_vector(const RootParentTransform& transform,const RootVector& value);
// Snapshot envelope: {state:{...},output:{active,weighted_orientation,weighted_position}}.
void write_root_animation(std::ostream& output, const RootAnimationState& state,
                          const RootAnimationOutput& result);
}
