#pragma once
#include "actor_auxiliary_probe.hpp"
#include "actor_edge_presets.hpp"
#include "actor_physical_initialization.hpp"
#include "actor_physical_response.hpp"
#include "actor_physical_state.hpp"
#include "actor_support_history.hpp"
#include "actor_support_motion.hpp"
#include <bit>

namespace omega {
// Unavailable-height sentinel at 004a0438: initialized data 0xff7fffff
// (-FLT_MAX); no executable store to it was found.
inline constexpr float actor_unavailable_height=std::bit_cast<float>(0xff7fffffU);

// Retained physical state touched by 001e9980. The sub-states are the existing
// component views; the driver copies shared fields between them at the
// original stage boundaries (mode/flags/radius, integrator, +1e0).
struct ActorPhysicalFrameState {
    ActorPhysicalHistory history;             // +18..+38, +370/+374
    ActorPhysicalPredictionState integrator;  // +2d0..+330, +9d0
    ActorPhysicalTail tail;                   // +10/+11, +1e0
    ActorPhysicalResponseState response;      // 001e2180 view
    ActorSupportHistory support_history;      // +1f0..+2c2
    ActorAuxiliaryProbeState auxiliary;       // +3d1..+3d4
    RootVector plane_f0{};                    // +f0, written by 001e9380
    // +164: the floor-contact record's source (record +100, +64), written by
    // 001e1b70 at 001e40ec on a floor-probe hit and retained otherwise; the
    // owning object is its first word, read by 001e6300. Port: the COL owner id.
    std::uint64_t floor_contact_object_164{};
    ActorEdgePresetState edges;                // +3e0 slots, +7a0/+7a1 (001dfb80)
};
// 001e9380 applied to the frame view: runs initialize_actor_physical_state and
// writes +40..+90, +dc/+dd/+e8, +e0, +e4, +f0, +2c4, +394 and the integrator.
void initialize_actor_physical_frame(ActorPhysicalFrameState&,const ActorPhysicalInitializationHost&);
struct ActorPhysicalFrameHost {
    // 001e9380, only when +11 is set. Not native as a whole frame stage yet.
    std::function<void(ActorPhysicalFrameState&)> initialize;
    // 001dfb80 edge/preset evaluation (nine +3e0 slots). Not native yet; the
    // host decides what runs here and must not fake slot results.
    std::function<void(ActorPhysicalFrameState&)> edge_presets;
    ActorSupportMotionHost support_motion;                 // 001e7110
    std::array<std::array<std::uint8_t,4>,3> root_keys{{{'f','l','o','r'},{'e','d','g','e'},{'l','a','d','d'}}};
    ActorPhysicalResponseHost response;                    // 001e2180
    ActorSupportHistoryHost support_history;               // 001e6300
    // 00133ed0(P+8, 0) translation; read only while +10 is clear.
    std::function<RootVector()> component_translation;
    ActorAuxiliaryProbeHost auxiliary;                     // 001ec580 (edge root)
    // 001e5cf0 with (entry capture, +e4, +1a8); see notify_actor_water_support.
    std::function<void(float previous_capture,float height,float normal_z)> water_support;
};
struct ActorPhysicalFrameTrace {
    float entry_capture{};
    ActorPhysicalPredictionDraft prediction;
    ActorSupportMotionOutput support;
    std::optional<ActorPhysicalResponseTrace> response;
    RootVector auxiliary_origin{};
    ActorAuxiliaryProbeOutput auxiliary;
};
// 001e9980 in original order: entry water capture, optional initialization,
// mode/flag history and radius, prediction, 001dfb80, 001e7110, 001e2180 for
// modes 0/5/9/10/11/12, 001e6300, override clear, auxiliary origin and
// 001ec580, 001e5cf0, then the tail (+1e0, +2d0, +10, +11). Host effects that
// already ran are not rolled back if a later stage throws.
ActorPhysicalFrameTrace run_actor_physical_frame(ActorPhysicalFrameState&,float elapsed,
    const ActorPhysicalFrameHost&,const ActorResponseTuning& tuning={},
    const RootVector& gravity=game_initial_gravity);
}
