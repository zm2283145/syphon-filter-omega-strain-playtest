#pragma once
#include "animation_root.hpp"
#include "actor_animation.hpp"
#include "animation_controller.hpp"
#include "actor_motion.hpp"
#include "actor_animation_receiver.hpp"
#include "actor_edge_types.hpp"
#include "actor_physical_state.hpp"
#include "actor_response_setup.hpp"
#include <functional>
#include <string_view>
#include <cstdint>
#include <optional>

namespace omega {
struct ActorLadderContacts {
    float upper_990{},lower_998{},reference_988{};
    bool contact_99c{},contact_99d{},contact_99f{};
};
struct ActorLadderIntentInput {
    std::optional<ActorLadderContacts> physical;
    std::uint32_t active_actions{};
    float vertical{},slide_amount{};
    bool slide_enabled{},dismount_requested{};
};
struct ActorLadderIntent {
    bool slide{},up{},down{};
    bool requested_slide{},requested_up{},requested_down{};
};
// Original004762a0. The first three results include contact inhibition;
// the request results precede that final inhibition.
ActorLadderIntent detect_actor_ladder_intent(const ActorLadderIntentInput& input);
bool actor_ladder_channel_arrived(float previous,float current,float target,float threshold);
struct ActorLadderTuning {float rung_spacing{},lower_offset{};};
struct ActorLadderProximity {bool upper{},lower{};};
ActorLadderProximity actor_ladder_proximity(const std::optional<ActorLadderContacts>& physical,
    float world_z,const ActorLadderTuning& tuning);
float actor_ladder_slide_amount(float value,float elapsed,bool sliding,bool enabled);
// Original00475d80 after its target helper. Target resolution and applying the
// returned world position are explicit host responsibilities.
RootVector blend_actor_ladder_position(const RootVector& current,const RootVector& target,
    float elapsed,const std::optional<ActorLadderContacts>& physical,const ActorLadderTuning& tuning);
struct ActorLadderSnapshot {
    ActorAnimationSnapshot actor;
    std::optional<ActorLadderContacts> physical;
    RootVector reference_position{0,0,0,1}; // Physical+980, Z must match reference_988.
    RootVector world_position{0,0,0,1},edge_position{0,0,0,1}; // Actor+3298/+3294.
    RootVector world_orientation{0,0,0,1};
    float previous21{},current21{},target21{},slide_amount{};
    bool slide_enabled{},dismount_requested{},exit_contact{};
    std::uint8_t exit_kind{};
    std::uint32_t exit_object{},physical_modes{};
};
enum class ActorLadderMotion {up_left,up_right,down_left,down_right,slide,stop_left,stop_right};
struct ActorLadderPatch {
    ActorAnimationPatch actor;
    std::optional<float> slide_amount;
    std::optional<bool> dismount_requested;
    std::optional<std::uint32_t> active_actions,physical_modes;
    std::optional<std::int32_t> highest_action,highest_physical_mode;
};
bool actor_ladder_left_hand(const ActorLadderSnapshot& input,const ActorLadderTuning& tuning,bool account_exit=false);
enum class ActorLadderExitMotion {top_left,top_right,bottom_left,bottom_right,
    side_left_left,side_left_right,side_right_left,side_right_right};
struct ActorLadderBodyFields {
    std::uint32_t pending_3208{};      // +3208 (bit 0 set by the stop/align/idle paths)
    float slide_31d8{};                // +31d8 slide amount
    bool slide_enabled_31b5{};         // +31b5
    bool exit_requested_31b6{};        // +31b6
    float vertical_31c8{};             // +31c8 desired direction z
};
struct ActorLadderPhysicalView {
    const ActorLadderContactFields* contacts{}; // +990/+998/+99c..+9a5
    RootVector reference_980{0,0,0,1};          // +980 (z = +988)
    ActorPhysicalMode* pending{};                // +28/+2c (mode bit 8 while climbing)
};
struct ActorLadderChannel {float previous{},current{},target{};};
// 00472e10 type 0 (kind 0, actions 30..35 through 001a92a0 -> 001ab990):
// select the exit and latch it. Gates: +31b6 clear, mode +320c 16, actions
// 12 and 15 clear. 35 sends action 0x24 to the actor (immediate, kind 0)
// when +9a4 is set and stops. Otherwise top (+9a0, or +3248 bit 26 with
// channel 21 below 0.15) for 32/30, bottom (+99e, or bit 25 likewise) for
// 31/30, and with +99f the left (+9a1) for 33/30 or right (+9a2) side for
// 34/30. A selection latches +31b6 first, then reads the hand parity with
// the exit inversion (00476120(actor, 1)) and stores the node-name map value
// (model +0x24, 0018c740; 0 when absent) at +3514 and the kind at +3518: top
// 4, bottom 2, side 3.
struct ActorLadderExitFields {
    bool requested_31b6{};
    std::uint32_t node_3514{};
    std::int8_t kind_3518{};
};
struct ActorLadderExitHost {
    std::function<ActorLadderChannel(std::size_t)> channel;
    std::function<RootVector(ActorRoot)> root_world_position;
    std::function<std::optional<std::uint32_t>(std::string_view)> find_node;  // 0018c740 on model +0x24
    std::function<void(std::int32_t)> send_self;                               // 003c9420 immediate, kind 0
};
void request_actor_ladder_exit(const ActorAnimationReceiverState& actions,ActorLadderExitFields& fields,
    const ActorLadderPhysicalView* physical,std::int32_t action,const ActorLadderExitHost& host);
// 004728d0, the dismount (from 00474050 while +31b6 is set). Without bounded
// contacts or with +99c[+3518] clear nothing is sent. For bottom (kind 2) and
// top (kind 4) exits the actor must be within 0.75 rungs (004a03c0) of
// +998+7 / +990, or be aligned there (bit 25 / 26) with channel 21 below
// 0.15. A player component (+58 virtual +54) needs actor +14 set. The
// message (type 004e00e0, kind 14, flags +1c 0x40, sent to the actor at once)
// carries action 0x1f (bottom: z = +998+7, -1 when aligned), 0x20 (top: z =
// +990) or 0x1e (other: the exact rung target 00475ee0(.., 1)), node +3514,
// no object (-1) and the 'ladd' root world orientation.
struct ActorLadderExitMessage {
    std::int32_t action{};
    std::uint32_t node{};
    std::optional<std::uint64_t> object;
    RootParentTransform placement;
};
struct ActorLadderDismountInput {
    bool player_component{};    // +58 virtual +54
    bool actor_byte_14{};       // actor +14
};
struct ActorLadderDismountHost {
    std::function<ActorLadderChannel(std::size_t)> channel;
    std::function<RootParentTransform(ActorRoot)> root_world_placement;
};
std::optional<ActorLadderExitMessage> dismount_actor_ladder(const ActorAnimationReceiverState& actions,
    const ActorLadderExitFields& fields,const ActorLadderPhysicalView* physical,const ActorLadderDismountInput& input,
    const ActorLadderDismountHost& host);
// 00472e10 type 14 (kind 14, through 001a92a0): life state 2/3 ignore it.
// The 'edge' root takes the message placement (00191100, root +160 = 0) and,
// with a physical component, support slot 1 is bound to it. Through the actor
// override {0.5, 0.5, 2, 2} (0049d038), the numeric node of the message
// (0018a420 tag 1) replaces (tokens {0x62 for action 0x20 else 0x5d, 0x5e, -,
// 0x5f, -, 0x60, 0x61}) and the retained motion +326c is queued (no tokens).
// An object reference with a transform component (195ee0/195de0) is not
// native; node 0 (the missing-name value of type 0) throws.
struct ActorLadderInstallHost {
    std::function<void(const RootParentTransform&)> place_edge_root;
    std::function<void(std::size_t,const RootParentTransform& local,std::uint64_t object)> bind_support;
    std::function<ActorMotionRegistration(std::uint32_t)> resolve_id;    // tag 1
    std::function<ActorMotionRegistration()> retained;                    // tag 2, +326c
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
};
void install_actor_ladder_exit(std::uint8_t life_state,bool physical_present,const ActorLadderExitMessage& message,
    ActorMotionTransitionState& transition,std::uint64_t receiver,const ActorLadderInstallHost& host);
// 001ab9a0, actor action 0x24 (36): with action 16, the retained motion +326c
// replaces (when the mode is 16; transition 0018a380(0049d038), no tokens),
// then actions 12..16 and physical bits 6..9 are cleared (the change word
// +3218 is left as it is).
struct ActorLadderCancelHost {
    std::function<ActorMotionRegistration()> retained;
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
};
void cancel_actor_ladder(ActorAnimationReceiverState& actions,ActorPhysicalMode* physical,const ActorLadderCancelHost& host);
RootVector actor_ladder_target_position(const ActorLadderSnapshot& input,const ActorLadderTuning& tuning,bool exact_bounds);
// 00474050, the actor mode-16 body (called from 0019a978 in the channel
// stage). It works on the live actor fields: installs deliver their cancel
// tokens synchronously through the host, and everything read afterwards is
// read again (as the original reads memory). The boundaries are the
// original's calls:
//   neutral_channels     0019dd80(actor, 0, 0, 0, 0)
//   channel(i)           channel record i of +2ed0 (+4 previous, +8 current, +0x10 target)
//   set_channel          0018d0f0 (channel 18 = slide amount)
//   set_channel_target   0019d5a0 (channel 21: target, duration 0.5, damping 0, unwrapped)
//   root_world_position  00132400 -> 00132370 of 'edge' (+3294) / 'ladd' (+3298)
//   set_root_base        001e1f10 (00475c00 on 'edge', 00475d80 on 'ladd')
//   sound                00188f00(actor) when channel 21 arrives (00475b40, 0.2)
//   dismount             004728d0(actor)
//   resolve              0018a420 tag 0 (sliderLadderSlide, sliderLadderClimbStop_*)
//   resolve_root         001badd0 on the model's named roots (+0x44), then 0018a420
//                        tag 2 (blendLadderClimb{Up,Down}_{Left,Right}Hand)
//   install              003ad500, mode 1 (replace)
// The helpers 004762a0, 00476120, 00475ee0, 00475d80, 00475c00, 00475bb0 and
// 00475b40 and the proximity test are part of the body.
struct ActorLadderBodyHost {
    std::function<void()> neutral_channels;
    std::function<ActorLadderChannel(std::size_t)> channel;
    std::function<void(std::size_t,float)> set_channel;
    std::function<void(std::size_t,float,float,float,bool)> set_channel_target;
    std::function<RootVector(ActorRoot)> root_world_position;
    std::function<void(ActorRoot,const RootVector&)> set_root_base;
    std::function<void()> sound;
    std::function<void()> dismount;
    std::function<ActorMotionRegistration(std::string_view)> resolve,resolve_root;
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
};
// physical is null without a physical component (+3310). transition is the
// actor override +2ef8 (set to {0.2, -0.2, 5, -5} before each install and
// consumed when the lookup resolves).
void run_actor_ladder_body(ActorAnimationReceiverState& actions,ActorLadderBodyFields& fields,
    const ActorLadderPhysicalView* physical,ActorMotionTransitionState& transition,std::uint64_t receiver,
    float elapsed,const ActorLadderBodyHost& host);
}
