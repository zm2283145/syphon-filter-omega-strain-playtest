#include "agent_camera.hpp"

#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(std::uint32_t address, const std::string& what) {
    char a[16];
    std::snprintf(a, sizeof a, "%08x", address);
    throw std::runtime_error(std::string(a) + ": " + what);
}
constexpr float pi = 3.14159274F;       // 0x40490fdb
constexpr float two_pi = 6.28318548F;   // 0x40c90fdb

}  // namespace

AgentCamera agent_camera_record(const ManSortRecord& r) {
    return {r.key, r.thermal_class, r.fov, r.location, r.rotation};
}

AgentCamera agent_camera_initial(const AgentPartCache& cache) {
    if (cache.sort.empty()) fail(0x2903fc, "the camera preset copy without MANSORT2 records (cache +11c10)");
    return agent_camera_record(cache.sort[0]);
}

float agent_camera_step(float& maxd, float target, float cur) {
    float step = (target - cur) / 8.0F;
    const float out = cur + step;
    if (step < 0.0F) step = -step;
    if (!(step <= maxd)) maxd = step;
    return out;
}

float agent_camera_step_angle(float& maxd, float target, float cur) {
    const float d = target - cur;
    if (!(d <= pi)) cur = cur + two_pi;
    if (d < -pi) cur = cur - two_pi;
    return agent_camera_step(maxd, target, cur);
}

bool agent_camera_approach(const AgentPartCache& cache, AgentCamera& cam, std::int32_t& transition,
                           const AgentPartOrder& order, std::int8_t pair, bool whole, std::uint32_t address) {
    if (transition == 0) return false;
    if (cache.sort.empty()) fail(address, "the camera approach without MANSORT2 records (cache +11c10)");
    std::size_t index = 0;
    if (!whole) {
        if (pair < 0 || pair >= 32)
            fail(address, "pair " + std::to_string(pair) + " outside the model's +2e10 record bytes");
        const std::int8_t r = order.record[static_cast<std::size_t>(pair)];
        if (r < 0 || static_cast<std::size_t>(r) >= cache.sort.size())
            fail(address, "record " + std::to_string(r) + " outside MANSORT2's " + std::to_string(cache.sort.size()));
        index = static_cast<std::size_t>(r);
    }
    const ManSortRecord& t = cache.sort[index];
    float maxd = 0.0F;
    cam.half_fov = agent_camera_step(maxd, t.fov, cam.half_fov);
    for (std::size_t k = 0; k < 3; ++k) cam.loc[k] = agent_camera_step(maxd, t.location[k], cam.loc[k]);
    for (std::size_t k = 0; k < 3; ++k) cam.rot[k] = agent_camera_step_angle(maxd, t.rotation[k], cam.rot[k]);
    ++transition;
    if (maxd < 1.0e-4F) {  // 0x38d1b717
        transition = 0;
        cam = agent_camera_record(t);
    }
    return true;
}

AgentCameraView agent_camera_view(const AgentCamera& cam, const std::array<float, 4>& turns) {
    for (float v : {cam.half_fov, cam.loc[0], cam.loc[1], cam.loc[2], cam.rot[0], cam.rot[1], cam.rot[2], turns[0],
                    turns[1], turns[2]})
        if (!std::isfinite(v)) fail(0x28d870, "a non-finite agent camera");
    AgentCameraView v;
    // 0028d9b4..0028da48: Mtx44_SetRotation (00140e20) of +b0 / +b4 / +b8 (zero for the agent screens;
    // GuiAgentDisplay's +80 / +84 / +88, 00361f84); the eye (+98, -+9c, +a0) through it.
    const auto rotation = figure_camera_rotation(turns);
    v.rotation = rotation;
    v.clip = figure_camera_clip(rotation, {cam.loc[0], -cam.loc[1], cam.loc[2]}, cam.half_fov, 2.0, 15000.0);
    // 0028dc08..0028dd34: identity x Rx(+a4) x Ry(+a8) x Rz(+ac) (001d3e20 / 00132130 / 0023b450), row 3
    // (0, 0, -3, 1); 0028dd3c..0028ddb4: the four rows through R.
    NiMatrix m{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    ni_rotate_rows(m, ni_axis_angle(1.0F, 0.0F, 0.0F, cam.rot[0]));
    ni_rotate_rows(m, ni_axis_angle(0.0F, 1.0F, 0.0F, cam.rot[1]));
    ni_rotate_rows(m, ni_axis_angle(0.0F, 0.0F, 1.0F, cam.rot[2]));
    m[3] = {0.0F, 0.0F, -3.0F, 1.0F};
    // The four rows through R (0023b450's form; 003622e4 in GuiAgentDisplay's 00361e70). R is the
    // identity for zero turns, so the agent screens skip it and stay bit-identical.
    if (turns[0] != 0.0F || turns[1] != 0.0F || turns[2] != 0.0F || turns[3] != 0.0F) {
        NiMatrix r{};
        for (std::size_t i = 0; i < 4; ++i)
            for (std::size_t k = 0; k < 4; ++k) r[i][k] = rotation[i][k];
        ni_rotate_rows(m, r);
    }
    v.model = m;
    return v;
}

}  // namespace omega
