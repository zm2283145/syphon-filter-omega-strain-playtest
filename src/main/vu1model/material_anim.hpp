#pragma once

#include "geometry.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace omega {

// Material animators of VU1 program 13 (shader 13), created per model
// instance and mesh record by 00376ad0 and stepped by the animator list
// 003d24b0. Flag 2 scrolls the UVs through the q1 vector of the mesh's VU1
// packet (program 13 adds q1.xy to the UV bias vf10, so the drawn UV is the
// stored UV minus q1.xy, in texture repeats); flag 4 steps the frame of the
// texture's TDX animation (texobj +0x4c).

// One VU1 buffer half of a mesh packet as the animators see it: the word at
// packet +0x10 / +0x90 (the material flags written by 00376ad0) and the q1
// vector at +0x20 / +0xa0.
struct MaterialPacketHalf {
    std::uint32_t flags{};
    float q1[4]{};
};

// The texman object fields the frame animator reads and writes.
struct TextureFrameObject {
    std::uint32_t frame{};     // +0x4c: the uploaded frame (0 at creation, 003bba44)
    std::uint16_t count{1};    // TDX header +0x22 (via texobj +0x44): frames in the image
    std::uint8_t uploaded[2]{1, 1};  // +0x60/+0x61: cleared to request an upload
};

class MaterialAnimator {
public:
    virtual ~MaterialAnimator() = default;
    virtual void update(float dt) = 0;   // vtable +8
    virtual void init() = 0;             // vtable +0xc
};

// 003d1e90 (0x1c bytes, vtable 0x4e0090): +0 texture, +4 parameters, +8/+0xc
// the packet halves, +0x14/+0x18 the speeds (material +8/+0xc as floats).
class UvScrollAnimator final : public MaterialAnimator {
public:
    UvScrollAnimator(std::int16_t texture, std::uint32_t speed_u, std::uint32_t speed_v, MaterialPacketHalf& a,
                     MaterialPacketHalf& b);
    void update(float dt) override;   // 003d1c80
    void init() override;             // 003d1e70
    float speed_u() const { return speed_u_; }
    float speed_v() const { return speed_v_; }
    // Checkpoint 132zf11: the tank's treads write +14 / +18 (00285810 / 00287a20).
    void set_speeds(float u, float v) { speed_u_ = u; speed_v_ = v; }
    std::int16_t texture() const { return texture_; }

private:
    std::int16_t texture_;
    MaterialPacketHalf* a_;
    MaterialPacketHalf* b_;
    float speed_u_, speed_v_;
};

// 003d1b60 (0x34 bytes, vtable 0x4e0060): +0 texture, +0x14 frame count,
// +0x18 first, +0x1c last, +0x20 mode (material +8), +0x24 period
// (1 / material +0xc), +0x28 current frame, +0x2c direction, +0x30 time.
class TextureFrameAnimator final : public MaterialAnimator {
public:
    // textures is the texman table (*0x539248 +8); random is 00127dd0, needed
    // by mode 5 only (absent: that mode throws naming the address).
    TextureFrameAnimator(std::int16_t texture, std::uint32_t mode, std::uint32_t rate,
                         std::vector<TextureFrameObject>& textures, std::function<std::uint32_t()> random = {});
    void update(float dt) override;   // 003d1960
    void init() override;             // 003d1950

    std::uint32_t mode() const { return mode_; }
    float period() const { return period_; }
    std::uint32_t count() const { return count_; }
    std::uint32_t first() const { return first_; }
    std::uint32_t last() const { return last_; }
    std::uint32_t current() const { return current_; }
    std::uint32_t direction() const { return direction_; }
    float time() const { return time_; }
    void set_time(float time) { time_ = time; }

private:
    TextureFrameObject& object() const;

    std::int16_t texture_;
    std::vector<TextureFrameObject>* textures_;
    std::function<std::uint32_t()> random_;
    std::uint32_t count_{}, first_{}, last_{}, mode_{};
    float period_{};
    std::uint32_t current_{}, direction_{};
    float time_{};
};

// The animator list *0x53924c: 003d2790 inserts before the first node, so the
// update visits animators in reverse registration order. The reset byte +0xc
// (set by 001358bc at level start) makes the next update call every
// animator's init first, then clears it.
class MaterialAnimationList {
public:
    MaterialAnimator& add(std::unique_ptr<MaterialAnimator> animator);   // 003d2790
    void request_reset() { reset_ = true; }
    void set_reset(bool reset) { reset_ = reset; }
    bool reset() const { return reset_; }
    void update(float dt);                                                // 003d24b0
    std::size_t size() const { return animators_.size(); }
    MaterialAnimator& at(std::size_t registration) const { return *animators_.at(registration); }

private:
    std::vector<std::unique_ptr<MaterialAnimator>> animators_;   // registration order
    bool reset_{};
};

// 00376ad0's choice per mesh record: flag 2 makes a UV scroll animator, else
// flag 4 a frame animator, else none. The record's first strip packet
// receives the material flags in both halves (single-pass materials).
enum class MaterialAnimationKind { none, uv_scroll, frames };
MaterialAnimationKind material_animation_kind(std::uint32_t material_flags);

// 00376ad0 for one model: for each mesh record (the mesh's surfaces
// [first_surface, first_surface + surface_count), one per record) whose
// material has flag 2 a UvScrollAnimator with material +8/+0xc as speeds,
// else flag 4 a TextureFrameAnimator on the material's texman index (mode
// and rate from material +8/+0xc), each registered in record order. The
// record's packet halves are halves[2 * surface] and [2 * surface + 1]
// (sized two per mesh surface, outliving the list); their flags words get
// the material flags. Animated multi-pass materials (00376e38: flags on
// every strip) occur on no disc model and throw. Returns the animators made.
std::size_t create_material_animators(const Mesh& mesh, std::size_t first_surface, std::size_t surface_count,
                                      std::vector<MaterialPacketHalf>& halves, MaterialAnimationList& list,
                                      std::vector<TextureFrameObject>& textures,
                                      std::function<std::uint32_t()> random = {});

}
