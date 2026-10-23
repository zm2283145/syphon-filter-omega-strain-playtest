#pragma once
// Actor message 001a92a0 action 0x2a (001ad490 -> 001930a0): the CAS_ACTION
// kind 14 sent by the edge request decision 001e1600 for the winning slot.
// It places the 'edge' root at the grab point, binds moving-support slot 1 to
// the edge's owner object and installs the climb motions for the slot mask.
#include "actor_animation_receiver.hpp"
#include "actor_motion.hpp"
#include "actor_physical_state.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {
// Message fields +28 (kind), +30 (mask), +34 (object identity or -1) and the
// object-relative grab placement +40/+50 written by 001959c0.
struct ActorLedgeActionMessage {
    std::uint32_t kind{14};
    std::uint32_t mask{};
    std::optional<std::uint64_t> object;
    RootParentTransform placement;
};
// Physical moving-support slot (+1f0+4i object, +200+20i local placement,
// +260+20i previous owner placement, +2c0+i enabled byte).
struct ActorLedgeSupportBinding {
    std::uint64_t object{};
    RootParentTransform local,previous_owner;
    bool enabled{};
};
struct ActorLedgeActionHost {
    // 0040c540 lookup then component +50 (00133330) and its 001831b0 matrix;
    // nullopt without a component (the placement is then kept as world).
    std::function<std::optional<std::array<float,16>>(std::uint64_t)> owner_matrix;
    // 00191100 world placement of root +3294 ('edge'), then root +160 = 0.
    std::function<void(const RootParentTransform&)> place_edge_root;
    // Physical moving-support slot write (slot 1 here).
    std::function<void(std::size_t,const ActorLedgeSupportBinding&)> bind_support;
    // 00132400 -> 00132370 world positions of roots +3290 ('flor') and +3294.
    std::function<RootVector()> floor_root_world_position,edge_root_world_position;
    // Name lookup (0018c5a0 / 0018a420 tag 0) with the node's own selector.
    std::function<ActorMotionRegistration(std::string_view)> resolve;
    // Actor +326c through 0018a420 tag 2.
    std::function<ActorMotionRegistration()> retained;
    // 003ad500 -> 003ad680 on actor +2ef4.
    std::function<void(const MotionRequest&,MotionInstallMode)> install;
    // Bits 5/6 (jump to grab): 001910a0 world placement of root +3294 after
    // the placement above, and the immediate self-sent CAS_ACTION (type
    // 004e00e0, action 0x14, kind 14, mask word 0, object -1, event byte +20
    // = 1, transport 1) carrying it unchanged through 001959c0 (object 0).
    // The actor handles it at once (001a92a0 -> 001914c0).
    std::function<RootParentTransform()> edge_root_world_placement;
    std::function<void(const RootParentTransform&)> jump_to_grab;
    // Bit 7 (air catch): 001a1c80 on the actor.
    std::function<void()> catch_ledge;
    // Bit 12 (ladder mount): 00191100 world placement of root +3298 ('ladd'),
    // then root +160 = 0; and with a physical component the ladder reference
    // +980 (its z lane is +988) = the grab position.
    std::function<void(const RootParentTransform&)> place_ladd_root;
    std::function<void(const RootVector&)> set_ladder_reference;
};
struct ActorLedgeActionResult {
    bool handled{};                 // false: rejected by the 001ad490 gates
    std::vector<std::string> motions; // installed names in order ("" = retained)
};
// 001ad490 gates (life state 2/3, edge root weight +d0 nonzero, kind not 14
// ignore the message; the multiplayer mirror send is not taken in a
// single-player game) and the 001930a0 body for the non-ladder masks:
//   bit 1 with bit 8: animClimbDown8 -> DATA_ID_JumpGrabLedge -> sliderHang
//     (direct installs, 0.5 s transitions);
//   bit 1: animClimbDown8 (bit 2) or animClimbDown4, then the retained motion
//     (actor transition override 0.5 s);
//   bit 3: DATA_ID_Climb4 when edge - flor height < 4.5 (004a03b8), otherwise
//     DATA_ID_HangClimbUp with a height-dependent bias, then the retained
//     motion;
// each enabling priority action 1 and physical mode bits 5, 3 (and 2 for
// bit 1) through 00195ce0. Side bits 16/17 lower the grab point by 1.0.
// Without bits 1/3: bits 5/6 send the jump-to-grab action 0x14 to the actor
// itself (00195820..001958ac), otherwise bit 7 calls the air catch 001a1c80
// (001958b8); neither enables a priority action or physical mode here.
// Bit 12 (ladder mount, 001932f8..193ea4, then the common exit): support
// slot 2 bound like slot 1 (physical only), the 'ladd' root placed at the
// grab placement, then through the actor override (0049d038: {0.5, 0.5, 2, 2})
// the GetOn motion replacing (bit 13 animLadderGetOnTop, else bit 14
// animLadderGetOn_LeftHand, bit 16 animLadderGetOnRight_LeftHand, bit 17
// animLadderGetOnLeft_LeftHand; tokens {0x46, 0x47, -, 0x48, -, 0x49, 0x4a};
// phase hooks sending 0x5c) and sliderLadderClimbStop_LeftHand queued (tokens
// {0x4b, 0x4c, -, 0x4d, -, 0x4e, 0x4f}), then +980 = grab position (physical
// only). No priority action or physical mode changes. A ladder mask without
// bits 13/14/16/17 (an empty node name; unreachable from detection) throws.
// An object with a transform component composes the local placement with its
// owner placement (195ee0 / 195de0) into the world grab placement and binds
// the support slots with that owner placement as the previous owner. A
// jump/catch/ladder mask without its host throws.
// pending_mode is physical +28/+2c (nullptr without a physical component).
ActorLedgeActionResult handle_actor_ledge_action(ActorAnimationReceiverState& actions,
    ActorPhysicalMode* pending_mode,ActorMotionTransitionState& transition,std::uint64_t receiver,
    std::uint8_t life_state,float edge_root_weight,const ActorLedgeActionMessage&,const ActorLedgeActionHost&);
// 00195ce0(mode, bit): set the bit in +2c (pending bits) and rescan bits
// 12..0 for the highest into +28 (-1 without one).
void set_actor_physical_mode_bit(ActorPhysicalMode& mode,unsigned bit);
}
