#include "hud_object.hpp"
#include "character_shadows.hpp"
#include "hud_map.hpp"
#include "game_libm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

[[noreturn]] void fail(const char* address, const std::string& what) {
    throw std::runtime_error(std::string("HUD callout ") + address + ": " + what);
}

float mul(float a, float b) { return ie_ftz(a * b); }
float add(float a, float b) { return ie_ftz(a + b); }
float sub(float a, float b) { return ie_ftz(a - b); }
// madd.s / vmadd lane: the product rounded to binary32, the sum flushed.
float madd(float acc, float a, float b) {
    const float p = a * b;
    return ie_ftz(acc + p);
}
// div.s as tools/ee/ee.py runs it: a zero divisor gives the largest float
// signed as S * T (positive for a zero S).
float ee_div(float s, float t) {
    if (t != 0.0F) return ie_ftz(s / t);
    const double sign = s != 0.0F ? static_cast<double>(s) * static_cast<double>(t) : 1.0;
    return std::copysign(std::numeric_limits<float>::max(), static_cast<float>(sign));
}

std::array<float, 4> colour_words(const std::array<std::uint32_t, 4>& words) {
    std::array<float, 4> out{};
    for (std::size_t k = 0; k < 4; ++k) out[k] = std::bit_cast<float>(words[k]);
    return out;
}

using Quad = std::array<float, 4>;
using Rows = std::array<Quad, 4>;

// vmulax / vmadday / vmaddaz / vmaddw: m's rows scaled by v's components,
// lane by lane.
Quad rows_apply(const Rows& m, const Quad& v) {
    Quad o{};
    for (std::size_t k = 0; k < 4; ++k)
        o[k] = madd(madd(madd(mul(m[0][k], v[0]), m[1][k], v[1]), m[2][k], v[2]), m[3][k], v[3]);
    return o;
}

Quad vsub(const Quad& a, const Quad& b) { return {sub(a[0], b[0]), sub(a[1], b[1]), sub(a[2], b[2]), sub(a[3], b[3])}; }

// vmul.xyz, vmulax.w / vmadday.w / vmaddz.w by vf0.w = 1, vrsqrt Q = 1 /
// (float)sqrt(|s|) (the VU's maximum for a zero s), vmulq.xyz; w kept.
Quad normalise(Quad v) {
    const float x2 = mul(v[0], v[0]), y2 = mul(v[1], v[1]), z2 = mul(v[2], v[2]);
    const float s = madd(madd(mul(1.0F, x2), 1.0F, y2), 1.0F, z2);
    const float d = static_cast<float>(std::sqrt(std::fabs(static_cast<double>(s))));
    const float q = d != 0.0F ? ie_ftz(1.0F / d) : std::bit_cast<float>(0x7f7fffffU);
    for (std::size_t k = 0; k < 3; ++k) v[k] = mul(v[k], q);
    return v;
}

// vopmula.xyz ACC, a, b; vopmsub.xyz c, b, a; vsubw.w c, vf0, vf0.
Quad cross(const Quad& a, const Quad& b) {
    return {sub(mul(a[1], b[2]), mul(b[1], a[2])), sub(mul(a[2], b[0]), mul(b[2], a[0])),
            sub(mul(a[0], b[1]), mul(b[0], a[1])), 0.0F};
}

// vmul.xyzw, then vmulax.w / vmadday.w / vmaddz.w by vf0.w = 1 (qfsrv: the
// w lane).
float dot(const Quad& a, const Quad& b) {
    return madd(madd(mul(1.0F, mul(a[0], b[0])), 1.0F, mul(a[1], b[1])), 1.0F, mul(a[2], b[2]));
}

// acosf 00121a88 -> 0011cfd8; outside [-1, 1] its (x - x) / (x - x)
// (0011d034..0011d040) is div.s 0 / 0: +FLT_MAX on the EE.
float acosf_ee(float x) {
    if (std::isfinite(x) && (std::bit_cast<std::uint32_t>(x) & 0x7fffffffU) > 0x3f800000U)
        return ee_div(sub(x, x), sub(x, x));
    return game_acosf(x);
}

// 00132130(out, x, y, z, angle): the rotation about (x, y, z), rows.
Rows rotation_132130(float x, float y, float z, float angle) {
    const float s = game_sinf(angle);   // 00121928
    const float c = game_cosf(angle);   // 00121688
    const float k = sub(1.0F, c);
    const float f9 = mul(y, mul(k, x)), f8 = mul(z, mul(k, y)), f7 = mul(x, mul(k, z));
    const float sz = mul(s, z), sy = mul(s, y), sx = mul(s, x);
    Rows r{};
    r[0] = {add(c, mul(k, mul(x, x))), add(f9, sz), sub(f7, sy), 0.0F};
    r[1] = {sub(f9, sz), add(c, mul(k, mul(y, y))), add(f8, sx), 0.0F};
    r[2] = {add(f7, sy), sub(f8, sx), add(c, mul(k, mul(z, z))), 0.0F};
    r[3] = {0.0F, 0.0F, 0.0F, 1.0F};
    return r;
}

// 002465b0(p, rect): !(x < r0) && x <= r0 + r2 && !(y < r1) && y <= r1 + r3.
bool inside_2465b0(const Quad& p, const Quad& r) {
    return !(p[0] < r[0]) && p[0] <= add(r[0], r[2]) && !(p[1] < r[1]) && p[1] <= add(r[1], r[3]);
}

} // namespace

std::array<float, 4> hud_callout_offscreen(const HudCalloutEdgeCamera& camera, const std::array<float, 4>& target,
                                           std::array<float, 4> p, float w, float h) {
    const Rows& m = camera.node_10;
    const float half_fov = camera.half_fov_60;                                     // f24, cam +60
    const float f23 = ee_div(ee_div(w, 2.0F), game_tanf(half_fov));               // 00243690..002436a8
    const float f22 = game_atanf(ee_div(ee_div(h, 2.0F), f23));                   // 002436b0..002436c8
    const float hw = ee_div(w, 2.0F), hh = ee_div(h, 2.0F);
    const Quad pos = m[3];                                                         // node +40
    Quad a = rows_apply(m, {0.0F, 1.0F, 0.0F, 0.0F});                             // 0024374c..00243758
    Quad d = vsub(target, pos);                                                    // 00243780
    d[2] = 0.0F;
    a[2] = 0.0F;
    a = normalise(a);
    d = normalise(d);
    const Quad c = cross(a, d);                                                    // 00243808..00243810
    const float side = dot(c, {0.0F, 0.0F, 1.0F, 0.0F});                          // f20
    const float ang = acosf_ee(dot(d, a));                                        // 002438b4
    if (ang < half_fov) {
        p[0] = mul(f23, game_tanf(ang));                                           // 002438d4
        if (!(side <= 0.0F)) p[0] = ie_ftz(-p[0]);
    } else {
        p[0] = side <= 0.0F ? hw : ie_ftz(-hw);
    }
    const float turned = mul(ang, side <= 0.0F ? -1.0F : 1.0F);                    // 0024395c
    const Rows r = rotation_132130(0.0F, 0.0F, 1.0F, turned);                      // 002439a0
    Rows m2{};
    for (std::size_t i = 0; i < 4; ++i) m2[i] = rows_apply(r, m[i]);              // 002439d0..00243a1c
    Quad u = rows_apply(m2, {0.0F, 1.0F, 0.0F, 0.0F});                            // 00243a98..00243aa4
    Quad e = vsub(target, pos);                                                    // 00243acc
    u = normalise(u);
    e = normalise(e);
    const Quad cr = cross(u, e);                                                   // 00243b48..00243b50
    const Quad r0 = rows_apply(m2, {1.0F, 0.0F, 0.0F, 0.0F});                     // 00243bb8..00243bc4
    const float side2 = dot(cr, r0);
    const float ang2 = acosf_ee(dot(e, u));                                       // 00243c40
    if (ang2 < f22) {
        p[1] = mul(f23, game_tanf(ang2));                                          // 00243c5c
        if (side2 <= 0.0F) p[1] = ie_ftz(-p[1]);
    } else {
        p[1] = side2 <= 0.0F ? ie_ftz(-hh) : hh;
    }
    return p;
}

std::array<float, 4> hud_project_374bc0(const HudCalloutCamera& c, const std::array<float, 4>& p) {
    const auto& v = c.viewport;
    const float hx = ee_div(v[2], 2.0F);                                   // div.s 00374bf4
    const float hz = mul(0.5F, sub(c.far_fc, c.near_f4));                 // 00374c00 / 00374c04
    const float hy = ee_div(v[3], 2.0F);                                   // 00374c0c
    const float ox = add(mul(0.5F, ie_ftz(-v[2])), add(v[0], mul(0.5F, v[2])));  // 00374c20..00374c30
    const float acc = add(v[1], mul(0.5F, v[3]));                          // adda.s 00374c44
    const float oy = madd(acc, 0.5F, ie_ftz(-v[3]));                       // madd.s 00374c4c
    const float oz = add(hz, c.near_f4);                                   // 00374c58
    ShadowMatrix projection;
    for (std::size_t k = 0; k < 4; ++k) projection.column[k] = c.projection_50[k];
    const auto m = shadow_multiply(projection, shadow_axis_matrix());     // 00374c84..00374cc0
    const auto t = shadow_transform(m, p);                                 // 00374d2c..00374d38
    const float inv = ee_div(1.0F, t[3]);                                  // 00374d7c
    return {add(mul(t[0], mul(inv, hx)), ox), add(mul(t[1], mul(inv, hy)), oy), add(mul(t[2], mul(inv, hz)), oz),
            1.0F};
}

std::array<float, 4> hud_map_callout_place(const HudMapCalloutCamera& camera, const std::array<float, 4>& p) {
    const Quad v = rows_apply(camera.view_55a290, p);           // 003e9490..003e94b4
    Quad out = hud_project_374bc0(camera.record, v);           // 003e94e0
    if (out[2] < 0.0F) out[2] = ie_ftz(-out[2]);               // 003e9500..003e9510
    return out;
}

HudCalloutCamera hud_map_callout_record(float zoom_48b2b0, float far_48b290, const std::array<float, 4>& viewport) {
    HudCalloutCamera c;
    c.viewport = viewport;
    c.near_f4 = 1.25F;   // 0x0026cdc0: 0x3fa00000
    c.far_fc = far_48b290;
    const auto w = hud_map_projection_003754a0(zoom_48b2b0, c.near_f4, c.far_fc);
    for (std::size_t k = 0; k < 4; ++k)
        for (std::size_t j = 0; j < 4; ++j) c.projection_50[k][j] = w[4 * k + j];
    return c;
}

void hud_map_callout_tracker_draw(HudLockTracker& t, const HudLockOps& ops, const HudMapCalloutCamera& camera) {
    if (t.deleted) fail("003e9460", "the draw of a deleted map callout tracker");
    if (t.active_64 == 0) return;   // the callers' +64 test (002610f0, 0x00244c4c)
    ops.point(t.element_60, hud_map_callout_place(camera, t.point_70));   // 003e8180(+60, sp+30)
}

void hud_callout_tracker_draw(HudLockTracker& t, const HudLockOps& ops,
                              const std::optional<std::array<std::array<float, 4>, 4>>& camera_node,
                              const HudCalloutCamera& camera) {
    if (t.deleted) fail("002610f0", "the draw of a deleted callout tracker");
    if (t.active_64 == 0) return;
    if (camera_node) {   // 003e9a60
        ShadowMatrix node;
        for (std::size_t k = 0; k < 4; ++k) node.column[k] = (*camera_node)[k];
        t.point_70 = shadow_transform(shadow_rigid_inverse(node), t.point_70);
    }
    const bool behind = t.point_70[1] < 0.0F;   // c.olt.s +74, 0.0
    t.point_70 = hud_project_374bc0(camera, t.point_70);
    if (behind) t.point_70[2] = ie_ftz(-t.point_70[2]);   // neg.s +78
    ops.point(t.element_60, t.point_70);   // 003e8180
}

void hud_callout_name_append(std::string& buf, std::string_view s) {
    if (buf.size() >= 0x7e) return;   // slti 0x7e on the count byte
    for (const char c : s) {
        if (c == '\0') return;
        buf.push_back(c);
        if (buf.size() >= 0x7e) {     // 002c9310: '\n', the count past the cap, NUL
            buf.push_back('\n');
            return;
        }
    }
}

std::string hud_player_callout_name(const std::optional<std::string>& agent_name, std::string_view slot_name,
                                    bool newline) {
    std::string buf;   // sb $zero, count (0026da70)
    if (agent_name) hud_callout_name_append(buf, *agent_name);   // 002c92c4: 00336170
    std::string slot(slot_name.substr(0, std::min<std::size_t>(slot_name.find('\0'), 0xf)));  // 0012f5e0's strncpy 0xf
    std::string part;
    part.push_back(newline ? '\n' : ' ');   // movz 0x20
    part += '[';
    part.push_back('\x11');
    part += slot;
    part.push_back('\x10');
    part += ']';
    hud_callout_name_append(buf, part);   // sprintf "%c[%c%s%c]" (004acf28), then the append
    return buf;
}

void HudObject::callout_text(int element, const std::optional<std::string>& text, const char* address) {
    if (element < 0) fail(address, "003e7ec0 on a null callout element");
    const auto& el = screen_.element_at(element);
    if (el.text < 0) fail(address, "003d08b0 on a callout element without a text object");
    if (text) screen_.set_text(el.text, *text);
    else screen_.text_object(el.text).text = std::string();   // 003d08b0(obj, null): the empty string only
}

bool HudObject::callout_set(std::int32_t key, const std::optional<std::string>& text, std::int32_t slot, bool t0,
                            bool t1, float value, float colour, const HudLockAlpha* alpha) {
    HudCalloutRecord* rec = nullptr;
    for (auto& r : callouts_)
        if (r.key_0 == key && r.slot_8 == slot) {
            rec = &r;
            break;
        }
    if (rec) {
        callout_text(rec->element_c, text, "00243340");
        rec->timer_4 = value;
    } else {
        HudCalloutRecord r;
        r.key_0 = key;
        r.timer_4 = value;
        r.bottom_18 = t1 ? 1 : 0;
        // Checkpoint 132hud2: t0 (the map's player callouts 0026d940) clones origincallout2 *004f7db0 and its
        // tracker is 002434b0 (003e99b0, then vtable 004e0240). Checkpoint 132map1: 004e0240 differs from
        // 003e99b0's 004e05a0 in the destructor +8 (003e93e0 / 003e9940) and in the draw +110 (003e9460 /
        // 003e97d0): map_callout. Checkpoint 132hud4: t1 without t0
        // clones hscallout_bottom *004f7dac (002431d8) with a 003e99b0 tracker and joins the hscallout path at
        // 00243268 (+54 = 1.0); 002434f0 places it.
        const int node = t0 ? globals_.origin_callout : t1 ? globals_.callouts[3] : globals_.hscallout;
        if (node < 0) fail(t0 ? "0024315c" : t1 ? "002431d8" : "00243224",
                           t0 ? "003ef010 on a null origincallout2 node"
                              : t1 ? "003ef010 on a null hscallout_bottom node" : "003ef010 on a null hscallout node");
        r.element_c = screen_.create_element(node);                  // 003ef010(*004f7db0 / *004f7dac / *004f7d90)
        HudLockTracker trk;                                          // 0013d5c0(0x90), 002434b0 / 003e99b0
        trk.element_60 = r.element_c;
        trk.active_64 = 1;
        trk.id_68 = key;
        trk.release_84 = 0;
        trk.callout = true;
        trk.map_callout = t0;                                        // 002434b0: vtable 004e0240
        trackers_.push_back(trk);                                    // 0040c9d0
        r.tracker_10 = static_cast<int>(trackers_.size() - 1);
        const int txt = screen_.element_at(r.element_c).text;        // 002434a0: element +74
        if (t0) {
            if (txt >= 0) {
                screen_.text_object(txt).byte_5b = 1;                // 00243490(text, 1)
                screen_.text_object(txt).byte_10 = 2;                // 00243480(text, 2)
            }
        } else if (txt >= 0) {
            screen_.text_object(txt).f54 = 1.0F;                     // 00243460(text, 1.0)
        }
        r.slot_8 = slot;
        callout_text(r.element_c, text, "00243298");
        float fade = 10.0F;
        if (!callout_line_count)
            fail("002432b0", "003d04c0: the callout text's line count needs the host's font layout");
        if (txt < 0) fail("002432b0", "003d04c0: a line count of a null text object");
        const std::int32_t lines = callout_line_count(screen_.text_object(txt));
        if (lines >= 2) fade = add(fade, static_cast<float>(lines - 1));   // cvt.s.w, add.s
        screen_.fade_element(r.element_c, fade);                     // 003f44e0
        auto& tk = trackers_[static_cast<std::size_t>(r.tracker_10)];  // 003e97b0(+10, &key, +8)
        tk.id_68 = key;
        tk.active_64 = 1;
        tk.sub_80 = r.slot_8;
        r.dist_14 = 0.0F;
        screen_.show_element(r.element_c, 0);                        // 003e8de0(+c, 0)
        callouts_.push_back(r);                                      // 00243430
        rec = &callouts_.back();
    }
    if (colour == 0.0F) {   // c.eq.s
        if (rec->element_c < 0) fail("003e7ee0", "the callout colour on a null element");
        auto& el = screen_.element_at(rec->element_c);
        const auto c = colour_words(hud_callout_colour_587d90);
        for (std::size_t k = 0; k < 4; ++k) el.colour[k] = ie_colour_byte(c[k]);
    } else if (colour < 0.0F) {   // c.olt.s
        if (rec->element_c < 0) fail("00243398", "the callout's +19 on a null element");
        auto& el = screen_.element_at(rec->element_c);
        el.flag_19 = 0;
        if (el.text < 0) fail("003d0810", "the callout text colour on a null text object");
        ie_text_colour(screen_.text_object(el.text), colour_words(hud_callout_colour_587db0));
    } else {
        if (!alpha) fail("00260d90", "a lock-ramp callout colour without the lock alphas");
        if (rec->element_c < 0) fail("003e7ee0", "the callout colour on a null element");
        auto& el = screen_.element_at(rec->element_c);
        hud_lock_meter_element(el, globals_, *alpha, colour);   // 00260d90, 003e7ee0
        if (el.text < 0) fail("003d0810", "the callout text colour on a null text object");
        ie_text_colour(screen_.text_object(el.text), colour_words(hud_callout_colour_587da0));
    }
    return true;
}

bool HudObject::callout_clear(std::int32_t key, std::int32_t slot, bool a3, bool t0, bool multiplayer) {
    if (multiplayer && t0) fail("00242b80", "the multiplayer callout clear message is not hosted");
    std::size_t i = 0;
    for (; i < callouts_.size(); ++i)
        if (callouts_[i].key_0 == key && callouts_[i].slot_8 == slot) break;
    if (i == callouts_.size()) return false;
    const auto rec = callouts_[i];
    if (a3) {
        if (rec.element_c < 0) fail("00242c70", "003f45a0 on a null callout element");
        screen_.evaluate_element(rec.element_c, 0.0F);   // 003f45a0
        screen_.show_element(rec.element_c, 0);
    }
    if (rec.element_c >= 0) callout_text(rec.element_c, std::nullopt, "00242cd4");
    if (rec.tracker_10 >= 0) {   // 003e9770
        auto& t = trackers_.at(static_cast<std::size_t>(rec.tracker_10));
        if (t.deleted) fail("003e9770", "a deleted callout tracker");
        screen_.fade_element(t.element_60, 0.0F);
        t.release_84 = 1;
    }
    callouts_.erase(callouts_.begin() + static_cast<std::ptrdiff_t>(i));   // 0036b130, 0015af40
    return true;
}

HudCalloutStep hud_callout_step(HudCalloutRecord& rec, const HudCalloutFrame& f, bool byte_6a9, bool map_6a0,
                                bool element_hscallout) {
    HudCalloutStep s;
    const auto show = [&](bool on) { s.show[static_cast<std::size_t>(s.shows++)] = on ? 1 : 0; };
    if (rec.element_c >= 0) {
        if (f.paused_4ffbb7) {
            show(false);
            return s;   // next record, no timer
        }
        if (f.gui_241ac0 && f.gs_d4 != 1) {   // 00242398..002423b4
            show(false);
            return s;
        }
        if (byte_6a9 && element_hscallout) {
            show(false);
            return s;
        }
        show(true);
        const auto obj = f.lookup ? f.lookup(rec.key_0) : std::nullopt;   // 0040c540
        if (!obj || !(obj->root[1] != 0 && obj->root[0] != 0 && obj->root[2] == 0)) {
            show(false);
        } else {
            if (rec.bottom_18 != 0) {   // 002424a0: the object's virtual +108
                if (!obj->life_108) fail("002424b4", "an hscallout_bottom callout's object without its virtual +108");
                if (*obj->life_108 == 3) {
                    s.clear = true;   // 00242b40(hud, rec, +8, 0, 0), next: no timer step
                    return s;
                }
            }
            if (map_6a0 && byte_6a9) {
                // the timer step
            } else if (f.player_e4) {
                if (!obj->position_e4) fail("00242538", "the callout object's virtual +e4 is not hosted");
                const auto& p = *f.player_e4;
                const auto& o = *obj->position_e4;
                const float dx = sub(p[0], o[0]), dy = sub(p[1], o[1]), dz = sub(p[2], o[2]);   // vsub
                // vmul.xyz, then vmulax / vmadday / vmaddz by vf0.x + vf0.w = 1.0
                const float sq = madd(madd(mul(1.0F, mul(dx, dx)), 1.0F, mul(dy, dy)), 1.0F, mul(dz, dz));
                rec.dist_14 = sq;
                if (!(sq <= hud_callout_max_dist_sq)) show(false);   // c.le.s 0x45fd2000
            }
        }
    }
    if (!(rec.timer_4 <= 0.0F)) {
        rec.timer_4 = sub(rec.timer_4, f.dt);
        if (rec.timer_4 <= 0.0F) s.clear = true;
    }
    return s;
}

void HudObject::callout_update(const HudCalloutFrame& f) {
    std::size_t i = 0;
    while (i < callouts_.size()) {
        auto& rec = callouts_[i];
        const bool hscallout = rec.element_c >= 0 && screen_.element_at(rec.element_c).node == globals_.hscallout;
        const auto s = hud_callout_step(rec, f, byte_6a9 != 0, map_6a0 != 0, hscallout);
        for (int k = 0; k < s.shows; ++k) screen_.show_element(rec.element_c, s.show[static_cast<std::size_t>(k)]);
        if (s.clear) {
            callout_clear(rec.key_0, rec.slot_8, false, false);   // the iterator was advanced first
            continue;
        }
        ++i;
    }
}

HudCalloutEdgeResult hud_callout_edge_step(const HudCalloutEdgeInput& in,
                                           const std::optional<HudCalloutEdgeCamera>& camera,
                                           const std::function<std::optional<std::array<float, 4>>()>& target) {
    const float w = static_cast<float>(in.video_w), h = static_cast<float>(in.video_h);   // cvt.s.w *00538200 / 04
    const float hw = ee_div(w, 2.0F), hh = ee_div(h, 2.0F);
    const float nhw = ie_ftz(-hw), nhh = ie_ftz(-hh);
    HudCalloutEdgeResult r;
    Quad p = in.p;
    if (p[2] < 0.0F || !inside_2465b0(p, {nhw, nhh, w, h})) {   // 002435f0..0024361c
        const auto t = target ? target() : std::nullopt;
        if (!t) return r;   // 0040c540 found nothing
        if (!camera) fail("00243668", "the off-screen callout placement without the game camera (*004ffc1c)");
        p = hud_callout_offscreen(*camera, *t, p, w, h);
    }
    Quad m = in.rect;   // 0x243cb8: hud +40..+4c
    const auto text = in.text ? in.text() : std::nullopt;
    if (text) {         // 0x243cec..0x243d48
        const float top = sub(text->first, static_cast<float>(text->second));
        if (top < m[1]) {
            const float delta = sub(m[1], top);
            m[1] = sub(m[1], delta);
            m[3] = add(m[3], delta);
        }
    }
    int s2 = 3;
    bool clamped = false;
    if (add(p[0], m[0]) < nhw) {   // 0x243da8
        p[0] = sub(nhw, m[0]);
        s2 = 0;
        clamped = true;
    }
    if (!(p[0] < hw)) {   // 0x243dd0
        s2 = 1;
        clamped = true;
        p[0] = sub(sub(hw, m[2]), m[0]);
    }
    if (!(add(m[3], add(p[1], m[1])) <= hh)) {   // 0x243e00
        if (s2 != 0) s2 = 2;
        clamped = true;
        p[1] = sub(sub(hh, m[3]), m[1]);
    }
    const float low = add(hud_callout_edge_bottom, nhh);   // 0x243e48
    if (add(p[1], m[1]) < low) {
        if (s2 != 0) s2 = 3;
        clamped = true;
        p[1] = sub(low, m[1]);
    }
    if (clamped && !(in.dist_14 <= hud_callout_edge_dist_sq)) return r;   // 0x243e90
    p[2] = 10.0F;
    r.placed = true;
    r.p = p;
    r.s2 = s2;
    return r;
}

void HudObject::callout_edges(const std::optional<HudCalloutEdgeCamera>& camera, const HudLockPoint& point,
                              std::int32_t video_w, std::int32_t video_h) {
    const auto ops = lock_ops(nullptr);
    for (std::size_t i = 0; i < callouts_.size(); ++i) {
        if (callouts_[i].tracker_10 < 0 || callouts_[i].element_c < 0 || callouts_[i].bottom_18 == 0) continue;
        if (screen_.element_at(callouts_[i].element_c).shown == 0) continue;
        auto& trk = trackers_.at(static_cast<std::size_t>(callouts_[i].tracker_10));
        HudCalloutEdgeInput in;
        in.p = trk.point_70;
        in.rect = rects_[3];
        in.dist_14 = callouts_[i].dist_14;
        in.video_w = video_w;
        in.video_h = video_h;
        const auto key = callouts_[i].key_0;
        const auto slot = callouts_[i].slot_8;
        const auto target = [&]() -> std::optional<std::array<float, 4>> {
            return point ? point(key, slot) : std::nullopt;
        };
        // The text adjustment reads the tracker's +60 element (after the off-screen placement).
        const auto text_of = [&]() -> std::optional<std::pair<float, std::int32_t>> {
            const auto& el = screen_.element_at(trk.element_60);
            if (el.text < 0 || screen_.text_object(el.text).word_20 == 0) return std::nullopt;
            const auto& frame = screen_.nodes().at(static_cast<std::size_t>(el.node)).text;
            if (!callout_line_count) fail("00243cfc", "003d04c0: the callout text's line count needs the host");
            if (!callout_font_height) fail("00243d08", "0036a2f0: the callout font's height needs the host");
            if (!frame) fail("00243d14", "a callout node without its TEXT frame (+d0)");
            const auto& txt = screen_.text_object(el.text);
            const std::int32_t lines = callout_line_count(txt);
            const std::int32_t fh = callout_font_height(txt.word_20);
            const auto prod = static_cast<std::int32_t>(static_cast<std::uint32_t>(lines) *
                                                        static_cast<std::uint32_t>(fh));   // mult
            return std::pair<float, std::int32_t>{frame->p0[1], prod};
        };
        in.text = text_of;
        const auto r = hud_callout_edge_step(in, camera, target);
        if (!r.placed) continue;
        Quad p = r.p;
        const int s2 = r.s2;
        const int node = screen_.element_at(callouts_[i].element_c).node;
        int s0 = -1;
        for (int k = 0; k < 4; ++k)   // 0x243d50..0x243da4
            if (globals_.callouts[static_cast<std::size_t>(k)] == node) {
                s0 = k;
                break;
            }
        if (s0 < 0) fail("00243da8", "an hscallout_bottom callout on none of the *004f7da0 nodes");
        if (s2 != s0) {   // 0x243e9c: the element moves to the edge's node
            const int old = callouts_[i].element_c;
            const int old_text = screen_.element_at(old).text;
            if (old_text < 0) fail("00243edc", "a callout element without a text object");
            const std::string copy = screen_.text_object(old_text).text.value_or(std::string());
            const IeAnim anim = screen_.element_at(old).anim;
            const int to = globals_.callouts[static_cast<std::size_t>(s2)];
            if (to < 0) fail("00243ef8", "003ef010 on a null callout edge node");
            const int fresh = screen_.create_element(to);                         // 003ef010
            callouts_[i].element_c = fresh;
            const int fresh_text = screen_.element_at(fresh).text;
            if (fresh_text < 0) fail("00243f14", "003d08b0 on a callout edge element without a text object");
            screen_.set_text(fresh_text, copy);                                   // 003d08b0
            screen_.text_object(fresh_text).f54 = 1.0F;                           // 00243460(text, 1.0)
            screen_.element_at(fresh).anim = anim;                                // +4..+14
            screen_.remove_element(old);                                          // 003e8eb0
            trackers_.at(static_cast<std::size_t>(callouts_[i].tracker_10)).element_60 = fresh;
            auto& el = screen_.element_at(fresh);
            const auto c = colour_words(hud_callout_colour_587d90);
            for (std::size_t k = 0; k < 4; ++k) el.colour[k] = ie_colour_byte(c[k]);   // 003e7ee0
        }
        auto& tk = trackers_.at(static_cast<std::size_t>(callouts_[i].tracker_10));
        tk.point_70 = p;
        ops.point(tk.element_60, p);   // 003e8180
    }
}

} // namespace omega
