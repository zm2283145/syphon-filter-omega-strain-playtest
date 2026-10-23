#pragma once
// The static impact block of the bullet update 00276460 (0x002768f4..
// 0x00276ee0, research/COMBAT.md "Checkpoint 131c"): when a bullet that
// reached the end of its range ends on a valid static object (no victim, the
// weapon's damage type +bc not 4), the surface's material row 00189560(bullet
// +d0) chooses a sound, an effect and a decal. The property index comes from
// the weapon record +48 (6 or 4: 3, 2: 2, otherwise 0); a -1 field of a
// nonzero property takes property 0's field.
//   sound  (+10 bank): -1 the static descriptor 004ef590 (bank 1
//          "B_HIT_CONCRETE"), -2 none, otherwise 001ca210(desc, bank, +14
//          name, 0, 0); 0020f860(msg, desc, bullet +100), +20 = 1,
//          003c9420(msg, 004ffb50, 0).
//   effect (+c): -1 00232120(4, frame), -2 none, otherwise 00232460(id, frame).
//   decal  (+2c): -1 00279ca0(*004ffc3c, frame, *00236f20(0x24), 8, object,
//          0.2), -2 none, otherwise 00279ca0(*004ffc3c, frame, id,
//          0048b350[+34 blend], object, +30 size) (a -1 decal of a nonzero
//          property also moves the size and blend to property 0).
// The frame: n = bullet +c0 (the hit normal), negated when it points along
// the step (n . step > 0); the reference axis 00497f20 (1, 0, 0, 0) or, when
// |n.z| < 0.707 (0x3f34fdf4), 00497f30 (0, 0, 1, 0); rows normalize(n x ref),
// normalize(n), their cross and bullet +100 + 0.08 n (VU arithmetic).
#include "animation_root.hpp"
#include "material_properties.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace omega {

struct BulletImpact {
    std::uint8_t property{};                     // s2 (the decal's fallback may move it to 0)
    enum class Sound : std::uint8_t { none, default_hit, named } sound{};
    std::int32_t bank{};                         // named: the property's +10
    std::string sound_name;                      // named: the property's +14
    enum class Effect : std::uint8_t { none, table, id } effect{};
    std::int32_t effect_id{};                    // id: the property's +c (table: 00232120(4))
    std::array<RootVector, 4> frame{};           // sp+570 (rows)
    enum class Decal : std::uint8_t { none, table, id } decal{};
    std::int32_t decal_id{};                     // id: +2c; table: *00236f20(0x24), filled by the caller
    float decal_size{};                          // f12
    std::uint8_t decal_mode{};                   // a3
    bool operator==(const BulletImpact&) const = default;
};

// The static descriptor 00276a80 posts (004ef590: bank 1, "B_HIT_CONCRETE").
inline constexpr const char* bullet_impact_default_sound = "B_HIT_CONCRETE";
// 0048b350: the decal mode by the property's blend byte +34 (read unchecked).
inline constexpr std::array<std::uint8_t, 3> bullet_impact_decal_modes{8, 0x10, 4};
// 0x00276860..0x00276878: sp+ad0 = the position after the step minus before (vsub.xyzw).
RootVector bullet_impact_step(const RootVector& before, const RootVector& after);
// 0x00276ae0..0x00276ce4: the impact frame.
std::array<RootVector, 4> bullet_impact_frame(const RootVector& normal_c0, const RootVector& step_ad0, const RootVector& point_100);
// The block's choices for one row (item_kind_48 the weapon record +48).
BulletImpact bullet_static_impact(const MaterialRow& row, std::uint8_t item_kind_48, const RootVector& normal_c0,
                                  const RootVector& step_ad0, const RootVector& point_100);

}