#include "hud_screen_draw.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
std::uint32_t bits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}
bool vu_operand(float f) { return ((bits(f) >> 23) & 0xff) != 0xff; }
// The interpreter's macro-mode VU0 (tools/ee/ee.py): binary32 products and sums, the
// accumulator and destination flushed to signed zero when denormal; lqc2 operands unflushed.
float ftz(float f) {
    const std::uint32_t b = bits(f);
    if ((b & 0x7f800000U) == 0 && (b & 0x007fffffU) != 0) {
        const std::uint32_t z = b & 0x80000000U;
        std::memcpy(&f, &z, 4);
    }
    return f;
}
}  // namespace

std::array<float, 16> hud_screen_draw_matrix(const HudScreenDrawGlobals& g) {
    // 0x003e7900..0x003e794c: VU0 products of the identity top with the rows; an exponent-255
    // operand (the VU's clamped arithmetic) is not modelled.
    if (!vu_operand(g.scale_493950) || !vu_operand(g.depth_493958))
        throw std::runtime_error("unhosted(0x3e7900): HUD screen draw scale / depth with exponent 255");
    const float s = g.scale_493950, z = g.depth_493958;
    // sp+40..+7c (0x003e782c..0x003e78d0).
    const float rows[4][4] = {{s, 0, 0, 0}, {0, 0, s, 0}, {0, 1, 0, 0}, {0, 0, z, 1}};
    // 0x003e7728..0x003e7744: the new top is vf0 and its shuffles (the identity).
    const float top[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    std::array<float, 16> out{};
    for (int r = 0; r < 4; ++r)
        for (int j = 0; j < 4; ++j) {
            // vmulax / vmadday / vmaddaz / vmaddw: ((t0*m.x + t1*m.y) + t2*m.z) + t3*m.w.
            float acc = ftz(top[0][j] * rows[r][0]);
            acc = ftz(acc + top[1][j] * rows[r][1]);
            acc = ftz(acc + top[2][j] * rows[r][2]);
            out[static_cast<std::size_t>(r * 4 + j)] = ftz(acc + top[3][j] * rows[r][3]);
        }
    return out;
}

bool hud_screen_draw_003e7670(IeDrawState& st, const HudScreenDrawGlobals& g, const std::function<void()>& child_003ed2e0) {
    if (g.enabled_493948 == 0) return false;  // 0x003e7690
    const auto m = hud_screen_draw_matrix(g);
    const std::uint32_t saved_mode = st.mode;  // 0x003e7764
    st.push();                                 // 0x003e76a0..0x003e7710
    st.top() = m;                              // 0x003e7728..0x003e794c
    st.dirty = 1;                              // 0x003e7958
    st.record_106 = 1;                         // 0x003e781c (the record's ortho near / far are host)
    st.record_104 = 1;                         // 0x003e7824
    child_003ed2e0();                          // 0x003e795c
    st.record_106 = 1;                         // 0x003e7974
    st.record_104 = 1;                         // 0x003e7978
    st.mode = saved_mode;                      // 0x003e797c
    st.dirty = 1;                              // 0x003e7984
    --st.depth;                                // 0x003e798c..0x003e7998
    return true;
}

bool hud_screen_draw_003e7670(GuiDrawList& out, IeDrawState& st, IeScreen& screen, const HudScreenDrawGlobals& g) {
    return hud_screen_draw_003e7670(st, g, [&] { ie_draw_child(out, st, screen, screen.root()); });
}

}  // namespace omega
