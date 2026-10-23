#include "decal_manager.hpp"
#include "vu_float.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
// VU results flush denormals to zero (sign kept); a vmadda product is rounded
// to binary32 but not flushed, only the sum is (tools/ee/ee.py, VU_ROUNDING.md).
float vu_flush(float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; }
float vu_mul(float a, float b) { return vu_flush(a * b); }
float vu_madd(float acc, float s, float t) {
    const float p = s * t;
    return vu_flush(acc + p);
}
// The EE FPU (cop1 .s) rounds to binary32 and flushes denormal results.
float fpu(float v) { return vu_flush(v); }
// cvt.w.s: truncation, saturating at the int32 range.
std::int32_t cvt_w(float v) {
    const double d = v;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return static_cast<std::int32_t>(d);
}
constexpr float dead_age = -2.0F;  // 0xc0000000
constexpr float fade_time = 2.0F;  // 0x40000000

// vmulax / vmadday / vmaddaz / vmaddw: rows m0..m3 weighted by v.
std::array<float, 4> vu_transform(const DecalMatrix& m, const std::array<float, 4>& v) {
    std::array<float, 4> out{};
    for (std::size_t k = 0; k < 4; ++k) {
        float acc = vu_mul(m[0][k], v[0]);
        acc = vu_madd(acc, m[1][k], v[1]);
        acc = vu_madd(acc, m[2][k], v[2]);
        out[k] = vu_madd(acc, m[3][k], v[3]);
    }
    return out;
}
// vmul.xyz squares, the w sum ((x2 + y2) + z2) through vmulax.w / vmadday.w /
// vmaddz.w with vf0.w = 1, VRSQRT, vmulq.xyz; w kept.
DecalVector vu_normalize3(const DecalVector& v, const char* where) {
    const float x2 = vu_mul(v[0], v[0]), y2 = vu_mul(v[1], v[1]), z2 = vu_mul(v[2], v[2]);
    const float s = vu_flush(vu_flush(x2 + y2) + z2);
    if (s == 0.0F) throw std::runtime_error(std::string(where) + ": normalizing a zero-length vector");
    const float q = vu_flush(vu::rsqrt(s));
    return {vu_mul(v[0], q), vu_mul(v[1], q), vu_mul(v[2], q), v[3]};
}
// 0011c928 (double to float): truncation toward zero.
float double_to_float(double d) {
    float f = static_cast<float>(d);
    if (std::fabs(static_cast<double>(f)) > std::fabs(d)) f = std::nextafter(f, 0.0F);
    return vu_flush(f);
}
}  // namespace

DecalMatrix decal_multiply(const DecalMatrix& a, const DecalMatrix& b) {
    DecalMatrix out{};
    for (std::size_t r = 0; r < 4; ++r) out[r] = vu_transform(b, a[r]);
    return out;
}

DecalMatrix decal_rigid_inverse(const DecalMatrix& m) {
    DecalMatrix out = m;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) out[r][c] = m[c][r];
    // 00133240..00133290: skipped only when the translation x, y and z all compare equal to 0.
    if (m[3][0] == 0.0F && m[3][1] == 0.0F && m[3][2] == 0.0F) return out;
    // vsub.xyz vf1 = vf0 - t, then vmulax / vmadday / vmaddz.xyz through the transposed rows; w kept.
    const std::array<float, 3> n{vu_flush(0.0F - m[3][0]), vu_flush(0.0F - m[3][1]), vu_flush(0.0F - m[3][2])};
    for (std::size_t k = 0; k < 3; ++k) {
        float acc = vu_mul(out[0][k], n[0]);
        acc = vu_madd(acc, out[1][k], n[1]);
        out[3][k] = vu_madd(acc, out[2][k], n[2]);
    }
    return out;
}

void decal_record_set(DecalRecord& record, std::uint32_t flags, const DecalMatrix& frame, std::uint64_t object,
                      float size, float texture, const DecalNodeLookup& node) {
    record.flags_0 = flags;
    record.size_50 = size;
    record.texture_54 = cvt_w(texture);
    record.object_58 = object;
    record.age_5c = -1.0F;
    if ((record.flags_0 & 1U) == 0) return;
    std::optional<DecalMatrix> rows;
    if (record.object_58 != 0) {
        rows = node(record.object_58);
        if (!rows) record.object_58 = 0;  // 0027a5c4: the object has no +50 node
    }
    if (record.object_58 == 0) {
        record.matrix_10 = frame;
        return;
    }
    // 00133300 copy, 00133200 rigid inverse, 0027a65c..0027a6b8: frame * inverse.
    record.matrix_10 = decal_multiply(frame, decal_rigid_inverse(*rows));
}

std::vector<DecalVector> blood_direction_table(std::int32_t n) {
    if (n <= 0) throw std::runtime_error("00278460: direction count must be positive");
    const float root = fpu(std::sqrt(static_cast<float>(n)));  // sqrt.s of cvt.s.w(n)
    const float pitch_step = fpu(180.0F / root), yaw_step = fpu(360.0F / root);
    float pitch = -90.0F, yaw = 0.0F;
    // 0011bd70 widens mul.s(pi / 180, angle); 00120bc0 / 001211a8 are the
    // soft-double cos / sin, 0011c268 the double product, 0011c928 the narrowing.
    auto radians = [](float a) { return static_cast<double>(fpu(std::bit_cast<float>(0x3c8efa35U) * a)); };
    std::vector<DecalVector> out(static_cast<std::size_t>(n));
    for (auto& v : out) {
        const double cp = std::cos(radians(pitch));
        v[0] = double_to_float(cp * std::cos(radians(yaw)));
        v[1] = double_to_float(cp * std::sin(radians(yaw)));
        v[2] = double_to_float(std::sin(radians(pitch)));
        v[3] = 0.0F;
        v = vu_normalize3(v, "00278460");
        yaw = fpu(yaw + yaw_step);
        if (!(yaw < 360.0F)) {
            yaw = fpu(yaw - 360.0F);
            pitch = fpu(pitch + pitch_step);
        }
    }
    return out;
}

void blood_record_set(DecalRecord& record, std::uint32_t flags, const DecalVector& point, const DecalVector& normal,
                      float size, float texture, std::uint64_t object, const DecalNodeLookup& node) {
    record.flags_0 = flags;
    record.size_50 = size;
    record.texture_54 = cvt_w(texture);
    record.object_58 = object;
    record.age_5c = -1.0F;
    if ((flags & 2U) == 0 || object == 0) return;
    DecalVector p = point, n = normal;
    if (const auto rows = node(object)) {
        // 00133300 / 00133200 / 00133300, then the vmulax..vmaddw products (0027a42c, 0027a474).
        const auto inverse = decal_rigid_inverse(*rows);
        p = vu_transform(inverse, point);
        n = vu_transform(inverse, normal);
    }
    // 0027a4ac..0027a4f4 / 0027a504..0027a54c: vf0 and its pexcw / pcpyud
    // shuffles give rows (1,0,0,0), -, (0,0,1,0); then the point and the normal.
    record.matrix_10[0] = {1.0F, 0.0F, 0.0F, 0.0F};
    record.matrix_10[1] = n;
    record.matrix_10[2] = {0.0F, 0.0F, 1.0F, 0.0F};
    record.matrix_10[3] = p;
}

DecalManager::DecalManager(std::int32_t list1, std::int32_t list2) : size_8(list1), size_c(list2) {
    if (list1 <= 0 || list2 <= 0) throw std::runtime_error("0027a020: decal list sizes must be positive");
    list1_.resize(static_cast<std::size_t>(list1));
    list2_.resize(static_cast<std::size_t>(list2));
    // 0027a2f8..: cvt.w.s(0.1 * size), the FPU product of 0x3dcccccd and cvt.s.w(size).
    ahead_20 = cvt_w(fpu(0.1F * static_cast<float>(list1)));
    ahead_24 = cvt_w(fpu(0.1F * static_cast<float>(list2)));
    const auto table = blood_direction_table(static_cast<std::int32_t>(blood_direction_count));
    std::copy(table.begin(), table.end(), directions_.begin());
}

void DecalManager::add_blood(const DecalVector& point, std::int32_t texture, const std::optional<DecalVector>& normal,
                             float size, const BloodTrace& trace, const DecalNodeLookup& node) {
    const float reach = fpu(2.0F * size);
    DecalVector sum{};
    std::int32_t hits = 0;
    std::vector<std::uint64_t> owners;
    for (const auto& dir : directions_) {
        // 003c9e30 request: +30 the point with w = 1, +40 vmulx.xyzw(direction, 2 * size) with w = 0, +58 = 4.
        const DecalVector origin{point[0], point[1], point[2], 1.0F};
        const DecalVector displacement{vu_mul(dir[0], reach), vu_mul(dir[1], reach), vu_mul(dir[2], reach), 0.0F};
        const auto hit = trace(origin, displacement);
        if (!hit) continue;
        std::uint64_t owner = 0;
        if (hit->owner_class == 1) {
            owner = hit->owner;
        } else if (hit->owner_class == 0xf) {
            if (hit->part != 0 && hit->part_loaded) owner = hit->part;
        } else if (hit->owner_class == 0x10) {
            // 002799d4..00279a3c: vt+124 (0 skips), its model word **(+68), then vt+124 again as the owner.
            if (hit->part != 0 && hit->part_loaded) owner = hit->part;
        }
        if (owner == 0) continue;
        for (std::size_t k = 0; k < 4; ++k) sum[k] = vu_flush(sum[k] + hit->normal[k]);  // vadd.xyzw
        if (std::find(owners.begin(), owners.end(), owner) == owners.end()) owners.push_back(owner);
        ++hits;
    }
    if (normal) {
        sum = *normal;
    } else {
        if (owners.empty()) return;  // no hit: the averaged normal is never used
        const float n = static_cast<float>(hits);
        sum = {fpu(sum[0] / n), fpu(sum[1] / n), fpu(sum[2] / n), 0.0F};
        sum = vu_normalize3(sum, "00279b48: averaged blood normal");
    }
    for (const auto owner : owners) {
        const std::int32_t index = write_1c;
        write_1c = index + 1;
        const auto ahead = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(index + ahead_24) % static_cast<std::uint32_t>(size_c));
        if (write_1c == size_c) write_1c = 0;
        if (count_14 < size_c) {
            ++count_14;
        } else {
            auto& fading = list2_.at(static_cast<std::size_t>(ahead));
            if (!(fading.age_5c == dead_age)) fading.age_5c = 0.0F;
        }
        blood_record_set(list2_.at(static_cast<std::size_t>(index)), 0xaU, point, sum, size,
                         static_cast<float>(texture), owner, node);
    }
}

std::vector<BloodProjectorRequest> DecalManager::blood_projectors(const DecalRootLookup& root,
                                                                  const BloodHiddenLookup& hidden,
                                                                  const DecalNodeLookup& node) {
    std::vector<BloodProjectorRequest> out;
    for (std::int32_t i = 0; i < count_14; ++i) {
        auto& r = list2_.at(static_cast<std::size_t>(i));
        if (r.age_5c == dead_age) continue;
        if (r.object_58 == 0) throw std::runtime_error("00279350: blood decal record without an object");
        if (hidden(r.object_58)) continue;
        const auto bytes = root(r.object_58);
        if (!(bytes[1] != 0 && bytes[0] != 0 && bytes[2] == 0)) continue;
        float alpha = 1.0F;
        if (!(r.age_5c < 0.0F)) {
            alpha = fpu(1.0F - fpu(r.age_5c / fade_time));
            if (alpha <= 0.0F) r.age_5c = dead_age;
        }
        if (alpha <= 0.0F) continue;
        BloodProjectorRequest q;
        q.record = i;
        q.object = r.object_58;
        q.position = r.matrix_10[3];
        q.normal = r.matrix_10[1];
        if (const auto rows = node(r.object_58)) {
            q.position = vu_transform(*rows, q.position);
            q.normal = vu_transform(*rows, q.normal);
        }
        q.size = r.size_50;
        q.strength = fpu(blood_projector_strength * alpha);
        q.texture = r.texture_54;
        out.push_back(q);
    }
    return out;
}

void DecalManager::add(const DecalMatrix& frame, std::int32_t texture, std::uint8_t mode, std::uint64_t object,
                       float size, const DecalNodeLookup& node) {
    const std::int32_t index = write_18;
    write_18 = index + 1;
    // divu: the unsigned remainder of (old write + ahead) by the list size.
    const auto ahead = static_cast<std::int32_t>(
        static_cast<std::uint32_t>(index + ahead_20) % static_cast<std::uint32_t>(size_8));
    if (write_18 == size_8) write_18 = 0;
    if (count_10 < size_8) {
        ++count_10;
    } else {
        auto& fading = list1_.at(static_cast<std::size_t>(ahead));
        if (!(fading.age_5c == dead_age)) fading.age_5c = 0.0F;
    }
    // 00279d54: f13 = cvt.s.w(texture).
    decal_record_set(list1_.at(static_cast<std::size_t>(index)), static_cast<std::uint32_t>(mode) | 1U, frame,
                     object, size, static_cast<float>(texture), node);
}

void DecalManager::kill_all() {
    for (std::int32_t i = 0; i < count_10; ++i) list1_.at(static_cast<std::size_t>(i)).age_5c = -2.0F;  // 002795b8
    for (std::int32_t i = 0; i < count_14; ++i) list2_.at(static_cast<std::size_t>(i)).age_5c = -2.0F;  // 002795f8
}

void DecalManager::service(float dt, const DecalRootLookup& root) {
    auto list = [&](std::vector<DecalRecord>& records, std::int32_t count) {
        for (std::int32_t i = 0; i < count; ++i) {
            auto& r = records.at(static_cast<std::size_t>(i));
            if (!(r.age_5c < 0.0F)) {
                r.age_5c = fpu(r.age_5c + dt);
                if (!(r.age_5c < fade_time)) r.age_5c = dead_age;
            }
            if (r.object_58 == 0) continue;
            const auto bytes = root(r.object_58);
            const bool present = bytes[1] != 0 && bytes[0] != 0 && bytes[2] == 0;
            if (!present) r.age_5c = dead_age;
        }
    };
    list(list1_, count_10);
    list(list2_, count_14);
}

std::vector<DecalFlush> DecalManager::draw(const DecalNodeLookup& node) {
    std::vector<DecalFlush> flushes;
    DecalFlush current;  // the open 00371200 batch
    std::int32_t texture = -1;
    std::int32_t blend = -1;
    std::uint64_t object = 0;
    bool first = true;
    auto flush = [&] {
        if (!current.quads.empty()) flushes.push_back(current);
        current.quads.clear();
    };
    for (std::int32_t i = 0; i < count_10; ++i) {
        auto& r = list1_.at(static_cast<std::size_t>(i));
        if (r.age_5c == dead_age) continue;
        float alpha = 1.0F;
        if (!(r.age_5c < 0.0F)) {
            alpha = fpu(1.0F - fpu(r.age_5c / fade_time));
            if (alpha <= 0.0F) r.age_5c = dead_age;
        }
        if (alpha <= 0.0F) continue;
        std::int32_t s5 = 3;
        if ((r.flags_0 & 8U) != 0) s5 = 3;
        else if ((r.flags_0 & 4U) != 0) s5 = 0;
        else if ((r.flags_0 & 0x10U) != 0) s5 = 2;
        if (first || r.texture_54 != texture || s5 != blend || r.object_58 != object) {
            flush();
            if (r.texture_54 != texture) {
                // 003820e0(*00539248, handle): the port binds only real handles.
                if (r.texture_54 < 0)
                    throw std::runtime_error("00278a14: decal texture handle " + std::to_string(r.texture_54) +
                                             " bound through 003820e0 (not hosted)");
                texture = r.texture_54;
                current.texture = texture;
            } else if (texture < 0) {
                throw std::runtime_error("00278a04: decal drawn with no texture bound (003820e0 never called)");
            }
            if (s5 != blend) {
                blend = s5;
                current.alpha = decal_alpha_words.at(static_cast<std::size_t>(s5));
            }
            if (r.object_58 != object) {
                object = r.object_58;  // 00278a54: pop the old node, push the new one
                current.object = object;
                current.node.reset();
                if (object != 0) {
                    current.node = node(object);
                    if (!current.node)
                        throw std::runtime_error("00278ae4: decal object without a +50 node at the draw");
                }
            }
        }
        const float s = r.size_50;
        const float ns = -s;
        const std::array<std::array<float, 4>, 4> corners{{{ns, 0.0F, ns, 1.0F},
                                                           {ns, 0.0F, s, 1.0F},
                                                           {s, 0.0F, ns, 1.0F},
                                                           {s, 0.0F, s, 1.0F}}};
        const std::array<std::array<float, 2>, 4> uv{{{0.0F, 1.0F}, {0.0F, 0.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}}};
        const std::int32_t a = cvt_w(fpu(128.0F * alpha));
        std::array<DecalVertex, 4> quad{};
        for (std::size_t k = 0; k < 4; ++k) {
            quad[k].position = vu_transform(r.matrix_10, corners[k]);
            quad[k].rgba = {128, 128, 128, a};
            quad[k].st = {fpu(uv[k][0] + 2048.0F), fpu(uv[k][1] + 2048.0F), 1.0F, 0.0F};
        }
        current.quads.push_back(quad);
        first = false;
    }
    flush();
    return flushes;
}

}  // namespace omega
