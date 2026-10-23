#pragma once
#include "animation_root.hpp"
#include <cstddef>
#include <functional>

namespace omega {
using ActorSupportIdentity=std::uint64_t;
RootParentTransform actor_support_matrix_placement(const std::array<float,16>&);
// Original190100: parent*local, preserving the admitted point W1.
RootParentTransform compose_actor_support_placement(const RootParentTransform& local,const RootParentTransform& parent);
// 001959c0 with an owner transform component: the 1831b0 matrix through its
// rigid inverse 0013eca0, the inline 195ee0 quaternion block, then 190100
// (inverse owner * world) -> the owner-relative placement.
RootParentTransform actor_owner_local_placement(const RootParentTransform& world,const std::array<float,16>& owner);
struct ActorSupportMotionHost {
    std::function<bool()> primary_enabled; // physical+e8
    std::function<ActorSupportIdentity(std::size_t)> object; // +1f0+4*slot
    std::function<ActorSupportIdentity(ActorSupportIdentity)> owner; // object+50
    std::function<RootParentTransform(std::size_t)> local; // +200+20*slot
    std::function<RootParentTransform(std::size_t)> previous_owner; // +260+20*slot
    std::function<bool(ActorSupportIdentity)> owner_dirty;
    std::function<void(ActorSupportIdentity)> refresh_owner;
    std::function<std::array<float,16>(ActorSupportIdentity)> owner_matrix;
    std::function<ActorSupportIdentity(const std::array<std::uint8_t,4>&)> root;
    std::function<RootParentTransform(ActorSupportIdentity)> root_world;
    std::function<bool(ActorSupportIdentity)> root_parented;
    // Implement original setters plus caller's normalization-counter reset.
    std::function<void(ActorSupportIdentity,const RootVector&)> set_orientation_and_reset;
    std::function<void(ActorSupportIdentity,const RootParentTransform&)> set_placement_and_reset;
};
struct ActorSupportMotionOutput {
    std::array<RootVector,3> displacement{},scaled_displacement{};
    std::array<RootParentTransform,3> delta{};
};
// 001e7110. Root keys are verified runtime c8/d0/d8 values, supplied by owner.
// Explicit live reads/setters preserve original ordering. External refresh and
// root setter effects cannot be rolled back if a later slot fails.
ActorSupportMotionOutput consume_actor_support_motion(float scale,
    const std::array<std::array<std::uint8_t,4>,3>& keys,const ActorSupportMotionHost&);
}
