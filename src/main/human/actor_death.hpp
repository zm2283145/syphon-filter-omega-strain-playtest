#pragma once
// Actor action 0x42, the death action 0018a950 (ACTOR_DAMAGE_AND_HEALTH.md),
// for kind 2 and every death code of the jump table 0049dbe0 (0..11): the
// motion name by code (crouch variants from +3204 and RNG draws for codes
// 0, 1, 6 and 7), the extra action 0x1c and token 0x91 of codes 2/3, the
// landing pose choice 0019e9a0 for codes 9/10 and the immediate dead state of
// code 11, then the tail 001977f0(actor, 0) (with a held item through
// choose_secondary_mode). The multiplayer branch (005721c8 set), actors
// without the root object bytes +2d/+2c (or with +2e), +326b and codes
// outside 0..11 are not native and throw.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include "animation_controller.hpp"
#include "animation_root.hpp"
#include "secondary_component.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace omega {

// 0019e9a0(actor, name, &blend, a3, t0).
struct ActorLandingPoseInput {
    // Actor +2e64 set: the skeleton-space matrices of nodes 0..3 of the node
    // collection +2e68 (node +10..+4f, memory order), each taken to world
    // through the component matrix (0019f350: row_i(N) x C). Otherwise every
    // matrix is the component matrix itself.
    std::optional<std::array<std::array<float,16>,4>> node_matrices;
    std::array<float,16> component{};    // model transform component (+2e78) +10..+4f, memory order
    RootVector flor_orientation{0,0,0,1};   // 'flor' world orientation (00132400 -> 001323f0)
    bool on_back_a3{};                   // a3: forces the on-back variant (0 from 0018a950)
    bool last_frame_only_t0{};           // t0: the _LastFrameOnly variants (code 10)
    bool physical_present{};             // +3310
    float velocity_z_310{};              // physical +310 z (004e1d90 zero vector without +3310)
    float fall_height_30a8{};            // +30a8 (the recorded drop)
};
struct ActorLandingPose {
    std::string motion;                  // animBodyLandFaceDown / animBodyLandOnBack (+ _LastFrameOnly)
    bool face_down{};
    bool upright{};                      // |node 0 row 2 z| > 0.70710677 (0019eaf4)
    float heading_channel{};             // model channel 0x16 (0018d0f0)
    float blend{};                       // the install's transition duration
    std::string sound;                   // M_BODY_FALL (+30a8 <= 12) / M_BODY_DROP, bank 1
};
// The pure part of 0019e9a0. Its calls, in order: channel 0x16 set, the
// sound (MSGGAME_PLAYSND to 004ffb50, transport 0, at the 'flor' position
// from actor virtual +f4), then 00188fb0(actor, 0, 1).
ActorLandingPose choose_actor_landing_pose(const ActorLandingPoseInput& input);

struct ActorDeathActor {
    bool controller_is_player{};         // +58 virtual +54
    bool byte_14{};                      // +14
    std::uint8_t byte_326b{};            // +326b
    bool physical_present{};             // +3310
    bool secondary_present{};            // +3314 (the secondary component)
    std::uint8_t object_2c{},object_2d{},object_2e{};   // root object bytes
    std::array<std::int32_t,12> motion_override_34e4{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};   // 001b6198
    std::uint8_t inventory_mode_84{6};   // +3524 +84 (6: nothing held), read by 001977f0
    std::uint32_t flags_3204{};          // +3204: bits 3/4 (codes 0 and 1) and 5 (code 0) pick the crouch motions
    std::uint64_t receiver{};            // token receiver (the actor)
};
struct ActorDeathHost {
    std::function<std::uint32_t()> random;                      // 00127dd0
    std::function<void(std::int32_t line)> voice;               // 0018c770(actor, line): the player's voice line 8/9
    std::function<void(std::int32_t action)> self_action;       // action 0x24 kind 0, transport 1
    std::function<ActorLandingPoseInput()> landing_pose_input;  // read when 0019e9a0 runs
    std::function<void(std::size_t channel,float value)> set_channel;                      // 0018d0f0
    std::function<void(const std::string& sound)> sound;                                   // 0019e9a0 sound
    std::function<void(std::int32_t a1,std::int32_t a2)> landing_effect;                   // 00188fb0(actor, a1, a2)
    std::function<std::optional<std::uint32_t>(std::string_view)> node_id;                 // 0018c740 on +2ec0 +24
    std::function<ActorMotionRegistration(std::uint32_t)> resolve_id;                      // 0018c6b0 (+2ec0 +3c table)
    std::function<void(const MotionRequest&,MotionInstallMode)> install;                   // 003ad500
    std::function<void()> dead_state;                                                      // 001a3780
    std::function<void(std::uint8_t mode)> secondary_mode;                                 // 001cb120(+3314, mode)
    std::function<ActorMotionRegistration(std::string_view)> resolve_name;                 // 0018c5a0 (0019e670)
    // 001977f0(actor, 0) with a held item (+3524 +84 != 6): the actor fields
    // it reads, read after the death action set its actions (raise and +320c
    // are filled in by handle_actor_death).
    std::function<SecondaryModeInput()> secondary_input;
};
struct ActorDeathResult {
    bool handled{};                      // kind 2 past the early return
    bool already_dying{};                // action 0x1b or 0x1f was active: nothing done
    std::optional<std::uint8_t> physical_14;   // physical +14 = 2 (NPC or replica)
    std::string motion;                  // the installed motion (empty: unresolved, not installed)
    std::optional<ActorLandingPose> pose;
    bool dead_state{};                   // 001a3780 called directly (code 11)
    // Tail: 001977f0(actor, 0) stores +31b2 and asks 001cb120 for the
    // secondary mode; then +2dd0 = 0, +2dd4 = -1 (0015ac90), +2dec = 0 and
    // secondary +415 = 0.
    std::uint8_t byte_31b2{};
    std::optional<std::uint8_t> secondary_mode;
    std::int32_t word_2dd0{},word_2dd4{},word_2dec{};
};
// 0019e670(actor, 0) from 001a3050 (after 0019f450): with physical +dc
// nothing; otherwise, with action 0x1d or 0x1e (a fall while dying or dead,
// 001a1590), both are cleared, 0019e9a0(actor, ..., 0, 0) chooses the
// landing pose (channel 0x16, sound, 00188fb0(actor, 0, 1)) and the pose's
// motion is looked up by name (0018c5a0) and installed replacing with
// token 0x92 in slots 4, 5 and 6. A controller whose virtual +58 or +5c is
// true (the AI's) then gets +114 = 0 (0019e978): clear_ai_114, done by the
// caller after the install. 001b0a50 calls it with a1 = 1, which skips the
// +dc test (unsupported_dc false).
struct ActorDeadLandingResult {
    bool handled{};
    ActorLandingPose pose;
    std::string motion;                  // installed (empty: unresolved)
    bool clear_ai_114{};
};
// The whole of 0019e9a0(actor, name, &blend, a3, t0) on a death host: the
// pose input, the choice, channel 0x16, the sound and 00188fb0(actor, 0, 1).
ActorLandingPose run_actor_landing_pose(const ActorDeathHost& host,bool on_back_a3,bool last_frame_only_t0);
ActorDeadLandingResult land_dead_actor(ActorAnimationReceiverState& actions,bool physical_present,bool unsupported_dc,
    bool controller_ai,std::uint64_t receiver,const ActorDeathHost& host);
// 001cb120(secondary, mode): +313 = mode; +312 = mode unless +312 is 0x15,
// 0x16 or 0x18.
void apply_actor_secondary_mode(std::uint8_t& byte_312,std::uint8_t& byte_313,std::uint8_t mode);
ActorDeathResult handle_actor_death(ActorAnimationReceiverState& actions,std::int32_t kind,std::int32_t code,
    const ActorDeathActor& actor,const ActorDeathHost& host);
}
