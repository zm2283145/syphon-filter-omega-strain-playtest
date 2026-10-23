#pragma once
// Actor message 001a92a0 actions 0x25/0x26/0x27 (0018d2c0): the object
// actions a script's game::PerformAction_WithObject (003e6540 -> 003e67c0)
// sends to an actor: push a button (0x25/0x26, DATA_ID_PushElevatorButton) or
// check a body for a pulse (0x27, dataCheckBodyForPulse) at an object. The
// object (an InteractGOBJ) is told the outcome through its CAS receiver: the
// motion's tokens 0x67/0x68/0x6a send it codes 0xe/0xf/0xd (actor animation
// receiver), a rejection sends 0xd here. The 4/5-argument forms (003e64d0 /
// 003e6450) send a named motion through 003e6910 (kind 10), which the actor
// turns into a kind-7 message with the motion's node id.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include "actor_physical_state.hpp"
#include "animation_root_clip.hpp"
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace omega {
// The message 003e67c0 builds (key 0x40 at +1c): +24 action (sign-extended
// byte), +28 kind (7 from 003e67c0; kind 5 resolves the object always), +2c
// the object's +c key (-1 without one; resolved by 0040c540 into object),
// +30 motion id (-1: the action's own motion), +34 time (003e67c0: -1.0 stays,
// -2.0 becomes FLT_MAX), +38 byte.
struct ActorObjectActionMessage {
    std::int32_t action{};
    std::uint32_t kind{7};
    std::optional<std::uint64_t> object;
    std::int32_t motion_id{-1};
    float time{-1.0F};
    std::uint8_t byte_38{};
};
// The message 003e6910 builds (kind 10, key 0x40 at +1c, +20 = 1, sent with
// transport 0): +24 the action as a signed byte, forced to 0x25 when the name
// is not empty (003e69e8); +28 = 0xa; +2c the object's +c key (-1 without
// one); +30..+5f the name lowercased in place (signed bytes 0x41..0x5a get
// +0x20, 003e6a28..), its first 47 bytes, zero padded with byte 47 = 0
// (003e6ad8..); +60 the time (-1.0 stays, -2.0 becomes FLT_MAX, others
// unchanged, 003e6968..003e69a4); +64 the byte t0 (0 from the 4/5-argument
// forms).
struct NamedObjectActionMessage {
    std::int32_t action{};
    std::optional<std::uint64_t> object;
    std::array<char,48> name{};
    float time{-1.0F};
    std::uint8_t byte_64{};
};
NamedObjectActionMessage build_named_object_action(std::int32_t action,std::optional<std::uint64_t> object,
    std::string_view name,float time,bool byte_64);
// 003e67c0(actor, action, object, a3, f12): the kind-7 message (+24 the action
// as a signed byte, +2c the object key or -1, +30 = -1, +34 the time with
// -1.0 kept and -2.0 as FLT_MAX, +38 the byte a3).
ActorObjectActionMessage build_object_action(std::int32_t action,std::optional<std::uint64_t> object,float time,std::uint8_t byte_38);
// game::PerformAction (2/3/4 script arguments: 003e6650 / 003e6600 /
// 003e65a0; no object). The wrappers pass the low byte of the action word
// (lb) and the time (-1.0 for 2/3 arguments); 003e6770 sends the kind-7
// message (003e67c0, a3 = t0 = 0) for an empty name (lb name[0] == 0; the
// 2-argument form, 003e6750, always) and the kind-10 message (003e6910,
// action 0x25, t0 = 0) otherwise. Both go with transport 0 to the actor or,
// when its type cell is the GOBJ controller's (*0x55a840: an AI or player
// component), to its owner +30 (EE goldens, tests/perform_action_goldens.hpp).
using PerformActionMessage=std::variant<ActorObjectActionMessage,NamedObjectActionMessage>;
PerformActionMessage perform_action_message(std::uint32_t action_word,std::optional<std::string_view> name,float time);
// 0018d2c0's kind-10 prelude (0018d2f4..0018d508, before the ability gate):
// taken only for the word +24 = 0x25/0x26/0x27 and the byte +28 = 0xa
// (otherwise nullopt: the message falls through to 0018d510). The name's
// first byte 0 (0018d3e0) gives id -1 without a lookup; otherwise 0018c740
// finds the 48-byte key in the model's +24 map (find returns nullopt when it
// is absent: the original then uses id 0, which the port does not host and
// throws). The resend is the kind-7 message {sign-extended byte of +24, +2c,
// id, +60 unchanged, +64 as +38}, delivered synchronously (003c9420
// transport 1) before 0018d2c0 returns (0018d504 -> 0018ff50).
std::optional<ActorObjectActionMessage> named_object_action_resend(std::uint32_t action_word,std::uint8_t kind,
    std::optional<std::uint64_t> object,const std::array<char,48>& name,float time,std::uint8_t byte_64,
    const std::function<std::optional<std::uint32_t>(const std::array<char,48>&)>& find);
// The resolved object's links (InteractGOBJ constructor 00216ae0: +a8 and +ac
// are its last two references): +a8 a node (position +80, yaw +8c), +ac the
// world object 0018fe68 pushes for a player (0 without one).
struct ActorObjectActionNode {
    std::array<float,3> position{};
    float yaw{};
};
struct ActorObjectActionTarget {
    std::optional<ActorObjectActionNode> node;
    std::uint64_t world_object{};
};
struct ActorObjectActionHost {
    // 0018c5a0 + 001914a0 on actor +2ec0 and the leaf clip header word +24
    // (SoloActorAnimation::motion_anchor_key): nullopt for a non-leaf node.
    std::function<std::optional<RootAnchorKey>(std::string_view)> anchor_key;
    // 00191100 world placement of root +3294 ('edge'), then root +160 = 0.
    std::function<void(const RootParentTransform&)> place_edge_root;
    // 001910a0 world placement of root +3290 ('flor').
    std::function<RootParentTransform()> floor_root_world_placement;
    // Name lookup (0018a420 tag 0) and actor +326c (tag 2).
    std::function<ActorMotionRegistration(std::string_view)> resolve;
    // An explicit motion id (+30 != -1, 0018da90): 0018c6b0 (the model's
    // node *(+3c)[id], unchecked in the original: the host throws past the
    // node count), 001914a0 and the leaf clip header word +24 (nullopt: not a
    // leaf); the registration 0018a890 with tag 1 and +40 = id (0018e1a0).
    std::function<std::optional<RootAnchorKey>(std::uint32_t)> anchor_key_by_id;
    std::function<ActorMotionRegistration(std::uint32_t)> resolve_id;
    std::function<ActorMotionRegistration()> retained;
    // 003ad500 -> 003ad680 on actor +2ef4.
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
    // 0018fee8: CAS code 0xd sent to the object at once (transport 1, +28 =
    // -1, +2c = the actor, event byte 1).
    std::function<void(std::uint64_t)> reject;
    // The +58 component's virtual +54 (nullptr: the actor has no +58).
    std::function<bool()> controller_is_player;
    // 00131320(004ffb50, object +ac).
    std::function<void(std::uint64_t)> push_world_object;
};
struct ActorObjectActionResult {
    bool handled{};
    bool rejected{};                   // the ability gate refused (code 0xd sent)
    RootAnchorKey anchor{};
    std::vector<std::string> motions;  // installed names in order ("" = retained, "#id" = by id)
    bool pushed_world_object{};
};
// 0018d2c0 for actions 0x25/0x26/0x27:
//   gate: priority actions 27, 31 and 23 (+3210) all clear, otherwise the
//     object (when resolved) gets code 0xd and nothing else happens;
//   object node (+a8): 0015c170 builds its matrix (axis (0, 0, 1) through
//     00132130, yaw +8c) with the node position;
//   priority action 20 set; +324c bit 0 = (kind 7 and byte +38 != 0); the
//     motion time (kind 7: cvt.s.w(cvt.w.s(+34)), else -1) is the request
//     limit +98;
//   the motion: the action's own (+30 = -1: 0x27 dataCheckBodyForPulse,
//     else DATA_ID_PushElevatorButton, by name) or node +30 (0018da90, the
//     tag-1 registration at 0018e0c8; otherwise the same install);
//   anchor of the motion's leaf clip: 'edge' with a node: the edge root is
//     placed at the node (orientation from the node rows), tokens 0x63/0x66;
//     'edge' without one: the edge root takes the 'flor' world placement,
//     tokens 0x64/0x65; 'flor' without a node: tokens 0x63/0x65;
//   the motion through the actor override {0.5, 0.5} replacing (tokens {s1,
//   s0, -, 0x68, -, 0x69, 0x6a}, phase hook [0.5, 0.5] sending 0x67), then the
//   retained motion queued;
//   tail 0018fe68: +3474 = the object; with the object's +ac, a player +58
//   component, +14 set and +326b clear: 00131320 pushes +ac, +326b = 1.
// Not hosted (throw before any write): a motion id below -1, a 'flor' anchor
// with a node (00192fe0), another anchor key, a kind other
// than 5/7, a time outside the integer range, another action.
ActorObjectActionResult handle_actor_object_action(ActorAnimationReceiverState& actions,
    ActorMotionTransitionState& transition,std::uint64_t receiver,const ActorObjectActionMessage&,
    const ActorObjectActionTarget&,const ActorObjectActionHost&);
// 0018d2c0's buddy-climb branches, the actions the InteractGOBJ pairing
// 00216230 sends (003e67c0, kind 7, motion id -1). The same ability gate and
// node matrix as above; then, with no priority action, +324c or time write:
//   0x31 / 0x35 (0018e688, idle below / on top): animBuddyClimbIdle /
//     animBuddyClimbIdle_SecondHalf replacing, tokens {0x6b / 0x6c, 0x6d, -,
//     0x6e, -, 0x6f, 0x70};
//   0x33 / 0x37 (0018ee50, follower): buddyclimb_secondguyup /
//     animBuddyClimbFollower_SecondHalf replacing (tokens {0x3d, 0x3e, -,
//     0x3f, -, 0x40, 0x41}), then DATA_ID_JumpGrabLedge ({0x14, 0x15, -, 0x16,
//     -, 0x17, 0x18}) and sliderHang ({0x19, -, -, 0x1a, -, 0x1b, 0x1c}) queued;
//   0x34 / 0x38 (0018f7c8, leader): buddyclimb_firstguyup /
//     animBuddyClimbLeader_SecondHalf replacing (tokens {0x3d, 0x42, -, 0x43,
//     -, 0x44, 0x45}), then the retained motion +326c queued (token block
//     inactive);
// every install direct (0018c5a0 + 003ad500, the {0.5, 0.5} transition, limit
// -1, phase 0, the override +2ef8 untouched; an unresolved name is skipped).
// With the object's node, after the installs: the edge root at the node
// (00191100, +160 = 0) and, for the idle branch with a physical component,
// physical mode bit 5 (0018ed9c). The 190f50 / 1901c0 / 17cd90 placement
// computations between the installs are dead stores. Tail 0018fe68 as above.
// pending_mode is physical +28/+2c (nullptr without a physical component).
// Not hosted (throw): another action, kind other than 5/7, an explicit motion
// id, a retained motion without a node (read unchecked).
ActorObjectActionResult handle_actor_buddy_climb_action(ActorAnimationReceiverState& actions,ActorPhysicalMode* pending_mode,
    std::uint64_t receiver,const ActorObjectActionMessage&,const ActorObjectActionTarget&,const ActorObjectActionHost&);
// 001a92a0 actions 0x32 / 0x36 (001ad3a8, the pairing's cancel): with
// priority actions 27 and 31 clear, the retained motion +326c (1ae270 node,
// 1ae260 selector) replacing with the direct {0.5, 0.5, 2, 2} transition
// (18a380), limit -1, phase 0, no tokens; otherwise nothing. Needs only
// host.retained and host.install.
ActorObjectActionResult handle_actor_buddy_climb_cancel(const ActorAnimationReceiverState& actions,std::uint64_t receiver,
    const ActorObjectActionMessage&,const ActorObjectActionHost&);
// 0015c170 rows: (c, s, 0), (-s, c, 0), (0, 0, c + (1 - c)) with the
// executable's sinf/cosf (00121928/00121688), as 00132130 computes them for
// the axis (0, 0, 1).
std::array<RootVector,3> actor_object_node_rows(float yaw);
}
