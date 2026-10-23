#pragma once
#include "animation_graph.hpp"
#include "animation_group.hpp"
#include "animation_pose.hpp"
#include "animation_root_clip.hpp"
#include <map>
#include <functional>

namespace omega {
using AnimationClipBank = std::map<std::string, AnimationClip>;
struct AnimationRootRequest {
    std::uint64_t host{};
    std::size_t node{};
    float weight{1}, initial_phase{};
    GraphQuaternion rotation{0,0,0,1};
    bool paused{};
    // Retain this host's groups even when controller traversal skips its node.
    bool submit_node{true};
};
struct AnimationGroupMember {
    std::size_t node{};
    float weight{};
    bool raw_rotation{true};
    GraphQuaternion rotation{0,0,0,1};
};
struct AnimationPlayerGroup {
    std::uint64_t host{};
    std::int32_t number{-1};
    AnimationGroupPhase phase;
    bool active{}, paused{};
    GroupDuration totals;
    std::vector<AnimationGroupMember> members;
};
struct AnimationPlayerAnchor {
    RootAnchorKey key{};
    RootAnimationState state;
    std::optional<RootParentTransform> parent;
    // Physical consumers may constrain different root anchors independently.
    std::optional<RootMotionConstraint> constraint;
    // Root+d0: cleared before graph/controller evaluation, accumulated during
    // sampling, retained after finalization for the next actor preparation.
    float weight{};
};
struct AnimationPlayerState {
    AnimationGraphContext graph;
    // Original model context +1c scales group playback only, after graph and
    // controller callbacks. Selector/channel/blend clocks use outer elapsed.
    float playback_rate{1};
    std::vector<AnimationPlayerGroup> groups;
    NonRootPose pose;
    std::array<float,16> root_local{};
    std::vector<AnimationPlayerAnchor> anchors;
    RootVector orientation{0,0,0,1}, position{0,0,0,1};
    RootVector base_orientation{0,0,0,1}, base_position{0,0,0,1};
    std::vector<std::array<float,16>> world;
    // Node 0 +1a0: the rotation finalizer 003ba2b0's cache for the root bone
    // (bone_rotation_cache over bone0_rotation_accumulator); all zero until
    // the first evaluation. The upper-body aim reads it (secondary_aim.hpp).
    RootVector bone0_rotation{};
};
AnimationPlayerState make_animation_player(const SkeletonDefinition& skeleton,
    std::span<const RootAnchorKey> anchors, std::size_t channels, std::size_t selectors);
// Internal controller boundary: after graph totals exist, before sampling.
// It may update group phase/pause fields and returns surviving hosts in sample
// order. The callback must manage its own external side effects transactionally.
using AnimationAfterGraph = std::function<std::vector<std::uint64_t>(std::vector<AnimationPlayerGroup>&, AnimationPlayerState&)>;
// Model-frame order: selectors, graph traversal, grouped clip playback,
// retained bones, root anchors, hierarchy, then selector history. Scalar
// channels are supplied after actor/channel updates. Each host owns its groups.
// Parent anchors use explicitly resolved world transforms and retained local
// placement. Skeleton bind poses are constructor-derived.
// All configuration and clips are caller-supplied; mutation is transactional.
// The root-collection float steps of 003ae750 under the EE float rules
// (denormal operands read as zero, denormal results flush; goldens
// tests/root_orientation_goldens.hpp from tools/ee/gen_root_orientation.py).
namespace animation_root_vu {
// Add an active anchor's weighted orientation, negated when its dot with the
// previous orientation is below zero (003ae7ec..003ae864).
void add_orientation(RootVector& sum, RootVector value, const RootVector& reference);
// vadd.xyzw into a running sum.
void add(RootVector& sum, const RootVector& value);
// The base pass's weight test c.ole.s (003ae9bc): false skips the anchor.
bool weight_included(float weight);
// vmulx.xyzw by the anchor weight (003ae9e8 / 003aea9c).
RootVector scaled(RootVector value, float weight);
// Identity for a zero sum (003ae8b8..003ae8d8), else the VU RSQRT
// normalization (003ae908..003ae928); throws when |q|^2 flushes to zero.
RootVector normalized(RootVector q);
}
// The group-member float steps under the same EE float rules (goldens
// tests/root_orientation_goldens.hpp, slices E and F).
namespace animation_member_vu {
// Merge a leaf contribution into an existing member (003b15b0).
void merge(AnimationGroupMember& dst, const GraphLeafContribution& src);
// The quaternion handed to the clip evaluator (003adde0): a raw member's as
// is, else normalized (identity for a zero sum, RSQRT at 003ade70).
GraphQuaternion rotation(const AnimationGroupMember& member);
// 003b17d0's weight test (003b181c): false returns before sampling.
bool sampled(float weight);
}
void update_animation_player(AnimationPlayerState& state, const AnimationGraph& graph,
    const SkeletonDefinition& skeleton, const AnimationClipBank& clips,
    std::span<const AnimationRootRequest> roots, float elapsed,
    const RootMotionConstraint& constraint = {}, const AnimationAfterGraph& after_graph = {});
}
