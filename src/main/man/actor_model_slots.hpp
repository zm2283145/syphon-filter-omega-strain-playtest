#pragma once
#include "morph_resources.hpp"

namespace omega {
struct ActorModelPart {
    std::uint64_t identity{}; // Caller-owned resource identity, not a game address.
    std::uint8_t type{}; // Original part byte +40c.
    MorphEquipmentGrip grip;
};
struct ActorModelSlots {std::vector<std::optional<ActorModelPart>> slots;};
MorphEquipment model_slot_equipment(const ActorModelSlots& state);
// 0038c7d0: ignored for a nonnull part whose type is >=8, otherwise replace
// and refresh both hand grips. These operations do not attach transform nodes.
bool set_actor_model_slot(ActorModelSlots& state,MorphHostState& morph,const MorphGripMap& map,
    std::size_t slot,std::optional<ActorModelPart> part);
// 0038c790: swap pointers (including equal indices), then refresh grips.
void swap_actor_model_slots(ActorModelSlots& state,MorphHostState& morph,const MorphGripMap& map,
    std::size_t first,std::size_t second);
// The same without a grip map. 0038c5e0 reads its tables (00571510 /
// 00571528) only through a non-null part in slot 0 or 1; with both null it
// only clears the four hand weights (+23d0/+23d4/+23e0/+23e4). A non-null hand
// part after the operation throws naming 0038c5e0, leaving the state unchanged.
bool set_actor_model_slot(ActorModelSlots& state,MorphHostState& morph,std::size_t slot,std::optional<ActorModelPart> part);
void swap_actor_model_slots(ActorModelSlots& state,MorphHostState& morph,std::size_t first,std::size_t second);
// The render model constructor 00392380 zeroes model +0..+2def (0x2df
// quadwords), so the ten attach points +2410..+2434 start null.
ActorModelSlots constructed_actor_model_slots();
inline constexpr std::size_t actor_model_slot_count=10;
}
