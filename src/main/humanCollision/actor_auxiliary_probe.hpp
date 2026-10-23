#pragma once
#include "collision_face.hpp"
#include "animation_root.hpp"
#include <functional>

namespace omega {
struct ActorAuxiliaryProbeState {
    std::uint8_t first_requested{},first_hit{},second_requested{},second_hit{};
};
struct ActorAuxiliaryProbeHost {
    std::function<float()> tuning; // runtime004a0358, each original read
    // nullopt is the original null collection; engaged empty span is a real
    // empty collection. A missing callback is never interpreted as no hit.
    std::function<std::optional<std::span<FaceCandidate>>()> candidates;
    std::function<RootParentTransform()> root_world; // real132400 root lookup/cache
    // Original3c3450 reconstructs +30 for edge/vertex even though this caller
    // reads only pointZ. Required real source/owner refresh; never a no-op stub.
    std::function<Vec3(const FaceContact&)> source_face_normal;
};
struct ActorAuxiliaryProbeOutput {
    std::array<std::optional<FaceResult>,2> queries;
    RootVector second_origin{};
    float second_reference_z{};
};
// Original001ec580, concrete static mode0 queries (119=0), not moving executor.
// Latch writes and host/candidate effects occur in original order; late failure
// leaves prior effects visible. Unused first-origin/reference inputs are skipped.
ActorAuxiliaryProbeOutput run_actor_auxiliary_probe(ActorAuxiliaryProbeState&,
    const RootVector& first_origin,float first_reference_z,const ActorAuxiliaryProbeHost&);
}
