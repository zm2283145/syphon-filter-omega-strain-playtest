#pragma once
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>

// The landing effect 00188fb0(actor, a1, a2) (research/COMBAT.md "Checkpoint
// 120e"). Without a physical (+3310) it returns. Otherwise s4 = (physical +e4
// != the sentinel *004a0438), then 001e96a0(physical) gives f20 (+e4 minus the
// 'flor' root's world z, or 0 for the sentinel). With s4 (in water) it branches
// to 00189440: f20 <= *0049cf40 (0.4) ? (f20 <= *0049cf38 (0.1) ?
// "F_WATERPUDDLE_M" (0049d960) : "F_WATER_ANKLE_M" (0049d950)) :
// "F_WATERDEEP_M" (0049d938) - a NaN compares false, so deep. The request
// 001ca210(desc, bank 0, name, 0, 0), 0020f990(msg, desc, actor) (attached to
// the actor), byte +20 = 1, sent at once (transport 1) to the sound manager
// 004ffb50 (003c9420 a2 = 1); a1 / a2 do not matter there. Without water (00189018) the
// surface probe 001e9710 picks a material effect (not ported).
inline constexpr float landing_water_ankle_depth=std::bit_cast<float>(0x3ecccccdU);   // 0049cf40
inline constexpr float landing_water_puddle_depth=std::bit_cast<float>(0x3dcccccdU);  // 0049cf38

// 001e96a0(physical): the sentinel gives 0; otherwise +e4 - flor z.
inline float landing_contact_depth(float e4,float sentinel,float flor_z){
    return e4==sentinel?0.0f:e4-flor_z;
}

inline const char* landing_water_sound(float depth){
    if(!(depth<=landing_water_ankle_depth))return "F_WATERDEEP_M";
    return depth<=landing_water_puddle_depth?"F_WATERPUDDLE_M":"F_WATER_ANKLE_M";
}
