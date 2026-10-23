#include "gobj_damage.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Object damage: " + what); }
float check(float v) {
    if (!std::isfinite(v)) fail("nonfinite arithmetic");
    return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v;
}
// cvt.w.s (truncation); values outside the integer range are not native.
std::int32_t trunc_word(float v) {
    check(v);
    if (!(std::fabs(v) < 2147483648.0F)) fail("value outside the integer range");
    return static_cast<std::int32_t>(v);
}
// VU lanes: each product and sum rounded to binary32, the result flushed.
float vu_ftz(float v) {
    if (!std::isfinite(v)) fail("nonfinite VU arithmetic");
    return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v;
}
float vu_mul(float a, float b) { return vu_ftz(a * b); }
float vu_madd(float acc, float a, float b) {
    const float p = a * b;
    return vu_ftz(acc + p);
}
float word_float(const std::uint8_t* p) {
    std::uint32_t w;
    std::memcpy(&w, p, 4);
    return std::bit_cast<float>(w);
}
}

std::uint8_t object_life_state(float health_40) { return trunc_word(health_40) == 0 ? 3 : 0; }

ObjectDamageOutcome object_base_damage(float& health_40, std::uint64_t source, std::uint8_t category,
                                       std::uint8_t type, float amount) {
    ObjectDamageOutcome out;
    if (source == 0) return out;                                          // 003ccb40
    if (check(health_40) <= 0.0F) {                                       // 003ccb5c (c.ole.s)
        out.result = object_life_state(health_40) == 3;                   // 003ccd28 -> 003ccdd8 (solo)
        return out;
    }
    health_40 = check(health_40 - check(amount));                         // 003ccbac
    out.message = ObjectDamageMessage{trunc_word(amount), source, type, category};   // 003ccbc8..003ccbfc, 003ccc90
    if (trunc_word(health_40) <= 0) {                                     // 003ccca8
        health_40 = 0.0F;                                                 // 003cccb0; +10c 003ccac0: no +58
        out.destroyed = true;
    }
    out.result = object_life_state(health_40) == 3;                       // 003ccdd8
    return out;
}

ObjectMatrix gob_matrix(const std::array<float, 3>& position, const std::array<float, 9>& basis) {
    return {RootVector{basis[0], basis[1], basis[2], 0.0F}, RootVector{basis[3], basis[4], basis[5], 0.0F},
            RootVector{basis[6], basis[7], basis[8], 0.0F}, RootVector{position[0], position[1], position[2], 1.0F}};
}

VumBounds vum_declared_bounds(std::span<const std::uint8_t> vum) {
    if (vum.size() < 0x68) fail("a VUM file shorter than its header");
    VumBounds b;
    for (std::size_t i = 0; i < 4; ++i) {
        b.min[i] = word_float(vum.data() + 0x30 + 4 * i);
        b.max[i] = word_float(vum.data() + 0x40 + 4 * i);
    }
    if (vum[0x64] == 0) b.min[3] = b.max[3] = 1.0F;                      // 00377920: the first fixup
    return b;
}

RootVector object_position_e4(const ObjectMatrix* transform, const std::optional<VumBounds>& bounds) {
    if (!transform) return {0.0F, 0.0F, 0.0F, 1.0F};                     // 003cd3e8: vf0
    const auto& m = *transform;
    if (!bounds) return m[3];                                             // 003cd3d8: world +40
    RootVector c{};
    for (std::size_t i = 0; i < 4; ++i) c[i] = vu_mul(vu_ftz(bounds->min[i] + bounds->max[i]), 0.5F);   // vadd, vmulx
    RootVector out{};
    for (std::size_t i = 0; i < 4; ++i) {
        float acc = vu_mul(m[0][i], c[0]);                                // vmulax
        acc = vu_madd(acc, m[1][i], c[1]);                                // vmadday
        acc = vu_madd(acc, m[2][i], c[2]);                                // vmaddaz
        out[i] = vu_madd(acc, m[3][i], c[3]);                             // vmaddw
    }
    return out;
}

RootVector object_eye_c0(const ObjectMatrix* transform, const std::optional<VumBounds>& bounds, std::uint64_t) {
    return object_position_e4(transform, bounds);                         // 0014d340: jr +e4
}

}
