#include "weather_field.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega::particle {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Weather field: " + what); }

// EE FPU / VU single precision as the interpreter models it (smoke_field.cpp).
float ftz(float v) { return std::fabs(v) < std::numeric_limits<float>::min() ? std::copysign(0.0f, v) : v; }
float add(float a, float b) { return ftz(a + b); }
float sub(float a, float b) { return ftz(a - b); }
float mul(float a, float b) { return ftz(a * b); }
float dvd(float a, float b) {
    if (b == 0.0f) {
        const bool negative = std::signbit(a) != std::signbit(b);
        return negative ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    }
    return ftz(a / b);
}
// MAX.S / MIN.S and vmax / vmini.
float fmax_s(float a, float b) { return a < b ? b : a; }
float fmin_s(float a, float b) { return b < a ? b : a; }
constexpr float kMax = std::numeric_limits<float>::max();

WeatherVector vadd(const WeatherVector& a, const WeatherVector& b) {
    return {add(a[0], b[0]), add(a[1], b[1]), add(a[2], b[2]), add(a[3], b[3])};
}
WeatherVector vsub(const WeatherVector& a, const WeatherVector& b) {
    return {sub(a[0], b[0]), sub(a[1], b[1]), sub(a[2], b[2]), sub(a[3], b[3])};
}
WeatherVector vscale(const WeatherVector& a, float s) { return {mul(a[0], s), mul(a[1], s), mul(a[2], s), mul(a[3], s)}; }
// vmini.xyz / vmax.xyz of the running bounds with v.
void expand(WeatherBounds& b, const WeatherVector& v) {
    for (std::size_t k = 0; k < 3; ++k) {
        b.lo[k] = fmin_s(b.lo[k], v[k]);
        b.hi[k] = fmax_s(b.hi[k], v[k]);
    }
}
WeatherBounds empty_bounds() { return {{kMax, kMax, kMax, 1.0f}, {-kMax, -kMax, -kMax, 1.0f}}; }
// vsub.xyz then the MAC sign flags 0xe0: no lane of (camera hi - box lo) or
// (box hi - camera lo) negative.
bool overlaps(const WeatherBounds& camera, const WeatherBounds& box) {
    for (std::size_t k = 0; k < 3; ++k)
        if (std::signbit(sub(camera.hi[k], box.lo[k]))) return false;
    for (std::size_t k = 0; k < 3; ++k)
        if (std::signbit(sub(box.hi[k], camera.lo[k]))) return false;
    return true;
}
// vmul.xyz, vmulax.w / vmadday.w / vmaddz.w by vf0.w = 1, vsqrt Q (|x|).
float length(const WeatherVector& v) {
    const float sum = add(add(mul(v[0], v[0]), mul(v[1], v[1])), mul(v[2], v[2]));
    return std::sqrt(std::fabs(sum));
}
WeatherMatrix identity_at(const WeatherVector& row3) {
    return {{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 0.0f}, row3}};
}
// The size test of 00234110: hi - lo (xyzw) when lo.x <= hi.x (only x is
// tested, for both axes), else zero.
WeatherVector clip_size(const WeatherBounds& b) {
    if (b.lo[0] <= b.hi[0]) return vsub(b.hi, b.lo);
    return {};
}
// 00233a18..00233b18 / 00233cf8..00233df8: 003a2ba0(instance, -1) + 0036b130
// when the cell has one, 0036b130(cell), 00138ab0(list, node).
void destroy_cell(WeatherField& field, std::list<WeatherCell>::iterator it, const WeatherFieldHost& host) {
    const std::uint64_t instance = it->instance;
    if (instance) host.destroy(instance);
    field.cells.erase(it);
}
}  // namespace

WeatherField make_weather_field(std::int8_t type, std::int32_t count, float f12, float f13, float f14,
                                std::optional<std::int32_t> effect, std::optional<std::int32_t> particles) {
    WeatherField field;
    // 0023497c..00234af8: the box is the bounds of (-h, -h, -h) and (h, h, h), h = .5 * +b4.
    field.box = empty_bounds();
    field.type = type;   // +c8
    field.near_b0 = f12;  // +b0
    field.far_b4 = f13;   // +b4
    field.b8 = f14;       // +b8
    field.count = count;  // +c0
    const float h = mul(0.5f, field.far_b4);
    expand(field.box, {-h, -h, -h, 1.0f});
    expand(field.box, {h, h, h, 1.0f});
    field.particles = 0;  // +bc
    // 00234afc..00234b54: 0x4f7a50[type].id (flag byte +4); a clear flag or
    // an index >= 0x29 reads address 0.
    if (!effect) fail("002348e0: 0x4f7a50[" + std::to_string(type) + "] has no entry (the original reads address 0)");
    field.effect = *effect;
    if (field.effect >= 0 && particles) field.particles = *particles;  // 00234b64..00234b7c
    return field;
}

WeatherBounds weather_clip(const WeatherBounds& a, const WeatherBounds& b) {
    WeatherBounds out = empty_bounds();
    WeatherVector v{};
    // 002341b8..00234208: the lower corner (x, y the larger of the two, z b's).
    v[0] = fmax_s(a.lo[0], b.lo[0]);
    v[1] = fmax_s(a.lo[1], b.lo[1]);
    v[2] = b.lo[2];
    expand(out, v);
    // 0023420c..00234260: the upper corner (x, y the smaller, z b's).
    v[0] = fmin_s(a.hi[0], b.hi[0]);
    v[1] = fmin_s(a.hi[1], b.hi[1]);
    v[2] = b.hi[2];
    expand(out, v);
    // 00234264..002343f8: narrower than 16 in x: grow by 16 - size on both
    // sides of a's x extent, inside b (v keeps its y, z).
    WeatherVector size = clip_size(out);
    if (size[0] < 16.0f) {
        size = clip_size(out);
        const float g = sub(16.0f, size[0]);
        v[0] = fmax_s(sub(a.lo[0], g), b.lo[0]);
        expand(out, v);
        v[0] = fmin_s(add(a.hi[0], g), b.hi[0]);
        expand(out, v);
    }
    // 00234400..00234598: the same in y (v keeps its x, z).
    size = clip_size(out);
    if (size[1] < 16.0f) {
        size = clip_size(out);
        const float g = sub(16.0f, size[1]);
        v[1] = fmax_s(sub(a.lo[1], g), b.lo[1]);
        expand(out, v);
        v[1] = fmin_s(add(a.hi[1], g), b.hi[1]);
        expand(out, v);
    }
    return out;
}

void update_weather_field(WeatherField& field, float dt, const std::optional<WeatherCameraView>& camera,
                          std::span<const WeatherBox> boxes, const WeatherFieldHost& host) {
    // 00232f44..00232f5c: the camera object, the effect id.
    if (!camera || field.effect < 0) return;
    // 00232f88..00233004: P = vt+e4 + vt+e8 row 1 * (.5 * +b4) (xyzw).
    const float half = mul(0.5f, field.far_b4);
    const WeatherVector point = vadd(camera->position, vscale(camera->matrix[1], half));
    // 00233008..00233054: the camera box +80 / +90 moved to P (xyz; w the box's).
    WeatherBounds camera_box = field.box;
    for (std::size_t k = 0; k < 3; ++k) {
        camera_box.lo[k] = add(field.box.lo[k], point[k]);
        camera_box.hi[k] = add(field.box.hi[k], point[k]);
    }
    // 00233058..00233230: the look-ahead points +b4 along x / y in the
    // direction P moved since the last update.
    const WeatherVector step_x{field.far_b4, 0.0f, 0.0f, 0.0f};
    const WeatherVector step_y{0.0f, field.far_b4, 0.0f, 0.0f};
    const WeatherVector ahead_x =
        sub(point[0], field.previous[0]) <= 0.0f ? vsub(point, step_x) : vadd(point, step_x);  // sp+2b0
    const WeatherVector ahead_y =
        sub(point[1], field.previous[1]) <= 0.0f ? vsub(point, step_y) : vadd(point, step_y);  // sp+2a0
    // 00233238..002334f0 bounds the listed hotboxes (vt+88) into sp+280 /
    // sp+290 and counts the unowned cells (s1); neither result is read.

    // 002334f8..00233848: every type 0xe hotbox of the world list 0x4ee688
    // that no owned cell holds and whose bounds meet the camera box gets a
    // cell with an instance at the identity with row 3 = P.
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].type != 0xe) continue;
        bool held = false;
        for (const auto& cell : field.cells)
            if (cell.box == i && cell.byte8) held = true;
        if (held) continue;
        if (!overlaps(camera_box, boxes[i].bounds)) continue;
        WeatherCell cell;
        cell.box = i;
        cell.instance = host.create(field.effect, identity_at(point));  // 003a2dd0(mem, id, &sp+2f0, 1.0)
        cell.byte8 = 1;
        field.cells.push_back(cell);  // 002340c0
    }

    // 00233850..00233c90: drop cells beyond 1.5 * +b4, mark the ones whose
    // box meets the camera box visible and count their drops; the farthest
    // invisible one whose box holds neither look-ahead point is remembered.
    std::int32_t total = 0;
    float farthest = 0.0f;
    auto best = field.cells.end();
    for (auto it = field.cells.begin(); it != field.cells.end();) {
        const auto node = it++;
        WeatherCell& cell = *node;
        if (!cell.byte8) {
            total += field.count;  // 00233c68
            continue;
        }
        const WeatherBounds& b = boxes[cell.box].bounds;
        // 002338e8..00233980: per lane lo - P if not negative, else P - hi if
        // not negative, else 0.
        WeatherVector d{};
        for (std::size_t k = 0; k < 3; ++k) {
            const float below = sub(b.lo[k], point[k]);
            const float above = sub(point[k], b.hi[k]);
            if (!std::signbit(below))
                d[k] = add(below, 0.0f);
            else if (!std::signbit(above))
                d[k] = add(above, 0.0f);
        }
        const float dist = length(d);
        if (!(dist <= add(field.far_b4, mul(0.5f, field.far_b4)))) {
            destroy_cell(field, node, host);
            continue;
        }
        if (overlaps(camera_box, b)) {
            cell.byte9 = 1;
            total += field.count;
            continue;
        }
        if (!(dist <= farthest) && !host.contains(cell.box, ahead_x) && !host.contains(cell.box, ahead_y)) {
            best = node;
            farthest = dist;
        }
        cell.byte9 = 0;
    }
    // 00233c98..00233dfc: five or more cells drop the remembered one.
    if (field.cells.size() >= 5 && best != field.cells.end()) destroy_cell(field, best, host);

    // 00233e00..00233e34: the share of the drops, the wind group.
    float fraction = 1.0f;
    if (total > 0) fraction = dvd(static_cast<float>(field.count), static_cast<float>(total));
    host.update_group(dt);

    // 00233e38..00234020: each visible cell's instance gets the camera's
    // ranges, the clipped box and the share, then runs at the box centre.
    for (const auto& cell : field.cells) {
        if (!cell.instance || !cell.byte9) continue;
        const std::uint32_t not_snow = field.type != 0x16 ? 1U : 0U;
        host.set_range(cell.instance, not_snow, field.near_b0, field.far_b4, field.b8);  // 003a33b0
        const WeatherBounds clip = weather_clip(camera_box, boxes[cell.box].bounds);
        const WeatherVector centre = vscale(vadd(clip.lo, clip.hi), 0.5f);
        host.set_box(cell.instance, clip, not_snow);  // 003a3340
        host.set_fraction(cell.instance, fraction);   // 003a3310(inst, f21, camera +60)
        host.update(cell.instance, identity_at(centre), dt);  // 003a2400(inst, &sp+2f0, 0, dt)
    }
    field.previous = point;  // 00234028
}

void draw_weather_field(const WeatherField& field,
                        const std::function<void(std::uint64_t instance, std::uint32_t flags)>& draw) {
    for (const auto& cell : field.cells)
        if (cell.instance && cell.byte9) draw(cell.instance, field.flags_38);
}

void destroy_weather_field(WeatherField& field, const std::function<void(std::uint64_t)>& destroy) {
    for (const auto& cell : field.cells)
        if (cell.instance) destroy(cell.instance);
    field.cells.clear();
}

}  // namespace omega::particle
