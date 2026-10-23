// The lobby's message box GuiNetMessages (checkpoint 132on2c): vtable 004deb10, ctor 00347a40 over the net screen
// 0033d580 (+84 = 9), and its list GuiNetMessageArray (vtable 004deb80, ctor 00348fc0 over GuiArrayWidget).
// NETMESS.GUI. research/ONLINE.md §9.8.
//
//   GuiNetMessages: init 00347820, wake 003477d0, sleep / update the net screen's 0033d360 / 0033d310, input 003473e0,
//   event 00346d60, +64 activate 003476b0, +68 deactivate 00347690; the message display 00346fb0.
//   GuiNetMessageArray: init 00348d90, wake 00348b90, sleep 00348ad0, update 00348a00; input / gain / lose / draw
//   GuiArrayWidget's (00423cc0 / 00424a10 / 004249d0 / 00423670), its row draw 00347b40 and cell draw 00348530.
//
// The navigator's op 9 opens it (START on a net screen whose +78 is set, 0033cef0: CMDCENTR, JOINMIS and itself);
// Triangle goes back through the navigator's op 00532fd0 (the +84 of the screen START came from), else CMDCENTR.
// The messages are the session's list (session +1c, GuiHost::lobby_message_count / lobby_message /
// lobby_messages_changed); the list's selection change (event 0x14) shows one, its buttons answer it (00450f30).
#include "gui_runtime.hpp"

#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
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

// cvt.w.s as the EE interpreter models it (gui_runtime.cpp's): denormals flush to zero, saturating.
std::int32_t cvt_w(float x, std::uint32_t address) {
    if (x != 0.0f && std::fabs(x) < FLT_MIN) x = std::copysign(0.0f, x);
    if (std::isnan(x)) throw std::runtime_error("cvt.w.s of NaN at " + std::to_string(address));
    if (x >= 2147483647.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    return static_cast<std::int32_t>(x);
}

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsA (no subclasses)
Widget* of_text(Widget* w) {  // vt +58 IsA "GuiTextWidget" (its button / edit subclasses answer too)
    return w && (w->cls == WidgetClass::text || w->cls == WidgetClass::button || w->cls == WidgetClass::edit) ? w
                                                                                                              : nullptr;
}

bool pressed(const PadEvent& ev, std::uint32_t mask) {  // +0 == 1, +2 == 0, +8 == mask, +6 == 0
    return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
}

constexpr std::uint32_t pad_right = 0x2000, pad_left = 0x8000, pad_start_mask = 0x800;
constexpr std::int32_t name_on_wake = 0x4ba378;  // "onWake"
constexpr std::array<std::uint32_t, 4> column_widths = {40, 200, 10, 10};  // 004ba290

bool visible(const Widget* w) { return (w->flags & flag_visible) != 0; }
void show(Widget* w, bool on) {
    if (on) w->flags |= flag_visible;
    else w->flags &= static_cast<std::uint16_t>(~flag_visible);
}

}  // namespace

// ---------------------------------------------------------------- GuiNetMessages (vtable 004deb10)

void GuiRuntime::nm_init(Widget* w) {  // 00347820
    auto& n = *w->nm;
    base_init(w);  // 0033d3f0: 0041f150, then +7c start_button
    n.start_button = find(w, "start_button");
    n.box = of_class(find(w, "messages_box"), WidgetClass::netmsgarray);  // *00492b60
    n.button1 = of_class(find(w, "button1"), WidgetClass::button);       // isA "GuiButtonWidget"
    n.button2 = of_class(find(w, "button2"), WidgetClass::button);
    n.button3 = of_class(find(w, "button3"), WidgetClass::button);
    n.info = of_text(find(w, "messageinfo"));
    n.nomessage = find(w, "nomessage");  // *00492b98 (any class)
    // 00418420(manager, "default"): its +10 / +20 colours from 00587b50 / 00587b60
    if (const std::uint32_t font = host_.font("default"))
        host_.font_colors(font, {1.0f, 1.0f, 1.0f, 1.0f}, {0.8f, 1.0f, 0.8f, 1.0f});
}

void GuiRuntime::nm_wake(Widget* w) {  // 003477d0
    auto& n = *w->nm;
    if (!n.button1 || !n.button2 || !n.button3 || !n.nomessage)
        unhosted(0x3477d4, "GuiNetMessages' wake on a missing button1 / button2 / button3 / nomessage");
    show(n.button1, false);
    show(n.button2, false);
    show(n.button3, false);
    show(n.nomessage, true);
    // 0033d3a0: 0041efa0, vt +64, start_button +10 = -1
    base_wake(w);
    nm_activate(w);
    if (n.start_button) n.start_button->id10 = -1;
}

void GuiRuntime::nm_sleep(Widget* w) {  // 0033d360: 0041ebf0, vt +68
    base_sleep(w);
    nm_deactivate(w);
}

void GuiRuntime::nm_net_update(Widget* w, float dt) {  // 0033d310: 0041e3b0, vt +60 0033cca0, byte 00532fe8
    base_update(w, dt);
    net_poll(w, w->nm->start_button);  // 0033cca0
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
}

void GuiRuntime::nm_activate(Widget* w) {  // 003476b0
    auto& n = *w->nm;
    if (n.active) return;
    if (Widget* t = of_text(find(w, "value_agent")))  // *00492b88
        set_text(t, cstr_of(host_.profile_name(host_.current_profile()).value_or("")), false);  // 00336170
    if (Widget* t = of_text(find(w, "value_rank")))  // *00492b90
        set_text(t, cstr_of(host_.profile_rank(host_.current_profile()).value_or("")), false);  // 00336110
    n.b78 = 1;
    n.active = 1;
}

void GuiRuntime::nm_deactivate(Widget* w) {  // 00347690
    w->nm->active = 0;
}

// 00346fb0(this, message): the message's buttons (an invitation's Decline / Accept, else OK), its text into
// "messageinfo", "nomessage" shown when there is none; each button newly shown gets "onWake", then the focus goes to
// the screen's first item.
void GuiRuntime::nm_show(Widget* w, const std::optional<GuiHost::LobbyMessage>& msg) {
    auto& n = *w->nm;
    if (!n.button1 || !n.button2 || !n.button3) return;
    if (!host_.net_session()) return;  // 004294c0
    const bool was1 = visible(n.button1), was2 = visible(n.button2), was3 = visible(n.button3);
    const auto text = [&](const char* key) { return cstr_of(host_.get_string(key).value_or("")); };
    if (!n.info || !n.nomessage) unhosted(0x347164, "GuiNetMessages' message on a missing messageinfo / nomessage");
    if (msg) {
        if (msg->type == 0 || msg->type == 1) {
            set_text(n.button1, text("MB_Decline"), false);  // 004ba340
            set_text(n.button2, text("MB_Accept"), false);   // 004ba350
            show(n.button1, true);
            show(n.button2, true);
            show(n.button3, false);
        } else {
            set_text(n.button3, text("MB_OK"), false);  // 004ba360
            show(n.button1, false);
            show(n.button2, false);
            show(n.button3, true);
        }
        std::string info;
        if (msg->type == 9) {  // 00128bd0(buf, "[%c%s%c] - %s", 0x11, +c, 0x10, +2c)
            info = "[";
            info.push_back(0x11);
            info += cstr_of(msg->from);
            info.push_back(0x10);
            info += "] - ";
            info += cstr_of(msg->text);
        } else if (msg->type == 10) {  // 0013b480(tmp, +2c), 003fd740
            info = cstr_of(msg->text);
            font_text(info);
        } else {
            std::int32_t id = -1;  // jump table 004ba380
            switch (msg->type) {
            case 0: id = 0x2d; break;
            case 1: id = 0x2e; break;
            case 2: id = 0x2f; break;
            case 3: id = 0x30; break;
            case 4: id = 0x31; break;
            case 5: id = 0x32; break;
            case 6: id = 0x7f; break;
            case 7: id = 0x33; break;
            case 8: id = 0x34; break;
            case 11: id = 0x87; break;
            default: break;
            }
            if (id != -1) info = lobby_string(static_cast<std::uint32_t>(id), msg->from, msg->text);  // 0033ce30
        }
        set_text(n.info, info, false);
        show(n.nomessage, false);
    } else {
        show(n.button1, false);
        show(n.button2, false);
        show(n.button3, false);
        set_text(n.info, std::string(), false);  // 004ba338
        show(n.nomessage, true);
    }
    if (visible(n.button1) && !was1) send(w, n.button1, 6, name_on_wake, 0);  // 0041e0f0
    if (visible(n.button2) && !was2) send(w, n.button2, 6, name_on_wake, 0);
    if (visible(n.button3) && !was3) send(w, n.button3, 6, name_on_wake, 0);
    set_focus(nullptr);    // 00414b30(manager, NULL)
    set_focus(v_first(w));  // vt +44
}

// 003473e0. Right / Left become Up / Down in the caller's event (the canvas' focus walk reads them after the screen
// declines); START (the net screen's own op 9) is swallowed.
std::uint8_t GuiRuntime::nm_input(Widget* w, PadEvent& ev) {
    auto& n = *w->nm;
    if (pressed(ev, pad_triangle)) {
        if (!net_navigate(w, g.b_2fd0)) net_navigate(w, 1);  // 0033c320 (00532fd0, else CMDCENTR)
        return 1;
    }
    if (pressed(ev, pad_up) || pressed(ev, pad_down)) {
        if (n.box) {  // the box's +44 vt +2c
            if (!n.box->deco) unhosted(0x3474e0, "the message box's input through a NULL +44");
            return v_input(n.box, ev);
        }
    }
    if (pressed(ev, pad_right)) ev.mask = pad_up;
    else if (pressed(ev, pad_left)) ev.mask = pad_down;
    if (pressed(ev, pad_start_mask)) return 1;
    // 0033cef0: +78 is set, but START (its only press) never reaches it
    return 0;
}

std::uint8_t GuiRuntime::nm_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 00346d60
    auto& n = *w->nm;
    const std::uint32_t t = type & 0xffff;
    if (a3 == 0x19 && t == 0x1a) {  // the log-out question's yes
        Widget* next = factory("GuiOnlineOfflineSelect", std::string("ONOFFSEL"));
        if (host_.net_session()) {
            const std::string m = lobby_string(0x77);
            const std::int32_t p = popup_open(0, std::string(), m, nullptr, 0);
            net_leave(1);  // 002ca640(app, 1)
            popup_close(p);
        }
        popup_close(n.popup);
        switch_screens(w, next);  // 00414ac0
        return 1;
    }
    if (t == 0x14) {  // the box's selection: t0 the row
        if (host_.net_session()) {
            n.shown = t0;
            nm_show(w, host_.lobby_message(n.shown));  // 00451010
        }
        return 1;
    }
    if (t == 2 && host_.net_session() && host_.lobby_message(n.shown)) {
        if (sender == n.button1 || sender == n.button3) {
            // Decline / OK: 00450f30(session, +98, button2 hidden)
            host_.lobby_message_answer(n.shown, (n.button2 && visible(n.button2)) ? 0 : 1);
            host_.lobby_messages_set_changed(true);  // 0044f280(session +1c, 1)
            return 1;
        }
        if (sender == n.button2) {
            host_.lobby_message_answer(n.shown, 1);
            host_.lobby_messages_set_changed(true);
            return 1;
        }
    }
    return net_event(w, sender, type, a3, t0);  // 0033cf90
}

// ---------------------------------------------------------------- GuiNetMessageArray (vtable 004deb80)

void GuiRuntime::nma_init(Widget* w) {  // 00348d90
    auto& m = *w->nma;
    array_init(w);  // 00424bc0
    m.header = of_class(find(w, "column_header"), WidgetClass::textarray);  // *00492b40
    m.icons[0] = find(w, "icon_notification");  // isA "GuiWidget"
    m.icons[1] = find(w, "icon_chat");
    m.icons[2] = find(w, "icon_question");
    array_set_dims(w, 4, 0);  // vt +5c
    for (std::uint32_t i = 0; i < 4; ++i) array_set_col_width(w, i, column_widths[i]);  // 004248c0
    if (!m.header) return;
    ta_set_dims(m.header, 4, 1);
    char key[32];
    for (std::uint32_t i = 0; i < 4; ++i) {
        array_set_col_width(m.header, i, column_widths[i]);
        if (i == 2) continue;
        std::snprintf(key, sizeof key, "MessagesTitle%d", static_cast<int>(i == 3 ? 2 : i));  // 004ba3f0
        if (const auto s = host_.get_string(key)) ta_set_cell(m.header, i, 0, cstr_of(*s));  // 00426510
    }
}

// The tail shared by the wake 00348b90 and the update 00348a00: the rows from the list's count (vt +5c), the
// selection's row clamped (00424410), then re-selected with +54 forced to 100000 so the box reports it (0x15, 0x14).
void GuiRuntime::nma_reselect(Widget* w) {
    auto& l = *w->list;
    if (w->nma->source) array_set_dims(w, 4, static_cast<std::uint32_t>(host_.lobby_message_count()));
    const std::uint32_t col = l.col;
    std::uint32_t row = l.row;
    if (row >= l.rows) row = l.rows ? l.rows - 1 : 0;
    l.row = 100000;
    array_select(w, col, row);  // 00424430
}

void GuiRuntime::nma_wake(Widget* w) {  // 00348b90
    auto& m = *w->nma;
    array_wake(w);  // 00424a90
    TextObject o;   // 003d0940(o, 0, 0, w, h, font "callout", 00587c20)
    o.rect = {0, 0, cvt_w(w->rect[2], 0x348c30), cvt_w(w->rect[3], 0x348c34)};
    o.font = host_.font("callout");
    o.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    host_.text_object(o);
    m.obj = o;
    m.obj->b12 = 1;
    m.obj->b10 = 2;
    m.source = host_.net_session();  // session +1c (004294c0)
    w->list->row = 0;
    nma_reselect(w);
    if (m.header && m.header->ta->obj) {  // the header's +90 text object (from its wake)
        m.header->ta->obj->b11 = 1;
        m.header->ta->obj->b58 = 1;
    }
}

void GuiRuntime::nma_sleep(Widget* w) {  // 00348ad0
    base_sleep(w);  // 00424a50: 0041ebf0, then vt +70
    send_up(w, 0x15, static_cast<std::int32_t>(w->list->col), static_cast<std::int32_t>(w->list->row));
    w->nma->obj.reset();  // the string object +14, then 0036b130
    w->nma->source = false;
}

void GuiRuntime::nma_update(Widget* w, float dt) {  // 00348a00
    base_update(w, dt);  // 0041e3b0
    if (!w->nma->source || !host_.lobby_messages_changed()) return;  // list +18
    nma_reselect(w);
    host_.lobby_messages_set_changed(false);  // 0044f280(list, 0)
}

// Checkpoint 132on2h: 00348530's choices, drawn by GuiDrawer::draw_net_message_array.
std::optional<GuiRuntime::NetMessageCell> GuiRuntime::nma_cell(const Widget& w, std::uint32_t col,
                                                               std::uint32_t row) const {
    const auto& m = *w.nma;
    if (!m.obj || !m.source) return std::nullopt;  // +a0, +a4
    if (row >= static_cast<std::uint32_t>(host_.lobby_message_count()) || col == 2) return std::nullopt;  // +a4 +4
    const auto msg = host_.lobby_message(static_cast<std::int32_t>(row));  // 0044f290
    if (!msg) return std::nullopt;
    NetMessageCell c;
    if (col == 0) {  // 003485b4: an invitation (0, 1) +9c, a chat (9) +98, else +94
        c.icon = (msg->type == 0 || msg->type == 1) ? m.icons[2] : msg->type == 9 ? m.icons[1] : m.icons[0];
        return c;
    }
    if (col == 1) {  // 0034876c: the sender (+c), or for type 10 the system name +a8; centred (+10 2), +5b 1
        c.text = msg->type == 10 ? m.system : cstr_of(msg->from);
        c.halign = 2;
        c.b5b = 1;
    } else if (col == 3 && msg->type >= 0 && msg->type < 12) {  // 003487d4: GetString("NetMessage%d") or 004ba338
        char key[32];
        std::snprintf(key, sizeof key, "NetMessage%d", static_cast<int>(msg->type));  // 004ba3b8
        if (const auto s = host_.get_string(key)) c.text = cstr_of(*s);
    }
    // 00348840: 003d0810(+a0, ...): an invitation 00587c10 (white), a chat 00587bf0, else 00587c00
    if (msg->type == 0 || msg->type == 1) c.rgba = {1.0F, 1.0F, 1.0F, 1.0F};
    else if (msg->type == 9) c.rgba = {0.0F, std::bit_cast<float>(0x3f22a2a3U), 1.0F, 1.0F};
    else c.rgba = {std::bit_cast<float>(0x3e74f4f5U), std::bit_cast<float>(0x3f46c6c7U), std::bit_cast<float>(0x3ee8e8e9U), 1.0F};
    return c;
}

}  // namespace omega::gui
