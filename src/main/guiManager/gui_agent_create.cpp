// GuiAgentCreate (class factory 0041f960, ctor 0028f760, vtable 004dd1b0; AGENTNEW.GUI): the title
// screen's NEW AGENT editor - gender, randomize, the body and head categories and the agent name -
// and GuiAgentSave (vtable 004dd3a0, ctor 00293fc0; AGENTOPN.GUI), the card slot it saves the new
// agent to. The category / part logic is agent_create.cpp; this file is the widgets. GuiAgentSave's
// slot event (00292980) is gui_runtime.cpp's as_event (it shares GuiAgentLoad's strtol).
// research/FRONTEND.md "Checkpoint 132cc5".
#include "gui_runtime.hpp"
#include "appearance.hpp"
#include "character_appearance.hpp"

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega::gui {
namespace {

// EE COP1 semantics (as gui_runtime.cpp): denormals flush to a signed zero, division by zero
// gives the signed maximum, cvt.w.s saturates.
float ftz(float x) {
    if (x != 0.0f && std::fabs(x) < FLT_MIN) return std::copysign(0.0f, x);
    return x;
}
float fadd(float a, float b) { return ftz(ftz(a) + ftz(b)); }
float fsub(float a, float b) { return ftz(ftz(a) - ftz(b)); }
float fmul(float a, float b) { return ftz(ftz(a) * ftz(b)); }
float fdiv(float a, float b) {
    a = ftz(a);
    b = ftz(b);
    if (b == 0.0f) {
        const float sign = a != 0.0f ? a * b : 1.0f;
        return std::copysign(FLT_MAX, std::signbit(sign) ? -1.0f : 1.0f);
    }
    return ftz(a / b);
}
std::int32_t cvt_w(float x, std::uint32_t address) {
    x = ftz(x);
    if (std::isnan(x)) throw std::runtime_error("cvt.w.s of NaN at " + std::to_string(address));
    if (x >= 2147483647.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    return static_cast<std::int32_t>(x);
}

[[noreturn]] void unhosted(std::uint32_t address, const std::string& what) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", address);
    throw std::runtime_error("gui runtime: unhosted " + what + " (" + buf + ")");
}

std::string cstr_of(const std::string& s) { return s.substr(0, s.find('\0')); }

constexpr float pi = std::bit_cast<float>(0x40490fdbu);
constexpr float two_pi = std::bit_cast<float>(0x40c90fdbu);
constexpr float minus_pi = std::bit_cast<float>(0xc0490fdbu);

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsA

// The idle clip's clock: the display actors step by the update's dt in seconds (manager +64,
// 00415980) after each draw (0028e4c8 / 0028e50c). PORT-ONLY: quantized to 30 Hz pictures.
float preview_clock(float seconds) { return std::floor(seconds * 30.0f) / 30.0f; }

omega::AgentPartCache& cache_of(GuiHost& host, std::uint32_t address) {
    omega::AgentPartCache* c = host.agent_parts();
    if (!c) unhosted(address, "the front end's character part cache (0055dda0)");
    return *c;
}

}  // namespace

// 0028f760: flags |= 0x20, +cc -1, +d0 1; D_00493a60 = -1, then Skl_Load 00402840 packs
// GAMEDATA\FRONTEND\MALE.SKL into 0055d510 and FEMALE.SKL into 0055d950 (Appearance_PackText
// 00406570 writes only the bytes the text names) and sets D_00493a60 = 0; the four models and the
// table (agent_create_open). The light object (+74) and the camera (+80..+ac) are draw state.
void GuiRuntime::ac_construct(Widget* w) {
    w->ac = std::make_unique<Widget::AgentCreateW>();
    auto& a = *w->ac;
    w->flags |= 0x20;
    a.group = -1;
    a.in_group = 1;
    g.packing_slot = -1;
    static constexpr const char* skl[2] = {"GAMEDATA\\FRONTEND\\MALE.SKL", "GAMEDATA\\FRONTEND\\FEMALE.SKL"};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto bytes = host_.skl_file(skl[i]);
        const omega::AppearancePacking ap = omega::pack_appearance(bytes);
        auto& dst = g.packings[i];
        for (std::size_t k = 0; k < dst.size(); ++k)
            if (ap.initialized[k]) dst[k] = ap.bytes[k];
    }
    g.packing_slot = 0;
    a.m = omega::agent_create_open(cache_of(host_, 0x28f760), g.packings[0], g.packings[1], g.ac_table);
    g.ac_table = a.m.table;
}

// 0028ed60
void GuiRuntime::ac_init(Widget* w) {
    auto& a = *w->ac;
    base_init(w);
    // "playercharacter" (IsA GuiCharacterDisplay 004a9bf0): D_00506390 = this, its +48 = 00290bc0.
    if (Widget* d = of_class(find(w, "playercharacter"), WidgetClass::chardisplay)) {
        d->cd_screen = w;
        a.display = d;
    }
    a.group_w = find(w, "createagent_grp");
    a.body = of_class(find(w, "body"), WidgetClass::button);
    a.head = of_class(find(w, "head"), WidgetClass::button);
    a.indicator = find(w, "selectindicator");  // IsA GuiWidget (004a9c40): any widget
    a.value_slot = of_class(find(w, "value_slot"), WidgetClass::text);
    a.beard = of_class(find(w, "beard"), WidgetClass::button);
    if (a.value_slot) set_text(a.value_slot, "", false);  // D_004a9a40
    // 002888c0's list, AgentData_FilterAwards(Agent_GetSelected(-1), list), 00403040(0055dda0, list).
    omega::agent_cache_lock(cache_of(host_, 0x28ed60), host_.agent_locked_parts(host_.current_profile()));
}

// 0028ec00: 00403310(0055dda0), 0040f540, then 0041f090.
void GuiRuntime::ac_deinit(Widget* w) {
    if (omega::AgentPartCache* cache = host_.agent_parts()) omega::agent_cache_unlock(*cache);
    host_.app_40f540();
    if (w->ac && w->ac->display) w->ac->display->cd_screen = nullptr;
}

// 0028ec80
void GuiRuntime::ac_wake(Widget* w) {
    auto& a = *w->ac;
    g.app_2f = 1;
    base_wake(w);
    if (a.indicator) send(w, a.indicator, 0xa, 0, 0);
    if (Widget* name = of_class(find(w, "value_agent_name"), WidgetClass::text))  // D_004a99b0
        set_text(name, cstr_of(host_.profile_name(host_.current_profile()).value_or("")), false);  // 00336170
    repeat_mask |= 0xc;  // L1 / R1 repeat
    ac_preview(w);
}

// 0028ec40
void GuiRuntime::ac_sleep(Widget* w) {
    base_sleep(w);
    repeat_mask &= ~0xcu;
}

// 0028b3c0
std::uint8_t GuiRuntime::ac_input(Widget* w, const PadEvent& ev) {
    auto& a = *w->ac;
    const auto pressed = [&](std::uint32_t mask) {
        return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
    };
    const auto keyboard = [&](std::int32_t arg) {  // 0041f960("GuiKeyboard", "KEYBOARD"), 0032b4f0
        Widget* kb = factory("GuiKeyboard", std::string("KEYBOARD"));
        if (kb)
            kb_open(kb, host_.get_string("SetAgentName"),
                    cstr_of(host_.profile_name(host_.current_profile()).value_or("")), w, 15, arg, 1);
    };
    if (pressed(0x80)) {  // Square: the look to the edited packing, then save (or name the agent first)
        if (g.packing_slot == -1) g.packing_slot = 0;
        auto& slot = g.packings[static_cast<std::size_t>(g.packing_slot)];
        slot = a.m.model().packing;  // 0x44 quadwords
        omega::AppearancePacking ap;
        std::copy(slot.begin(), slot.end(), ap.bytes.begin());
        ap.initialized.fill(true);
        omega::appearance_remove_blank_pairs(ap);  // 00406e00
        std::copy(ap.bytes.begin(), ap.bytes.end(), slot.begin());
        const auto name = host_.profile_name(host_.current_profile());  // 00336170
        if (name && !cstr_of(*name).empty()) {
            ac_save(w);
            return 1;
        }
        keyboard(2);
        return 1;
    }
    if (pressed(0x10)) {  // Triangle: back to the title screen
        Widget* next = factory("GuiTitleScreen", std::string("titlescr"));
        switch_screens(w, next);
        return 1;
    }
    if (pressed(0x2000) && a.in_group) {  // Right: into the group's items
        if (a.group != 0 && a.group != 1) send(w, &canvas_, 0x10, 0, 0);
        if (a.group == 2) {
            am_group_lock(a.body);  // 0028c0c0
            am_group_unlock(a.group_w);  // 0028bf90
            set_focus(v_first(w));
        } else if (a.group == 3) {
            am_group_lock(a.head);
            am_group_unlock(a.group_w);
            if (a.m.sex == 0 && a.beard) a.beard->flags &= ~flag_visible;
            set_focus(v_first(w));
        } else if (a.group == 4) {
            keyboard(1);
        }
    }
    if (pressed(0x8000) && !a.in_group) {  // Left: back to the groups
        send(w, &canvas_, 0x10, 0, 0);
        am_group_lock(a.group_w);
        Widget* b = a.group == 2 ? a.body : a.head;
        am_group_unlock(b);
        set_focus(b);
    }
    if (pressed(0x4)) ac_step(w, true, false);  // L1
    if (pressed(0x8)) ac_step(w, false, true);  // R1
    return 0;
}

// 0028ba90: the edited look to the agent (00334000), then the card slot screen, or without a card
// the MC_NOCARDCONT alert (its OK: event 0x1a / 3).
void GuiRuntime::ac_save(Widget* w) {
    auto& a = *w->ac;
    omega::AppearancePacking ap;
    const auto& packing = a.m.model().packing;
    std::copy(packing.begin(), packing.end(), ap.bytes.begin());
    ap.initialized.fill(true);
    omega::appearance_remove_blank_pairs(ap);  // 00406e00 on the local copy
    omega::AgentPacking copy;
    std::copy(ap.bytes.begin(), ap.bytes.end(), copy.begin());
    omega::AgentPartCache& cache = cache_of(host_, 0x28ba90);
    omega::agent_cache_unlock(cache);  // 00403310
    const auto p = host_.current_profile();
    host_.profile_set_byte(p, 0x8dc, 0);
    host_.profile_set_byte(p, 0x91c, 0);
    host_.profile_set_word(p, 0x99c, -1);
    host_.profile_set_byte(p, 0x95c, 0);
    host_.profile_set_word(p, 0x9a0, -1);
    host_.agent_store_appearance(p, copy, cache);  // 00334000
    if (g.card.present) {
        Widget* next = factory("GuiAgentSave", std::string("AGENTOPN"));
        switch_screens(w, next);
    } else {
        popup_open(4, host_.get_string("MC_ALERT"), host_.get_string("MC_NOCARDCONT"), w, 3);  // 002cae30
    }
}

// 0028ea40: the base update, then L2 / R2 (0055a44c bits 0 / 1) turn the model while no camera
// transition runs (+c0); D_004ffbb8 = the card is present.
void GuiRuntime::ac_update(Widget* w, float dt) {
    auto& a = *w->ac;
    base_update(w, dt);
    const float step = fmul(pi, fdiv(dt, 30.0f));
    if (a.m.transition == 0) {
        if (g.pad_held & 1) {
            a.m.camera.rot[2] = fadd(a.m.camera.rot[2], step);
            if (!(a.m.camera.rot[2] <= pi)) a.m.camera.rot[2] = fsub(a.m.camera.rot[2], two_pi);
        }
        if (g.pad_held & 2) {
            a.m.camera.rot[2] = fsub(a.m.camera.rot[2], step);
            if (a.m.camera.rot[2] < minus_pi) a.m.camera.rot[2] = fadd(a.m.camera.rot[2], two_pi);
        }
    }
    g.card_ok = g.card.present != 0;
    ++a.m.timer;        // PORT-ONLY: the T0_COUNT stand-in the randomize seeds with
    ac_preview(w);
    // 0028e4c8..0028e50c: after the draw every model steps vt +48 (00392b40 -> 003ce650 submits it,
    // +30 = 0 at 00394120), then 0040f3e0(dt) runs vt +50 00392aa0 -> 003ab510(+2ec0, dt): the clip
    // advances by the update's dt (in 1/30 s ticks here, 00415950), so a picture shows the pose
    // before this update's step.
    a.anim_time += dt / 30.0f;
}

// 0028bbe0
std::uint8_t GuiRuntime::ac_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {
    auto& a = *w->ac;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x10 && a3 == 1) return 1;
    if (t == 0) {
        if (!sender) unhosted(0x28bbe0, "an activation without a sender");
        a.in_group = 0;
        const std::string name = cstr_of(sender->name);
        ac_select(w, name);  // 0028d610
        if (name == "gender") {
            a.group = 0;
            a.in_group = 1;
        } else if (name == "randomize") {
            am_group_hide(a.body);  // 0028c1d0
            a.group = 1;
            a.in_group = 1;
            omega::agent_create_randomize(cache_of(host_, 0x28cb20), a.m, 1, false, a.m.timer);  // 0028cb20(this, 1, 0)
            g.ac_table = a.m.table;
        } else if (name == "body") {
            am_group_show(sender);  // 0028c2a0
            am_group_hide(a.head);
            a.group = 2;
            a.in_group = 1;
        } else if (name == "head") {
            am_group_show(sender);
            am_group_hide(a.body);
            if (a.m.sex == 0 && a.beard) a.beard->flags &= ~flag_visible;
            a.group = 3;
            a.in_group = 1;
        } else if (name == "agentname") {
            am_group_hide(a.head);
            a.group = 4;
            a.in_group = 1;
        }
        return base_event(w, sender, type, a3, t0);  // 0041ef10
    }
    if (t == 0x1a && (a3 == 1 || a3 == 2)) {  // the keyboard's accept (0032b2d0): arg 1 Right, 2 Square
        if (!sender || !sender->kb) unhosted(0x28bbe0, "a keyboard reply from a widget not a GuiKeyboard");
        const std::string s = cstr_of(sender->kb->result.value_or(""));
        const auto p = host_.current_profile();
        if (!s.empty()) {
            host_.agent_set_name(p, s);  // 003340b0
            if (Widget* name = of_class(find(w, "value_agent_name"), WidgetClass::text)) set_text(name, s, false);
        }
        if (a3 == 2 && !s.empty()) {
            ac_save(w);
            return 1;
        }
        if (Widget* b = find(w, "agentname")) set_focus(b);  // 00414b30
        return 1;
    }
    if (t == 0x1a && a3 == 3) {  // MC_NOCARDCONT's OK: the agent is created without a card
        g.card_ok = 0;
        g.card_slot = -1;
        host_.agent_create(host_.current_profile(), 0, 1);  // 00333f60(agent, 0, nosave 1)
        Widget* next = g.app_online ? factory("GuiCommandCenter", std::string("CMDCENTR"))
                                    : factory("GuiOnlineOfflineSelect", std::string("onoffsel"));
        switch_screens(w, next);
    }
    return base_event(w, sender, type, a3, t0);
}

// 0028d610 (the category bookkeeping is agent_create_select)
void GuiRuntime::ac_select(Widget* w, const std::string& name) {
    auto& a = *w->ac;
    auto& m = a.m;
    const auto old = m.category;
    omega::agent_create_select(cache_of(host_, 0x28d610), m, name);
    g.ac_table = m.table;
    if (m.category == old) return;
    ac_show(w);
}

// The +58 / +5c writes of 0028d610: shown with the slider at the position and "%d/%d" (or "0/0")
// for a category; hidden and "" for none.
void GuiRuntime::ac_show(Widget* w) {
    auto& a = *w->ac;
    const auto& m = a.m;
    if (m.category < 11) {
        if (a.indicator) {
            a.indicator->flags |= flag_visible;
            send(w, a.indicator, 0xa, m.slider_value, 0);
        }
        if (a.value_slot) set_text(a.value_slot, m.slot_text, false);
    } else {
        if (a.indicator) a.indicator->flags &= ~flag_visible;
        if (a.value_slot) set_text(a.value_slot, "", false);
    }
}

// 0028c390 (the part step is agent_create_step)
void GuiRuntime::ac_step(Widget* w, bool back, bool forward) {
    auto& a = *w->ac;
    auto& m = a.m;
    const bool active = (back || forward) && m.category < 11 && m.count != 0;
    const auto changes = m.changes;
    omega::agent_create_step(cache_of(host_, 0x28c390), m, back, forward);
    g.ac_table = m.table;
    if (!active) return;
    if (m.changes != changes) send(w, &canvas_, 0x10, 6, 0);  // 004147a0: the step sound
    if (a.value_slot) set_text(a.value_slot, m.slot_text, false);
    if (a.indicator) send(w, a.indicator, 0xa, m.slider_value, 0);
}

// GuiCharacterDisplay's picture. The original draws the current model through the display's +48
// callback (0028d870): first the camera approach 0028e640 (agent_camera_approach: +80..+ac easing to
// the category's MANSORT2 record over +c0 frames, record 0 while +d0), then the record mode 0x15
// projection and the model turned by +a4 / +a8 / +ac on (0, 0, -3) (agent_camera_view). Checkpoint
// 132cc6: the approach and the camera are the original's, stepped once per screen update; the draw
// (gui_draw.cpp's GuiCharacterDisplay case, the host's character_display_draw) projects the model
// through a.m.camera, posed by ANIMIDLE.SKA (the ctor's 003ab430, the only clip) at pose_time.
void GuiRuntime::ac_preview(Widget* w) {
    auto& a = *w->ac;
    if (!a.display || !(w->flags & flag_awake)) return;
    if (omega::AgentPartCache* cache = host_.agent_parts())
        omega::agent_camera_approach(*cache, a.m.camera, a.m.transition, a.m.model().order, a.m.pair,
                                     a.in_group != 0, 0x28e640);
    a.pose_time = preview_clock(a.anim_time);
}

// ---------------------------------------------------------------- GuiAgentSave (vtable 004dd3a0)

// 00292a20
void GuiRuntime::as_wake(Widget* w) {
    g.app_2f = 0;
    Widget* t = find(w, "menutitle");
    if (t && t->cls == WidgetClass::text) {  // IsA "GuiTextWidget" (004a7520)
        const auto s = host_.get_string("MC_SAVETITLE");
        set_text(t, s ? *s : std::string(), false);
    }
    al_slots(w);  // 00292e80 (+7c 1: the empty slots stay selectable)
    base_wake(w);
}

// 00292820: Triangle back to a fresh GuiAgentCreate.
std::uint8_t GuiRuntime::as_input(Widget* w, const PadEvent& ev) {
    if (ev.kind == 1 && ev.port == 0 && ev.mask == 0x10 && ev.released == 0) {
        Widget* next = factory("GuiAgentCreate", std::string("AGENTNEW"));
        switch_screens(w, next);
        return 1;
    }
    return 0;
}

// 002928e0: the save flow's end, whatever its result: the command centre online, else the online /
// offline select.
void GuiRuntime::as_done(Widget* w) {
    Widget* next = g.app_online ? factory("GuiCommandCenter", std::string("CMDCENTR"))
                                : factory("GuiOnlineOfflineSelect", std::string("onoffsel"));
    switch_screens(w, next);
}

}  // namespace omega::gui
