#include "actor_footstep.hpp"
#include "actor_navigation.hpp"

#include "vu_float.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {

// 00198e00 / 00198dd0: a group's key {node, group} against the selector {0 (no
// node), number}: a single leaf member with group -1 keys by its node, otherwise
// the leaf's group (one member) or the group's number.
bool selects_group(const AnimationPlayerGroup& group, const AnimationGraph& graph, std::int32_t number) {
    if (group.members.size() == 1) {
        const auto node = group.members.front().node;
        if (node >= graph.nodes().size()) throw std::runtime_error("Gait sample: invalid group member (00198e00)");
        const auto& leaf = graph.nodes()[node];
        if (leaf.kind != AnimationGraphKind::Leaf && leaf.kind != AnimationGraphKind::ControlledLeaf)
            throw std::runtime_error("Gait sample: group member is not a leaf (00198e00)");
        return leaf.leaf.group != -1 && leaf.leaf.group == number;
    }
    return group.number == number;
}

// VU arithmetic (VU_ROUNDING.md): results flush denormals (sign kept); a
// vopmsub / vmadd product is rounded, not flushed.
float vu_flush(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; }
float vu_mul(float a, float b) { return vu_flush(a * b); }
float vu_add(float a, float b) { return vu_flush(a + b); }
float vu_msub(float acc, float s, float t) { const float p = s * t; return vu_flush(acc - p); }
// 001320f0 (and inside 00131c70): vopmula / vopmsub, W = 0.
RootVector vu_cross(const RootVector& a, const RootVector& b) {
    return {vu_msub(vu_mul(a[1], b[2]), b[1], a[2]), vu_msub(vu_mul(a[2], b[0]), b[2], a[0]),
            vu_msub(vu_mul(a[0], b[1]), b[0], a[1]), 0.0F};
}
// 00132060 / 00132030: xyz * RSQRT((x*x + y*y) + z*z) (FLT_MAX for a zero sum), W kept.
RootVector vu_normalize(RootVector v) {
    const float s = vu_add(vu_add(vu_mul(v[0], v[0]), vu_mul(v[1], v[1])), vu_mul(v[2], v[2]));
    const float q = s == 0.0F ? std::numeric_limits<float>::max() : vu_flush(vu::rsqrt(s));
    for (std::size_t i = 0; i < 3; ++i) v[i] = vu_mul(v[i], q);
    return v;
}
// 001320b0: vaddx.xyz v + 0, the MAC flags equal to 0xe (x, y, z zero).
bool vu_zero_xyz(const RootVector& v) {
    return vu_flush(v[0]) == 0.0F && vu_flush(v[1]) == 0.0F && vu_flush(v[2]) == 0.0F;
}
constexpr float steep = 0.70700002F;   // 00131c70: 0x3f34fdf4

}

ActorGaitSample actor_gait_sample(const AnimationController& controller,
    const AnimationPlayerState& player, const AnimationGraph& graph, std::int32_t number) {
    if (controller.active.empty()) throw std::runtime_error("Gait sample: no active motion record (00198e90)");
    ActorGaitSample out;
    const auto host = controller.active.back().host;
    for (const auto& group : player.groups) {
        if (group.host != host || !selects_group(group, graph, number)) continue;
        out.phase = group.phase.current;
        break;
    }
    out.weight = actor_navigation_group_metric(controller, player, graph, {std::nullopt, number});   // 001987d0
    return out;
}

ActorGaitStep actor_gait_step(float phase_351c, const ActorGaitSample& sample, bool physical, bool contact_dd) {
    ActorGaitStep out;
    if (!sample.phase || !physical || !contact_dd) return out;   // 00198010: +351c = 0
    float f20 = *sample.phase + std::bit_cast<float>(0x3ea66666U);
    if (f20 < 0.0F) f20 += 1.0F;
    else if (!(f20 < 1.0F)) f20 += -1.0F;
    std::uint8_t foot;
    if (phase_351c < 0.5F && !(f20 < 0.5F)) foot = 2;
    else if (phase_351c <= f20) foot = 0;
    else foot = 1;
    out.foot = foot;
    out.effect = foot != 0 && !(sample.weight <= 0.5F);
    out.phase_351c = f20;
    return out;
}

ActorShimmyStep actor_shimmy_step(float phase_3520, const ActorGaitSample& sample) {
    ActorShimmyStep out;
    if (!sample.phase) return out;   // 00198210: +3520 = 0
    const float f20 = *sample.phase;
    // 00198034..00198134: c.olt.s old < t, then !(phase < t) plays; past the last threshold !(old <= phase).
    static constexpr std::uint32_t thresholds[] = {0x3de147aeU, 0x3e99999aU, 0x3eeb851fU, 0x3f28f5c3U, 0x3f4ccccdU};
    bool crossed = false;
    for (const auto t : thresholds) {
        const float th = std::bit_cast<float>(t);
        if (phase_3520 < th && !(f20 < th)) {
            crossed = true;
            break;
        }
    }
    if (!crossed) crossed = !(phase_3520 <= f20);
    out.crossed = crossed;
    out.sound = crossed && !(sample.weight <= 0.25F);   // 00198144: 0x3e800000
    out.phase_3520 = f20;                                // 0019820c
    return out;
}

float actor_plane_dot(const RootVector& a, const RootVector& b) {
    const float x = a[0] * b[0], y = a[1] * b[1], z = a[2] * b[2], w = a[3] * b[3];
    float acc = x;
    acc = acc + y;
    acc = acc + z;
    return acc + w;
}

RootVector actor_floor_plane(const RootVector& normal, const RootVector& point) {
    const float x = normal[0] * point[0], y = normal[1] * point[1], z = normal[2] * point[2];
    float d = x;
    d = d + y;
    d = d + z;
    return {normal[0], normal[1], normal[2], -d};
}

ActorFootProbe actor_foot_probe(bool contact_e8, std::uint8_t foot, const RootVector& plane_f0, float threshold,
    const std::function<RootVector(int)>& node_world_position) {
    ActorFootProbe out;
    if (!contact_e8 || foot == 0) return out;
    const auto position = node_world_position(foot == 1 ? 7 : 11);
    const float x = position[0], y = position[1];
    float z = 0.0F;
    if (!(plane_f0[2] == 0.0F)) z = -actor_plane_dot(plane_f0, {x, y, 0.0F, 1.0F}) / plane_f0[2];
    out.point = {x, y, z, 1.0F};
    out.hit = position[2] - z < threshold;
    return out;
}

ActorFootstepSound actor_footstep_sound(const MaterialRow& row, std::uint32_t flags_3204, std::uint32_t mode_320c,
    bool argument_a2) {
    const bool crawl = (flags_3204 >> 4) & 1U;
    const auto& prop = row.props[crawl ? 4 : 1];
    const bool loud = ((flags_3204 >> 2) & 1U) || ((flags_3204 >> 1) & 1U) || mode_320c == 0x15 || argument_a2;
    ActorFootstepSound out;
    if (prop.bank == -1) {
        out.kind = ActorFootstepSound::Kind::owned;
        if (loud) { out.bank = 0; out.name = "F_CONCRETE_M"; }   // 0049d928
        else { out.bank = 1; out.name = "F_SNEAK"; }              // 0049d920
    } else if (prop.bank != -2) {
        out.kind = ActorFootstepSound::Kind::positional;
        if (loud || crawl) { out.bank = prop.bank; out.name = prop.sound; }
        else { out.bank = 1; out.name = "F_SNEAK"; }
    }
    out.effect_id = prop.effect_id;
    out.effect_named = prop.effect_id == -1 && !prop.effect.empty();
    return out;
}

std::array<RootVector, 4> actor_footstep_effect_frame(const RootVector& u, const RootVector& probe_point) {
    const RootVector last{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<RootVector, 4> m{};
    const RootVector c = vu_cross(u, RootVector{0.0F, 0.0F, 1.0F, 0.0F});   // sp+c0 = u x sp+d0
    if (!vu_zero_xyz(c)) {
        const auto n = vu_normalize(u);        // 00132060 -> sp+b0
        const auto cn = vu_normalize(c);       // 00132030, 00132020 -> sp+a0
        m = {cn, n, vu_cross(cn, n), last};    // 001320f0 sp+90, 00132000, 00131f00
    } else if (!vu_zero_xyz(u)) {
        // 00131c70: the reference 00497f30 (0, 0, 1, 0) or 00497f20 (1, 0, 0, 0).
        const RootVector ref = std::fabs(u[2]) < steep ? RootVector{0.0F, 0.0F, 1.0F, 0.0F} : RootVector{1.0F, 0.0F, 0.0F, 0.0F};
        const auto n = vu_normalize(u);
        const auto cn = vu_normalize(vu_cross(u, ref));
        m = {cn, n, vu_cross(cn, n), last};    // 00131e90, 00131e70
    } else {
        m = {RootVector{1.0F, 0.0F, 0.0F, 0.0F}, RootVector{0.0F, 1.0F, 0.0F, 0.0F}, RootVector{0.0F, 0.0F, 1.0F, 0.0F}, last};   // 00131c40
    }
    m[3] = probe_point;                        // 00189420..00189438: sq sp+b20 -> sp+460
    return m;
}

}
