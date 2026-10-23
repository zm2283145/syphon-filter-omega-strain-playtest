#pragma once
// The actor's aim and look actions (cases of the action handler 001a92a0;
// COMBAT.md §4 "Action handlers"), interpreter-verified:
//   0x39 kind 1    001977f0(actor, +2c byte): the weapon raise / lower request;
//   0x39 kind 4    with a secondary component (+3314): 001977f0(actor, 1), then
//                  001ae230(+3314, 0, +30): aim slot 0 (+3a0: +3a5 = 1, +3b0 =
//                  the point);
//   0x39 kind 0xb  the object named by +30 (0040c540): its eye (virtual +c0
//                  with the subtarget +34), then as kind 4 when +3314 exists;
//   0x39 kind 5    the object named by +2c: its virtual +e0 point, then as kind 4;
//   0x3a           001977f0(actor, 0);
//   0x3e           001ae100(actor + 60, object named by +2c): the look target
//                  +2dd0 / +2dd4 (the object's +c, or -1) and +2dec = 0;
//   0x3f           the same with no object.
//   0x3d           the head look (001aa2f0, static): with a secondary
//                  component, kind 4 sets aim slot 1 (001ae230(+3314, 1,
//                  +30): +415 = 1, +420 = the point), kind 0xb the eye +c0 of
//                  the object named by +30 (its subtarget +34; the original
//                  calls it without a null check), kind 5 the +e0 point of
//                  the object named by +2c (none: nothing), kind 1 with a
//                  zero byte +2c clears slot 1 (001ae150(+3314, 1): +415 = 0).
//                  Slot 1 is read only by the upper-body IK of 001cb160,
//                  which is not ported (secondary_aim.hpp).
// Other kinds of 0x39 / 0x3d do nothing. The handler returns 1 for all of them.
#include "animation_root.hpp"
#include <cstdint>
#include <functional>

namespace omega {

struct ActorAimMessage {
    std::uint8_t action{};              // +24
    std::uint8_t kind{};                // +28
    std::uint32_t word_2c{};            // +2c (byte +2c for 0x39 kind 1; an object's +c for 0x39 kind 5 and 0x3e)
    std::int32_t object_30{-1};         // +30: 0x39 kind 0xb object
    std::int32_t subtarget_34{-1};      // +34: 0x39 kind 0xb subtarget
    RootVector point_30{};              // +30: 0x39 kind 4 point
};
struct ActorAimState {
    std::uint8_t aim_active_3a5{};      // secondary +3a0 slot 0: +5
    RootVector aim_point_3b0{};         // and +10
    std::uint64_t look_2dd0{};          // actor +60 + 2d70
    std::int32_t look_id_2dd4{-1};      // actor +60 + 2d74
    std::uint32_t look_2dec{};          // actor +60 + 2d8c
    std::uint8_t head_active_415{};     // secondary +3a0 slot 1 (+410): +5
    RootVector head_point_420{};        // and +10
};
struct ActorAimHost {
    bool secondary{};                                                  // +3314 present
    std::function<void(std::uint8_t raise)> secondary_mode;            // 001977f0(actor, raise)
    std::function<std::uint64_t(std::int32_t id)> find_object;         // 0040c540 (0: none)
    std::function<std::int32_t(std::uint64_t object)> object_id;       // +c
    std::function<RootVector(std::uint64_t object,std::int32_t subtarget)> eye_c0;   // virtual +c0
    std::function<RootVector(std::uint64_t object)> point_e0;          // virtual +e0
};
// Returns false for actions other than 0x39 / 0x3a / 0x3e / 0x3f (not handled here).
bool handle_actor_aim_action(ActorAimState& state,const ActorAimMessage& message,const ActorAimHost& host);

}
