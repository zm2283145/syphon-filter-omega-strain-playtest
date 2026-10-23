#pragma once
#include "actor_movement_scheduler.hpp"
#include <bit>
#include <functional>
#include <optional>

namespace omega {
// Physics tuning written by static initializer 004cc5e0 (program constants).
struct ActorResponseTuning {
    RootVector offset{0,0,0.5F,0};                        // 004a03f0
    float crouch_rate{15.0F},crouch_target{0.75F};        // 004a0348, literal 0.75
    float stand_rate{std::bit_cast<float>(0x3eb33333U)};  // 004a0350 (0.35)
    float stand_target{1.0F};                             // literal 1.0
    float height_offset_unflagged{std::bit_cast<float>(0x3fc66666U)}; // 004a0338 (1.55)
    float height_offset_flag1{0.75F};                     // 004a0340
};

// Physical +a0..+d7: animated scalar (vtable 004da3e0, base ctor 0017f480).
// Segment +b8..+cc is a cubic Hermite (p0, m0*T, p1, m1*T, T, 1/T); +d0 points
// at it and +d4 counts down. Holds the filtered floor height in root space.
struct ActorHeightFilter {
    float previous{},value{},velocity{},goal{},damping{}; // +a4 +a8 +ac +b0 +b4
    float p0{},m0{},p1{},m1{},duration{},inverse_duration{}; // +b8..+cc
    float timer{};                                         // +d4
    bool operator==(const ActorHeightFilter&) const=default;
};

// Physical fields read and written by 001e2180 before its body query.
struct ActorResponsePhysical {
    float e0{},e4{};std::uint8_t e8{};       // reset to -FLT_MAX/-FLT_MAX/0
    RootVector offset_380{};                 // +380 (Z also adjusted via +388)
    std::uint8_t use_new_offset_11{};        // +11
    std::uint8_t enabled_12{};               // +12
    std::uint8_t shape_14{};                 // +14
    std::int32_t mode_20{};                  // +20
    std::uint32_t flags_34{};                // +34
    RootVector predicted_2f0{},position_300{};
    float radius_370{},scale_390{},height_394{};
    RootVector contact_40{},contact_50{};float contact_60{};
    RootVector contact_70{},contact_80{};float contact_90{};
    std::uint8_t byte_dc{},byte_dd{},keep_contact_9c0{};
    float floor_2c4{},blend_2c8{};std::uint8_t blend_2cc{};
    // Read/written by the post-floor filter 1e43ac..4be0.
    ActorHeightFilter height_filter;
    float floor_height_d8{};   // previous filtered-goal floor Z
    std::uint8_t byte_3d4{};
    // Integrator block at +2e0 (mass; +2f0 position, +300 previous position).
    RootVector velocity_310{},acceleration_320{},force_330{};
    RootVector contact_motion_1e0{},up_axis_9f0{};
    std::uint8_t byte_13{},airborne_de{};
};

enum class ActorRoot:std::uint8_t { floor=0 /*"flor" 4a03c8*/, edge=1 /*"edge" 4a03d0*/, ladder=2 /*"ladd" 4a03d8*/ };
struct ActorResponseSetupHost {
    std::function<bool()> floor_root_parented;            // root +b0 nonnull
    std::function<float(ActorRoot)> root_weight;          // root +d0, each original read
    std::function<float()> component_scale_z;             // P+8 -> +28 dirty refresh, then +38
    std::function<RootVector()> model_anchor;             // 001e5880(P+8)
    std::function<float()> floor_root_world_z;            // 00132400/00132370 position Z
    std::function<ActorMovementScheduleResult(const RootVector& endpoint)> adjust_movement; // 001e7f40
};

struct ActorResponseSetup {
    float previous_e0{};       // f22
    float vertical_offset{};   // f21: 0 when the floor root is parented, else +394
    RootVector base{};         // sp+1490 (position +300)
    RootVector origin{};       // sp+1480: body query origin
    RootVector anchor{};       // sp+1470: model anchor - weighted offset + support motion
    RootVector anchor_top{};   // sp+1460: anchor + new +380
    std::array<RootVector,2> saved_contact_vectors{},saved_contact_vectors_b{}; // 1430/1440, 1400/1410
    float saved_contact_60{},saved_contact_90{};
    std::uint8_t previous_dc{};
    bool proceed{};            // false: original jumps to the tail at 1e4dd8
    bool specialized{},blended{},supported{}; // s6, s7, s8
    RootVector root_displacement{};  // sp+11f0
    RootVector intended{};           // sp+1210
    RootVector blend_delta{};        // sp+1200 (supported path only)
    RootVector adjustment{};         // sp+11c0 from 001e7f40
    bool adjusted{};                 // sp+1b0
    RootVector displacement{};       // sp+11d0: body query displacement
};

// 001e2180..1e29f8. support_displacement is 001e7110's first displacement
// output (caller sp+370). Mutates physical fields exactly where the original
// stores them; host callbacks run in original order.
ActorResponseSetup prepare_actor_response(ActorResponsePhysical&,const RootVector& support_displacement,
    float elapsed,const ActorResponseTuning&,const ActorResponseSetupHost&);

struct ActorResponseResolveHost {
    std::function<RootParentTransform()> floor_root_pose;   // 001e56a0 (+80 or parented 001a7560)
    std::function<RootVector()> floor_root_base_position;   // 001a91a0, each original call
    std::function<RootVector()> anchor_local;               // P+8 object +50
    std::function<RootVector()> floor_root_motion;          // 001e5640 (+70, or +20 after 001a76a0)
    std::function<RootVector()> component_translation;      // P+8 -> +28 dirty refresh, +40
    std::function<void(const RootVector&)> set_component_position; // component virtual +14
    // Ledge/ladder (s6) inputs, required only when setup.specialized.
    std::function<float(ActorRoot)> root_weight;             // root +d0, each original read
    std::function<RootVector(int)> node_position;           // 00132910(P+8, node): 12, 16, 0
    std::function<RootParentTransform()> edge_root_resolved; // 001910a0(edge)
    std::function<RootParentTransform()> edge_root_world;    // 00132400(edge): quaternion, position
    std::function<bool()> edge_root_parented;                // edge +b0 nonnull
    std::function<RootVector()> edge_root_base_position;     // 001a91a0(edge)
    std::function<void(const RootVector&)> set_edge_root_base_position; // 001e1f10(edge, v)
};
struct ActorResponsePosition {
    RootVector resolved{};        // sp+10d0
    RootVector root_target{};     // sp+10e0: water-probe target
    RootVector probe_reference{}; // sp+10c0: floor probe origin, water probe base
    RootVector root_delta{};      // sp+10b0: published component displacement
    RootVector published{};       // component translation + root_delta
};
// 1e3074..3cd0. resolved_displacement is the body query's public displacement
// (result +80 = sp+12a0) and query_hit its status byte. Ledge/ladder setups run
// 1e3074..3628 (edge root updates included); blended setups lerp the ledge and
// floor results by the floor weight. Writes +2c8/+2cc on the supported path and
// publishes the component position.
ActorResponsePosition resolve_actor_response_position(ActorResponsePhysical&,const ActorResponseSetup&,
    const RootVector& support_displacement,const RootVector& resolved_displacement,bool query_hit,
    const ActorResponseResolveHost&);

// 1e4430..4be0: floor-height filter and published floor plane. floor is the
// floor result after any 1e43b8 re-probe (point = result +50, normal = +30).
// previous_dc is +dc captured at 1e2598 (setup.previous_dc); current_dc is
// +dc after the floor response. floor_root_orientation is the quaternion of
// 0019d500 / the 00132850 branch (base placement, refreshed when parented);
// it is read twice, as in the original. Writes +40..+90, +a4..+d4, +d8, +2f0.
struct ActorFloorSample { RootVector point{}; RootVector face_normal{}; };
struct ActorFloorFilterResult {
    float retime{-1.0F};      // f27: new segment duration, negative = none
    RootVector goal_local{};  // sp+e80 (Z becomes the filter goal)
    RootVector floor_point{}; // published +40 (filtered Z)
};
ActorFloorFilterResult update_actor_floor_filter(ActorResponsePhysical&,const ActorResponseSetup&,
    const ActorResponsePosition&,const RootVector& support_displacement,
    const std::optional<ActorFloorSample>& floor,bool current_dc,float elapsed,float inverse_elapsed,
    const std::function<RootVector()>& floor_root_orientation);

// 1e4dd8..4e34: early exit when +12 is clear or mode +20 is 12 (setup.proceed
// false). anchor_world is 00182f30(P+8 +50, refreshed component matrix).
void skip_actor_response(ActorResponsePhysical&,const RootVector& anchor_world);
// 1e4e38..end: +de airborne flag (support_scaled_z is caller t0 slot 0 Z),
// +9f0 up axis from +50/+80, else +1e0, else (0,0,1,0), and the integrator
// reset (1e50a0/5090/5070) when +13 is clear.
void finish_actor_response(ActorResponsePhysical&,const ActorResponseSetup&,float support_scaled_z);

// 001e5940: clamped half-height helper (also used by the body-query plan).
float actor_response_height_delta(float top,float reference,float radius,float scale,bool flag1,
    const ActorResponseTuning&);
}
