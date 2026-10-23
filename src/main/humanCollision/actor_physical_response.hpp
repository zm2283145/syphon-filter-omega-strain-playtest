#pragma once
#include "actor_body_query.hpp"
#include "actor_floor_response.hpp"
#include "actor_response_placement.hpp"
#include "actor_response_probes.hpp"
#include "actor_response_setup.hpp"

namespace omega {
// Everything 001e2180 reads or writes that is not owned by a live root/node.
struct ActorPhysicalResponseState {
    ActorResponsePhysical physical;
    float contact_blend_a20{};   // physical +a20
    float mass_2e0{};            // physical +2e0
    std::optional<ActorWaterProbeHit> water; // +170..+1d0 (and +e4) from 001e5f30
};
struct ActorPhysicalResponseInput {
    RootVector support{};        // caller sp+370 slot 0 (001e7110 displacement)
    RootVector support_scaled{}; // caller sp+340 slot 0 (scaled displacement)
    float elapsed{},inverse_elapsed{};
    RootVector gravity=game_initial_gravity; // 004f5430
    ActorResponseTuning tuning;
};
struct ActorPhysicalResponseHost {
    ActorResponseSetupHost setup;
    ActorResponseResolveHost resolve;
    ActorResponseProbeHost collision;
    // 1e2a10..3060: plan construction (ordinary/shaped/specialized) and the
    // one- or two-pass execution are host-owned because they need the live
    // moving-component host; the input below carries every value 001e2180
    // supplies. Returns the public result (+80 displacement, +20/+30/+50).
    std::function<ActorBodyQueryResult(const ActorBodyQueryInput&,bool specialized)> body_query;
    std::function<void(const ActorBodyQueryResult&)> contact_events; // 1e306c (001e5a10)
    std::function<void()> refresh_skeleton_nodes;                     // 1e3d74..3e84 side effects
    std::function<RootVector()> floor_root_orientation;               // 0019d500 quaternion
    std::function<RootVector()> anchor_world;                         // early exit only
    // 1e4be4..4dcc on the live root tree (apply_actor_response_placement and
    // apply_actor_response_orientation); receives the retained +e0/+394 state.
    std::function<void(ActorResponsePlacementState&,const ActorResponsePlacementInput&)> placement;
};
struct ActorPhysicalResponseTrace {
    ActorResponseSetup setup;
    std::optional<ActorBodyQueryResult> body;
    std::optional<ActorResponsePosition> position;
    std::optional<ActorFloorProbeHit> floor;
    std::optional<ActorFloorProbeHit> reprobe;
    std::optional<ActorFloorResponseDraft> floor_response;
    std::optional<ActorFloorFilterResult> filter;
};
// Complete 001e2180 order. Each stage is the corresponding native component;
// external effects through the host cannot be rolled back on a late failure.
ActorPhysicalResponseTrace run_actor_physical_response(ActorPhysicalResponseState&,
    const ActorPhysicalResponseInput&,const ActorPhysicalResponseHost&);
}
