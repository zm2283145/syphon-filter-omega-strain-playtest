#include "material_anim.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

// EE FPU results flush denormals to zero (add.s/sub.s/mul.s/div.s).
float ftz(float r) { return std::fpclassify(r) == FP_SUBNORMAL ? (std::signbit(r) ? -0.0F : 0.0F) : r; }
float add_s(float a, float b) { return ftz(a + b); }
float sub_s(float a, float b) { return ftz(a - b); }
float mul_s(float a, float b) { return ftz(a * b); }
// A zero divisor gives +-FLT_MAX (sign of S*T, + when S is 0).
float div_s(float a, float b) {
    if (b == 0)
        return std::copysign(std::numeric_limits<float>::max(),
                             a != 0 && std::signbit(a) != std::signbit(b) ? -1.0F : 1.0F);
    return ftz(a / b);
}
// cvt.w.s truncates and saturates.
std::int32_t cvt_w_s(float f) {
    const double d = f;
    if (std::isnan(d)) throw std::runtime_error("material animator 003d1aec: cvt.w.s of NaN");
    if (d >= 2147483647.0) return std::numeric_limits<std::int32_t>::max();
    if (d <= -2147483648.0) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(d);
}
float cvt_s_w(std::uint32_t v) { return static_cast<float>(static_cast<std::int32_t>(v)); }
// The compiler's unsigned-to-float (003d1aa4): cvt.s.w, or for the sign bit
// set cvt.s.w((u >> 1) | (u & 1)) doubled with add.s.
float u2f(std::uint32_t u) {
    if (static_cast<std::int32_t>(u) >= 0) return cvt_s_w(u);
    const float half = cvt_s_w((u >> 1) | (u & 1));
    return add_s(half, half);
}
bool signed_less(std::uint32_t a, std::uint32_t b) { return static_cast<std::int32_t>(a) < static_cast<std::int32_t>(b); }

// 003d1d10..003d1d7c (and 003d1db0.. for the second half): each component
// above 1 loses 1, then each below -1 gains 1, x before y.
void wrap(MaterialPacketHalf& half) {
    for (int k = 0; k < 2; ++k)
        if (!(half.q1[k] <= 1.0F)) half.q1[k] = sub_s(half.q1[k], 1.0F);
    for (int k = 0; k < 2; ++k)
        if (half.q1[k] < -1.0F) half.q1[k] = add_s(half.q1[k], 1.0F);
}

}  // namespace

UvScrollAnimator::UvScrollAnimator(std::int16_t texture, std::uint32_t speed_u, std::uint32_t speed_v,
                                   MaterialPacketHalf& a, MaterialPacketHalf& b)
    : texture_(texture), a_(&a), b_(&b), speed_u_(std::bit_cast<float>(speed_u)), speed_v_(std::bit_cast<float>(speed_v)) {
    init();   // 003d1ed4: vtable +0xc
}

void UvScrollAnimator::init() {
    a_->q1[0] = 0; a_->q1[1] = 0;
    b_->q1[0] = 0; b_->q1[1] = 0;
}

void UvScrollAnimator::update(float dt) {
    const float du = mul_s(speed_u_, dt), dv = mul_s(speed_v_, dt);
    // 003d1cc4/003d1d0c: each q1 qword is rewritten as (x + du, y + dv, 0, 0).
    const float ax = add_s(a_->q1[0], du), ay = add_s(a_->q1[1], dv);
    a_->q1[0] = ax; a_->q1[1] = ay; a_->q1[2] = 0; a_->q1[3] = 0;
    const float bx = add_s(b_->q1[0], du), by = add_s(b_->q1[1], dv);
    b_->q1[0] = bx; b_->q1[1] = by; b_->q1[2] = 0; b_->q1[3] = 0;
    wrap(*a_);
    wrap(*b_);
}

TextureFrameAnimator::TextureFrameAnimator(std::int16_t texture, std::uint32_t mode, std::uint32_t rate,
                                           std::vector<TextureFrameObject>& textures, std::function<std::uint32_t()> random)
    : texture_(texture), textures_(&textures), random_(std::move(random)), mode_(mode),
      period_(div_s(1.0F, std::bit_cast<float>(rate))) {
    count_ = object().count;
    first_ = 0;
    last_ = count_ - 1;
    // Table 0x4bd360: modes 0/2/4 run forwards from the first frame, 1/3
    // backwards from the last, 5 and anything above stand at the first.
    switch (mode_) {
    case 0: case 2: case 4: current_ = first_; direction_ = 1; break;
    case 1: case 3: current_ = last_; direction_ = 0xffffffffU; break;
    default: current_ = first_; direction_ = 0; break;
    }
    init();   // 003d1c50: vtable +0xc
}

TextureFrameObject& TextureFrameAnimator::object() const {
    // *(*0x539248 + 8)[texture]: the s16 index from the instance's texture map.
    if (texture_ < 0 || static_cast<std::size_t>(texture_) >= textures_->size())
        throw std::out_of_range("material animator 003d1b14: texture index " + std::to_string(texture_)
                                + " outside the texman table");
    return (*textures_)[static_cast<std::size_t>(texture_)];
}

void TextureFrameAnimator::init() { time_ = 0; }

void TextureFrameAnimator::update(float dt) {
    time_ = add_s(time_, dt);
    if (!(time_ <= period_)) {
        time_ = sub_s(time_, period_);
        current_ += direction_;
        switch (mode_) {   // jump table 0x4bd340; modes 6 and above only advance
        case 0:
            if (signed_less(last_, current_)) { current_ = last_; direction_ = 0; }
            break;
        case 1:
            if (signed_less(current_, first_)) { current_ = first_; direction_ = 0; }
            break;
        case 2:
            if (signed_less(last_, current_)) current_ = first_;
            break;
        case 3:
            if (signed_less(current_, first_)) current_ = last_;
            break;
        case 4:
            if (signed_less(current_, first_) || signed_less(last_, current_)) {
                direction_ = 0u - direction_;
                current_ += direction_;
            }
            break;
        case 5: {
            if (!random_) throw std::logic_error("material animator 003d1a98: rand 00127dd0 not hosted");
            const std::uint32_t r = random_();
            const float scale = div_s(cvt_s_w(r), 2147483648.0F);
            current_ = static_cast<std::uint32_t>(cvt_w_s(mul_s(u2f(count_), scale)));
            break;
        }
        default: break;
        }
    }
    // 003d1b00: a different, valid frame is stored and its upload requested.
    auto& texobj = object();
    if (current_ != texobj.frame && current_ < texobj.count) {
        texobj.uploaded[0] = 0;
        texobj.uploaded[1] = 0;
        texobj.frame = current_;
    }
}

MaterialAnimator& MaterialAnimationList::add(std::unique_ptr<MaterialAnimator> animator) {
    if (!animator) throw std::invalid_argument("material animation list 003d2790: null animator");
    animators_.push_back(std::move(animator));
    return *animators_.back();
}

void MaterialAnimationList::update(float dt) {
    // Nodes are inserted before the first (00138ea0 at begin), so the list
    // runs from the latest registration to the earliest.
    if (reset_) {
        for (auto it = animators_.rbegin(); it != animators_.rend(); ++it) (*it)->init();
        reset_ = false;
    }
    for (auto it = animators_.rbegin(); it != animators_.rend(); ++it) (*it)->update(dt);
}

MaterialAnimationKind material_animation_kind(std::uint32_t material_flags) {
    if (material_flags & 2) return MaterialAnimationKind::uv_scroll;
    if (material_flags & 4) return MaterialAnimationKind::frames;
    return MaterialAnimationKind::none;
}

std::size_t create_material_animators(const Mesh& mesh, std::size_t first_surface, std::size_t surface_count,
                                      std::vector<MaterialPacketHalf>& halves, MaterialAnimationList& list,
                                      std::vector<TextureFrameObject>& textures, std::function<std::uint32_t()> random) {
    if (first_surface > mesh.surfaces.size() || surface_count > mesh.surfaces.size() - first_surface
        || halves.size() != mesh.surfaces.size() * 2)
        throw std::out_of_range("material animators 00376ad0: mesh record range or packet halves");
    std::size_t made = 0;
    for (std::size_t s = first_surface; s < first_surface + surface_count; ++s) {
        const auto& material = mesh.materials.at(mesh.surfaces[s].material);
        const auto kind = material_animation_kind(material.flags);
        if (kind != MaterialAnimationKind::none && material.passes.size() != 1)
            throw std::runtime_error("material animators 00376e38: animated multi-pass material (flags on every strip)");
        auto& a = halves[2 * s];
        auto& b = halves[2 * s + 1];
        // *(instance texmap +8)[material +0x38], read as s16 (00376c88..00376ca4).
        const auto texture = static_cast<std::int16_t>(material.texture);
        if (kind == MaterialAnimationKind::uv_scroll)
            list.add(std::make_unique<UvScrollAnimator>(texture, material.animation[0], material.animation[1], a, b));
        else if (kind == MaterialAnimationKind::frames)
            list.add(std::make_unique<TextureFrameAnimator>(texture, material.animation[0], material.animation[1], textures, random));
        made += kind != MaterialAnimationKind::none;
        // 00376dd0..00376df4: the first strip's packet halves take the material flags.
        a.flags = material.flags;
        b.flags = material.flags;
    }
    return made;
}

}
