// The online account screen GuiAccountLogin (checkpoint 132on2b): vtable 004dd120, ctor 0028b2c0 over the net screen
// 0033d580 (+84 = 7). ACNTLOGN.GUI carries both the login and, with +ad set (00288de0), the sign-up; ACNTNEW.GUI is
// never loaded by the executable. research/ONLINE.md §9.
//
//   init 0028a5c0, wake 0028a4b0, sleep / update the net screen's 0033d360 / 0033d310, input 00289990, event
//   00289c80, name 00288d40, IsType 00288da0, +5c (the save flow's end) 002891d0, +64 activate 0028a470, +68
//   deactivate 0028a430; set-up 00288de0, register 00288e90, login 00289440, keyboards 0028a230 / 0028a180, and
//   the setting LOBBY_DISABLE_REGISTER 002ca070.
//
// ONLINECN's end (0029dfc0) opens it. A successful login stores the password into the agent (+8cc) and renames the
// agent to the account name (00441c20) or saves it (00441a10 mode 1) when either changed, then joins the main
// lobby's channel (00442750) and opens CMDCENTR. Every session call goes through GuiHost (lobby_login /
// lobby_register / lobby_check_account / lobby_enter / lobby_error), whose defaults are unhosted throws.
#include "gui_runtime.hpp"

#include "agent_save_flow.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

namespace omega::gui {
namespace {

[[noreturn]] void unhosted(std::uint32_t address, const std::string& what) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", address);
    throw std::runtime_error("gui runtime: unhosted " + what + " (" + buf + ")");
}

std::string cstr_of(const std::string& s) { return s.substr(0, s.find('\0')); }

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsA (no subclasses)
Widget* of_text(Widget* w) {  // vt +58 IsA "GuiTextWidget" (its button / edit subclasses answer too)
    return w && (w->cls == WidgetClass::text || w->cls == WidgetClass::button || w->cls == WidgetClass::edit) ? w
                                                                                                              : nullptr;
}

bool pressed(const PadEvent& ev, std::uint32_t mask) {  // +0 == 1, +2 == 0, +8 == mask, +6 == 0
    return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
}

constexpr std::uint32_t pad_right = 0x2000, pad_square = 0x80, pad_circle = 0x20;
constexpr std::uint32_t agent_password = 0x8cc;  // the agent record's online password (0x10 bytes)

std::string text_of(const Widget* w) { return cstr_of(w->text); }  // 001692d0(+48)

// 00129a70(dst, src, 0xf) into a 0x10-byte buffer whose first byte was cleared.
std::string strncpy15(const std::string& s) { return cstr_of(s).substr(0, 15); }

}  // namespace

bool GuiRuntime::register_enabled() {  // 002ca070
    if (!g.reg_cached) {
        const auto lower = [](std::string s) {
            for (char& c : s)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return s;
        };
        // 003cba90("LOBBY_DISABLE_REGISTER"), 00128e80(.., "1") == 0
        const bool disabled = lower(cstr_of(host_.setting("LOBBY_DISABLE_REGISTER"))) == "1";
        g.reg_enabled = disabled ? 0 : 1;
        g.reg_cached = 1;
    }
    return g.reg_enabled != 0;
}

void GuiRuntime::save_rename(Widget* w, const std::string& name) {  // 00441c20
    save_flow(w, nullptr, [&](SaveAgentFlow& f) {
        f.rename(name);
        return 0;
    });
}

void GuiRuntime::acl_init(Widget* w) {  // 0028a5c0
    auto& a = *w->acl;
    base_init(w);  // 0033d3f0: 0041f150, then +7c start_button
    a.start_button = find(w, "start_button");
    a.record = host_.current_profile();  // 001314f0(004ffb50, -1)
    a.username = find(w, "username");    // 0048b400
    a.username_txt = of_class(find(w, "username_txt"), WidgetClass::edit);  // isA "GuiTextEditWidget"
    if (a.username_txt) a.username_txt->max_len = 15;
    a.password = find(w, "password");
    a.password_txt = of_class(find(w, "password_txt"), WidgetClass::edit);
    if (a.password_txt) a.password_txt->max_len = 15;
    a.login = of_class(find(w, "login"), WidgetClass::button);  // isA "GuiButtonWidget"
    a.menutitle = of_text(find(w, "menutitle"));                // isA "GuiTextWidget"
    acl_texts(w, 0);
    a.mode = 0;
    if (!g.app_265) w->focus = a.login;
    a.wait = -1;
}

// The two text sets of 00288de0 (and their copies in 002891d0 / 00289c80): 0041beb0(w, 003fdb50(key), 0), a NULL
// string clearing the text as an empty one does.
void GuiRuntime::acl_texts(Widget* w, std::uint8_t create) {
    auto& a = *w->acl;
    if (a.menutitle)
        set_text(a.menutitle, cstr_of(host_.get_string(create ? "CreateAccount" : "LoginAccount").value_or("")),
                 false);  // 004a9708 / 004a96f8
    if (a.login)
        set_text(a.login, cstr_of(host_.get_string(create ? "Register" : "Login").value_or("")),
                 false);  // 004a9720 / 004a9718
}

void GuiRuntime::acl_setmode(Widget* w, std::uint8_t create) {  // 00288de0
    acl_texts(w, create);
    w->acl->mode = create;
}

void GuiRuntime::acl_activate(Widget* w) {  // 0028a470
    auto& a = *w->acl;
    if (a.active) return;
    policy_fetch();  // 002ca350(app)
    a.active = 1;
}

void GuiRuntime::acl_deactivate(Widget* w) {  // 0028a430
    auto& a = *w->acl;
    if (!a.active) return;
    policy_free();  // 002ca160(app)
    a.active = 0;
}

void GuiRuntime::acl_wake(Widget* w) {  // 0028a4b0
    auto& a = *w->acl;
    if (w->id10 == 1) {  // back from the region choice (ONLIPREF): the texts kept at 00506350 / 00506340
        if (a.username_txt) {
            a.username_txt->flags &= static_cast<std::uint16_t>(~flag_enabled);
            set_text(a.username_txt, g.acl_user, false);
        }
        if (a.password_txt) {
            a.password_txt->flags &= static_cast<std::uint16_t>(~flag_enabled);
            set_text(a.password_txt, g.acl_pass, false);
        }
        a.kb_ok = g.acl_kb_ok;
        w->id10 = -1;
    } else {
        if (a.username_txt) {
            a.username_txt->flags &= static_cast<std::uint16_t>(~flag_enabled);
            set_text(a.username_txt, cstr_of(host_.profile_name(a.record).value_or("")), false);  // 00336170
        }
        if (a.password_txt) {
            a.password_txt->flags &= static_cast<std::uint16_t>(~flag_enabled);
            set_text(a.password_txt, cstr_of(host_.profile_text(a.record, agent_password)), false);
        }
        a.kb_ok = 0;
    }
    // 0033d3a0: 0041efa0, vt +64, start_button +10 = -1
    base_wake(w);
    acl_activate(w);
    if (a.start_button) a.start_button->id10 = -1;
}

void GuiRuntime::acl_sleep(Widget* w) {  // 0033d360: 0041ebf0, vt +68
    base_sleep(w);
    acl_deactivate(w);
}

void GuiRuntime::acl_net_update(Widget* w, float dt) {  // 0033d310: 0041e3b0, vt +60 0033cca0, byte 00532fe8
    base_update(w, dt);
    net_poll(w, w->acl->start_button);  // 0033cca0
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
}

std::uint8_t GuiRuntime::acl_input(Widget* w, const PadEvent& ev) {  // 00289990
    auto& a = *w->acl;
    if (pressed(ev, pad_triangle)) {  // the log-out question (its yes is the event 0x1e)
        popup_open(5, std::string(), lobby_string(0xe), w, 0x1e);
        return 1;
    }
    if (pressed(ev, pad_square)) {
        if (a.mode) acl_register(w);
        else acl_login(w);
        return 1;
    }
    if (pressed(ev, pad_right)) {
        if (canvas_.focus == a.username) {  // 004147a0()->+20
            acl_keyboard_user(w);
            return 1;
        }
        if (canvas_.focus == a.password) {
            acl_keyboard_pass(w);
            return 1;
        }
    }
    if (pressed(ev, pad_circle)) {  // the region choice (ONLIPREF), the texts kept for the way back
        g.acl_user.clear();
        g.acl_pass.clear();
        g.acl_kb_ok = a.kb_ok;
        if (a.username_txt && a.password_txt) {
            g.acl_pass = strncpy15(text_of(a.password_txt));
            g.acl_user = strncpy15(text_of(a.username_txt));
        }
        g.app_268 = 7;
        net_navigate(w, 5);  // 0033c320 op 5: GuiOnlineOptions / ONLIPREF (gui_online_options.cpp)
        return 1;
    }
    // 0033cef0: +78 is 0 (nothing in this class sets it)
    if (a.b78) unhosted(0x33cf54, "net screen input");
    return 0;
}

void GuiRuntime::acl_keyboard_user(Widget* w) {  // 0028a230
    auto& a = *w->acl;
    if (a.kb_ok) {
        Widget* kb = factory("GuiKeyboard", std::string("KEYBOARD"));
        if (kb) kb_open(kb, lobby_string(0x27), text_of(a.username_txt), w, 15, 0x21, 1);  // 0032b4f0
    } else {  // the warning first (its OK is the event 0x25)
        const std::string title = lobby_string(0x29);
        const std::string message = lobby_string(0x2a);
        popup_open(2, title, message, w, 0x25);
    }
}

void GuiRuntime::acl_keyboard_pass(Widget* w) {  // 0028a180
    auto& a = *w->acl;
    Widget* kb = factory("GuiKeyboard", std::string("KEYBOARD"));
    if (kb) kb_open(kb, lobby_string(0x28), text_of(a.password_txt), w, 15, 0x22, 1);  // 0032b4f0
}

// The shared tail of a login (00289440) and of the sign-up's OK (00289c80, a3 0x23 / 0x24): the account's password
// into the agent (+8cc), then the rename (00441c20) when the name differs, else the save (00441a10 mode 1) when the
// agent is new or the password changed, else (a login only) the end 002891d0 at once. `registered` is the sign-up
// (+ac 1: the end goes back to the login mode) and clears app +265 before the save.
void GuiRuntime::acl_signed_in(Widget* w, bool registered) {
    auto& a = *w->acl;
    a.saved = registered ? 1 : 0;
    bool changed = false;
    if (text_of(a.password_txt) != cstr_of(host_.profile_text(a.record, agent_password))) {  // 00129218
        host_.profile_set_text(a.record, agent_password, text_of(a.password_txt));        // 00129360
        changed = true;
    }
    const auto name = host_.profile_name(a.record);  // 00336170
    if (!name) unhosted(registered ? 0x289f14 : 0x28955c, "strcmp with the agent's NULL name");
    if (text_of(a.username_txt) != cstr_of(*name)) {
        save_rename(w, text_of(a.username_txt));
        return;
    }
    if (g.app_265 || changed) {
        if (registered) g.app_265 = 0;
        a.save.mode = 1;
        save_start(w);  // 00441a10
        return;
    }
    if (registered) {
        acl_texts(w, 0);
        a.mode = 0;
        return;
    }
    acl_done(w);  // vt +5c (a1 1)
}

void GuiRuntime::acl_register(Widget* w) {  // 00288e90
    auto& a = *w->acl;
    if (!a.password_txt || text_of(a.password_txt).empty()) {
        const std::string title = lobby_string(0x20);
        const std::string message = lobby_string(0x8c);
        popup_open(2, title, message, nullptr, 0);
        return;
    }
    a.wait = popup_open(0, std::string(), lobby_string(0x26), nullptr, 0);
    if (!host_.net_session()) unhosted(0x288ffc, "the account registration 00445b90 with no session");
    if (host_.lobby_register(text_of(a.username_txt), text_of(a.password_txt))) {  // 00445b90
        popup_open(2, std::string(), lobby_string(0x25), w, 0x23);
    } else {
        std::int32_t a3 = -1;
        std::uint32_t code = 0x22;
        switch (host_.lobby_error() & 0xff) {  // 00452ff0(session, 0)
        case 0x3d:  // the account exists: its password signs in (the event 0x24 then logs in)
            if (host_.lobby_check_account(text_of(a.username_txt), text_of(a.password_txt))) {  // 004459d0
                code = 0x24;
                a3 = 0x24;
            } else {
                code = 0x23;
            }
            break;
        case 3: code = 0x21; break;
        default: break;
        }
        const std::string title = lobby_string(0x20);
        const std::string message = lobby_string(code, text_of(a.username_txt));
        popup_open(2, title, message, a3 == -1 ? nullptr : w, a3);
    }
    popup_close(a.wait);
}

void GuiRuntime::acl_login(Widget* w) {  // 00289440
    auto& a = *w->acl;
    if (!host_.net_session()) return;  // 004294c0
    a.wait = popup_open(0, std::string(), lobby_string(0x1b), nullptr, 0);
    if (host_.lobby_login(text_of(a.username_txt), text_of(a.password_txt))) {  // 00445a40
        acl_signed_in(w, false);  // the wait popup stays up until 002891d0
        return;
    }
    std::uint32_t title = 0x1f, message = 0x88;
    std::int32_t type = 2;
    switch (host_.lobby_error() & 0xff) {  // 00452ff0(session, 0)
    case 0x3a: title = 0x75; message = 0x81; break;  // the password is wrong
    case 0x3b:                                       // no such account: the sign-up question when it is on
        if (register_enabled()) {
            title = 0x1d;
            message = 0x1c;
            type = 4;
        } else {
            title = 0x1d;
            message = 0x8b;
        }
        break;
    case 0x3c: title = 0x1e; message = 0x80; break;  // already logged in
    default: break;
    }
    const std::string t = lobby_string(title);
    const std::string m = lobby_string(message);
    popup_open(type, t, m, w, type == 4 ? 0x1f : -1);
    popup_close(a.wait);
    g.app_264 = 0;
}

void GuiRuntime::acl_done(Widget* w) {  // 002891d0
    auto& a = *w->acl;
    if (a.saved) {  // the sign-up's save: back to the login
        acl_texts(w, 0);
        a.mode = 0;
        return;
    }
    if (!host_.net_session()) return;  // 004294c0
    policy_free();                     // 002ca160(app)
    if (!host_.lobby_enter()) {        // 00442750(session, 002cc260)
        const std::string t = lobby_string(0x1f);
        const std::string m = lobby_string(0x88);
        popup_open(2, t, m, w, 0x1e);
        popup_close(a.wait);
        g.app_264 = 0;
        return;
    }
    if (Widget* cc = factory("GuiCommandCenter", std::string("CMDCENTR"))) {
        cc->id10 = 1;
        cc->flags &= static_cast<std::uint16_t>(~flag_visible);
        push(cc, 0);  // canvas vt +64
    }
    popup_close(a.wait);
    g.app_264 = 1;
    pop(w);  // canvas vt +68
    if (Widget* cc = lookup("GuiCommandCenter")) {  // 00418980
        cc->flags |= flag_visible;
        send(cc, &canvas_, 0x10, 2, 0);
    }
}

std::uint8_t GuiRuntime::acl_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                   std::int32_t t0) {  // 00289c80
    auto& a = *w->acl;
    if (g.acl_busy) return net_event(w, sender, type, a3, t0);  // 0033cf90 (a login / registration under way)
    const std::uint32_t t = type & 0xffff;
    if (t == 2) {
        const auto lock = [&](Widget* edit, Widget* focus) {  // an edit takes the pad: the other items wait
            for (Widget* x : {a.username, a.password, a.login})
                if (x) x->flags |= flag_enabled;
            edit->flags &= static_cast<std::uint16_t>(~flag_enabled);
            set_focus(focus);  // 004147a0, 00414b30
        };
        if (sender == a.username && a.username_txt) {
            acl_keyboard_user(w);
        } else if (sender == a.password && a.password_txt) {
            acl_keyboard_pass(w);
        } else if (sender == a.login) {
            g.acl_busy = 1;
            if (a.mode) acl_register(w);
            else acl_login(w);
            g.acl_busy = 0;
        } else if (sender == a.username_txt) {
            lock(a.username_txt, a.username);
        } else if (sender == a.password_txt) {
            lock(a.password_txt, a.password);
        } else {
            return net_event(w, sender, type, a3, t0);
        }
        return 1;
    }
    if (t != 0x1a) return net_event(w, sender, type, a3, t0);
    switch (a3) {
    case 0x23:  // the sign-up's OK
    case 0x24:  // the existing account's OK
        acl_signed_in(w, true);
        return 1;
    case 0x1f:  // "create one?" yes
        acl_setmode(w, 1);
        return 1;
    case 0x1e: {  // the log-out yes / the lobby failure's OK
        net_leave(1);  // 002ca640(app, 1)
        Widget* next = factory("GuiOnlineOfflineSelect", std::string("ONOFFSEL"));
        switch_screens(w, next);
        return 1;
    }
    case 0x21:  // the user name keyboard's accept (t0 its text, NULL as "")
    case 0x22:  // the password keyboard's
    {
        Widget* edit = a3 == 0x21 ? a.username_txt : a.password_txt;
        if (edit) {
            if (!sender || !sender->kb) unhosted(0x28a0bc, "a keyboard reply from a widget not a GuiKeyboard");
            set_text(edit, cstr_of(sender->kb->result.value_or("")), false);
        }
        return 1;
    }
    case 0x25:  // the warning's OK: the keyboard from now on
        a.kb_ok = 1;
        acl_keyboard_user(w);
        return 1;
    default: return net_event(w, sender, type, a3, t0);
    }
}

}  // namespace omega::gui
