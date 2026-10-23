// GuiAgentModify (vtable 004de920, ctor 00343e10 over the GuiSaveAgentWidget 00441d00; AGENTMOD.GUI):
// the agent appearance editor the command centre's personnel "modify_agent" opens (00295f20). The
// part search and stepping are agent_modify.cpp (interpreter goldens tools/ee/gen_agent_modify.py);
// this file is the widget: its init, wake, input, event and the group / availability helpers.
// research/FRONTEND.md "Checkpoint 132cc4b".
#include "gui_runtime.hpp"
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

constexpr float pi = std::bit_cast<float>(0x40490fdbu);      // 00342cf0
constexpr float two_pi = std::bit_cast<float>(0x40c90fdbu);  // 00342d40
constexpr float minus_pi = std::bit_cast<float>(0xc0490fdbu);

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsA

// The idle clip's clock: the display actors step by the update's dt in seconds (manager +64,
// 00415980) after each draw (00342768 / 00342798). PORT-ONLY: quantized to 30 Hz pictures.
float preview_clock(float seconds) { return std::floor(seconds * 30.0f) / 30.0f; }

}  // namespace

// 00343e10: the base 00441d00 (+48 popup -2), flags |= 0x20, +ec -1, +f0 0, +7c 0; D_00493a60 -1
// becomes 0; the packing copy from 0055d510 + 0x440 * D_00493a60, its pairs and categories, the
// refresh 00343940; then 00332500(agent, list) and 00403040(0055dda0, list) lock the parts the
// agent has not unlocked (after the refresh: the counts include them).
void GuiRuntime::am_construct(Widget* w) {
    w->am = std::make_unique<Widget::AgentMod>();
    auto& a = *w->am;
    w->flags |= 0x20;
    a.group = -1;
    a.in_group = 0;
    omega::AgentPartCache* cache = host_.agent_parts();
    if (!cache) unhosted(0x22a220, "the front end's character part cache (0055dda0)");
    if (g.packing_slot == -1) g.packing_slot = 0;
    a.m = omega::agent_modify_open(*cache, g.packings[static_cast<std::size_t>(g.packing_slot)], g.am_table);
    const auto locked = host_.agent_locked_parts(host_.current_profile());
    omega::agent_cache_lock(*cache, locked);
    am_store_table(w);
}

void GuiRuntime::am_store_table(Widget* w) { g.am_table = w->am->m.table; }  // D_00492a00 is static

// 00342fa0
void GuiRuntime::am_init(Widget* w) {
    auto& a = *w->am;
    base_init(w);
    auto& m = a.m;
    static constexpr const char* five[6] = {"TATT1", "TATT2", "TATT3", "PATC1", "PATC2", "PATC3"};  // 004068b0
    for (std::size_t i = 0; i < 6; ++i) a.pairs[i] = omega::agent_pair_of(m.packing, five[i]);
    omega::AgentPartCache& cache = *host_.agent_parts();
    omega::agent_modify_patch_advance(cache, m, a.pairs[3], false);  // 003437c0
    omega::agent_modify_patch_advance(cache, m, a.pairs[4], false);
    omega::agent_modify_patch_advance(cache, m, a.pairs[5], true);   // 00343680
    static constexpr const char* names[10] = {"leftarm", "rightarm", "back", "leftarm2", "rightarm2",
                                              "hat2", "body", "head", "tattoos", "patches"};
    for (std::size_t i = 0; i < 10; ++i) a.buttons[i] = of_class(find(w, names[i]), WidgetClass::button);
    a.group_w = find(w, "createagent_grp");
    a.indicator = find(w, "selectindicator");
    a.value_slot = of_class(find(w, "value_slot"), WidgetClass::text);
    a.beard = of_class(find(w, "beard"), WidgetClass::button);
    a.preview = of_class(find(w, "bitmap_preview"), WidgetClass::bitmap);
    a.value_name = of_class(find(w, "value_name"), WidgetClass::text);
    for (std::size_t i = 6; i < 10; ++i) am_group_hide(a.buttons[i]);  // body, head, tattoos, patches
    // "playercharacter" (IsA GuiCharacterDisplay D_004a9bf0): D_005331d0 = this, its +48 = 00345020.
    if (Widget* d = of_class(find(w, "playercharacter"), WidgetClass::chardisplay)) {
        d->cd_screen = w;
        a.display = d;
    }
    Widget* agent = of_class(find(w, "value_agent"), WidgetClass::text);  // IsA GuiTextWidget D_004a7520
    Widget* rank = of_class(find(w, "value_rank"), WidgetClass::text);
    const auto p = host_.current_profile();
    std::string agent_text, rank_text;
    if (host_.profile_byte(p, 0xb70)) {
        agent_text = cstr_of(host_.profile_name(p).value_or(""));  // 00336170
        rank_text = cstr_of(host_.profile_rank(p).value_or(""));   // 00336110
    } else {
        agent_text = "Player";                                      // D_004ba090
        rank_text = cstr_of(host_.loc_text(host_.loc_find("Rank_00") + 1).value_or(""));  // D_004ba098
    }
    if (!agent) unhosted(0x3435cc, "AGENTMOD without its value_agent text");
    if (!rank) unhosted(0x3435f4, "AGENTMOD without its value_rank text");
    set_text(agent, agent_text, false);
    set_text(rank, rank_text, false);
    if (a.value_slot) set_text(a.value_slot, "", false);  // D_004b9f20
    am_avail(w);
}

// 00342e80: 00403310(0055dda0), 0040f540, then 0041f090.
void GuiRuntime::am_deinit(Widget* w) {
    if (omega::AgentPartCache* cache = host_.agent_parts()) omega::agent_cache_unlock(*cache);
    host_.app_40f540();
    if (w->am && w->am->display) w->am->display->cd_screen = nullptr;
}

// 00342f00
void GuiRuntime::am_wake(Widget* w) {
    auto& a = *w->am;
    g.app_2f = 1;
    base_wake(w);
    if (a.indicator) {
        send(w, a.indicator, 0xa, 0, 0);
        a.indicator->flags &= ~flag_visible;
    }
    repeat_mask |= 0xc;  // L1 / R1 repeat
    const auto p = host_.current_profile();
    host_.profile_set_byte(p, 0xb68, 0);
    host_.profile_set_byte(p, 0xb72, 1);
    a.m.dirty = false;
    am_preview(w);
}

// 00342ec0
void GuiRuntime::am_sleep(Widget* w) {
    base_sleep(w);
    repeat_mask &= ~0xcu;
}

// 0033fdc0 / 00340ec0: 00403310(0055dda0), 0040f540, then 0041e0f0's switch to GuiCommandCenter.
void GuiRuntime::am_leave(Widget* w) {
    if (omega::AgentPartCache* cache = host_.agent_parts()) omega::agent_cache_unlock(*cache);
    host_.app_40f540();
    Widget* next = factory("GuiCommandCenter", std::string("CMDCENTR"));  // 004b9ed0 / 004b9ee8
    if (next) switch_screens(w, next);
}

// 0033fa80
std::uint8_t GuiRuntime::am_input(Widget* w, const PadEvent& ev) {
    auto& a = *w->am;
    auto& m = a.m;
    const auto pressed = [&](std::uint32_t mask) {
        return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
    };
    if (pressed(0x80)) {  // Square: save
        omega::agent_modify_save_merge(m.packing);  // 004068b0 "VEST_" (0033fb60..0033fbb4)
        if (g.packing_slot == -1) g.packing_slot = 0;
        auto& slot = g.packings[static_cast<std::size_t>(g.packing_slot)];
        slot = m.packing;  // 0x44 quadwords
        omega::AppearancePacking ap;
        std::copy(slot.begin(), slot.end(), ap.bytes.begin());
        ap.initialized.fill(true);
        omega::appearance_remove_blank_pairs(ap);  // 00406e00
        std::copy(ap.bytes.begin(), ap.bytes.end(), slot.begin());
        omega::AgentPartCache& cache = *host_.agent_parts();
        omega::agent_cache_unlock(cache);  // 00403310
        host_.agent_store_appearance(host_.current_profile(), slot, cache);  // 00334000
        // 0033fc80..0033fce4: with the network manager (004294c0) up, 00585e90 copied, rebuilt (002cbd40) and uploaded
        // (004505e0, AccountUpdateStats); the host's lobby does it (132on2e; offline: nothing).
        host_.lobby_agent_edited();
        a.save.mode = 1;
        save_start(w);  // 00441a10
        return 1;
    }
    if (pressed(0x10)) {  // Triangle
        if (m.dirty)
            popup_open(5, host_.get_string("MC_ALERT"), host_.get_string("AGM_NOSAVE"), w, 0xc);  // 002cae30
        else
            am_leave(w);
        return 1;
    }
    if (pressed(0x2000) && a.in_group) {  // Right: into the group's items
        send(w, &canvas_, 0x10, 0, 0);
        if (a.group == 2) {
            am_group_lock(a.buttons[6]);
            am_group_unlock(a.group_w);
            set_focus(v_first(w));
        } else if (a.group == 3) {
            am_group_lock(a.buttons[7]);
            am_group_unlock(a.group_w);
            if (m.sex == 0 && a.beard) a.beard->flags &= ~flag_visible;
            set_focus(v_first(w));
        } else if (a.group == 0) {
            am_group_lock(a.buttons[8]);
            am_group_unlock(a.group_w);
            am_avail(w);
            set_focus(v_first(w));
        } else if (a.group == 1) {
            am_group_lock(a.buttons[9]);
            am_group_unlock(a.group_w);
            am_avail(w);
            set_focus(v_first(w));
        }
    }
    if (pressed(0x8000) && !a.in_group) {  // Left: back to the groups
        send(w, &canvas_, 0x10, 0, 0);
        am_group_lock(a.group_w);
        const int k = a.group == 2 ? 6 : a.group == 3 ? 7 : a.group == 0 ? 8 : a.group == 1 ? 9 : -1;
        if (k != -1) {
            am_group_unlock(a.buttons[static_cast<std::size_t>(k)]);
            set_focus(a.buttons[static_cast<std::size_t>(k)]);
        }
        am_avail(w);
    }
    if (pressed(0x4)) {  // L1
        am_step(w, true, false);
        if (!a.in_group && a.group == 1) am_patch(w);
    }
    if (pressed(0x8)) {  // R1
        am_step(w, false, true);
        if (!a.in_group && a.group == 1) am_patch(w);
    }
    return 0;
}

// 00342cd0: the base update, then L2 / R2 (0055a44c bits 0 / 1) turn the model while no camera
// transition runs (+e0).
void GuiRuntime::am_update(Widget* w, float dt) {
    auto& a = *w->am;
    base_update(w, dt);
    const float step = fmul(pi, fdiv(dt, 30.0f));
    if (a.m.refresh_e0 == 0) {
        if (g.pad_held & 1) {
            a.m.camera.rot[2] = fadd(a.m.camera.rot[2], step);
            if (!(a.m.camera.rot[2] <= pi)) a.m.camera.rot[2] = fsub(a.m.camera.rot[2], two_pi);
        }
        if (g.pad_held & 2) {
            a.m.camera.rot[2] = fsub(a.m.camera.rot[2], step);
            if (a.m.camera.rot[2] < minus_pi) a.m.camera.rot[2] = fadd(a.m.camera.rot[2], two_pi);
        }
    }
    am_preview(w);
    // 00342768..00342798: after the draw the model steps vt +48 and 0040f3e0(dt) (vt +50 00392aa0 ->
    // 003ab510), as GuiAgentCreate's 0028e4c8: the picture shows the pose before this update's step.
    a.anim_time += dt / 30.0f;
}

// GuiCharacterDisplay's picture. The original draws the model through the display's callback
// (00341f00): first the camera approach 003428d0 (agent_camera_approach: +a0..+cc easing to the
// category's MANSORT2 record, cache +11c10, over +e0 frames; record 0 while +f0), then the record
// mode 0x15 projection and the model turned by +c4 / +c8 / +cc on (0, 0, -3) (agent_camera_view).
// Checkpoint 132cc6: the approach and the camera are the original's, stepped once per screen update;
// the draw (gui_draw.cpp, the host's character_display_draw) projects the model through a.m.camera,
// posed by ANIMIDLE.SKA (the only clip) at pose_time.
void GuiRuntime::am_preview(Widget* w) {
    auto& a = *w->am;
    if (!a.display || !(w->flags & flag_awake)) return;
    if (omega::AgentPartCache* cache = host_.agent_parts())
        omega::agent_camera_approach(*cache, a.m.camera, a.m.refresh_e0, a.m.order, a.m.pair, a.in_group != 0,
                                     0x3428d0);
    a.pose_time = preview_clock(a.anim_time);
}

// 00340c80
std::uint8_t GuiRuntime::am_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {
    auto& a = *w->am;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x10 && a3 == 1) return 1;
    if (t == 0) {
        if (!sender) unhosted(0x340ce0, "an activation without a sender");
        a.in_group = 0;
        const std::string name = cstr_of(sender->name);  // 001692d0
        am_select(w, name);
        if (name == "head") {  // D_004b9f50
            am_group_hide(a.buttons[6]);
            am_group_hide(a.buttons[8]);
            am_group_hide(a.buttons[9]);
            am_group_show(sender);
            if (a.m.sex == 0 && a.beard) a.beard->flags &= ~flag_visible;
            a.group = 3;
            a.in_group = 1;
        }
        if (name == "body") {  // D_004b9f58
            am_group_hide(a.buttons[7]);
            am_group_hide(a.buttons[8]);
            am_group_hide(a.buttons[9]);
            am_group_show(sender);
            a.group = 2;
            a.in_group = 1;
        }
        if (name == "tattoos") {  // D_004b9f60
            am_group_hide(a.buttons[7]);
            am_group_hide(a.buttons[6]);
            am_group_hide(a.buttons[9]);
            am_group_show(sender);
            a.group = 0;
            a.in_group = 1;
        }
        if (name == "patches") {  // D_004b9f68
            am_group_hide(a.buttons[7]);
            am_group_hide(a.buttons[6]);
            am_group_hide(a.buttons[8]);
            am_group_show(sender);
            a.group = 1;
            a.in_group = 1;
        }
        if (!a.in_group && a.group == 1) am_patch(w);
        am_avail(w);
    } else if (t == 0x1a && a3 == 0xc) {  // AGM_NOSAVE confirmed
        am_leave(w);
        return 1;
    }
    return save_widget_event(w, sender, type, a3, t0);  // 00441ca0
}

// 00341c50 (the category bookkeeping is agent_modify_select)
void GuiRuntime::am_select(Widget* w, const std::string& name) {
    auto& a = *w->am;
    auto& m = a.m;
    const auto old = m.category;
    omega::agent_modify_select(*host_.agent_parts(), m, name);
    if (m.category == old) return;
    am_store_table(w);
    if (m.category >= 16) {
        if (a.indicator) a.indicator->flags &= ~flag_visible;
        if (a.value_slot) set_text(a.value_slot, "", false);
        return;
    }
    if (m.pair == -1) {
        if (a.indicator) a.indicator->flags &= ~flag_visible;
        return;
    }
    if (a.indicator) a.indicator->flags |= flag_visible;
    if (a.indicator) send(w, a.indicator, 0xa, m.slider_value, 0);
    if (a.value_slot) set_text(a.value_slot, m.slot_text, false);
}

// 00341330 (the part step is agent_modify_step)
void GuiRuntime::am_step(Widget* w, bool back, bool forward) {
    auto& a = *w->am;
    auto& m = a.m;
    if (m.category >= 16 || m.count == 0) return;
    const auto changes = m.changes;
    omega::agent_modify_step(*host_.agent_parts(), m, back, forward);
    am_store_table(w);
    if (m.changes != changes) send(w, &canvas_, 0x10, 6, 0);  // 004147a0: the step sound
    if (a.indicator) send(w, a.indicator, 0xa, m.slider_value, 0);
    if (a.value_slot) set_text(a.value_slot, m.slot_text, false);
    if (a.group == 2) am_avail(w);
}

// 00340850: a tattoo / patch position is active unless its pair is blank; the group button is
// active when any of its three is. Activation is the decorator's vt +14 / +18 (00416c70 /
// 00416cc0): the widget's 0041f080 (flags |= 4) / 0041f070 (&= ~4), then the named record.
void GuiRuntime::am_avail(Widget* w) {
    auto& a = *w->am;
    const auto set = [&](Widget* b, bool on) {
        if (!b) return;
        if (on) b->flags |= flag_enabled;
        else b->flags &= ~flag_enabled;
        if (b->deco && deco_named) deco_named(b, on ? "onActivate" : "onDeactivate");
    };
    for (std::size_t base : {std::size_t{0}, std::size_t{3}}) {
        int count = 0;
        for (std::size_t i = base; i < base + 3; ++i) {
            const bool on = !omega::agent_modify_part_blank(a.m.packing, a.pairs[i]);
            set(a.buttons[i], on);
            count += on;
        }
        set(a.buttons[base == 0 ? 8 : 9], count != 0);
    }
}

// 00340320: the selected patch's picture (bitmap_preview) and name (value_name).
void GuiRuntime::am_patch(Widget* w) {
    auto& a = *w->am;
    if (a.in_group) {
        if (a.preview) {
            bm_set_image(a.preview, "", 0);  // 0043c810: release, archive 0, no name
            a.preview->flags &= ~flag_visible;
        }
        if (a.value_name) {
            set_text(a.value_name, "", false);
            a.value_name->flags &= ~flag_visible;
        }
        return;
    }
    if (a.preview) bm_set_image(a.preview, "", 0);
    const std::size_t at = static_cast<std::size_t>(0x50 + 0x20 * a.m.pair);
    if (a.m.pair < -2 || at + 16 > a.m.packing.size()) unhosted(0x3403d0, "a patch pair outside the packing");
    std::string buf;
    for (std::size_t i = 0; i < 16 && a.m.packing[at + i]; ++i) buf.push_back(static_cast<char>(a.m.packing[at + i]));
    buf.resize(16, '\0');
    std::int32_t s0 = -1, s1 = 0, s2 = 0;
    if (buf.compare(6, 3, "IPC") == 0 && buf[10] != 'B') {  // String_CompareN(buf + 6, D_004b9f28, 3)
        s0 = buf[10] - '0';
        s2 = buf[11] - '0';
        std::string name{buf[10], '_', buf[11], '_', buf[12]};
        if (buf[13] >= '0' && buf[13] <= '9') {
            name += buf[13];
            name += 's';
            s1 = (buf[12] - '0') * 10 + (buf[13] - '1');
        } else {
            name += 's';
            s1 = buf[12] - '1';
        }
        if (a.preview) bm_set_image(a.preview, name + ".TDX", host_.zeus_archive());  // D_004b9f30
    } else if (a.preview) {
        bm_set_image(a.preview, cstr_of(buf) + ".TDX", 0);
    }
    if (a.preview) a.preview->flags |= flag_visible;
    if (!a.value_name) return;
    if (s0 == -1) {
        set_text(a.value_name, "", false);
        a.value_name->flags &= ~flag_visible;
        return;
    }
    std::optional<std::string> text;
    if (s2 == 2 || s2 == 4 || (s2 == 3 && s1 >= 25)) {
        std::int32_t kind = s2;
        if (s2 == 3) {
            kind = 0;
            s1 -= 25;
        } else if (s2 == 4) {
            kind = 1;
        }
        const auto rows = host_.profile_vec(host_.current_profile(), kind);  // 003334c0
        if (s1 >= 0 && static_cast<std::size_t>(s1) < rows.size())
            text = host_.loc_text(rows[static_cast<std::size_t>(s1)].key);  // the row's +0
    } else if (s2 == 3) {
        // 003405fc: a rating patch (s1 < 25) reads 00333600's rating rows; the row's +0 is 003fdb80(+4 key).
        const auto rows = host_.profile_ratings(host_.current_profile());
        if (s1 >= 0 && static_cast<std::size_t>(s1) < rows.size())
            text = host_.loc_text(rows[static_cast<std::size_t>(s1)].key);
    } else if (s2 == 6) {
        text = host_.loc_text(s1 + host_.loc_find("RANK_01"));  // D_004b9f38
    }
    set_text(a.value_name, text ? cstr_of(*text) : std::string("missing string"), false);  // D_004b9f40
    a.value_name->flags |= flag_visible;
}

// 00341060: every GuiButtonWidget child: flags |= 4, |= 0x80.
void GuiRuntime::am_group_lock(Widget* gw) {
    if (!gw) return;
    for (Widget* c : gw->children)
        if (c != gw && c->cls == WidgetClass::button) c->flags |= 0x84;
}
// 00340f60: every GuiButtonWidget child: flags &= ~0x80.
void GuiRuntime::am_group_unlock(Widget* gw) {
    if (!gw) return;
    for (Widget* c : gw->children)
        if (c != gw && c->cls == WidgetClass::button) c->flags &= ~0x80;
}
// 00341170: every child: flags &= ~2, then its +44's sleep (vt +20).
void GuiRuntime::am_group_hide(Widget* gw) {
    if (!gw) return;
    const auto children = gw->children;
    for (Widget* c : children) {
        if (c == gw) continue;
        c->flags &= ~flag_visible;
        o_sleep(c);
    }
}
// 00341240: every child: flags |= 2, its +44's wake (vt +1c), |= 4, &= ~0x80.
void GuiRuntime::am_group_show(Widget* gw) {
    if (!gw) return;
    const auto children = gw->children;
    for (Widget* c : children) {
        if (c == gw) continue;
        c->flags |= flag_visible;
        o_wake(c);
        c->flags |= flag_enabled;
        c->flags &= ~0x80;
    }
}

}  // namespace omega::gui
