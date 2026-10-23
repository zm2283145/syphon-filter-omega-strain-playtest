#pragma once
// Checkpoint 132ms2 (stage A2): the weather record of game::SnowBelarus2 /
// RainTokyo / RainLorelei (00232690 / 002326d0 / 002326b0 -> 00232c00(type
// 0x16..0x18, count, f12..f15)): a 0xd0 object (vtable 004dbb60, derived from
// the particle asset 004df860 with +60 = -1 and no instance +64) that keeps
// one rain / snow instance (the emitter 0039b3a0) in each weather hotbox
// (type 0xe) near the camera object. research/WEATHER.md.
//   ctor   002348e0(obj, type, count, f12 = near, f13 = far, f14 = f14)
//   logic  vt+48 00232f10(obj, f12 = dt)
//   draw   vt+58 00232e00(obj): 003a1ad0(instance, +38, identity) per visible cell
//   dtor   vt+8 002345d0
//   clip   00234110(out, camera box, hotbox bounds)
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <span>

namespace omega::particle {

using WeatherVector = std::array<float, 4>;
using WeatherMatrix = std::array<WeatherVector, 4>;

struct WeatherBounds {
    WeatherVector lo{}, hi{};
    bool operator==(const WeatherBounds&) const = default;
};

// One 0xc list value (0036b180(0xc)): +0 the instance (0036b180(0x70) +
// 003a2dd0), +4 the hotbox, +8 byte "owned" (always 1 once listed), +9 byte
// "visible" (written by every update before it is read).
struct WeatherCell {
    std::uint64_t instance = 0;
    std::size_t box = 0;  // index into the update's box list
    std::uint8_t byte8 = 1;
    std::uint8_t byte9 = 0;
};

struct WeatherField {
    std::int8_t type = 0;        // +c8 (signed byte; the effect table index)
    std::int32_t count = 0;      // +c0 (the native's drop count)
    float near_b0 = 0.0f;        // +b0 f12
    float far_b4 = 0.0f;         // +b4 f13 (also the camera box size)
    float b8 = 0.0f;             // +b8 f14
    std::int32_t effect = -1;    // +c4 the effect table entry's id (-1: none)
    std::int32_t particles = 0;  // +bc the effect definition's numParticles (+c) when its slot exists
    WeatherBounds box{};         // +80 / +90: (-.5 far, +.5 far) xyz, w 1
    // +a0: the camera point of the last update. The constructor never writes
    // it (0036b180 does not clear); the port starts from zero.
    WeatherVector previous{};
    std::list<WeatherCell> cells;   // +74 (count +74, sentinel +78; push_back 002340c0 -> 00138ea0)
    std::uint32_t flags_38 = 0x80;  // +38 (the scene walk sets 2: vt+88 returns null)
};

// 002348e0. `effect` is 0x4f7a50[type].id (nullopt: the flag byte +4 clear,
// or type >= 0x29, which the original reads from address 0); `particles` the
// definition's numParticles when 00234bb0(effect) has a slot. The base
// 003cf0e0 (key 0040c240(0xe7), kind 4) and the drop pool 0039b210(count)
// (called when effect >= 0) are the host's.
WeatherField make_weather_field(std::int8_t type, std::int32_t count, float f12, float f13, float f14,
                                std::optional<std::int32_t> effect, std::optional<std::int32_t> particles);

// 00234110(out, a, b): the part of b inside a's xy extent (z is b's), grown
// to at least 16 x 16 inside b.
WeatherBounds weather_clip(const WeatherBounds& a, const WeatherBounds& b);

// A world list (0x4ee688) entry: the +90 type word and the vt+88 bounds
// (+70 / +80, LOCAL to the box's transform).
struct WeatherBox {
    std::int32_t type = 0;
    WeatherBounds bounds{};
};

struct WeatherCameraView {
    WeatherVector position{};  // vt+e4
    WeatherMatrix matrix{};    // vt+e8 (row 1 the view direction)
};

struct WeatherFieldHost {
    // vt+110 001719c0 (hotbox_contains) of box `index` at `point`.
    std::function<bool(std::size_t index, const WeatherVector& point)> contains;
    // 0036b180(0x70) then 003a2dd0(mem, id, &matrix, 1.0).
    std::function<std::uint64_t(std::int32_t id, const WeatherMatrix& matrix)> create;
    // 003a2ba0(instance, -1) then 0036b130.
    std::function<void(std::uint64_t)> destroy;
    // 003a33b0(instance, camera, a2, f12, f13, f14).
    std::function<void(std::uint64_t, std::uint32_t a2, float f12, float f13, float f14)> set_range;
    // 003a3340(instance, camera, &box, a3).
    std::function<void(std::uint64_t, const WeatherBounds& box, std::uint32_t a3)> set_box;
    // 003a3310(instance, f12, f13 = camera +60).
    std::function<void(std::uint64_t, float f12)> set_fraction;
    // 003a2400(instance, &matrix, 0, f12 = dt).
    std::function<void(std::uint64_t, const WeatherMatrix& matrix, float dt)> update;
    // 003a5c50(0053b5c0, dt).
    std::function<void(float dt)> update_group;
};

// vt+48 00232f10. `camera` nullopt = *(00159560(004ffc14)) is null (nothing
// happens). `boxes` is the world list 0x4ee688 in order.
void update_weather_field(WeatherField& field, float dt, const std::optional<WeatherCameraView>& camera,
                          std::span<const WeatherBox> boxes, const WeatherFieldHost& host);

// vt+58 00232e00: each cell with an instance and byte 9 set, in list order,
// drawn at the identity.
void draw_weather_field(const WeatherField& field,
                        const std::function<void(std::uint64_t instance, std::uint32_t flags)>& draw);

// vt+8 002345d0: every cell's instance destroyed (003a2ba0(inst, -1) + free), the list cleared.
void destroy_weather_field(WeatherField& field, const std::function<void(std::uint64_t)>& destroy);

}  // namespace omega::particle
