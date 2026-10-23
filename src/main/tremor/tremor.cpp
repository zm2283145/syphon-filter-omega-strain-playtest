#include "tremor.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("Tremor: " + what); }
// EE FPU / VU lanes: binary32, round to nearest, denormal results flushed.
float ftz(float v) {
    if(!std::isfinite(v)) fail("non-finite arithmetic");
    return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v;
}
float add(float a, float b) { return ftz(a + b); }
float sub(float a, float b) { return ftz(a - b); }
float mul(float a, float b) { return ftz(a * b); }
float div(float a, float b) {
    if(b == 0) fail("division by zero");
    return ftz(a / b);
}
std::int32_t cvt_w_s(float v) {                    // cvt.w.s: truncation
    if(!(std::fabs(v) < 2147483648.0F)) fail("cvt.w.s outside the integer range");
    return static_cast<std::int32_t>(v);
}
}

void add_tremor(TremorList& list, const RootVector& position, float duration, float magnitude, float inner,
    float outer) {
    TremorRecord r;
    r.duration_00 = duration;
    r.elapsed_04 = 0.0F;
    r.inner_08 = inner;
    r.outer_0c = outer;
    r.magnitude_10 = magnitude;
    r.position_20 = position;
    list.records.push_back(r);
}

void add_script_tremor(TremorList& list, const RootVector& position, float duration, float magnitude,
    float radius) {
    add_tremor(list, position, duration, magnitude, div(radius, 3.0F), radius);   // 0012f964 div.s
}

TremorFrame update_tremors(TremorList& list, float dt, const RootVector& camera_e4, const TremorVariables& v,
    const std::function<std::uint32_t()>& random) {
    float sum = 0.0F;
    std::vector<TremorRecord> kept;
    kept.reserve(list.records.size());
    for(auto r : list.records) {
        r.elapsed_04 = add(r.elapsed_04, dt);
        if(!(r.elapsed_04 <= r.duration_00)) continue;           // 00138da0, Mem_Free 0036b130
        RootVector d{};
        for(int i = 0; i < 4; ++i) d[i] = sub(camera_e4[i], r.position_20[i]);   // vsub.xyzw
        const float squared = add(add(mul(d[0], d[0]), mul(d[1], d[1])), mul(d[2], d[2]));
        const float distance = add(0.0F, ftz(vu::sqrt(squared)));             // vsqrt, vaddq.x
        const float third = div(r.duration_00, 3.0F);
        const float left = sub(r.duration_00, r.elapsed_04);
        float m = r.magnitude_10;
        if(left < third) m = mul(m, div(left, third));
        if(distance < r.inner_08) {
        } else if(distance < r.outer_0c) {
            m = mul(m, div(sub(r.outer_0c, distance), sub(r.outer_0c, r.inner_08)));
        } else {
            m = 0.0F;
        }
        sum = add(sum, m);
        kept.push_back(r);
    }
    list.records = std::move(kept);
    TremorFrame f;
    sum = std::min(sum, 1.0F);                                       // min.s
    f.sum = sum;
    f.shake_target = mul(sum, v.cam_factor_d0);
    if(!(sum <= 0.0F)) {
        if(!random) fail("002722b0: the game random 00127dd0 is not hosted");
        const float spread = mul(sum, v.rand_pct_e8);
        const float scale = add(1.0F, 2147483648.0F);
        const float u = div(game_int_to_float(static_cast<std::int32_t>(random())), scale);
        sum = add(sum, sub(mul(spread, u), spread));
        const float range = game_int_to_float(v.max_rumble_e0 - v.min_rumble_d8);   // subu, cvt.s.w
        f.rumble_large = cvt_w_s(add(game_int_to_float(v.min_rumble_d8), mul(sum, range)));
    }
    return f;
}

void set_tremor_shake(CameraScalarCurve& c, float target, float duration) {
    c.target = target;
    if(duration < 0.0F) return;
    const float factor = mul(duration, c.inverse_duration);
    c.m0 = mul(c.m0, factor);
    c.m1 = mul(c.m1, factor);
    c.duration = duration;
    c.inverse_duration = duration == 0.0F ? 0.0F : div(1.0F, duration);
    c.remaining = c.duration;
}

}