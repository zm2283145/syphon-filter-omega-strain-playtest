#include "hud_markers.hpp"
#include "game_libm.hpp"
#include "ni_event_object.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

[[noreturn]] void fail(const char* address, const std::string& what) {
    throw std::runtime_error(std::string("HUD markers ") + address + ": " + what);
}

float mul(float a, float b) { return ie_ftz(a * b); }
float add(float a, float b) { return ie_ftz(a + b); }
float sub(float a, float b) { return ie_ftz(a - b); }
// The COP1 madd / VU vmadd lane: the product rounded to binary32, the sum flushed.
float madd(float acc, float a, float b) {
    const float p = a * b;
    return ie_ftz(acc + p);
}

// 00261218..00261238 (and 00132030): vmul.xyz, then vmulax.w / vmadday.w /
// vmaddz.w with vf0.w = 1, vrsqrt Q = 1 / (float)sqrt(|s|) (the VU's
// maximum for a zero s), vmulq.xyz; w is kept.
std::array<float, 4> normalise(std::array<float, 4> v) {
    const float x2 = mul(v[0], v[0]), y2 = mul(v[1], v[1]), z2 = mul(v[2], v[2]);
    const float s = madd(madd(mul(1.0F, x2), 1.0F, y2), 1.0F, z2);
    const float d = static_cast<float>(std::sqrt(std::fabs(static_cast<double>(s))));
    const float q = d != 0.0F ? ie_ftz(1.0F / d) : std::bit_cast<float>(0x7f7fffffU);
    for (std::size_t k = 0; k < 3; ++k) v[k] = mul(v[k], q);
    return v;
}

const std::array<float, 4>& constant_colour(HudMarkerColour c) {
    switch (c) {
    case HudMarkerColour::player: return hud_marker_player_4ffdc0;
    case HudMarkerColour::friendly: return hud_marker_friendly_4ffdb0;
    case HudMarkerColour::blocked: return hud_marker_blocked_4ffde0;
    case HudMarkerColour::threat: break;
    }
    fail("00274260", "no constant colour for the threat case");
}

} // namespace

std::array<float, 4> hud_marker_colour(HudMarkerColour colour, float progress, float targets_30, float danger_mag) {
    if (colour != HudMarkerColour::threat) return constant_colour(colour);
    float f = progress;
    if (!(mul(f, danger_mag) <= std::bit_cast<float>(0x3f7ae148U))) f = mul(f, targets_30);  // 00274600..00274610
    const float g = sub(1.0F, f);                                                               // 0027462c
    std::array<float, 4> c{};
    for (std::size_t k = 0; k < 4; ++k)  // mula.s enemy_k, g; madd.s threat_k, f
        c[k] = madd(mul(hud_marker_enemy_4ffdd0[k], g), hud_marker_threat_4ffdf0[k], f);
    return c;
}

HudMatrix hud_marker_matrix(const std::optional<HudMarkerView>& view) {
    if (!view) {
        // 00261310: row 3 = vf0 (0, 0, 0, 1); pcpyud v1, v1, zero gives (0, 1, 0, 0) (row 1); pexcw gives
        // (0, 0, 1, 0) (row 2); pcpyud again (1, 0, 0, 0) (row 0).
        return {{{1.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.0F, 1.0F}}};
    }
    auto r0 = view->camera_d8[0];
    auto r1 = view->camera_d8[1];
    r0[2] = 0.0F;  // 00261210 (sp + e8)
    r1[2] = 0.0F;  // 00261214 (sp + d8)
    r0 = normalise(r0);
    r1 = normalise(r1);
    // 00131f00(out, r0, r1, (0, 0, 1, 0), f4 row 3), then 0013ee20.
    HudMatrix m{{r0, r1, {0.0F, 0.0F, 1.0F, 0.0F}, view->object_f4[3]}};
    std::swap(m[0][1], m[1][0]);
    std::swap(m[0][2], m[2][0]);
    std::swap(m[1][2], m[2][1]);
    auto& t = m[3];
    if (t[0] == 0.0F && t[1] == 0.0F && t[2] == 0.0F) return m;  // 0013edb0
    std::array<float, 4> n = t;
    for (std::size_t k = 0; k < 3; ++k) n[k] = sub(0.0F, n[k]);  // 0013ed90
    std::array<float, 4> r = n;
    for (std::size_t k = 0; k < 3; ++k)                           // 0013ed60
        r[k] = madd(madd(mul(m[0][k], n[0]), m[1][k], n[1]), m[2][k], n[2]);
    t = r;                                                         // 00131e70
    return m;
}

HudBlipPlacement hud_blip_place(const HudMatrix& m, const HudBlipObject& object, const HudBlipVars& vars) {
    const auto& v = object.position_e4;
    std::array<float, 4> p{};
    for (std::size_t k = 0; k < 4; ++k)  // 002613c4..002613d0
        p[k] = madd(madd(madd(mul(m[0][k], v[0]), m[1][k], v[1]), m[2][k], v[2]), m[3][k], v[3]);
    const float r = ie_ftz(static_cast<float>(std::sqrt(std::fabs(static_cast<double>(add(mul(p[0], p[0]), mul(p[1], p[1])))))));
    const float pix = static_cast<float>(vars.pix_wid);
    float s = 0.0F;
    if (r <= vars.max_dist) {  // c.ole.s 002613fc
        const float divisor = add(vars.max_dist, mul(r, sub(vars.max_scale, 1.0F)));
        if (divisor == 0.0F) fail("00261454", "div.s by zero (not modelled)");
        s = ie_ftz(mul(pix, vars.max_scale) / divisor);
    } else {
        if (r == 0.0F) fail("00261410", "div.s by zero (not modelled)");
        s = ie_ftz(pix / r);
    }
    HudBlipPlacement out;
    out.words_70 = {mul(p[0], s), 0.0F, mul(p[1], s), 1.0F};
    const auto& f = object.f4_row1;
    std::array<float, 3> u{};
    for (std::size_t k = 0; k < 3; ++k)  // 002614ec..002614f4
        u[k] = madd(madd(mul(m[0][k], f[0]), m[1][k], f[1]), m[2][k], f[2]);
    const float a = game_atan2f(ie_ftz(-u[0]), u[1]);  // 00261504
    out.rotation = {0.0F, ie_ftz(-a), 0.0F};
    return out;
}

void HudBlips::apply(IeScreen& screen, const HudRadar& radar, const HudSpriteEvent& e, float targets_30,
                     float danger_mag) {
    using K = HudSpriteEvent::Kind;
    if (e.kind == K::build) {
        const int node = (e.state == 1 || e.state == 2) ? radar.square_4ffd60
                         : e.enemy                       ? radar.triangle_4ffd50
                                                         : radar.triangle_4ffd58;
        if (node < 0) fail("003ef010", "a radar blip on a null node (00274940)");
        HudBlip b;
        b.serial = e.serial;
        b.marker_68 = e.id;
        b.element_60 = screen.create_element(node);
        current_[e.serial] = blips_.size();
        blips_.push_back(b);
        return;
    }
    const auto it = current_.find(e.serial);
    if (it == current_.end()) fail(e.kind == K::kill ? "003e9770" : "003e7ee0", "a marker without a radar blip (+4)");
    auto& b = blips_[it->second];
    if (e.kind == K::kill) {
        screen.fade_element(b.element_60, 0.0F);
        b.ending_84 = 1;
        current_.erase(it);
        return;
    }
    const auto c = hud_marker_colour(e.colour, e.progress, targets_30, danger_mag);
    auto& el = screen.element_at(b.element_60);
    for (std::size_t k = 0; k < 4; ++k) el.colour[k] = ie_colour_byte(c[k]);
}

std::vector<std::size_t> HudBlips::step(const IeScreen& screen, const std::function<bool(std::int32_t)>& found) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < blips_.size(); ++i) {
        auto& b = blips_[i];
        if (!b.registered) continue;
        if (b.follow_64 && !found(b.marker_68)) b.follow_64 = 0;  // 003e98d8
        if (b.ending_84 && screen.element_at(b.element_60).anim.t < 0.5F) {
            b.registered = false;  // 40c9a0
            b.ending_84 = 0;
            out.push_back(i);
        }
    }
    return out;
}

void HudBlips::release(IeScreen& screen, std::size_t blip) {
    auto& b = blips_.at(blip);
    if (b.released) fail("00261090", "a blip released twice");
    screen.remove_element(b.element_60);
    b.released = true;
}

void HudBlips::place(IeScreen& screen, const HudMatrix& m,
                     const std::function<std::optional<HudBlipObject>(std::int32_t)>& object, const HudBlipVars& vars) {
    for (auto& b : blips_) {
        if (!b.registered || !b.follow_64 || b.element_60 < 0) continue;
        const auto o = object(b.marker_68);
        if (!o) continue;
        const auto p = hud_blip_place(m, *o, vars);
        b.words_70 = p.words_70;
        auto& el = screen.element_at(b.element_60);
        el.words_20 = p.words_70;  // 003e8180
        ie_element_rotate(el, p.rotation);
    }
}

std::optional<std::size_t> HudBlips::current(std::uint32_t serial) const {
    const auto it = current_.find(serial);
    if (it == current_.end()) return std::nullopt;
    return it->second;
}

void HudBlips::set_colour(IeScreen& screen, std::uint32_t serial, const std::array<float, 4>& rgba) {
    const auto it = current_.find(serial);
    if (it == current_.end()) fail("003e7ee0", "a marker without a radar blip (+4)");
    auto& el = screen.element_at(blips_[it->second].element_60);
    for (std::size_t k = 0; k < 4; ++k) el.colour[k] = ie_colour_byte(rgba[k]);
}

HudMapNodes hud_map_nodes(IeScreen& iamap) {
    const int root = iamap.root();
    HudMapNodes n;
    n.triangle_70 = iamap.find_child(root, "radartrianglelong");  // *004ffd70
    n.triangle_68 = iamap.find_child(root, "radartrianglelong");  // *004ffd68
    n.big_78 = iamap.find_child(root, "radarobjectivebig");       // *004ffd78
    n.sel_80 = iamap.find_child(root, "radarobjectivesel");       // *004ffd80
    if (n.triangle_70 < 0 || n.big_78 < 0 || n.sel_80 < 0) fail("0026e070", "an IAMAP node is missing");
    for (const int node : {n.triangle_70, n.triangle_68, n.big_78, n.sel_80}) iamap.set_flag_61(node, 1);
    return n;
}

std::array<float, 16> hud_map_sprite_matrix(const HudMatrix& f4) {
    const auto r = ni_axis_angle(1.0F, 0.0F, 0.0F, std::bit_cast<float>(0xbfc90fdbU));  // 00140c50
    auto p = ni_matrix_product(f4, r);
    for (std::size_t i = 0; i < 3; ++i)
        for (std::size_t j = 0; j < 3; ++j) p[i][j] = mul(p[i][j], 1.0F);  // *00493980
    std::array<float, 16> out{};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t j = 0; j < 4; ++j) out[4 * i + j] = p[i][j];       // 003e8040
    return out;
}

std::size_t HudMapSprites::make(IeScreen& screen, int node, std::uint32_t serial, std::int32_t id,
                                const char* address) {
    if (node < 0) fail(address, "a map sprite on a null node (003ef010)");
    HudBlip b;
    b.serial = serial;
    b.marker_68 = id;
    b.element_60 = screen.create_element(node);
    sprites_.push_back(b);
    return sprites_.size() - 1;
}

void HudMapSprites::kill(IeScreen& screen, std::size_t sprite) {  // 003e9770
    auto& b = sprites_.at(sprite);
    screen.fade_element(b.element_60, 0.0F);
    b.ending_84 = 1;
}

void HudMapSprites::apply(IeScreen& screen, const HudMapNodes& nodes, const HudSpriteEvent& e, float targets_30,
                          float danger_mag) {
    using K = HudSpriteEvent::Kind;
    if (e.kind == K::build) {
        const int node = (e.state == 1 || e.state == 2) ? nodes.big_78 : e.enemy ? nodes.triangle_68 : nodes.triangle_70;
        const auto i = make(screen, node, e.serial, e.id, "002747b8");
        current_[e.serial] = i;
        const auto& c = e.enemy ? hud_map_enemy_4a8f50 : hud_map_other_4a8f40;  // 0027487c
        auto& el = screen.element_at(sprites_[i].element_60);
        for (std::size_t k = 0; k < 4; ++k) el.colour[k] = ie_colour_byte(c[k]);
        return;
    }
    const auto it = current_.find(e.serial);
    if (it == current_.end()) fail(e.kind == K::kill ? "003e9770" : "003e7ee0", "a marker without a map sprite (+8)");
    if (e.kind == K::kill) {
        kill(screen, it->second);
        current_.erase(it);
        return;
    }
    set_colour(screen, e.serial, hud_marker_colour(e.colour, e.progress, targets_30, danger_mag));
}

void HudMapSprites::set_colour(IeScreen& screen, std::uint32_t serial, const std::array<float, 4>& rgba) {
    const auto it = current_.find(serial);
    if (it == current_.end()) fail("003e7ee0", "a marker without a map sprite (+8)");
    auto& el = screen.element_at(sprites_[it->second].element_60);
    for (std::size_t k = 0; k < 4; ++k) el.colour[k] = ie_colour_byte(rgba[k]);
}

void HudMapSprites::replace(IeScreen& screen, int node, std::uint32_t serial, std::int32_t id) {
    const auto it = current_.find(serial);
    if (it == current_.end()) fail("00272ad0", "a record without a map sprite (+8)");
    kill(screen, it->second);
    it->second = make(screen, node, serial, id, "00272ad0");
}

void HudMapSprites::open_player(IeScreen& screen, const HudMapNodes& nodes, std::optional<std::int32_t> player_id) {
    if (nodes.triangle_70 < 0) fail("0026dc1c", "the player arrow on a null node (003ef010)");
    const int element = screen.create_element(nodes.triangle_70);
    auto& el = screen.element_at(element);
    for (std::size_t k = 0; k < 4; ++k) el.colour[k] = ie_colour_byte(hud_map_player_4ff0b0[k]);
    if (!player_id) return;  // 0026dc40 / 0026dc4c: no player object or no +30
    HudBlip b;
    b.serial = 0;
    b.marker_68 = *player_id;
    b.element_60 = element;
    sprites_.push_back(b);
    player_c_ = sprites_.size() - 1;
}

void HudMapSprites::close_player(IeScreen& screen) {
    if (!player_c_) return;
    kill(screen, *player_c_);
    player_c_.reset();
}

std::vector<std::size_t> HudMapSprites::step(const IeScreen& screen, const std::function<bool(std::int32_t)>& found) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < sprites_.size(); ++i) {
        auto& b = sprites_[i];
        if (!b.registered) continue;
        if (b.follow_64 && !found(b.marker_68)) b.follow_64 = 0;  // 003e98d8
        if (b.ending_84 && screen.element_at(b.element_60).anim.t < 0.5F) {
            b.registered = false;  // 40c9a0
            b.ending_84 = 0;
            out.push_back(i);
        }
    }
    return out;
}

void HudMapSprites::release(IeScreen& screen, std::size_t sprite) {
    auto& b = sprites_.at(sprite);
    if (b.released) fail("003e9940", "a map sprite released twice");
    screen.remove_element(b.element_60);
    b.released = true;
}

void HudMapSprites::place(IeScreen& screen, const std::function<std::optional<HudMatrix>(std::int32_t)>& object_f4) {
    for (auto& b : sprites_) {
        if (!b.registered || !b.follow_64 || b.element_60 < 0) continue;
        const auto f4 = object_f4(b.marker_68);  // 0040c540(+68)
        if (!f4) continue;
        screen.element_at(b.element_60).matrix = hud_map_sprite_matrix(*f4);
    }
}

std::optional<std::size_t> HudMapSprites::current(std::uint32_t serial) const {
    const auto it = current_.find(serial);
    if (it == current_.end()) return std::nullopt;
    return it->second;
}

std::size_t HudMapSprites::live() const {
    std::size_t n = 0;
    for (const auto& b : sprites_)
        if (!b.released) ++n;
    return n;
}

std::size_t HudBlips::live() const {
    std::size_t n = 0;
    for (const auto& b : blips_)
        if (!b.released) ++n;
    return n;
}

} // namespace omega
