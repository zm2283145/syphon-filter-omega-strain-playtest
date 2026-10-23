#pragma once
#include "animation_root.hpp"
#include <functional>

namespace omega {
using ActorSupportIdentity=std::uint64_t;
struct ActorSupportHistory {
    std::array<ActorSupportIdentity,3> owners{};
    std::array<RootParentTransform,3> local{},previous_owner{};
    // Original bytes +2c0/+2c1/+2c2; slot zero is unconditionally cleared.
    std::array<bool,3> retain{};
};
struct ActorSupportHistoryComponent {
    std::array<float,16> matrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    bool dirty{};
};
struct ActorSupportHistoryRoot {
    float weight{};
    RootParentTransform placement;
};
struct ActorSupportHistoryHost {
    std::function<bool()> primary_enabled;
    std::function<ActorSupportIdentity()> contact_owner;
    std::function<ActorSupportHistoryComponent*(ActorSupportIdentity)> component;
    std::function<ActorSupportHistoryRoot*(std::size_t)> root;
    std::function<void(ActorSupportHistoryComponent&,ActorSupportHistory&)> refresh;
    std::function<RootParentTransform(ActorSupportHistoryRoot&,ActorSupportHistory&)> world_placement;
};
// Original 001e6300. Root slots are supplied by the caller's verified registry;
// owner/component identity and pointer lifetime are host responsibilities.
// All callbacks see stable captured pointers; refresh/world_placement receive
// the staged in-progress state, so later slots and retained flags remain live.
// State commits only on success; external callback effects are not rolled back.
// Evaluated matrices are affine, with finite normal-or-zero arithmetic.
void capture_actor_support_history(ActorSupportHistory&,const ActorSupportHistoryHost&);
}
