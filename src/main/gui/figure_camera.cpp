#include "figure_camera.hpp"
#include "secondary_aim.hpp"
#include "game_libm.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
float fin(float v) {
    if (!std::isfinite(v)) throw std::runtime_error("0035b390: a non-finite stats figure camera or placement");
    return v;
}
float mul(float a, float b) { return fin(a * b); }
float add(float a, float b) { return fin(a + b); }
float sub(float a, float b) { return fin(a - b); }
}  // namespace

FigureRows figure_camera_rotation(const std::array<float, 4>& angles) {
    // f22 / f21 of +1d0, f24 / f23 of +1d4, f20 / f0 of +1d8.
    const float f22 = fin(game_cosf(angles[0])), f21 = fin(game_sinf(angles[0]));
    const float f24 = fin(game_cosf(angles[1])), f23 = fin(game_sinf(angles[1]));
    const float f20 = fin(game_cosf(angles[2])), f0 = fin(game_sinf(angles[2]));
    const float i = mul(f22, f24);
    const float f2 = mul(f0, f24), f1 = mul(f0, f23);
    const float f13 = mul(-f0, f22), f18 = mul(-f22, f23);
    const float f4 = mul(f20, f24);
    const float f12 = sub(f4, mul(f1, f21));
    const float f17 = sub(f1, mul(f4, f21));
    const float f3 = mul(f20, f23);
    const float f14 = add(f3, mul(f2, f21));
    const float f16 = mul(f20, f22);
    const float f15 = add(f2, mul(f3, f21));
    return {{{f12, f15, f18, 0.0F}, {f13, f16, f21, 0.0F}, {f14, f17, i, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}}};
}

std::array<float, 3> figure_camera_eye(const FigureRows& r) { return figure_camera_eye(r, {0.0F, -5.0F, 1.5F}); }

std::array<float, 3> figure_camera_eye(const FigureRows& r, const std::array<float, 3>& eye) {
    std::array<float, 3> e{};
    for (std::size_t j = 0; j < 3; ++j)
        e[j] = add(add(mul(eye[0], r[0][j]), mul(eye[1], r[1][j])), mul(eye[2], r[2][j]));
    return e;
}

std::array<std::array<float, 4>, 4> figure_camera_clip(const FigureRows& r) {
    return figure_camera_clip(r, {0.0F, -5.0F, 1.5F}, 0.5236, 1.0, 40.0);
}

std::array<std::array<float, 4>, 4> figure_camera_clip(const FigureRows& r, const std::array<float, 3>& eye,
                                                        double half_fov_100, double near_f4, double far_fc) {
    if (!(half_fov_100 > 0.0 && near_f4 > 0.0 && far_fc > near_f4))
        throw std::runtime_error("003754a0: a camera record without a positive half fov and near < far");
    const double px = 1.0 / std::tan(half_fov_100), py = px * 4.0 / 3.0;
    const double pz = (far_fc + near_f4) / (far_fc - near_f4), pw = -2.0 * far_fc * near_f4 / (far_fc - near_f4);
    const auto e = figure_camera_eye(r, eye);
    // Camera coordinate i of a world point p: (p - e) . row i; A: (x, y, z) -> (x, z, y).
    std::array<double, 3> t{};
    for (std::size_t i = 0; i < 3; ++i)
        t[i] = -(static_cast<double>(e[0]) * r[i][0] + static_cast<double>(e[1]) * r[i][1] + static_cast<double>(e[2]) * r[i][2]);
    std::array<std::array<float, 4>, 4> col{};
    for (std::size_t k = 0; k < 3; ++k)
        col[k] = {static_cast<float>(px * r[0][k]), static_cast<float>(py * r[2][k]), static_cast<float>(pz * r[1][k]),
                  static_cast<float>(r[1][k])};
    col[3] = {static_cast<float>(px * t[0]), static_cast<float>(py * t[2]), static_cast<float>(pz * t[1] + pw),
              static_cast<float>(t[1])};
    return col;
}

FigurePlacement figure_slot_placement(const std::array<float, 4>& turns, const std::array<float, 4>& translation,
                                      const FigureRows& rotation) {
    NiMatrix m{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    ni_rotate_rows(m, ni_axis_angle(1.0F, 0.0F, 0.0F, turns[0]));   // 0035b7a8 / 0035b7b4
    ni_rotate_rows(m, ni_axis_angle(0.0F, 1.0F, 0.0F, turns[1]));   // 0035b7ec / 0035b7f8
    ni_rotate_rows(m, ni_axis_angle(0.0F, 0.0F, 1.0F, turns[2]));   // 0035b830 / 0035b83c
    m[3] = {fin(translation[0]), fin(translation[1]), fin(translation[2]), fin(translation[3])};
    ni_rotate_rows(m, rotation);                                    // 0035b84c..0035b8b8
    const std::array<float, 16> rows{m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3],
                                     m[2][0], m[2][1], m[2][2], m[2][3], m[3][0], m[3][1], m[3][2], m[3][3]};
    return {secondary_matrix_quaternion(rows), m[3]};
}

std::array<float, 3> figure_camera_direction(const FigureRows& r, const std::array<float, 3>& d) {
    std::array<float, 3> out{};
    for (std::size_t j = 0; j < 3; ++j)
        out[j] = add(add(mul(d[0], r[0][j]), mul(d[1], r[1][j])), mul(d[2], r[2][j]));
    return out;
}
}  // namespace omega
