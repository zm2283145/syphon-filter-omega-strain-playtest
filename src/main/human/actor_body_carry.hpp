#pragma once
// The body carry of actor message 001a92a0: the carrier's pickup (action
// 0x2b, 001abbe8, sent by the pickup menu's kind-3 entry 0025220c), its drop
// (0x2c lay down / 0x2d toss, 001ac360, from the pad), the body's own actions
// (0x2e picked up, 001acae0; 0x2f / 0x30 laid down / tossed, 001acd88) and
// the unlink 001a02c0 that 0019f890 runs when the carrier leaves action 21.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {
// The body 0040c540 resolves from the message's +2c key, as the carrier reads it.
struct ActorCarryBody {
    bool human{};                    // +4c == *0049d010 (the human actor type)
    std::int32_t mode_320c{};        // the highest priority action
    bool active{};                   // +2d && +2c && !+2e
    bool byte_14{};                  // +14
    bool backpack_3464{};            // +3464 != 0
    std::uint32_t flags_324c{};
    RootVector floor_position{};     // 00132400 -> 00132370: 'flor' (+3290) world position
    RootVector component_row2{};     // +50 transform: the matrix (at +10) row at +30 (00132900 refreshed)
};
struct ActorCarryPickupInput {
    std::uint8_t life_state{};       // virtual +108
    bool byte_14{true};
    std::int32_t mode_320c{};
    bool linked{};                   // 001adaf0(+3478): the +347c key still matches
    std::uint8_t kind_28{};          // message +28 (5 from the pickup menu)
    std::optional<ActorCarryBody> body;  // nullopt: 0040c540 found nothing
    bool controller_is_player{};     // +58 virtual +54
};
struct ActorCarryDropInput {
    std::int32_t action{};           // message +24: 0x2c or 0x2d
    std::uint8_t life_state{};
    bool byte_14{true};
    bool physical_dc{};              // +3310 present and its +dc
    bool linked{};
    std::uint32_t body_flags_324c{}; // the linked body's +324c (read only when linked)
    float edge_root_weight{};        // root +3294 +d0
};
struct ActorBodyCarryHost {
    // 001adab0(+3478, body): +3478 = body, +347c = its +c.
    std::function<void()> link;
    // The inactive body: its virtual +5c(1) (+14 set) or +20.
    std::function<void()> activate_body;
    // 00143b60 on +3524: holster the held item (the switch gate 00143bc0).
    std::function<void()> holster;
    // 00256120 on the body's backpack +3464: its box removed and released.
    std::function<void()> hide_backpack;
    // 001ad9c0 on root +3294 ('edge'): world placement (00191480), +a0 = 1, +160 = 0.
    std::function<void(const RootParentTransform&)> place_edge_root;
    // 00132400 -> 001910a0: root +3290 ('flor') world placement; 00191100 on
    // +3294 with it, then +160 = 0 (the drop with a zero edge weight).
    std::function<RootParentTransform()> floor_root_world_placement;
    // 001ad8c0(+2fc0, body +2fc0): the body becomes this actor's relation child.
    std::function<void()> relate;
    // For each relation child: 001a6d40(own 'edge', the child's 'edge', 1).
    std::function<void()> attach_child_edges;
    // 003c9420 transport 1 of the action message (vtable 004e0840, kind 0,
    // event byte 1) to the linked body: 001a92a0 runs at once.
    std::function<void(std::int32_t)> body_action;
    // AIBodyMovedMsg (004ff2b0, vtable 004dcc70): +24 = this actor (+58 +30),
    // +28 = PickedUp, event byte 2, queued (transport 0) to the body.
    std::function<void(bool)> notify_body_moved;
    std::function<ActorMotionRegistration(std::string_view)> resolve;   // 0018c5a0 on +2ec0
    std::function<ActorMotionRegistration()> retained;                  // +326c (0018a420 tag 2)
    std::function<void(const MotionRequest&,MotionInstallMode)> install; // 003ad500
};
struct ActorBodyCarryResult {
    bool handled{};                   // false: an exit before any effect
    std::vector<std::string> motions; // installed names in order ("" = retained)
};
// 001abbe8: exits (life 2/3; +324c bit 1; +14 with a mode; linked; +28 != 5;
// no body; not human; +14 and the body's mode not 0x1f), then the link, the
// body's activation, the holster, the backpack hide, the 'edge' placement at
// the body's 'flor' with yaw atan2(r.x, -r.y) - pi/2 of the body's transform
// row r (00121ab8, sinf/cosf 00140e70/00140e80), the relation, the children's
// edges under the own edge, the body action 0x2e, the player's AIBodyMovedMsg
// (PickedUp 1), "pickupbody_pt1" (replace, 0.5 s, tokens {0x71, 0x72, -,
// 0x74, -, 0x75, 0x76}, phase hook [0.12, 0.12] -> 0x73) and
// "switchCarryBody_Carrier" (queue, tokens {0x77, 0x78, -, 0x79, -, 0x7a,
// 0x7b}). The multiplayer mirror (005721c8) is not taken in single player.
ActorBodyCarryResult handle_actor_carry_pickup(std::uint32_t flags_324c,std::uint64_t receiver,
    const ActorCarryPickupInput&,const ActorBodyCarryHost&);
// 001ac360: exits (life 2/3; +324c bit 4; +14 and bit 2; +14 and +3310 +dc),
// then 001ac8c0 (+14 and +324c bit 3 clear, nothing linked, or +14 and the
// body's bit 6 clear): with a link the retained motion through the actor
// override (0.5 s, replace, no tokens). Otherwise the edge placed at 'flor'
// when its weight is 0, the children's edges re-attached, the carrier motion
// ("animLayDownBody_Carrier" / "animTossBody_Carrier", replace, 0.5 s,
// tokens {0x7c, 0x7d, -, 0x7e, -, 0x7f, 0x80}), the body action 0x2f / 0x30
// and the retained motion queued directly ({0.5, 0}, no tokens).
ActorBodyCarryResult handle_actor_carry_drop(ActorMotionTransitionState& transition,std::uint32_t flags_324c,
    std::uint64_t receiver,const ActorCarryDropInput&,const ActorBodyCarryHost&);
// The body's actions (receiver = the body): 0x2e (001acae0; exits with +324c
// bit 5) "pickupbody_pt2" (replace, tokens {0x81, 0x82, -, 0x83, -, 0x84,
// 0x85}) and "switchCarryBody_Body" (queue, tokens {0x86, 0x87, -, 0x89, -,
// 0x88, 0x8a}); 0x2f / 0x30 (001acd88; exits with bit 7) "animLayDownBody_Body"
// / "animTossBody_Body" (replace, tokens {0x8b, 0x8c, -, 0x8d, -, 0x8e, 0x8f})
// then "animBodyFinalPoseAfterLaidDown" / "animBodyFinalPoseAfterTossed"
// (queue, {0.5, 0}, no tokens). Only resolve and install are used.
ActorBodyCarryResult handle_actor_body_action(std::int32_t action,std::uint32_t flags_324c,std::uint64_t receiver,
    const ActorBodyCarryHost&);
struct ActorCarryUnlinkInput {
    bool linked{};
    bool controller_is_player{};
    bool byte_14{true};
    // The action history after 0019f6c0's shift: +3254 (this frame's +324c)
    // and +325c (the previous frame's).
    std::uint32_t flags_3254{},flags_325c{};
};
struct ActorCarryUnlinkHost {
    std::function<void()> detach_relation_roots;   // 001a08d0(+2ec0)
    std::function<void()> clear_relation;          // 001a07f0(+2fc0)
    std::function<void(bool)> notify_body_moved;   // AIBodyMovedMsg, PickedUp 0
    // cTurnOnWeaponPickupMsg (004ee960, vtable 004da890) queued to the body;
    // its handler 001b48f8 runs 001a3250.
    std::function<void()> turn_on_weapon_pickup;
    // The body's landing (001a0520..001a06dc), all on the body: 0019e9a0(body,
    // name, &blend, 0, 0) (channel 0x16, the sound, 00188fb0(body, 0, 1))
    // returning the motion name (its blend is unused), 0018c5a0 on the body's
    // +2ec0, then 003ad500 on its controller.
    std::function<std::string()> body_landing_pose;
    std::function<ActorMotionRegistration(std::string_view)> resolve_body;
    std::function<void(const MotionRequest&,MotionInstallMode)> install_body;
    std::function<void()> unlink;                  // +3478 = 0, +347c = -1
};
// 001a02c0. Unless the drop finished last frame (+325c bit 4 without +3254
// bit 4), the body lands: 0019e9a0 picks the pose and its motion replaces
// with the fixed transition {0.5, 0.5, 2, 2}, limit -1, phase 0, no tokens.
void unlink_actor_body(const ActorCarryUnlinkInput&,const ActorCarryUnlinkHost&);
// 0019f890's bit-21 leave: 0019fd60 on the own 'flor' then 'edge' (their
// children detached keeping the world), 00143cf0 on +3524, then 001a02c0.
struct ActorCarryLeaveHost {
    std::function<void()> detach_floor_children,detach_edge_children,restore_inventory,unlink;
};
void leave_actor_carry(const ActorCarryLeaveHost&);
// 0019d950, the channel body 00199f30 runs (0019a988..0019a9b4) for +324c bit 3
// (carrying) or bit 6 (carried) outside modes 7 / 8 / 16. Only bit 3 acts.
struct ActorCarryPairInput {
    std::uint32_t flags_324c{};
    bool linked{};                   // 001adaf0(+3478)
    float speed_31d0{},desired_y_31c4{},turn_31d4{};
};
struct ActorCarryPairPlan {
    bool carrying{};                 // bit 3: otherwise 0019d950 returns at once
    bool linked{};
    // Linked: on the carrier, then the body: 0019d6c0 selector 4 and 0019d5a0
    // channel 14 (duration *0049cfb8, damping 6.5, unwrapped), then +3208 bit 0.
    std::uint32_t selector_4{};
    float channel_14{};
    bool pending_bit0{};             // unlinked: cleared on the carrier only
    // Then 0019dd80(actor, 1, 0, 0, 0): the carrier, then (linked) the body.
};
ActorCarryPairPlan plan_actor_carry_pair(const ActorCarryPairInput&);
}
