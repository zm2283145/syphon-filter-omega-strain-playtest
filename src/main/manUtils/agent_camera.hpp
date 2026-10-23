#pragma once
// Checkpoint 132cc6: the agent screens' model camera (research/FRONTEND.md "Checkpoint 132cc6").
// GuiAgentCreate (+80..+ac, transition +c0, selector +d0, model +70, pair +ed) and GuiAgentModify
// (the same fields +0x20 on: +a0..+cc, +e0, +f0, +8c, +10d) keep a camera preset: a MANSORT2.TXT
// record (man_sort.hpp ManSortRecord) copied by the ctors (002903fc / 00344934: record 0) and
// eased each draw toward the current category's record (0028e640 / 003428d0). The draws 0028d870 /
// 00341f00 project the model through it: half fov +94, near 2, far 15000 (record mode 0x15), the eye
// (+98, -+9c, +a0) through the camera turns +b0 / +b4 / +b8 (never written: zero), and the model's
// anchor (key 004a9a40 "", ANIMIDLE's root anchor, 003ae5e0) placed at (0, 0, -3) turned
// identity x Rx(+a4) x Ry(+a8) x Rz(+ac) (191100).
#include "agent_modify.hpp"
#include "figure_camera.hpp"
#include "ni_event_object.hpp"

#include <array>
#include <cstdint>

namespace omega {

// A record's fields as a preset: key, class (+90), fov, location, rotation.
AgentCamera agent_camera_record(const ManSortRecord& record);

// The ctors' copy of record 0 (0028f760 at 002903fc, 00343e10 at 00344934). No records: throws.
AgentCamera agent_camera_initial(const AgentPartCache& cache);

// 003ffa20(&maxd, target, cur): cur + (target - cur) / 8; maxd = max(maxd, |step|) (not when <=).
float agent_camera_step(float& maxd, float target, float cur);
// 003ff970: the same after wrapping cur by 2 pi toward target (d = target - cur: d > pi adds, d < -pi
// subtracts, both tested against the first d).
float agent_camera_step_angle(float& maxd, float target, float cur);

// 0028e640 (GuiAgentCreate) / 003428d0 (GuiAgentModify): nothing while `transition` is 0. The target
// is record 0 when `whole` (+d0 / +f0 nonzero), else record order.record[pair] (model +2e10 by +ed /
// +10d). Fov and location step by 003ffa20, the turns by 003ff970; `transition` counts the frames;
// when the largest step is under 1e-4 (0x38d1b717) it is 0 and the record is copied exactly (name and
// value too). Returns whether it ran. `address` names the caller in the throws (a pair or index
// outside the arrays reads other fields in the original).
bool agent_camera_approach(const AgentPartCache& cache, AgentCamera& camera, std::int32_t& transition,
                           const AgentPartOrder& order, std::int8_t pair, bool whole, std::uint32_t address);

// The draw's projection (0028d870 / 00341f00): `clip` the record's columns for a world point (as
// figure_camera_clip) and `model` the model's rows (0..2 the turns, 3 the anchor (0, 0, -3, 1)): a
// model-frame point p is drawn at p x model.
// `turns` makes the camera rotation R (+b0..+b8, zero for the agent screens; GuiAgentDisplay's
// +80..+88, (0, 0, pi) from its ctor 00361d90): the eye and, when nonzero, the model's rows go through R.
struct AgentCameraView {
    std::array<std::array<float, 4>, 4> clip{};
    NiMatrix model{};
    std::array<std::array<float, 4>, 4> rotation{};  // R
};
AgentCameraView agent_camera_view(const AgentCamera& camera, const std::array<float, 4>& turns = {});

}  // namespace omega
