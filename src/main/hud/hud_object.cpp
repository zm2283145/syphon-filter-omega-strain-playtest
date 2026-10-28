#include "hud_object.hpp"
#include "weapon_hud.hpp"
#include "character_shadows.hpp"
#include "game_libm.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {

[[noreturn]] void fail(const char* address, const std::string& what) {
    throw std::runtime_error(std::string("HUD ") + address + ": " + what);
}

std::string hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", v);
    return buf;
}

std::string node_name(int node) { return node < 0 ? std::string("0") : "N" + std::to_string(node); }

int require(int index, const char* address, const char* what) {
    if (index < 0) fail(address, std::string("the IE screen has no ") + what + " (null dereference)");
    return index;
}

} // namespace

std::string HudObject::text_name(int text) const { return text < 0 ? std::string("0") : "T" + std::to_string(text); }

std::uint32_t HudObject::timer_widget(bool value) const {
    const auto& w = value ? timer_value_ : timer_text_;
    if (!w) fail("0041db80", "the GuiGameScreen timer widgets are not resolved by the hosted GUI manager");
    return *w;
}

HudObject HudObject::construct(HudServices& services, std::uint32_t game, std::string_view screen_name,
                               std::string_view map_name, const HudGlobals& globals, bool host_radar) {
    HudObject hud;
    hud.globals_ = globals;
    hud.byte_c = 1;                  // 00246590 cleared +0 / +4 / +8
    hud.game_50 = game;
    {                                // 00241c70
        const std::string name(screen_name);
        const std::string hog = "GAMEDATA\\COMMON\\" + name + ".HOG";
        const std::string member = name + ".IE";
        hud.record("file " + hog + " " + member);
        const auto bytes = services.screen_file(hog, member);
        hud.screen_ = IeScreen::load(decode_ie(bytes), hud.globals_.text_word, services.textures());
        hud.screen_.set_text_colour_block(hud.globals_.text_colour);
    }
    auto& s = hud.screen_;
    const int root = s.root();
    hud.node_58 = s.find_child(root, "fptarget");
    hud.node_5c = s.find_child(root, "target_lock");
    hud.node_60 = s.find_child(root, "target_lockswitch");
    hud.tracker_64 = -1;
    hud.element_68 = -1;
    hud.word_7c = hud.word_80 = 0;   // 00246570
    hud.byte_88 = 1;
    // 002741f0(+90): the lists +90 / +9c / +b4 (00139070), 00222b60(+a8).
    hud.record("222b60 H+a8");
    hud.f_c0 = 1.0F;
    hud.waypoint_c4 = 0;
    hud.word_c8 = -2;
    hud.word_cc = 0;
    hud.globals_.byte_48b308 = 1;
    if (host_radar)
        hud.radar_d0 = hud_radar_construct(s, s.find_child(root, "radargroup"));
    else
        hud.record("2619b0 H+d0 " + node_name(s.find_child(root, "radargroup")));
    hud.word_ec = 0;
    hud.font = services.font(hud.globals_.font_index);
    {                                // 003fa500(+100, 00260d50(root, "infobar"), font)
        auto& tk = hud.ticker_;
        tk.element = require(s.find_element(root, "infobar"), "003fa530", "infobar element");
        tk.text = s.element_at(tk.element).text;
        if (tk.text < 0) fail("003fa564", "the infobar element has no text object (003d08b0 on null)");
        tk.level = 0;
        tk.f514 = 0.5F;
        tk.f518 = 5.0F;              // *004be210
        tk.f51c = 0.5F;
        tk.f520 = 1.5F;
        tk.f524 = 5.0F;              // *004be218
        tk.current = -1;
        s.set_text(tk.text, "");
        s.text_object(tk.text).word_20 = hud.font;
        tk.byte_510 = tk.byte_511 = tk.byte_512 = 0;
        for (auto& slot : tk.slots) slot.active = 0;
    }
    hud.record("259420 H+634");
    hud.map_6a0 = services.create_map(game, std::string(map_name));
    hud.record("26e070 " + hex(game) + " " + std::string(map_name));
    hud.gui_6a4 = 0;
    // +6a8..+6cc cleared, 00139070(+6e8). 00246540 appends with item +0 = hud + 90.
    hud.list_90 = {"MAP", "H+d0"};
    s.set_flag_61(require(hud.node_5c, "00246138", "target_lock"), 1);
    s.set_flag_61(require(hud.node_60, "00246144", "target_lockswitch"), 1);
    const int meter = require(s.find_child(root, "armorhealthmeter"), "00246168", "armorhealthmeter");
    s.set_visible(meter, 1, true);
    hud.health_6c = s.find_element(meter, "healthmeter");
    hud.armor_70 = s.find_element(meter, "armormeter");
    hud.gadget_74 = s.find_element(root, "gadgeticon");
    hud.gadget_fade(0);
    hud.word_8c = -1;
    hud.ticker_style(false);
    const int panel = require(s.find_child(root, "weaponselectpanel"), "002461e0", "weaponselectpanel");
    hud.weapon_panel_630 = s.create_element(panel);
    s.set_element_data(hud.weapon_panel_630, 0x634);  // 003e7eb0(+630, hud + 634)
    hud.ammo_69c = s.create_element(require(s.find_child(panel, "ammocount"), "00246208", "ammocount"));
    if (const int text = s.element_at(hud.ammo_69c).text; text >= 0) {
        s.text_object(text).byte_58 = 1;
        s.text_object(text).byte_10 = 2;
    }
    auto& g = hud.globals_;
    g.hscallout = require(s.find_child(root, "hscallout"), "0024626c", "hscallout");
    s.set_flag_62(g.hscallout, 1);
    g.callouts[0] = s.find_child(root, "hscallout_left");
    g.callouts[1] = s.find_child(root, "hscallout_right");
    g.callouts[2] = s.find_child(root, "hscallout_top");
    g.callouts[3] = s.find_child(root, "hscallout_bottom");
    static constexpr const char* callout_store[] = {"002462d0", "002462dc", "002462e8", "002462f8"};
    for (std::size_t i = 0; i < 4; ++i) s.set_flag_62(require(g.callouts[i], callout_store[i], "hscallout_* node"), 1);
    hud.callout_rects();
    g.origin_callout = require(s.find_child(root, "origincallout2"), "00246320", "origincallout2");
    s.set_flag_62(g.origin_callout, 1);
    s.set_visible(g.origin_callout, 0, true);
    hud.nofire_78 = require(s.find_element(root, "nofire"), "003e8df4", "nofire element");
    s.show_element(hud.nofire_78, 0);
    hud.select_colours(false);
    hud.word_f0 = 0;
    hud.word_6dc = 0;
    hud.f_6e0 = -1.0F;
    hud.record("2cb3d0 1");
    hud.gui_6a4 = services.gui_load("GuiGameScreen", "GAMEGUI");
    hud.record("4187c0 GuiGameScreen GAMEGUI");
    hud.record("4147a0+64 GUI 0");
    hud.timer_text_ = services.gui_text_widget(hud.gui_6a4, "value_timer_description");
    hud.record("41db80 value_timer_description");
    hud.timer_value_ = services.gui_text_widget(hud.gui_6a4, "value_timer");
    hud.record("41db80 value_timer");
    hud.record("418a80 GuiStartMenuBackdrop GAMEBACK");
    hud.record("418a80 GuiStartMenu INGAMENU");
    return hud;
}

void HudTickerView::fade(float target, float seconds) const {
    if (log)
        log->push_back("3d0850 " + ref + " " + hex(std::bit_cast<std::uint32_t>(target)) + " " +
                       hex(std::bit_cast<std::uint32_t>(seconds)));
    ie_text_fade(text, target, seconds);
}

HudTickerView HudObject::ticker_view() {
    return {ticker_, screen_.text_object(ticker_.text), &log_, text_name(ticker_.text)};
}

void HudObject::ticker_add(std::string_view text, const std::array<float, 4>& colour, std::uint8_t priority,
                           bool clear) {
    if (clear) ticker_clear(priority, false);
    hud_ticker_add(ticker_, text, colour, priority);
}

void HudObject::ticker_clear(std::uint8_t priority, bool hard) { hud_ticker_clear(ticker_view(), priority, hard); }

void HudObject::ticker_level(std::uint8_t priority) { hud_ticker_level(ticker_view(), priority); }

void HudObject::ticker_reset() { hud_ticker_reset(ticker_view()); }

void HudObject::ticker_style(bool a1) {
    if (ticker_.text >= 0) hud_ticker_style(screen_.text_object(ticker_.text), a1);
}

void HudObject::ticker_set_current(int slot) {
    if (slot < -1 || slot >= 8) fail("003f9e60", "ticker slot out of range");
    ticker_.current = slot;
}

void HudObject::ticker_update(float dt) { hud_ticker_update(ticker_view(), dt); }

void HudObject::ticker_show(bool v) {  // 003f9e30
    if (ticker_.element >= 0) screen_.show_element(ticker_.element, v ? 1 : 0);
}

void hud_ticker_add(HudTicker& tk, std::string_view text, const std::array<float, 4>& colour,
                    std::uint8_t priority) {
    for (auto& slot : tk.slots) {
        if (slot.active) continue;
        if (text.size() >= 0x80) fail("003fa43c", "a ticker text of 128 bytes or more overruns the slot");
        slot.text = std::string(text);
        slot.colour = colour;
        slot.priority = priority;
        slot.f98 = slot.f94 = 0.0F;
        slot.active = 1;
        return;
    }
}

void hud_ticker_clear(const HudTickerView& v, std::uint8_t priority, bool hard) {
    auto& tk = v.ticker;
    for (int i = 0; i < 8; ++i) {
        auto& slot = tk.slots[static_cast<std::size_t>(i)];
        if (!slot.active || slot.priority != priority) continue;
        if (i != tk.current) {
            slot.active = 0;
            continue;
        }
        if (hard) {
            slot.active = 0;
            tk.current = -1;
            ie_text_set(v.text, "");
            v.fade(1.0F, 0.0F);
            tk.byte_512 = tk.byte_511 = tk.byte_510 = 0;
        } else {
            tk.byte_511 = tk.byte_510 = 0;
            tk.byte_512 = 1;
            v.fade(0.0F, tk.f51c);
        }
    }
}

void hud_ticker_reset(const HudTickerView& v) {
    auto& tk = v.ticker;
    for (int i = 0; i < 8; ++i) {
        auto& slot = tk.slots[static_cast<std::size_t>(i)];
        slot.active = 0;  // 003fa20c / 003fa248
        if (i != tk.current) continue;
        tk.current = -1;
        ie_text_set(v.text, "");  // 003d08b0(+50c, 004be220)
        v.fade(1.0F, 0.0F);
        tk.byte_512 = tk.byte_511 = tk.byte_510 = 0;
    }
}

void hud_ticker_level(const HudTickerView& v, std::uint8_t priority) {
    auto& tk = v.ticker;
    if (tk.current >= 0 && tk.slots[static_cast<std::size_t>(tk.current)].priority < priority) {
        tk.byte_511 = 0;
        tk.byte_510 = 0;
        tk.byte_512 = 1;
        v.fade(0.0F, tk.f51c);
    }
    tk.level = priority;
}

void hud_ticker_style(IeTextObject& text, bool a1) { text.byte_10 = a1 ? 3 : 2; }

void hud_ticker_update(const HudTickerView& v, float dt) {
    auto& tk = v.ticker;
    auto& text = v.text;
    const auto fade = [&](float target, float seconds) { v.fade(target, seconds); };
    auto& slots = tk.slots;
    // 003f9e90..003f9f04: age the waiting slots, pick the highest priority at
    // or above +504 (ties: the oldest +98).
    int best = -1;
    int best_priority = -1;
    float best_age = -1.0F;
    for (int i = 0; i < 8; ++i) {
        auto& s = slots[static_cast<std::size_t>(i)];
        if (i == tk.current || !s.active) continue;
        s.f98 = ie_ftz(s.f98 + dt);
        if (s.priority < tk.level) continue;
        if (best_priority < s.priority || (s.priority == best_priority && !(s.f98 <= best_age))) {
            best_priority = s.priority;
            best_age = s.f98;
            best = i;
        }
    }
    // 003f9f10..003f9f58: waiting priority-0 slots are dropped.
    for (int i = 0; i < 8; ++i) {
        auto& s = slots[static_cast<std::size_t>(i)];
        if (i == tk.current || !s.active || s.priority != 0) continue;
        s.active = 0;
        if (best == i && tk.current >= 0) best = -1;
    }
    if (tk.current < 0) {
        if (best < 0) return;                                      // 003f9f6c
        tk.current = best;                                         // 003f9f74..003f9fd0
        const auto& s = slots[static_cast<std::size_t>(best)];
        ie_text_set(text, s.text);
        ie_text_colour(text, {s.colour[0], s.colour[1], s.colour[2], 0.0F});
        fade(s.colour[3], tk.f514);
        tk.byte_510 = 1;
        return;
    }
    auto& cur = slots[static_cast<std::size_t>(tk.current)];
    if (!tk.byte_510) cur.f94 = ie_ftz(cur.f94 + dt);              // 003f9fe4
    if (text.byte_10 == 3) {                                       // scroll mode: retire once scrolled out
        if (text.byte_5a) {
            cur.active = 0;
            tk.current = -1;
            ie_text_set(text, "");
        }
    } else if (best >= 0 && !(cur.f94 <= tk.f520) && !tk.byte_512) {  // 003fa048: a waiting slot pre-empts
        tk.byte_512 = 1;
        fade(0.0F, tk.f51c);
    } else if (!(tk.f524 <= 0.0F) && !(cur.f94 <= tk.f524) && !tk.byte_511) {  // 003fa098: expiry
        tk.byte_511 = 1;
        fade(0.0F, tk.f518);
    }
    // 003fa0e8..003fa16c: a finished fade ends the fade-in, or retires the slot after a fade-out.
    if (!(tk.byte_510 || tk.byte_512 || tk.byte_511) || text.colour[3] != text.f50) return;
    tk.byte_510 = 0;
    if (!(tk.byte_512 || tk.byte_511)) return;
    tk.byte_512 = tk.byte_511 = 0;
    if (tk.current < 0) fail("003fa158", "a fade-out finishing with no current slot stores through null");
    slots[static_cast<std::size_t>(tk.current)].active = 0;
    tk.current = -1;
    ie_text_set(text, "");
}

void HudObject::notice(const std::optional<std::string>& text) {
    if (text) ticker_add(*text, colour_selected(), 1, false);
}

void HudObject::add_pda(std::string_view name, std::string_view text) {
    // The record is built at sp+30 in a frame that ends at sp+60: the name
    // goes to +11 first, then the text to +1 (it may run into the name), then
    // +0 = 0; 0x21 bytes are inserted.
    if (name.size() > 0x1e) fail("00241db8", "a PDA name longer than 30 bytes overruns the frame");
    if (text.size() > 0x2e) fail("00241dc4", "a PDA text longer than 46 bytes overruns the frame");
    std::array<int, 0x30> buf{};
    buf.fill(-1);
    for (std::size_t i = 0; i < name.size(); ++i) buf[0x11 + i] = static_cast<unsigned char>(name[i]);
    buf[0x11 + name.size()] = 0;
    for (std::size_t i = 0; i < text.size(); ++i) buf[1 + i] = static_cast<unsigned char>(text[i]);
    buf[1 + text.size()] = 0;
    buf[0] = 0;
    const auto cstr = [&](std::size_t from, const char* address) {
        std::string out;
        for (std::size_t i = from;; ++i) {
            if (i >= 0x21) fail(address, "a PDA record string is not terminated inside the 0x21-byte record");
            if (buf[i] < 0) fail(address, "a PDA record string reads stack garbage");
            if (buf[i] == 0) return out;
            out.push_back(static_cast<char>(buf[i]));
        }
    };
    HudPdaRecord rec;
    rec.flag = static_cast<std::uint8_t>(buf[0]);
    rec.text = cstr(1, "00241dc4");
    rec.name = cstr(0x11, "00241db8");
    record("41e0f0 10 5");
    pda_.push_back(std::move(rec));
}

void HudObject::add_pda_quip(const std::optional<std::string>& name, const std::optional<std::string>& text,
                             const std::function<std::optional<std::string>(const std::string&)>& get_string,
                             bool multiplayer) {
    if (!name) return;                                  // 001c63d8
    const std::string body = text && !text->empty() ? *text : std::string("PDAMsg");
    add_pda(*name, body);
    record("3fdb50 PDAMsg");
    notice(get_string("PDAMsg"));
    if (multiplayer) fail("001c6444", "the multiplayer PDA quip message is not hosted");
}

void HudObject::select_colours(bool a1) {
    globals_.colour_select = a1 ? 1 : 0;  // 00260ec0
    colour_b_ = a1;
    globals_.text_colour = colour_selected();
    screen_.set_text_colour_block(globals_.text_colour);
}

void HudObject::set_colour_table(bool table_b, std::int32_t r, std::int32_t g, std::int32_t b, float alpha) {
    set_colour_table(globals_, table_b, r, g, b, alpha);
}

void HudObject::set_colour_table(HudGlobals& globals, bool table_b, std::int32_t r, std::int32_t g, std::int32_t b,
                                 float alpha) {
    (table_b ? globals.colours_b : globals.colours_a) = script_colour_255(r, g, b, alpha);
}

std::array<float, 4> script_colour_255(std::int32_t r, std::int32_t g, std::int32_t b, float alpha) {
    return {ie_ftz(static_cast<float>(r) / 255.0F), ie_ftz(static_cast<float>(g) / 255.0F),
            ie_ftz(static_cast<float>(b) / 255.0F), alpha};
}

namespace {
// div.s as tools/ee/ee.py runs it: a zero divisor gives the largest float
// signed as S * T (positive for a zero S).
float ee_div(float s, float t) {
    if (t != 0.0F) return ie_ftz(s / t);
    const double sign = s != 0.0F ? static_cast<double>(s) * static_cast<double>(t) : 1.0;
    return std::copysign(std::numeric_limits<float>::max(), static_cast<float>(sign));
}
} // namespace

void hud_set_lock_colour(HudGlobals& globals, bool nv, std::int32_t index, float threshold, std::int32_t r,
                         std::int32_t g, std::int32_t b) {
    if (nv) globals.lock_ramp_b_set = 1;  // 00260f14, before the index test
    if (index >= 10) return;              // slti 0xa
    if (index < 0)
        fail(nv ? "00260ed0" : "00260fb0", "a negative lock colour index stores before the table (not hosted)");
    auto& e = (nv ? globals.lock_ramp_b : globals.lock_ramp_a)[static_cast<std::size_t>(index)];
    if (!(threshold <= 0.0F)) e[0] = threshold;  // c.ole.s thr, 0 skips the store
    e[1] = static_cast<float>(r);                // cvt.s.w
    e[2] = static_cast<float>(g);
    e[3] = static_cast<float>(b);
}

std::optional<std::array<float, 4>> hud_lock_colour(const HudGlobals& globals, const HudLockAlpha& alpha, float value) {
    const bool nv = globals.colour_select != 0 && globals.lock_ramp_b_set != 0;  // 004f8258, then 004f8230
    const auto& t = nv ? globals.lock_ramp_b : globals.lock_ramp_a;               // 004f8190 : 004f80f0
    for (std::size_t i = 0; i < 9; ++i) {
        const float f10 = t[i + 1][0];
        if (!(value < f10)) continue;  // c.olt.s
        const auto& e = t[i];
        const auto& n = t[i + 1];
        const float f = ee_div(ie_ftz(value - e[0]), ie_ftz(f10 - e[0]));
        const float inv = ie_ftz(1.0F - f);
        std::array<float, 4> out{};
        for (std::size_t c = 0; c < 3; ++c) {
            const float acc = ie_ftz(inv * e[c + 1]);  // mula.s
            const float p = f * n[c + 1];
            out[c] = ee_div(ie_ftz(acc + p), 255.0F);  // madd.s, div.s 0x437f0000
        }
        out[3] = nv ? alpha.nv : alpha.lock;  // 0048b0a8 : 0048b0a0
        return out;
    }
    return std::nullopt;
}

void HudObject::set_lock_colour(bool nv, std::int32_t index, float threshold, std::int32_t r, std::int32_t g,
                                std::int32_t b) {
    hud_set_lock_colour(globals_, nv, index, threshold, r, g, b);
}

void HudObject::lock_meter(float value, const HudLockAlpha& alpha) {  // 00244960
    if (element_68 < 0) return;
    hud_lock_meter_element(screen_.element_at(element_68), globals_, alpha, value);
}

void hud_lock_meter_element(IeElement& element, const HudGlobals& globals, const HudLockAlpha& alpha, float value) {
    const auto c = hud_lock_colour(globals, alpha, value);
    if (!c)
        fail("00260d90", "no lock colour segment covers " + hex(std::bit_cast<std::uint32_t>(value)) +
                             " (003e7ee0 would read 00244960's uninitialised stack)");
    for (std::size_t k = 0; k < 4; ++k) element.colour[k] = ie_colour_byte((*c)[k]);  // 003e7ee0: +1a..+1d
}

namespace {
HudLockTracker& tracker_at(std::vector<HudLockTracker>& trackers, int index, const char* address) {
    if (index < 0 || static_cast<std::size_t>(index) >= trackers.size())
        fail(address, "a null target-lock tracker");
    auto& t = trackers[static_cast<std::size_t>(index)];
    if (t.deleted) fail(address, "a deleted target-lock tracker");
    return t;
}

// 0036b180(0, 0x90, "hud.cc", line), 003e99b0(obj, +68, &id), vtable 004e0480.
int new_tracker(const HudLockRefs& hud, std::int32_t id) {
    HudLockTracker t;
    t.element_60 = hud.element_68;
    t.active_64 = 1;
    t.id_68 = id;
    t.release_84 = 0;
    hud.trackers.push_back(t);  // 0040c9d0
    return static_cast<int>(hud.trackers.size() - 1);
}

void tracker_evaluate_0(HudLockTracker& t, const HudLockOps& ops) {  // 003e9730
    ops.evaluate(t.element_60, 0.0F);
    t.release_84 = 1;
}

void tracker_fade_0(HudLockTracker& t, const HudLockOps& ops) {  // 003e9770
    ops.fade(t.element_60, 0.0F);
    t.release_84 = 1;
}
} // namespace

void hud_lock_target(const HudLockRefs& hud, const HudLockOps& ops, std::int32_t id, std::int32_t sub) {
    const int node = sub == -1 ? hud.node_5c : hud.node_60;
    if (hud.element_68 < 0) {
        hud.element_68 = ops.create(node);         // 003ef010
        hud.tracker_64 = new_tracker(hud, id);     // line 0x3a1
    } else if (ops.node(hud.element_68) != node) {
        if (hud.tracker_64 >= 0) tracker_fade_0(tracker_at(hud.trackers, hud.tracker_64, "003e9770"), ops);
        else ops.remove(hud.element_68);           // 003e8eb0
        hud.element_68 = ops.create(node);
        hud.tracker_64 = new_tracker(hud, id);     // line 0x3ad
    }
    auto& t = tracker_at(hud.trackers, hud.tracker_64, "003e97b0");   // 003e97b0
    t.id_68 = id;
    t.active_64 = 1;
    t.sub_80 = sub;
}

void hud_lock_changed(const HudLockRefs& hud, const HudLockOps& ops) {
    if (hud.tracker_64 >= 0) tracker_evaluate_0(tracker_at(hud.trackers, hud.tracker_64, "003e9730"), ops);
    hud_lock_release(hud, ops);
}

void hud_lock_release(const HudLockRefs& hud, const HudLockOps& ops) {
    if (hud.element_68 >= 0) {
        if (hud.tracker_64 < 0) ops.remove(hud.element_68);
        hud.element_68 = -1;
    }
    if (hud.tracker_64 >= 0) {
        tracker_fade_0(tracker_at(hud.trackers, hud.tracker_64, "003e9770"), ops);
        hud.tracker_64 = -1;
    }
}

void hud_lock_aim(const HudLockRefs& hud, const HudLockOps& ops, float x) {
    if (hud.element_68 < 0) return;
    ops.colour(hud.element_68, ie_ftz(1.0F - x));                    // sub.s
    if (x < std::bit_cast<float>(0x3f733333U)) ops.evaluate(hud.element_68, ie_ftz(15.0F * x));   // mul.s
    else ops.evaluate(hud.element_68, 15.0F);
}

bool hud_lock_tracker_update(HudLockTracker& t, const HudLockOps& ops, const HudLockPoint& point) {
    if (t.deleted) fail("003e9880", "the update of a deleted target-lock tracker");
    if (t.active_64 != 0) {
        if (const auto p = point(t.id_68, t.sub_80)) t.point_70 = *p;  // 0040c540, virtual +c0
        else t.active_64 = 0;
    }
    if (t.release_84 == 0 || !(ops.time(t.element_60) < 0.5F)) return false;
    t.in_tree = false;   // 0040c9a0
    t.release_84 = 0;    // after 003cb230(obj, 0.1)
    return true;
}

void hud_lock_tracker_draw(HudLockTracker& t, const HudLockOps& ops,
                           const std::optional<std::array<std::array<float, 4>, 4>>& camera_node) {
    if (t.deleted) fail("002610f0", "the draw of a deleted target-lock tracker");
    if (t.active_64 == 0) return;
    if (camera_node) {   // 003e9a60
        ShadowMatrix node;
        for (std::size_t k = 0; k < 4; ++k) node.column[k] = (*camera_node)[k];
        t.point_70 = shadow_transform(shadow_rigid_inverse(node), t.point_70);
    }
    ops.point(t.element_60, t.point_70);   // 003e8180
}

void hud_lock_tracker_delete(HudLockTracker& t, const HudLockOps& ops) {
    if (t.deleted) fail(t.callout ? "003e9940" : "003e9ba0", "a target-lock tracker deleted twice");
    ops.remove(t.element_60);   // 003e8eb0(+60)
    t.in_tree = false;
    t.deleted = true;
}

HudLockOps HudObject::lock_ops(const HudLockAlpha* alpha) {
    HudLockOps ops;
    ops.create = [this](int node) {
        if (node < 0) fail("003ef010", "a target-lock element on a null node");
        return screen_.create_element(node);
    };
    ops.remove = [this](int e) { screen_.remove_element(e); };
    ops.evaluate = [this](int e, float t) { screen_.evaluate_element(e, t); };
    ops.fade = [this](int e, float t) { screen_.fade_element(e, t); };
    ops.colour = [this, alpha](int e, float value) {
        if (!alpha) fail("00260d90", "the lock colour without the lock alphas");
        hud_lock_meter_element(screen_.element_at(e), globals_, *alpha, value);
    };
    ops.node = [this](int e) { return screen_.element_at(e).node; };
    ops.time = [this](int e) { return screen_.element_at(e).anim.t; };
    ops.point = [this](int e, const std::array<float, 4>& v) { screen_.element_at(e).words_20 = v; };
    return ops;
}

void HudObject::lock_target(std::int32_t id, std::int32_t sub) { hud_lock_target(lock_refs(), lock_ops(nullptr), id, sub); }

void HudObject::lock_changed() { hud_lock_changed(lock_refs(), lock_ops(nullptr)); }

void HudObject::lock_aim(float x, const HudLockAlpha& alpha) { hud_lock_aim(lock_refs(), lock_ops(&alpha), x); }

std::vector<int> HudObject::update_trackers(const HudLockPoint& point) {
    std::vector<int> released;
    const auto ops = lock_ops(nullptr);
    for (std::size_t i = 0; i < trackers_.size(); ++i) {
        auto& t = trackers_[i];
        if (t.deleted || !t.in_tree) continue;
        if (hud_lock_tracker_update(t, ops, point)) released.push_back(static_cast<int>(i));
    }
    return released;
}

void HudObject::delete_tracker(int tracker) {
    hud_lock_tracker_delete(tracker_at(trackers_, tracker, "003e9ba0"), lock_ops(nullptr));
}

void HudObject::draw_trackers(const std::optional<std::array<std::array<float, 4>, 4>>& camera_node,
                              const HudCalloutCamera* callout_camera, const HudMapCalloutCamera* map_camera) {
    const auto ops = lock_ops(nullptr);
    for (auto& t : trackers_) {
        if (t.deleted || !t.in_tree) continue;
        if (!t.callout) {
            hud_lock_tracker_draw(t, ops, camera_node);
        } else if (t.map_callout) {
            if (!map_camera) fail("003e9460", "a map callout tracker's draw without the map's view 0055a290 and the render camera");
            hud_map_callout_tracker_draw(t, ops, *map_camera);
        } else {
            if (!callout_camera) fail("003e97d0", "a callout tracker's draw without the render camera (00374bc0)");
            hud_callout_tracker_draw(t, ops, camera_node, *callout_camera);
        }
    }
}

void HudObject::map_callouts_place(const std::optional<std::array<std::array<float, 4>, 4>>& camera_node,
                                   const HudMapCalloutCamera& map_camera) {
    const auto ops = lock_ops(nullptr);
    for (const auto& r : callouts_) {   // 0x00244c30..0x00244c88: the +6e8 list in order
        if (r.tracker_10 < 0 || r.element_c < 0) continue;   // +10 / +c null
        auto& t = trackers_.at(static_cast<std::size_t>(r.tracker_10));
        if (t.deleted) fail("00244c58", "the map path's draw of a deleted callout tracker");
        if (t.active_64 == 0) continue;   // 0x00244c4c: +64
        if (t.map_callout) hud_map_callout_tracker_draw(t, ops, map_camera);     // 003e9460
        else hud_callout_tracker_draw(t, ops, camera_node, map_camera.record);  // 003e97d0
    }
}

void HudObject::gadget_fade(std::uint8_t a1) {
    if (gadget_74 < 0) return;
    const std::int32_t t = 200 + 30 * static_cast<std::int32_t>(a1);
    screen_.evaluate_element(gadget_74, static_cast<float>(t));
    screen_.fade_element(gadget_74, static_cast<float>(t + 10));
}

void HudObject::flashlight_icon(std::uint8_t a1, std::int32_t a2) {
    if (gadget_74 < 0) return;
    const std::int32_t t = 210 + 30 * static_cast<std::int32_t>(a1);
    const std::int32_t from = a2 == 0 ? t + 10 : t;
    const std::int32_t to = a2 == 0 ? t : t + 10;
    screen_.evaluate_element(gadget_74, static_cast<float>(from));
    screen_.fade_element(gadget_74, static_cast<float>(to));
}

void HudObject::callout_rects() {
    for (std::size_t i = 0; i < 4; ++i) {
        const int node = globals_.callouts[i];
        if (node < 0) fail("003ecb50", "a callout rectangle on a null node");
        rects_[i] = screen_.rect(node, 10.0F);
    }
}

namespace {
// 00139200 inserting one entry at the end of a full vector.
std::uint32_t grown_capacity(std::uint32_t capacity, std::uint32_t needed) {
    if (needed > 0x1800000U) fail("00139200", "the reticle vector grows past 0x1800000 (aborts)");
    if (capacity >= needed) return capacity;
    std::uint32_t n = capacity ? capacity : 1U;
    while (n < needed) n = n < 0xc00000U ? n * 2 : 0x1800000U;
    return n;
}
} // namespace

void HudObject::build_reticles(const std::vector<std::string>& names) {  // 00245e30
    auto& s = screen_;
    for (const auto& name : names) {
        const int element = s.find_element(s.root(), name);  // 00260d50
        word_7c = grown_capacity(word_7c, word_80 + 1);      // 00245ee0 -> 00139200
        reticles_84.push_back(element);
        word_80 = static_cast<std::uint32_t>(reticles_84.size());
        if (element < 0) fail("003e8df4", "the reticle '" + name + "' has no root child (003e8de0 on null)");
        s.show_element(element, 0);
    }
}

void HudObject::reticle_release() { hud_lock_release(lock_refs(), lock_ops(nullptr)); }  // 00244280

void HudObject::reticle_default() {  // 00244a10
    reticle_release();
    if (node_58 < 0) return;
    screen_.set_flag_61(node_58, 0);                // 003ee830
    element_68 = screen_.create_element(node_58);   // 003ef010
    screen_.show_element(element_68, 1);            // 003e8de0
    screen_.evaluate_element(element_68, 0.0F);     // 003f45a0
    screen_.fade_element(element_68, 17.0F);        // 003f44e0
}

HudThrownMarker hud_thrown_marker(const std::array<std::array<float, 4>, 4>& camera_node, const std::array<float, 4>& point,
                                  const std::array<float, 4>* direction) {  // 00244580 past its +68 test
    const auto ftz = [](float v) { return std::fpclassify(v) == FP_SUBNORMAL ? (std::signbit(v) ? -0.0F : 0.0F) : v; };
    const auto add = [&](float a, float b) { return ftz(a + b); };
    const auto sub = [&](float a, float b) { return ftz(a - b); };
    const auto mul = [&](float a, float b) { return ftz(a * b); };
    ShadowMatrix node;
    for (std::size_t k = 0; k < 4; ++k) node.column[k] = camera_node[k];
    const ShadowMatrix v = shadow_rigid_inverse(node);   // 00133330, 00133190 (sp+1b0)
    HudThrownMarker out;
    std::array<float, 3> angle{0.0F, 0.0F, 0.0F};        // sp+1a0..+1a8
    if (direction) {
        const float k03 = std::bit_cast<float>(0x3e99999aU);
        ShadowVector p{};
        for (std::size_t i = 0; i < 4; ++i) p[i] = add(point[i], mul((*direction)[i], k03));   // vmulx, vadd
        out.words_20 = shadow_transform(v, p);                                              // 003e8180(+68, sp+140)
        ShadowVector d = *direction;                                                         // vmaddz.xyz: w kept
        for (std::size_t i = 0; i < 3; ++i)
            d[i] = add(add(mul(v.column[0][i], (*direction)[0]), mul(v.column[1][i], (*direction)[1])),
                       mul(v.column[2][i], (*direction)[2]));
        ShadowVector n{};
        for (std::size_t i = 0; i < 4; ++i) n[i] = sub(0.0F, d[i]);                          // vmulax ACC 0, vmsubw
        const float y2 = mul(n[1], n[1]), z2 = mul(n[2], n[2]);                              // sp+80.x = 0
        const float q = std::sqrt(add(add(0.0F, y2), z2));                                   // vsqrt, vaddq.x
        const float a = game_atan2f(-n[0], add(0.0F, q));                                    // neg.s f12
        const float b = game_atan2f(n[2], n[1]);
        angle = {b, 0.0F, a};
    } else {
        out.words_20 = shadow_transform(v, point);
    }
    const float cy = game_cosf(angle[1]), sy = game_sinf(angle[1]);
    const float cz = game_cosf(angle[2]), sz = game_sinf(angle[2]);
    const float cx = game_cosf(angle[0]), sx = game_sinf(angle[0]);
    const float f15 = add(mul(sz, mul(cx, cy)), mul(sx, sy));
    const float s0 = add(mul(sz, mul(sx, sy)), mul(cx, cy));    // mula.s / madd.s, the 00140e20 stack word
    const float f2 = mul(cx, sy);
    const float f17 = sub(mul(sz, f2), mul(sx, cy));            // mula.s / msub.s
    const float f18 = sub(mul(cy, mul(sx, sz)), f2);
    const float f12 = mul(cy, cz), f13 = -sz, f14 = mul(sy, cz), f16 = mul(cx, cz), f19 = mul(sx, cz);
    const float s = std::bit_cast<float>(0x3df5c28fU);          // 0048ae50 (0.12)
    out.matrix = {mul(f12, s), mul(f15, s), mul(f18, s), 0.0F, mul(f13, s), mul(f16, s), mul(f19, s), 0.0F,
                  mul(f14, s), mul(f17, s), mul(s0, s),  0.0F, 0.0F,        0.0F,        0.0F,        1.0F};
    return out;
}

void HudObject::thrown_marker(const std::array<std::array<float, 4>, 4>& camera_node, const std::array<float, 4>& point,
                              const std::array<float, 4>* direction) {  // 00244580
    if (element_68 < 0) return;
    screen_.set_flag_61(require(node_58, "002445c0", "fptarget (+58)"), 1);   // 003ee830(+58, 1)
    const auto m = hud_thrown_marker(camera_node, point, direction);
    auto& el = screen_.element_at(element_68);
    el.words_20 = m.words_20;   // 003e8180
    el.matrix = m.matrix;       // 003e8040
}

void HudObject::pad_meters(const ActorHealthState& health) {  // 0024cec8..0024ceec
    const HudMeters m = hud_meters(health, health_6c >= 0, armor_70 >= 0);
    if (m.health) screen_.fade_element(health_6c, *m.health);  // 002441e0
    if (m.armor) screen_.fade_element(armor_70, *m.armor);     // 00244180
}

void HudObject::ammo(const HudAmmoText& a) {  // 00242190
    if (ammo_69c < 0) fail("003e7ec4", "the ammo text on a null element (+69c)");
    const int text = screen_.element_at(ammo_69c).text;
    if (text < 0) fail("003d08e4", "the ammocount element has no text object (003d08b0 on null)");
    screen_.set_text(text, a.text);
    model_694 = a.icon_694;
    byte_698 = a.byte_698;
}

void HudObject::nofire_hide() {  // 002449a0
    if (nofire_78 >= 0) screen_.show_element(nofire_78, 0);
}

void HudObject::nofire_show() {  // 002449d0
    if (nofire_78 >= 0) screen_.show_element(nofire_78, 1);
}

HudCrosshairResult HudObject::crosshair(std::int32_t s2, const std::optional<HudCrosshairRecord>& held) {
    HudCrosshairState state;
    state.index_8c = word_8c;
    for (const int e : reticles_84) {
        if (e < 0) state.elements_84.emplace_back(std::nullopt);
        else state.elements_84.emplace_back(screen_.element_at(e).anim.t);
    }
    // The events only touch the switched-to element (s2 - 1) and +68 / +78,
    // so the +4 values read up front are the ones the hide loop reads.
    auto result = hud_crosshair_update(state, s2, held);
    using K = HudCrosshairEvent::Kind;
    const auto at = [&](std::int32_t k) { return reticles_84.at(static_cast<std::size_t>(k)); };
    for (const auto& ev : result.events) {
        switch (ev.kind) {
        case K::default_a10: reticle_default(); break;
        case K::release_a00: reticle_release(); break;
        case K::clear_9a0: nofire_hide(); break;
        case K::visible_de0: screen_.show_element(at(ev.element), static_cast<std::uint8_t>(ev.value)); break;
        case K::alpha_5a0: screen_.evaluate_element(at(ev.element), std::bit_cast<float>(ev.value)); break;
        case K::fade_4e0: screen_.fade_element(at(ev.element), std::bit_cast<float>(ev.value)); break;
        }
    }
    word_8c = state.index_8c;
    return result;
}

void HudObject::update_screen(float dt) { screen_.update(dt); }  // 003e79c0

void HudObject::radar_update(const HudRadarFrame& frame) {
    if (!radar_d0) fail("00261690", "the radar +d0 is not hosted (constructed without host_radar)");
    hud_radar_update(screen_, *radar_d0, frame);
}

void HudObject::select_waypoint(std::uint64_t object, std::int32_t object_id) {
    waypoint_c4 = object;                // 00272d18
    word_c8 = object ? object_id : -1;   // 00272d38..00272d3c
}

std::uint64_t HudObject::waypoint(const std::function<std::int32_t(std::uint64_t)>& object_id) const {
    if (!waypoint_c4) return 0;
    return object_id(waypoint_c4) == word_c8 ? waypoint_c4 : 0;   // 00272ce4..00272cf8
}

void HudObject::radar_meter(bool environmental, float value, const HudMeterVars& vars) {
    if (!radar_d0) fail("00261540", "the radar +d0 is not hosted (constructed without host_radar)");
    if (const auto t30 = hud_radar_meter(screen_, *radar_d0, environmental, value, vars)) f_c0 = *t30;
}

void HudObject::subtitle(std::string_view name, std::uint32_t a2, std::uint32_t a3) {
    if (!gui_6a4) return;
    record("45ce10 GUI " + std::string(name) + " " + hex(a2) + " " + hex(a3));
}

} // namespace omega