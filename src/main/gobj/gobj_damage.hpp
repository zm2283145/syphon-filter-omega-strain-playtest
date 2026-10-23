#pragma once
// The base object's damage and position virtuals on the level's population
// objects (GOB: kinds 1/2/4/5/6, vtables 004db4c0 / 004db280 / 004db160 /
// 004db3a0) and event objects (NIE: 004dc050, NIL: 004dbed0); none of them
// overrides +100 / +108 / +10c / +e4 (COMBAT.md "Checkpoint 120f").
//   +100  003ccb00 in single player (*005721c8 clear) without a controller
//         (+58 = 0: 003cf164; no GOB constructor writes it): a null source
//         returns false; +40 <= 0 (c.ole.s) returns +108 == 3 with no message;
//         otherwise +40 -= amount, the DamageMsg (key 005716f0, vtable
//         004e08a0: +24 cvt.w.s amount, +28 source, +2c type, +2d category,
//         +2e 1) is queued to the object (003c9420 transport 0), and
//         cvt.w.s(+40) <= 0 stores +40 = 0 and calls +10c (003ccac0: +58's
//         +7c, nothing without a controller). Returns +108 == 3.
//   +108  00141850: 3 when cvt.w.s(+40) == 0, else 0.
//   +e4   003cd260: (0, 0, 0, 1) without the +50 transform; with the bounds
//         +88 (not when +49 == 0xe) the world matrix applied to their centre
//         ((min + max) * 0.5, vmulax / vmadday / vmaddaz / vmaddw), else the
//         world translation row. The GOB transform (0021add0 -> 0036fcf0) is
//         built from the record: rows basis[0..2], basis[3..5], basis[6..8]
//         (w 0) and the position (w 1); with no parent (+a0 = 0) the dirty
//         refresh 0036ecf0 copies the same local matrix.
//   +88   GOB 00213ac0: +60 (the asset is not "no_vum" and 0036def0 loaded)
//         and the model's VUM -> 003947a0: the VUM file +30 (min) and +40
//         (max); the first fixup 00377920 (file byte +64 == 0) sets both w
//         lanes to 1.0.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace omega {

struct ObjectDamageMessage {
    std::int32_t damage_24{};
    std::uint64_t source_28{};
    std::uint8_t type_2c{};
    std::uint8_t category_2d{};
    bool operator==(const ObjectDamageMessage&) const = default;
};
struct ObjectDamageOutcome {
    std::optional<ObjectDamageMessage> message;   // queued to the object itself
    bool destroyed{};                             // +40 stored as 0 and +10c called
    bool result{};                                // +108 == 3
};
// 00141850.
std::uint8_t object_life_state(float health_40);
// 003ccb00 on an object without a controller in single player. `source` is the
// a1 pointer (0: null). Nonfinite values and +40 outside the integer range
// throw.
ObjectDamageOutcome object_base_damage(float& health_40, std::uint64_t source, std::uint8_t category,
                                       std::uint8_t type, float amount);

using ObjectMatrix = std::array<RootVector, 4>;
// 0021b038..0021b158 + 00131e90 / 00131e70: the GOB record's matrix.
ObjectMatrix gob_matrix(const std::array<float, 3>& position, const std::array<float, 9>& basis);
struct VumBounds {
    RootVector min{}, max{};
    bool operator==(const VumBounds&) const = default;
};
// The bounds 003947a0 returns for a loaded VUM file (00377920's fixup applied).
// Throws for a file shorter than its header.
VumBounds vum_declared_bounds(std::span<const std::uint8_t> vum);
// 003cd260: `transform` null for an object without +50; `bounds` nullopt when
// +88 returns null (or +49 == 0xe).
RootVector object_position_e4(const ObjectMatrix* transform, const std::optional<VumBounds>& bounds);
// +c0 0014d340 on every GOB vtable: `lw t9,(a1); lw t9,0xe4(t9); jr t9` -- the eye a
// RequestFacial / look entry 2 facing reads is +e4 and the selector a2 is ignored.
RootVector object_eye_c0(const ObjectMatrix* transform, const std::optional<VumBounds>& bounds, std::uint64_t selector);

}
