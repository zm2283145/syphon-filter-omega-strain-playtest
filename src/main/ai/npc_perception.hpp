#pragma once
// The AI component's perception object (component +1ac; constructor
// 0016e0a0) as far as the awareness entry needs it (COMBAT.md §5,
// NPC_SPAWN_AND_AI.md): the awareness record list +30 (0x40-byte records
// from "ai.cc", new records inserted at the front by 0016cee0), the entry
// 0016c9d0(perception, object, cause, value), the hit notice 0014f970 (AI
// component virtual +78) and the clear 0016dc60. Interpreter-verified
// (tests/npc_perception_goldens.hpp). The per-think pipeline 0016b580 and
// the rest of the perception object are in npc_perception_think.hpp.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace omega {

struct AwarenessRecord {
    std::uint64_t object{};          // +0 (weak handle with +4)
    std::int32_t id_4{-2};           // +4: the object's +c (-1 for a null object); -2 when created
    std::int32_t id_8{-1};           // +8: the object's +c again
    RootVector position_10{};        // +10: row 3 of the object's virtual +e8 matrix
    std::uint8_t cause_20{};         // +20 the cause of the last registration (eCauseForAwareness)
    float level_24{};                // +24 the age in seconds since last sensed (0 on every entry; cause 4 / 7 may make it a negative delay)
    // +28 is not written by 0016c9d0 (a new record keeps the allocator's
    // bytes; the interpreter's heap is zeroed): the update 0016b580 owns it.
    float time_28{};
    float score_2c{},score_30{},score_34{};
    std::uint8_t byte_38{},byte_39{},byte_3a{},dead_notice_3b{},byte_3c{};
    bool operator==(const AwarenessRecord&) const=default;
};
// The perception's flag byte +20 is NpcAiComponent::perception_flags_20.
// The other fields (npc_perception_think.hpp) hold 0016e0a0's values.
struct NpcPerception {
    std::vector<AwarenessRecord> records;    // +30, front first
    RootVector last_known_00{};              // +00 (0016c030)
    float radius_10{},half_angle_14{};       // +10 / +14 (0016dfa0, SetViewCone*)
    std::int32_t region_18{-1};              // +18 the target's region
    std::int32_t priority_1c{};              // +1c target priority
    std::uint8_t flags_21{};                 // +21 bit 0: NewAwareness and candidates for non-enemy players
    std::uint64_t target_40{};               // +40 / +44 the target (weak handle)
    std::int32_t target_id_44{-1};
    std::uint64_t candidate_48{};            // +48 / +4c the last visibility candidate
    std::int32_t candidate_id_4c{-2};
    std::uint32_t start_frame_50{};          // +50 (0016dfa0)
};
// What the entry reads from the object.
struct AwarenessObjectView {
    std::uint64_t object{};
    std::int32_t id{-1};                     // +c
    std::uint8_t life_108{};                 // virtual +108
    bool actor_class{};                      // +4c is the actor class word (*0049d010)
    std::array<std::uint8_t,3> root{};       // +2c, +2d, +2e
    std::uint8_t flag_3468{};
    RootVector position{};                   // virtual +e8 row 3
};
// A message the perception sends to its component (+3c) with transport 0:
// the dead-object notice (key 004ff2a8, vtable 004dcbd0, +20 = 2, +24 the object).
struct PerceptionSend {
    std::uint64_t object{};
    bool operator==(const PerceptionSend&) const=default;
};
// 0016c9d0. The object's record is found by its weak handle (0016db40). A
// dead object (life 3) is taken only when it is an actor (class word) that is
// present (+2d, +2c, !+2e) with +3468 set; then the component is told once
// (+3b). An existing record with +24 < 0 is left alone. Otherwise +24 = 0
// and by cause: 6 sets +38/+39/+3a; 4 sets +34 = 0.2 and, unless +39 and +38,
// +24 = -value / 500 for value > 25; 8 sets +34 = 1.0; 7 sets +34 = 5.0 and
// +24 = -1. Then the handle, +8, the position and +20 = cause.
void perception_add_awareness(NpcPerception& perception,const AwarenessObjectView& object,std::uint8_t cause,float value,
    std::vector<PerceptionSend>& sends);
// 0016dc60: every record is freed and the list emptied.
void perception_clear(NpcPerception& perception);

}
