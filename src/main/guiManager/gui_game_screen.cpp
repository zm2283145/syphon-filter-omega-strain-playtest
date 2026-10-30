// GuiGameScreen's weapon select (checkpoint 132hud): the HUD's +6a4 screen (GAMEGUI.GUI) shows, while
// Select is held 200 ms (the pad producer 0024b5d0 at 0x0024cc74), "weaponselect": five GuiWeaponDisplay
// panels over the player's weapons and the hand (holster), stepped with L1 / L2 / R1 / R2 and chosen when
// Select is released. 0045ca20 opens / closes it, 0045c760 gathers the list, 0045c5e0 fills the panels,
// 0045c540 steps, 0045c480 equips the choice, 0045d420 is the screen's input, 0045d960 its update (the
// lines_arrows clock and the panels' fades) and 0045c0f0 the panels' round counts. research/COMBAT.md
// "Checkpoint 132hud".
#include "gui_runtime.hpp"

#include <algorithm>
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
// min.s / max.s (COP1 funct 0x29 / 0x28) on flushed operands.
float fmin_ee(float a, float b) { return ftz(a) < ftz(b) ? ftz(a) : ftz(b); }
float fmax_ee(float a, float b) { return ftz(a) > ftz(b) ? ftz(a) : ftz(b); }
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

// 00497acc / 00497aec, indexed by +dc (0..5): the lines_arrows element's closed / open clocks. Index 5
// (five panels) reads the words after the five-entry tables, 00497ae0 = 1 and 00497b00 = 10 (no code
// writes either).
constexpr std::array<std::int32_t, 6> closed_acc = {0, 50, 60, 70, 80, 1};
constexpr std::array<std::int32_t, 6> open_aec = {0, 53, 64, 76, 88, 10};
constexpr std::int32_t hand_item = -2;  // *00497b18: the list's hand (holster) entry

constexpr std::int32_t name_on_wake = 0x4c21f8;      // "onWake"
constexpr std::int32_t name_on_sleep = 0x4c2200;     // "onSleep"
constexpr std::int32_t name_on_activate = 0x4c2278;  // "onActivate"

}  // namespace

void GuiRuntime::gs_init(Widget* screen) {  // 0045dfd0 (the weapon select's parts), then 0045cc90
    game_screen = screen;
    gs = GameScreen{};
    // 0045e094: 003d0940(obj, 0, 0, 10, 10, 00418420("callout"), 00587d80); +10 = 2, +11 = 1.
    TextObject o;
    o.rect = {0, 0, 10, 10};
    o.font = host_.font("callout");
    o.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    host_.text_object(o);
    o.b10 = 2;
    o.b11 = 1;
    gs.text = o;
    // 0045e140..0045e194: +68 = 0041db80(gs, "subtitles") when its vt +58 isA "GuiSubTitleDisplay", else 0.
    {
        Widget* s = find(screen, "subtitles");
        gs.sub_68 = s && s->cls == WidgetClass::subtitle ? s : nullptr;
    }
    // 0045e18c..0045e2e4: the lift floor chooser. +e0 = "choosemenu" (004c22e0); with it, +e4 = its "title"
    // (004c22f0) when isA GuiTextWidget (004a7520) and 0041c0f0(+e4, 1); +e8 = its "choices" (004c22f8)
    // when isA GuiTextListWidget (004aa640), its +b0 = 00587ca0 and +e0 = 00587cb0, +f0 = 1. +e0 is
    // cleared unless all three were found.
    gs.choose_e0 = find(screen, "choosemenu");
    if (gs.choose_e0) {
        Widget* t = find(gs.choose_e0, "title");
        gs.title_e4 = t && (t->cls == WidgetClass::text || t->cls == WidgetClass::button ||
                                t->cls == WidgetClass::edit) ? t : nullptr;
        if (!gs.title_e4) unhosted(0x45e1ec, "0041c0f0 on a missing choosemenu \"title\" (a NULL +e4)");
        gs.title_e4->b75 = 1;  // 0041c0f0
        if (gs.title_e4->obj) gs.title_e4->obj->b58 = 1;
        Widget* c = find(gs.choose_e0, "choices");
        gs.choices_e8 = c && c->cls == WidgetClass::textlist && c->list ? c : nullptr;
        if (!gs.choices_e8) unhosted(0x45e264, "a missing choosemenu \"choices\" list (a NULL +e8)");
        gs.choices_e8->list->rgba = {0.6509804f, 0.7019608f, 1.0f, 1.0f};  // 00587ca0
        gs.choices_e8->list->rgba_e0 = {1.0f, 1.0f, 1.0f, 1.0f};           // 00587cb0
        gs.choices_e8->list->f0 = 1;
    }
    if (!gs.choose_e0 || !gs.title_e4 || !gs.choices_e8) gs.choose_e0 = nullptr;
    // 0045e2e8..0045e3e4 (checkpoint 132hud5): +114 = "value_codename" when it isA GuiTextWidget. With +114: in a
    // multiplayer game (byte 005721c8) the slot *00494178 (002ebaf8(00494178) first while it is -1) names it,
    // 0041beb0(+114, 0012f5d0(004ffb50, slot) or "" (004c2268) when NULL, 0), and +14 |= 2; otherwise +14 &= ~2.
    // 005721c8 is 0 natively; the port-only co-op HUD (gs_coop_codename) takes the multiplayer branch with slot 0.
    const auto text_widget = [](Widget* w) {
        return w && (w->cls == WidgetClass::text || w->cls == WidgetClass::button || w->cls == WidgetClass::edit) ? w
                                                                                                                 : nullptr;
    };
    gs.codename_114 = text_widget(find(screen, "value_codename"));
    if (gs.codename_114) {
        if (gs_coop_codename) {
            set_text(gs.codename_114, gs_coop_codename().value_or(std::string()), false);
            gs.codename_114->flags |= flag_visible;
        } else {
            gs.codename_114->flags &= static_cast<std::uint16_t>(~flag_visible);
        }
    }
    // 0045e470..0045e4e8: +120 "headset_playing" when it isA GuiTextWidget, +124 "label_talking" (the timer texts
    // +118 / +11c in between are the host's PlayTimerWidgets).
    gs.headset_120 = text_widget(find(screen, "headset_playing"));
    gs.talking_124 = find(screen, "label_talking");
    // 0045e4fc..: 0041db80 "weaponselect" (+6c), "lines_arrows" (+a4), "brackets" (+a0).
    gs.ws = find(screen, "weaponselect");
    if (gs.ws) {
        gs.lines = find(gs.ws, "lines_arrows");
        gs.brackets = find(gs.ws, "brackets");
    }
    // 0045cc90
    if (!gs.ws) return;
    gs.sel_90 = gs.sel_94 = -1;
    gs.dc = 0;
    gs.b9c = 0;
    gs.t98 = 0;
    gs.d0 = gs.d4 = 0;
    gs.ws->flags &= static_cast<std::uint16_t>(~flag_visible);
    gs.ws->flags &= static_cast<std::uint16_t>(~flag_enabled);
    for (int i = 0; i < 5; ++i) {
        Widget* panel = new_widget("GuiWeaponDisplay");  // 0041f960
        if (!panel || !panel->wd) unhosted(0x45cd14, "the GuiWeaponDisplay panel class");
        char name[16];
        std::snprintf(name, sizeof name, "panel0%d", i + 1);
        Widget* r = find(gs.ws, name);
        if (!r) unhosted(0x45cd38, std::string("weaponselect without ") + name);
        set_pos(panel, r->rect[0], r->rect[1]);  // 0041eba0
        panel->rect[2] = r->rect[2];
        panel->rect[3] = r->rect[3];
        panel->parent = gs.ws;  // 0041dd60 GuiWidget_AddChild
        gs.ws->children.push_back(panel);
        panel->wd->a4 = 0;
        gs.panels[static_cast<std::size_t>(i)] = panel;
    }
    // 0045cdd4: 00147150(*004ffd30, "icon_hand.skl", gs + d8, -1, 0, 0, 0)
    if (const auto h = host_.hand_model()) {
        gs.d8 = *h;
    } else {
        gs.d8 = 0;
        gs.d8_unhosted = true;
    }
}

// ---- The lift floor chooser (checkpoint 132el2; research/ELEVATORS.md "The floor chooser").

bool GuiRuntime::gs_choose_shown_bd00() const {  // 0045bd00
    return gs.choose_e0 && (gs.choose_e0->flags & flag_visible);
}

// 0045bd30: the first call records the agent position (+100, +110 = 1); later calls close the chooser once
// the agent is not within 3 (0x40400000) of it: VU vsub, vmul.xyz, the x + y + z accumulation, vsqrt and
// c.ole.s.
void GuiRuntime::gs_choose_track_bd30(const std::array<float, 4>& pos) {
    if (!gs.b110) {
        gs.pos_100 = pos;
        gs.b110 = 1;
        return;
    }
    const float dx = fsub(gs.pos_100[0], pos[0]), dy = fsub(gs.pos_100[1], pos[1]), dz = fsub(gs.pos_100[2], pos[2]);
    const float d = std::sqrt(fadd(fadd(fmul(dx, dx), fmul(dy, dy)), fmul(dz, dz)));
    if (!(d <= 3.0f)) gs_choose_cancel_bf80();
}

// 0045bdf0: a row other than the last ("Cancel") with a lift replies the row; then 0045bf80.
void GuiRuntime::gs_choose_confirm_bdf0() {
    if (!gs.choose_e0) return;
    const auto& l = *gs.choices_e8->list;
    const std::uint32_t row = l.row;                                         // +e8 +54
    const auto last = static_cast<std::uint32_t>(l.items.size()) - 1U;       // +e8 +94 - 1 (sltu)
    if (row < last && gs.receiver_ec != 0) {
        if (!gs_choice_reply) unhosted(0x45be68, "the chooser's MSGGAME_MENU_CHOICE to the lift (003c9420)");
        gs_choice_reply(gs.receiver_ec, gs.object_f0, static_cast<std::int32_t>(row));
    }
    gs_choose_cancel_bf80();
}

void GuiRuntime::gs_choose_lift_beb0(std::uint32_t lift) { gs.receiver_ec = lift; }  // 0045beb0

void GuiRuntime::gs_choose_select_bec0(std::int32_t row) {  // 0045bec0
    if (!gs.choose_e0 || !(row > 0)) return;
    if (!(row < static_cast<std::int32_t>(gs.choices_e8->list->items.size()))) return;
    array_select(gs.choices_e8, 0, static_cast<std::uint32_t>(row));  // 00424430
}

void GuiRuntime::gs_choose_title_bf10(const std::optional<std::string>& text) {  // 0045bf10
    if (!gs.choose_e0) return;
    if (!text) unhosted(0x41beb0, "the chooser title's NULL string (0045bf28)");
    set_text(gs.title_e4, *text, false);  // 0041beb0(+e4, text, 0)
}

void GuiRuntime::gs_choose_add_bf40(const std::optional<std::string>& name) {  // 0045bf40
    if (!gs.choose_e0) return;
    list_add(gs.choices_e8, name, -1, 0);  // 00421290(+e8, name, -1, 0)
}

// 0045bf80: hide, clear the list, reply -0xff to +ec when an agent is set, forget both and 0045de40 (the
// focus back to the game screen, 0036a240(0055a300)).
void GuiRuntime::gs_choose_cancel_bf80() {
    if (!gs.choose_e0) return;
    gs.choose_e0->flags &= static_cast<std::uint16_t>(~flag_visible);
    list_clear(gs.choices_e8);  // 00421070
    if (gs.object_f0 != 0) {
        if (!gs_choice_reply) unhosted(0x45bfe8, "the chooser's MSGGAME_MENU_CHOICE to the lift (003c9420)");
        gs_choice_reply(gs.receiver_ec, gs.object_f0, -0xff);
    }
    gs.receiver_ec = 0;
    gs.object_f0 = 0;
    set_focus(game_screen);  // 0045de40: 00414b30(004147a0(), gs)
    host_.pad_reset();       // 0036a240(0055a300)
}

// 0045c030: an empty list closes; otherwise show, focus the list, add "Cancel" (004c21e0 through 003fdb50),
// +f0 = the agent, +110 = 0 and the GS_POPUP send (0x10, 2) to the canvas.
void GuiRuntime::gs_choose_open_c030(std::uint64_t actor) {
    if (!gs.choose_e0) return;
    if (gs.choices_e8->list->items.empty()) {
        gs_choose_cancel_bf80();
        return;
    }
    gs.choose_e0->flags |= flag_visible;
    set_focus(gs.choices_e8);                           // 00414b30(004147a0(), +e8)
    list_add(gs.choices_e8, host_.get_string("Cancel"), -1, 0);
    gs.object_f0 = actor;
    gs.b110 = 0;
    send(gs.choose_e0, &canvas_, 0x10, 2, 0);           // 0041e0f0
}

// 0045d3d0: event 0x13 from +e8 confirms (0045bdf0); everything else is the base event 0041ef10.
std::uint8_t GuiRuntime::gs_event_d3d0(Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if ((type & 0xffff) == 0x13 && sender == gs.choices_e8) {
        gs_choose_confirm_bdf0();
        return 1;
    }
    return base_event(game_screen, sender, type, a3, t0);  // 0041ef10
}

void GuiRuntime::gs_subtitle_ce10(const std::optional<std::string>& name, std::int32_t, std::int32_t a3) {  // 0045ce10
    Widget* sub = gs.sub_68;
    if (!name || name->empty() || (*name)[0] == '\0' || !sub) {
        // 0045ce50..0045ce60: +68's flags &= ~2, with no NULL test.
        if (!sub) unhosted(0x45ce54, "the subtitle hide through a null +68 (no \"subtitles\" GuiSubTitleDisplay)");
        sub->flags &= static_cast<std::uint16_t>(~flag_visible);
        return;
    }
    if (const auto text = host_.get_string(*name)) {  // 003fdb50; NULL skips the display
        if (st_set_text(sub, *text)) {                 // 00459500
            if (!(sub->flags & flag_visible)) send(game_screen, sub, 6, name_on_wake, 0);  // 0041e0f0(gs, +68, 6, "onWake", 0)
            sub->st->vis = 4;  // +78 = 4
            sub->flags |= flag_visible;
        }
    }
    // 0045ced8: a3 != 0 (the PlayXA natives through 0045edb0) also sends the line's voice, whether or not the
    // string was found or shown.
    if (a3 != 0) host_.subtitle_voice(*name);
}

void GuiRuntime::gs_gather_c760() {  // 0045c760
    const std::int32_t held = host_.equip_held();  // slots[+84].item, -1 with +84 == 6
    gs.list.clear();
    gs.sel_90 = -1;
    const std::uint32_t s3 = held != -1 ? static_cast<std::uint32_t>(host_.weapon_class(held)) & 0xff : 0;
    // A held item whose slot class is 6 or more never meets s3 again (s2 wraps 5 -> 0): the original loops forever.
    // Checkpoint 132wf1: +84 == 6 (holstered, the goggles' slot) reads -1 (0045c79c), so only +84 == 7 with a
    // slot-7 item (the flashlight) could get here, which no equip path sets (0045c480 refuses classes >= 6).
    if (s3 >= 6) unhosted(0x45c7dc, "the weapon select's gather from slot " + std::to_string(s3) + " (the original never ends)");
    const auto slots = host_.equip_slots();
    std::uint32_t s2 = s3;
    std::int32_t s1 = 0;
    do {
        const std::int32_t item = slots[s2 & 0xff];
        if (item != -1 && std::find(gs.list.begin(), gs.list.end(), item) == gs.list.end()) {
            gs.list.push_back(item);  // 001c1080
            if (item == held) gs.sel_90 = s1;
            ++s1;
        }
        ++s2;
        if (!(s2 < 6)) s2 = 0;
    } while (s2 != s3);
    gs.list.push_back(hand_item);
    if (gs.sel_90 == -1) gs.sel_90 = static_cast<std::int32_t>(gs.list.size()) - 1;
    gs.dc = std::min<std::int32_t>(static_cast<std::int32_t>(gs.list.size()), 5);
}

void GuiRuntime::gs_fill_c5e0() {  // 0045c5e0
    if (!gs.ws || !(gs.ws->flags & flag_enabled)) return;
    const auto count = static_cast<std::int32_t>(gs.list.size());
    std::int32_t s2 = gs.sel_90;
    bool done = false;
    for (std::size_t i = 0; i < gs.panels.size(); ++i) {
        Widget* panel = gs.panels[i];
        if (!done) {
            const std::int32_t item = gs.list.at(static_cast<std::size_t>(s2));
            if (item == hand_item) {
                if (gs.d8_unhosted) unhosted(0x147150, "the hand icon model (icon_hand.skl) without a model loader");
                wd_set_item(panel, hand_item, gs.d8, std::nullopt);  // 00441000(panel, -2, +d8, 0)
            } else {
                wd_set(panel, item);  // 00441060
            }
            ++s2;
            if (!(s2 < count)) s2 = 0;
            if (s2 == gs.sel_90) done = true;
        } else {
            wd_set(panel, -1);
        }
    }
    auto& d = *gs.panels[0]->wd;
    d.a4 = gs.list.at(static_cast<std::size_t>(gs.sel_90)) != hand_item ? 1 : 0;
    d.rot = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    d.t = 0.0f;
}

void GuiRuntime::gs_step_c540(bool forward) {  // 0045c540
    const auto count = static_cast<std::int32_t>(gs.list.size());
    if (count < 2) return;
    gs.b9c = 1;
    gs.sel_90 += forward ? 1 : -1;
    if (gs.sel_90 < 0) gs.sel_90 = count - 1;
    else if (!(gs.sel_90 < count)) gs.sel_90 = 0;
    gs_fill_c5e0();
    send(gs.ws, &canvas_, 0x10, 0, 0);  // 00465a50: the cursor sound
}

void GuiRuntime::gs_choose_c480() {  // 0045c480
    if (gs.sel_90 == -1) return;
    const std::int32_t item = gs.list.at(static_cast<std::size_t>(gs.sel_90));
    if (item == hand_item) {
        host_.holster_current();  // 00143b60(+3524)
        return;
    }
    if ((static_cast<std::uint32_t>(host_.weapon_class(item)) & 0xff) >= 6) return;
    if (!host_.weapon_record(item)) return;  // 00147840
    if (host_.weapon_is_held(item)) return;
    host_.equip_item(item);  // 00175ff0(player, item)
}

void GuiRuntime::gs_open_ca20(bool open) {  // 0045ca20
    Widget* ws = gs.ws;
    if (!ws) return;
    if (open == ((ws->flags & flag_enabled) != 0)) return;
    const std::size_t dc = static_cast<std::size_t>(gs.dc);
    if (open) {
        if (g.controller_6a8) return;
        ws->flags |= flag_visible;
        ws->flags |= flag_enabled;
        gs_gather_c760();
        gs.sel_94 = gs.sel_90;
        gs.t98 = host_.time_ms();  // 003698b0
        gs.b9c = 0;
        gs_fill_c5e0();
        const std::size_t n = static_cast<std::size_t>(gs.dc);
        if (gs.d4 == 0 && gs.d0 == 0) send(game_screen, gs.lines, 0xa, closed_acc.at(n), 0);
        gs.d4 = 1;
        gs.d0 = 2;
        send(game_screen, gs.lines, 0xb, open_aec.at(n), 0);
        send(game_screen, gs.brackets, 6, name_on_wake, 0);
        ws->flags |= flag_focusable;
        set_focus(ws);  // 00414b30(004147a0(), ws)
        send(ws, &canvas_, 0x10, 2, 0);  // GS_POPUP
        return;
    }
    gs.d4 = 0;
    gs.d0 = 2;
    out_value = 0.0f;
    send(game_screen, gs.lines, 9, out_arg, 0);
    const float t = fmin_ee(static_cast<float>(open_aec.at(dc)), out_value);
    send(game_screen, gs.lines, 0xc, cvt_w(t, 0x45cbe8), closed_acc.at(dc));
    ws->flags &= static_cast<std::uint16_t>(~flag_enabled);
    send(game_screen, gs.brackets, 6, name_on_sleep, 0);
    ws->flags &= static_cast<std::uint16_t>(~flag_focusable);
    set_focus(game_screen);
    send(ws, &canvas_, 0x10, 3, 0);  // GS_POPUP
}

std::uint8_t GuiRuntime::gs_input_d420(const PadEvent& ev) {  // 0045d420
    // 0045d434..0045d4a8: pad 0's Triangle (0x10) press closes a shown chooser.
    if (ev.kind == 1 && ev.port == 0 && ev.mask == 0x10 && ev.released == 0 && gs_choose_shown_bd00()) {
        gs_choose_cancel_bf80();
        return 1;
    }
    if (gs.d4 != 1) return 0;
    const bool pad0 = ev.kind == 1 && ev.port == 0;
    if (pad0 && ev.mask == 0x100 && ev.released == 1) {
        // A quick release (under 100 ms, not stepped, the selection unchanged) steps forward first.
        if (!gs.b9c && gs.sel_94 == gs.sel_90) {
            const std::int64_t now = host_.time_ms();
            if (static_cast<std::uint64_t>(now - static_cast<std::int64_t>(gs.t98)) < 100) gs_step_c540(true);
        }
        gs_choose_c480();
        gs_open_ca20(false);
        return 1;
    }
    if (pad0 && ev.released == 0 && (ev.mask == 8 || ev.mask == 2)) {  // R1 / R2
        gs_step_c540(false);
        return 1;
    }
    if (pad0 && ev.released == 0 && (ev.mask == 4 || ev.mask == 1)) {  // L1 / L2
        gs_step_c540(true);
        return 1;
    }
    return 0;
}

void GuiRuntime::gs_update_d960(float) {  // 0045d960 after 0041e3b0
    // 0045d984..0045d9a8: the base update has already advanced the child
    // GuiSubTitleDisplay's +54 clock. Its last line end is a strict bound;
    // once it is earlier than the clock, hide the subtitle widget.
    if (gs.sub_68 && (gs.sub_68->flags & flag_visible)) {
        const auto& st = *gs.sub_68->st;
        if (st.times.empty() || st.times.back() < st.time)
            gs.sub_68->flags &= static_cast<std::uint16_t>(~flag_visible);
    }
    // 0045d9b8..0045db3c: with the voice chat byte 00582b88 (written only by the network voice code), +120 is shown
    // and named after the talker 00437600(*00582cf0) (+124 shown for another player's). The port has no voice chat
    // (the byte stays 0; the port-only co-op HUD's CPU partner never talks): 0045db40 clears +120's +14 bit 2, with
    // no NULL test.
    if (!gs.headset_120) unhosted(0x45db40, "the game screen's update without \"headset_playing\" (a NULL +120)");
    gs.headset_120->flags &= static_cast<std::uint16_t>(~flag_visible);
    if (!gs.ws) return;
    const std::size_t dc = static_cast<std::size_t>(gs.dc);
    out_value = 0.0f;
    send(game_screen, gs.lines, 9, out_arg, 0);
    float t = fmin_ee(static_cast<float>(open_aec.at(dc)), out_value);
    if (gs.d4 != gs.d0) {
        if (gs.d4 == 1) {
            if (t == static_cast<float>(open_aec.at(dc))) {
                gs.d0 = 1;
                send(game_screen, gs.brackets, 6, name_on_activate, 0);
            }
        } else if (t == static_cast<float>(closed_acc.at(dc))) {
            gs.d0 = 0;
            gs.ws->flags &= static_cast<std::uint16_t>(~flag_visible);
        }
    }
    if (!(gs.ws->flags & flag_visible) || !(gs.dc > 0)) return;
    t = fsub(t, static_cast<float>(closed_acc.at(dc)));
    const float f5 = fdiv(static_cast<float>(open_aec.at(dc) - closed_acc.at(dc)), static_cast<float>(gs.dc));
    for (std::int32_t i = 0; i < gs.dc; ++i) {
        const float f4 = fmul(static_cast<float>(i), f5);
        float v;
        if (t < fadd(f4, f5)) v = t < f4 ? 1.0f : fsub(1.0f, fdiv(fsub(t, f4), f5));
        else v = 0.0f;
        const auto k = static_cast<std::size_t>(i);
        gs.panels[k]->wd->fade = fmin_ee(fmax_ee(v, 0.0f), 1.0f);
        gs.bc[k] = v;
    }
}

std::vector<GuiRuntime::GsCount> GuiRuntime::gs_counts_c0f0() const {  // 0045c0f0
    std::vector<GsCount> out;
    const auto count = static_cast<std::int32_t>(gs.list.size());
    std::int32_t s3 = gs.sel_90;
    for (std::int32_t s2 = 0; s2 < gs.dc; ++s2) {
        if (!(s2 < count)) break;
        const std::int32_t item = gs.list.at(static_cast<std::size_t>(s3));
        if (item != hand_item) {
            const auto rec = host_.weapon_record(item);  // 00147840
            if (!rec) unhosted(0x45c16c, "weapon record of item " + std::to_string(item) + " is null");
            const auto hold = static_cast<std::uint8_t>(rec->hold_f0);
            const std::int32_t slot = host_.weapon_class(item);  // 00143d20
            char buf[0x20] = {};
            if (hold == 1 || hold == 7) {
                std::snprintf(buf, sizeof buf, "%d", host_.weapon_spare(slot));
            } else if (host_.weapon_has_clip(item)) {
                const std::int32_t clip = host_.weapon_clip(slot);
                std::snprintf(buf, sizeof buf, "%d/%d", clip, host_.weapon_spare(slot));
            }
            if (buf[0]) {
                const Widget* panel = gs.panels[static_cast<std::size_t>(s2)];
                out.push_back({panel, buf, panel->rect, gs.bc[static_cast<std::size_t>(s2)]});
            }
        }
        ++s3;
        if (!(s3 < count)) s3 = 0;
    }
    return out;
}

}  // namespace omega::gui
