// The online connection screen GuiOnlineConnect (checkpoint 132ln2): vtable 004dd620, ctor 0029f2a0 over the net
// screen 0033d580 (ONLINECN.GUI; destroy 0029cac0, init 0029ef90, deinit 0029ef30, wake 0029ed40, sleep 0029ecd0,
// input 0029e8c0, update 0029e1a0, event 0029e440, name 0029cb50, IsType 0029cb60), the net screen's lobby
// strings 0033ce30 and the online screens' navigator 0033c320. research/FRONTEND.md "Checkpoint 132ln2".
//
// Everything that reaches the network stays an unhosted throw at the original's call site: the online start
// 002ca710 (GuiHost::net_start), and every call the original makes on a non-null DME session (004294c0,
// GuiHost::net_session, null in the port). The paths that do not reach the network are golden-verified
// (tools/ee/gen_gui_runtime.py --132ln2).
//
// PORT-ONLY (research/LAN_PLAY.md, the user's decision of 2026-10-18): with g.lan_mode set (the ONOFFSEL "LAN
// PLAY" entry, checkpoint 132ln1) the screen starts no PS2 network. Its configuration group offers HOST LAN GAME /
// JOIN LAN GAME, and selecting them takes the navigator's op 1 (the command centre in online mode, as after an
// online login) or op 2 (GuiNetJoinMission / JOINMIS). The LAN branches are oc_lan_wake, oc_lan_select and the
// Circle guard in oc_input; everything else is the original's.
#include "gui_runtime.hpp"

#include <array>
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

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsType (exact)

bool pressed(const PadEvent& ev, std::uint16_t mask) {  // +0 == 1, +2 == 0, +8 == mask, +6 == 0
    return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
}

constexpr std::uint16_t pad_circle = 0x20, pad_r1 = 0x8, pad_l1 = 0x4;

// *0048c138: 002ca0e0's MUIS server name when app +2ac is empty.
constexpr const char* muis_universe_default = "SYPHONFILTER ONLINE QA";

// 0048b990: the DNAS error table of 0029f340, 0x22 rows {s16 code, s8 message, s8 footer} (-1: none).
struct DnasRow {
    std::int16_t code;
    std::int8_t message, footer;
};
constexpr std::array<DnasRow, 0x22> dnas_rows = {{
    {101, 1, -1},  {102, 2, 0},   {103, 2, 0},   {104, 3, 0},   {105, 4, -1},  {106, 5, 0},   {107, 5, 0},
    {108, 5, 0},   {401, 6, 0},   {402, 7, 0},   {403, 7, 0},   {404, 6, 0},   {601, 8, -1},  {602, 9, -1},
    {603, 10, -1}, {610, 11, -1}, {611, 12, -1}, {612, 13, -1}, {613, 12, -1}, {614, 12, -1}, {615, 14, -1},
    {616, 11, -1}, {617, 10, -1}, {618, 11, -1}, {619, 11, -1}, {620, 11, -1}, {621, 15, -1}, {622, 15, -1},
    {623, 15, -1}, {624, 15, -1}, {625, 15, -1}, {626, 11, -1}, {701, 16, -1}, {703, 17, -1},
}};

// 00128bd0 (sprintf) of a string-table format with one int argument. The table strings only use %d; any other
// conversion would read the argument differently and is not modelled.
std::string format_int(const std::string& fmt, std::int32_t v) {
    std::string out;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out.push_back('%');
            ++i;
            continue;
        }
        if (i + 1 < fmt.size() && (fmt[i + 1] == 'd' || fmt[i + 1] == 'i')) {
            out += std::to_string(v);
            ++i;
            continue;
        }
        unhosted(0x29f490, "sprintf of a DNAS message with a conversion other than %d / %i / %%");
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------- the net screen (0033d580)

// 0033ce30(screen, id, a2, a3, t0, t1): GetString("LOBBY_STR%d", id & 0xff) through 0033d6a0 into the shared
// string 00532fe0 (allocated by the first net screen's constructor), "" (004b99d8) when the key has no string.
// 0033d6a0 copies at most 0x3ff bytes; "$1".."$4" insert the argument a2..t1 between the control bytes
// 0x11..0x14 and 0x10, a NULL argument drops the two characters. Every call in this class and the navigator
// passes NULL arguments.
std::string GuiRuntime::lobby_string(std::uint32_t id) {
    char key[32];
    std::snprintf(key, sizeof key, "LOBBY_STR%d", static_cast<int>(id & 0xff));
    const auto s = host_.get_string(key);
    if (!s) return std::string();
    const std::string in = cstr_of(*s);
    std::string out;
    for (std::size_t i = 0; i < in.size() && out.size() < 0x3ff;) {
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] >= '1' && in[i + 1] <= '4') {
            i += 2;  // the NULL argument
            continue;
        }
        out.push_back(in[i++]);
    }
    return out;
}

// 002ca640(app, a1): 002ca160 frees the policy / announcement lists, then with a DME session the leave
// (GuiHost::lobby_leave: 00445850 when logged in, 002c8090, 00453b00(session, a1), 00429500(0)).
void GuiRuntime::net_leave(std::int32_t a1) {
    policy_free();
    if (!host_.net_session()) return;
    host_.lobby_leave(a1);
}

// 002ca350(app): once (app +2ec), with a session, the three lists app +2f0 / +2f4 / +2f8 and the request
// 00445e80(session, 002cc260, +2f0, +2f4, +2f8) that the downloads 00445570 / 004456d0 / 004456c0 append to.
void GuiRuntime::policy_fetch() {
    if (g.app_2ec || !host_.net_session()) return;
    g.policy_usage.clear();
    g.policy_privacy.clear();
    g.announcements.clear();
    g.app_2ec = 1;
    host_.lobby_policy_begin();
}

// 002ca160(app): with app +2ec set, 00445dc0(session) when there is one, then the three lists are freed.
void GuiRuntime::policy_free() {
    if (!g.app_2ec) return;
    if (host_.net_session()) host_.lobby_policy_end();
    g.policy_usage.clear();
    g.policy_privacy.clear();
    g.announcements.clear();
    g.app_2ec = 0;
}

// 003fd740(string): with the character map loaded (byte 0055d450), the string's UTF-8 is decoded up to and
// including its terminator and each code point below 0x10000 the map knows (0055ca90 +5c0 page table) becomes its
// font code (the map's byte, +0x20 for code points from 0x20); the others and malformed sequences are dropped (a bad
// lead byte skips one byte, a bad continuation the bytes read so far). Returns 0 when the map is not loaded.
bool GuiRuntime::font_text(std::string& s) {
    if (!charmap.ready) return false;
    const std::string in = cstr_of(s);
    const auto n = static_cast<std::ptrdiff_t>(in.size());
    if (n == 0) return true;
    const auto at = [&](std::ptrdiff_t i) -> std::uint8_t {
        return i < n ? static_cast<std::uint8_t>(in[static_cast<std::size_t>(i)]) : 0;
    };
    std::string out;
    std::ptrdiff_t i = 0;
    while (i <= n) {
        const std::uint8_t b = at(i);
        std::uint32_t code = 0;
        std::ptrdiff_t step = 0;
        if (!(b & 0x80)) {
            code = b;
            step = 1;
        } else if (b < 0xc0 || b >= 0xfe) {
            step = -1;
        } else {
            int extra = 5;
            if ((b >> 5) == 6) extra = 1;
            else if ((b >> 4) == 0xe) extra = 2;
            else if ((b >> 3) == 0x1e) extra = 3;
            else if ((b >> 2) == 0x3e) extra = 4;
            std::uint32_t acc = b & ((1u << (6 - extra)) - 1);
            int k = 1;
            for (; k <= extra; ++k) {
                const std::uint8_t c = at(i + k);
                if (c == 0 || !(c & 0x80) || (c & 0x40)) break;
                acc = (acc << 6) + (c & 0x3f);
            }
            if (k <= extra) {
                step = -k;
            } else {
                code = acc;
                step = extra + 1;
            }
        }
        if (step > 0) {
            if (code < 0x10000) {
                const std::int8_t pg = charmap.page[(code >> 8) & 0xff];
                const std::uint8_t v = pg < 0 ? 0xff : charmap.pages[static_cast<std::size_t>(pg) * 256 + (code & 0xff)];
                if (v != 0xff) out.push_back(static_cast<char>(code < 0x20 ? v : static_cast<std::uint8_t>(v + 0x20)));
            }
            i += step;
        } else {
            i -= step;
        }
    }
    s = out;
    return true;
}

// 0033cca0(screen): the net screens' lobby poll (vt +60 of 0033d310), with a session. The first lobby message whose
// +210 is 9 (the server's notice) ends the session: unhosted here (00532dc8 / 00532dd0, 003fd2d0, app +31,
// 002ca640(app, 1), 002ca6f0). Otherwise the start button (+7c) follows the session's +20: "onActivate" (004b9b58)
// once while it is set, "onWake" (004b9b68) once after it clears, its +10 recording which.
void GuiRuntime::net_poll(Widget* w, Widget* start) {
    if (!host_.net_session()) return;
    if (const auto notice = host_.lobby_notice())
        unhosted(0x33cd08, "the server's notice (00532dc8, 003fd2d0, 002ca640, 002ca6f0): " + *notice);
    if (!start) return;
    if (host_.lobby_unread()) {
        if (start->id10 == 1) return;
        send(w, start, 6, 0x4b9b58, 0);
        start->id10 = 1;
    } else {
        if (start->id10 == 0) return;
        send(w, start, 6, 0x4b9b68, 0);
        start->id10 = 0;
    }
}

// 0033c320(screen, op): the online screens' navigator (jump table 004b9b30).
bool GuiRuntime::net_navigate(Widget* screen, std::int32_t op) {
    struct Target {
        const char* cls;
        const char* file;
    };
    static constexpr std::array<Target, 10> targets = {{{"GuiOnlineConnect", "ONLINECN"},
                                                        {"GuiCommandCenter", "CMDCENTR"},
                                                        {"GuiNetJoinMission", "JOINMIS"},
                                                        {"GuiNetAgencyCell", "AGENTCEL"},
                                                        {"GuiNetContacts", "CONTACTS"},
                                                        {"GuiOnlineOptions", "ONLIPREF"},
                                                        {"GuiMissionSetup", "MSNSETUP"},
                                                        {"GuiAccountLogin", "ACNTLOGN"},
                                                        {nullptr, nullptr},
                                                        {"GuiNetMessages", "NETMESS"}}};
    std::int32_t text = -1;  // the wait popup's LOBBY_STR id
    switch (op) {
    case 6:
    case 2: text = 0x79; break;
    case 9: text = 0x7c; break;
    case 4: text = 0x7b; break;
    case 3: text = 0x7a; break;
    case 1: text = 0x78; break;
    default: break;
    }
    if (op == 1) {
        std::int32_t w84 = 0;  // screen +84
        if (screen->oc) w84 = screen->oc->w84;
        else if (screen->cc) w84 = screen->cc->w84;
        else if (screen->ms) w84 = screen->ms->w84;
        else if (screen->lb) w84 = screen->lb->w84;  // GuiNetJoinMission: 2 (132ln4)
        else if (screen->acl) w84 = screen->acl->w84;  // GuiAccountLogin: 7 (132on2b)
        else if (screen->opt) w84 = screen->opt->w84;  // GuiOnlineOptions: 5 (132on2c)
        else if (screen->nm) w84 = screen->nm->w84;    // GuiNetMessages: 9 (132on2c)
        else unhosted(0x33c3cc, "the navigator from a screen whose +84 is not modelled");
        if (w84 == 9) text = -1;
    }
    std::int32_t popup = -1;
    if (text != -1) {
        const std::string m = lobby_string(static_cast<std::uint32_t>(text));
        popup = popup_open(0, std::string(), m, nullptr, 0);
        host_.app_2c82f0();
    }
    Widget* next = nullptr;
    if (op >= 0 && op < 10 && targets[static_cast<std::size_t>(op)].cls)
        next = factory(targets[static_cast<std::size_t>(op)].cls, std::string(targets[static_cast<std::size_t>(op)].file));
    if (text != -1) host_.app_2c81d0();
    if (next) {
        bool ok = true;
        if (op == 6 || op == 2) {
            if (g.lan_mode) {  // PORT-ONLY (132ln3, LAN_PLAY.md §4 row 5): the LAN check stands in for 00442510
                ok = lan_lobby_check();
            } else {
                // 0033c608: 00442510(session, app +248) with a session (checkpoint 132on2b); none releases the screen
                ok = host_.net_session() && host_.lobby_mission(g.app_248) != 0;
            }
        } else if (op == 3) {
            // 0033c684: 0044a230(session), the lobby's "am I an agency cell member" (checkpoint 132on2i); none or
            // not a member releases the screen (omega_server keeps no cells: always false there)
            ok = host_.net_session() && host_.lobby_cell_member();
        } else if (op == 0 || op == 7) {
            next->id10 = 1;
        }
        if (!ok) {
            if (popup != -1) popup_close(popup);
            release(next);
            return false;
        }
    }
    // vt +68 of the screen
    if (screen->cls == WidgetClass::cmdcentr) cc_deactivate(screen);       // 00297350
    else if (screen->cls == WidgetClass::msnsetup) mset_deactivate(screen);  // 002a1700
    else if (screen->cls == WidgetClass::netjoin) jm_deactivate(screen);     // 002a8fd0 (132ln4)
    else if (screen->cls == WidgetClass::netcontacts) ct_deactivate(screen);  // 002ade00 (132on2h)
    else if (screen->cls == WidgetClass::acntlogn) acl_deactivate(screen);  // 0028a430 (132on2b)
    else if (screen->cls == WidgetClass::onlinepref) opt_deactivate(screen);  // 002a9b50 (132on2c)
    else if (screen->cls == WidgetClass::netmess) nm_deactivate(screen);      // 00347690 (132on2c)
    else if (screen->cls != WidgetClass::onlinecn)                         // 0029cba0: nothing
        unhosted(0x33c6e8, "the navigator's vt +68 of this screen");
    if (next) {
        next->flags &= ~flag_visible;
        push(next, 0);  // canvas vt +64
    }
    pop(screen);  // canvas vt +68
    if (popup != -1) popup_close(popup);
    if (next) {
        next->flags |= flag_visible;
        send(next, &canvas_, 0x10, 2, 0);
    }
    return true;
}

// ---------------------------------------------------------------- GuiOnlineConnect (vtable 004dd620)

void GuiRuntime::oc_init(Widget* w) {  // 0029ef90
    auto& o = *w->oc;
    base_init(w);  // 0033d3f0: 0041f150, then +7c
    o.start_button = find(w, "start_button");
    o.groups[0] = find(w, "config_grp");    // *0048b920
    o.groups[1] = find(w, "policy_grp");    // *0048b938
    o.groups[2] = find(w, "announce_grp");  // *0048b968
    o.config_list = of_class(find(w, "config_list"), WidgetClass::textlist);
    if (o.config_list) {
        array_set_visible(o.config_list, 1, 0xa);                        // 004235b0
        o.config_list->list->cursor = find(w, "select_config_select");  // list +88
        list_set_f1(o.config_list, 1);                                   // 00421650
    }
    o.policy_title = of_class(find(w, "policy_title"), WidgetClass::text);
    o.policy_text = of_class(find(w, "policy_text"), WidgetClass::mltext);
    o.policy_accept = find(w, "policy_accept");
    o.policy_decline = find(w, "policy_decline");
    o.announce_text = of_class(find(w, "announce_text"), WidgetClass::mltext);
    o.announce_ok = find(w, "announce_ok");
    if (o.policy_text) {
        o.policy_text->ml->scroll = of_class(find(w, "vscrollbar"), WidgetClass::scrollbar);  // ML +7c
        ml_set_font(o.policy_text, "callout");
        o.policy_text->ml->max_lines = 0;  // ML +60
    }
    if (o.announce_text) {
        o.announce_text->ml->scroll = of_class(find(w, "vscrollbar2"), WidgetClass::scrollbar);
        ml_set_font(o.announce_text, "callout");
        o.announce_text->ml->max_lines = 0;
    }
    o.dnas = factory("GuiWidget", std::string("DNASLOGO"));
    if (!o.dnas) unhosted(0x29f278, "DNASLOGO did not load (the +14 store of a null screen)");
    o.dnas->flags &= ~flag_releasable;
}

void GuiRuntime::oc_deinit(Widget* w) {  // 0029ef30, after 0041f090
    auto& o = *w->oc;
    if (o.dnas) {
        unload(o.dnas, false);  // 00418e00(manager, +108, 0)
        o.dnas = nullptr;
    }
}

void GuiRuntime::oc_show(Widget* w, std::int32_t group) {  // 0029cbb0
    auto& o = *w->oc;
    for (Widget* gr : o.groups)
        if (gr) gr->flags &= ~flag_visible;
    if (group == 4) {
        o.shown = 4;
        return;
    }
    if (group < 0 || group > 2) unhosted(0x29cc28, "a group index outside 0..2");
    if (Widget* gr = o.groups[static_cast<std::size_t>(group)]) {
        gr->flags |= flag_visible;
        gr->flags |= flag_enabled;
    }
    set_focus(v_first(w));  // 00414b30(canvas, vt +44 (0041d8c0))
    if (group == 1) {
        // The policy: its title by the state, its text the chunks of app +2f0 (state 8, usage) / +2f4 (state 9,
        // privacy), joined (00209620), converted (003fd740) and set (0043dce0) after 0043db80. The lists exist only
        // after the policy download 002ca350 (app +2ec); a null list is the "no policy" popup, state 4, and +118 is
        // not stored.
        const std::vector<std::string>* list = nullptr;
        if (o.state == 8 || o.state == 9) {
            const auto title = host_.get_string(o.state == 8 ? "PolicyTitle1" : "PolicyTitle2");
            if (o.policy_title && title) set_text(o.policy_title, cstr_of(*title), false);  // 0041beb0(+f0, title, 0)
            if (g.app_2ec) list = o.state == 8 ? &g.policy_usage : &g.policy_privacy;
        }
        if (!list) {
            const std::string m = lobby_string(5);
            popup_open(2, std::string(), m, w, 0);
            o.state = 4;
            return;
        }
        if (o.policy_text) {
            ml_clear(o.policy_text);
            std::string text;
            for (const std::string& chunk : *list) text += cstr_of(chunk);
            font_text(text);
            ml_set_text(o.policy_text, text);
        }
        o.shown = group;
        return;
    }
    // group 2: the announcement (app +2f8, from 002ca350 as well): the rows with the first non-empty row's id are
    // joined; then every null, empty or matching row is removed (0044f2e0, one per pass), or, with no non-empty
    // row, the list is cleared (0044f7e0). A non-empty result is converted (003fd740) and set (0043dce0).
    if (g.app_2ec && o.announce_text) {
        auto& list = g.announcements;
        ml_clear(o.announce_text);
        std::string text;
        if (!list.empty()) {
            bool found = false;
            std::int32_t id = 0;
            for (const LobbyAnnouncement& a : list) {
                if (cstr_of(a.text).empty()) continue;
                if (!found) {
                    id = a.id;
                    found = true;
                }
                if (a.id == id) text += cstr_of(a.text);
            }
            if (found) {
                for (bool removed = true; removed;) {
                    removed = false;
                    for (std::size_t i = 0; i < list.size(); ++i)
                        if (cstr_of(list[i].text).empty() || list[i].id == id) {
                            list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                            removed = true;
                            break;
                        }
                }
            } else {
                list.clear();
            }
        }
        if (!text.empty()) {
            font_text(text);
            ml_set_text(o.announce_text, text);
        }
    }
    o.shown = group;
}

void GuiRuntime::oc_back(Widget* w, bool error) {  // 0029de70
    auto& o = *w->oc;
    Widget* next = factory("GuiOnlineOfflineSelect", std::string("ONOFFSEL"));
    switch_screens(w, next);  // 00414ac0(canvas, this, next)
    if (o.connected) {
        if (host_.net_session()) host_.lobby_disconnect();  // 0044c3c0
        o.connected = 0;
    }
    net_leave(1);  // 002ca640(app, 1)
    if (error) {
        const std::string t = lobby_string(0xb);
        const std::string m = lobby_string(0x84);
        popup_open(2, t, m, nullptr, 0);
    }
}

void GuiRuntime::oc_next(Widget* w, int depth) {  // 0029dfc0
    auto& o = *w->oc;
    // 0029cbb0(1) with no policy list (app +2f0 null) stores no +118, so an empty policy text recurses without end
    // (the EE's stack runs out); the model stops there.
    if (depth > 64) unhosted(0x29e014, "0029dfc0 recursing without end (no policy list, an empty policy text)");
    const auto lines = [&](Widget* t, std::uint32_t at) -> std::size_t {  // lw +70 (the ML text's line count)
        if (!t) unhosted(at, "a null ML text's line count (+70 read)");
        return t->ml->lines.size();
    };
    bool go = false;
    if (o.shown == 4 || o.shown == 0) {
        o.state = 8;
        oc_show(w, 1);
        if (lines(o.policy_text, 0x29e008) == 0) oc_next(w, depth + 1);
    } else if (o.shown == 1 && o.state == 8) {
        o.state = 9;
        oc_show(w, 1);
        if (lines(o.policy_text, 0x29e054) == 0) oc_next(w, depth + 1);
    } else if (o.shown == 1 || o.shown == 2) {
        oc_show(w, 2);
        if (lines(o.announce_text, 0x29e098) != 0) o.state = 0xa;
        else go = true;
    }
    if (!go) return;
    oc_show(w, 4);
    Widget* acnt = factory("GuiAccountLogin", std::string("ACNTLOGN"));
    if (!acnt) return;
    // the sign-up when the agent has no account (app +265) or no stored password (agent +8cc) and registration is on
    const bool fresh = g.app_265 || host_.profile_text(host_.current_profile(), 0x8cc).empty();
    acl_setmode(acnt, fresh && register_enabled() ? 1 : 0);  // 00288de0
    switch_screens(w, acnt);  // 00414ac0(canvas, this, acnt)
}

std::uint8_t GuiRuntime::oc_retry(Widget* w) {  // 0029d020 (no jal to it was found in the executable)
    return oc_policy(w) ? 1 : 0;
}

// The policy fetch shared by 0029d020, 0029d390's state 7 and 0029e1a0's state 7 (each inlines it): no session goes
// back with the error; otherwise the wait popup LOBBY_STR 0xd over +120, then 002ca350, the announcements
// 00445570, the usage policy 004456d0 and, when that succeeded, the privacy policy 004456c0.
bool GuiRuntime::oc_policy(Widget* w) {
    auto& o = *w->oc;
    if (!host_.net_session()) {
        oc_back(w, true);
        return false;
    }
    const std::string m = lobby_string(0xd);
    const std::int32_t p = popup_open(0, std::string(), m, nullptr, 0);
    if (o.popup_120 != -1) popup_close(o.popup_120);
    policy_fetch();
    std::vector<LobbyAnnouncement> rows;
    host_.lobby_announcements(rows);
    for (auto& r : rows) g.announcements.push_back(std::move(r));
    std::vector<std::string> chunks;
    if (host_.lobby_policy(false, chunks)) {
        for (auto& c : chunks) g.policy_usage.push_back(std::move(c));
        chunks.clear();
        host_.lobby_policy(true, chunks);
        for (auto& c : chunks) g.policy_privacy.push_back(std::move(c));
    } else {
        for (auto& c : chunks) g.policy_usage.push_back(std::move(c));
    }
    popup_close(p);
    return true;
}

// 0029d120: with a session, the configuration list 0044c440 fills +98 / +d0. Success: +94 = 1, state 5, the rows'
// names (+34) into config_list with the default row (+0 set) chosen, and the configuration group. Failure:
// 0044c3c0, +94 = 0, and by the error 00452ff0: 0x2f / 0x30 (state 3, the leave 002ca640(app, 1) and the question
// LOBBY_STR 6 with LOBBY_STR 8 / 0x83) or another (state 4, LOBBY_STR 5).
void GuiRuntime::oc_list(Widget* w) {
    auto& o = *w->oc;
    if (!host_.net_session()) return;
    o.configs.clear();  // 0044c440 allocates the lists afresh (0044f880)
    o.servers.clear();
    const std::int32_t r = host_.lobby_configs(o.configs, o.servers);
    if (r) {
        o.connected = 1;
        o.state = 5;
        if (o.config_list) list_clear(o.config_list);  // 00421070
        std::int32_t sel = -1;
        for (std::size_t i = 0; i < o.configs.size(); ++i) {
            if (o.config_list)
                list_add(o.config_list, cstr_of(o.configs[i].name), static_cast<std::int32_t>(i), 0);  // 00421290
            if (o.configs[i].selected) sel = static_cast<std::int32_t>(i);
        }
        if (sel != -1 && o.config_list) array_select(o.config_list, 0, static_cast<std::uint32_t>(sel));  // 00424430
        oc_show(w, 0);
        return;
    }
    host_.lobby_disconnect();
    o.connected = 0;
    const std::int32_t err = host_.lobby_error() & 0xff;
    if (err == 0x2f || err == 0x30) {
        o.state = 3;
        net_leave(1);
        const std::string t = lobby_string(6);
        const std::string m = lobby_string(err == 0x2f ? 8 : 0x83);
        popup_open(5, t, m, w, 0);
    } else {
        o.state = 4;
        const std::string m = lobby_string(5);
        popup_open(2, std::string(), m, w, 0);
    }
}

// 0029d390: after the connect (or the DNAS overlay's retry, a3 0x20), the MUIS step: the universe request 00442f40,
// the server named app +2ac (002ca0e0, "" names 0048c138) or any with the setting MUIS_SKIP "1", its universe to
// app +26c (002ca100), the MUIS connect 00442b50, the agent's universe (+9a0 / +95c) selected 00442840 with
// MUIS_SKIP, else the default 00442970; then ONLIPREF (op 5) for an agent with no universe (app +265) or no default,
// otherwise state 7 and the policy fetch, then 0029dfc0.
void GuiRuntime::oc_dnas_retry(Widget* w) {
    auto& o = *w->oc;
    if (!host_.net_session()) {
        popup_close(o.popup_11c);
        oc_back(w, true);
        return;
    }
    if (o.dnas) pop(o.dnas);  // manager vt +68
    {
        const std::string m = lobby_string(0xf);
        o.popup_120 = popup_open(0, std::string(), m, nullptr, 0);
    }
    popup_close(o.popup_11c);
    host_.lobby_disconnect();  // 0044c3c0
    o.connected = 0;
    g.app_2a = 1;
    if (!host_.lobby_universes_begin()) {
        popup_close(o.popup_120);
        oc_back(w, true);
        return;
    }
    const auto lower = [](std::string s) {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    };
    const auto same = [&](const std::string& a, const std::string& b) {  // 00128e80 (String_CompareNoCase) == 0
        return lower(cstr_of(a)) == lower(cstr_of(b));
    };
    const bool skip = same(host_.setting("MUIS_SKIP"), "1");  // 003cba90, 004aac08
    const std::string server_name = g.app_2ac.empty() ? std::string(muis_universe_default) : cstr_of(g.app_2ac);  // 002ca0e0
    std::int32_t pick = -1;
    for (std::size_t i = 0; i < o.servers.size(); ++i)
        if (same(o.servers[i].name, server_name) || skip) {
            g.app_26c = cstr_of(o.servers[i].universe).substr(0, 0x40);  // 002ca100: strncpy(app +26c, .., 0x40)
            pick = static_cast<std::int32_t>(i);
            break;
        }
    if (pick == -1) {
        o.state = 4;
        const std::string t = lobby_string(2);
        const std::string m = lobby_string(0x84);
        popup_open(2, t, m, w, 0);
        popup_close(o.popup_120);
        return;
    }
    std::vector<LobbyUniverse> universes;
    if (!host_.lobby_muis_connect(pick, universes)) {
        o.state = 4;
        const std::string t = lobby_string(3);
        const std::string m = lobby_string(0x84);
        popup_open(2, t, m, w, 0);
        popup_close(o.popup_120);
        host_.lobby_universes_end();
        return;
    }
    bool selected = false, no_default = false;  // s1 / s7
    if (skip) {
        const std::int32_t p = host_.current_profile();  // Agent_GetSelected(004ffb50, -1)
        const std::int32_t id = host_.profile_word(p, 0x9a0);
        const std::string region = host_.profile_text(p, 0x95c);
        for (std::size_t i = 0; i < universes.size(); ++i)
            if (universes[i].id == id && same(universes[i].name, region)) {
                selected = host_.lobby_universe_select(static_cast<std::int32_t>(i)) != 0;
                break;
            }
    }
    if (!selected && !host_.lobby_universe_default()) no_default = true;
    host_.lobby_universes_end();
    // PORT-ONLY (132on2): GuiHost::lobby_region_choice false skips ONLIPREF (ported in 132on2c) on the way in for a
    // single-universe server, where there is nothing to choose
    if ((g.app_265 || no_default) && host_.lobby_region_choice()) {
        o.state = 6;
        g.app_268 = 0;
        net_navigate(w, 5);
    } else {
        o.state = 7;
        if (oc_policy(w)) oc_next(w);
    }
    popup_close(o.popup_120);
}

GuiRuntime::DnasText GuiRuntime::oc_dnas_text(std::int32_t code) {  // 0029f340
    DnasText r;
    std::int32_t found = 0, message = 0, footer = 0;  // a3 / s1 / s0
    for (const DnasRow& row : dnas_rows)
        if (row.code == -code) {  // negu a0, code against the table's halfword
            found = row.code;
            message = row.message;
            footer = row.footer;
            break;
        }
    const auto put = [](std::string& dst, const std::string& v, std::size_t size, std::uint32_t at) {
        if (v.size() >= size) unhosted(at, "a DNAS text longer than its buffer");
        dst = v;
    };
    if (found != -1) {
        const char* key = code == -0x2bd || code == -0x2bf ? "DNASFailTitle" : "DNASErrorTitle";
        if (const auto t = host_.get_string(key)) put(r.title, cstr_of(*t), 0x80, 0x29f434);  // strcpy
    }
    if (message != -1) {
        if (message == 0 && code >= -0x2bb && code < -0x258) message = 15;
        char key[32];
        std::snprintf(key, sizeof key, "DNASMessage%d", message);
        if (const auto t = host_.get_string(key)) put(r.message, format_int(cstr_of(*t), code), 0x100, 0x29f490);
    }
    if (footer != -1) {
        char key[32];
        std::snprintf(key, sizeof key, "DNASFooter%d", footer);
        if (const auto t = host_.get_string(key)) put(r.footer, cstr_of(*t), 0x80, 0x29f4c8);
    }
    return r;
}

void GuiRuntime::oc_dnas_popup(Widget* w, std::int32_t code) {  // 0029d980
    auto& o = *w->oc;
    const DnasText t = oc_dnas_text(code);
    std::string m = t.message;
    if (!t.footer.empty()) {
        m += "\n";  // 004aac10
        m += t.footer;
    }
    popup_open(2, t.title, m, w, o.state == 4 ? 0 : 0x20);
    popup_close(o.popup_11c);
}

// 0029daf0(this, row): with a session, the connect 0044bab0(session, row, 1) under the wait popup LOBBY_STR 9 (+11c)
// with the DNAS logo pushed (cancel_grp shown, dnas_grp hidden) and byte 00587b38 set. Success goes to the DNAS
// result's popup 0029d980 when the overlay's event 0x2002 arrived (+95), else to 0029d390. Failure is state 4 and,
// by the error 00452ff0: 0x3e LOBBY_STR 0xc, 6 the DNAS result's popup, another LOBBY_STR 0xb / 0x84.
void GuiRuntime::oc_connect(Widget* w, std::int32_t row) {
    auto& o = *w->oc;
    if (!host_.net_session()) return;
    oc_show(w, 4);
    {
        const std::string m = lobby_string(9);
        o.popup_11c = popup_open(0, std::string(), m, nullptr, 0);
    }
    o.dnas_done = 0;
    if (o.dnas) {
        push(o.dnas, 0x7d0);  // manager vt +64
        if (Widget* c = find(o.dnas, "cancel_grp")) c->flags |= flag_visible;
        if (Widget* d = find(o.dnas, "dnas_grp")) d->flags &= ~flag_visible;
    }
    g.sound_gate = 1;  // byte 00587b38
    host_.app_2c9aa0();
    const std::int32_t r = host_.lobby_connect(row) & 0xff;
    g.sound_gate = 0;
    host_.app_2c9af0(0);
    if (o.dnas) pop(o.dnas);  // manager vt +68
    if (r) {
        if (o.dnas_done) oc_dnas_popup(w, host_.lobby_dnas_result());
        else oc_dnas_retry(w);
        return;
    }
    o.state = 4;
    std::string title, msg;
    const std::int32_t e = host_.lobby_error() & 0xff;
    if (e == 0x3e) {
        msg = lobby_string(0xc);
    } else if (e == 6) {
        oc_dnas_popup(w, host_.lobby_dnas_result());
    } else {
        title = lobby_string(0xb);
        msg = lobby_string(0x84);
    }
    if (!msg.empty()) popup_open(2, title, msg, w, 0);  // 002071f0
    popup_close(o.popup_11c);
}

void GuiRuntime::oc_wake(Widget* w) {  // 0029ed40
    auto& o = *w->oc;
    base_wake(w);  // 0033d3a0: 0041efa0, vt +64 (0029cb90: nothing)
    if (o.start_button) o.start_button->id10 = -1;
    g.app_2a = 0;
    host_.card_sync();  // 0040d5d0(0055a830)
    o.connected = 0;
    oc_show(w, 4);
    if (g.lan_mode) {  // PORT-ONLY (132ln2)
        oc_lan_wake(w);
    } else {
        o.state = w->id10 == 1 ? 6 : 0;  // 1: stored by the navigator's op 0
        if (o.state == 0) {
            const std::string m = lobby_string(0);
            o.save.popup = popup_open(0, std::string(), m, nullptr, 0);  // +48
            o.t8c = 15.0f;
            oc_show(w, 4);
            const std::int32_t p = host_.current_profile();
            g.app_265 = host_.profile_word(p, 0x9a0) == -1 ? 1 : 0;
        } else {
            const std::int32_t p = host_.current_profile();
            if (host_.profile_word(p, 0x9a0) == -1) {
                const std::string t = lobby_string(3);
                const std::string m = lobby_string(0x84);
                popup_open(2, t, m, w, 0);
                o.state = 4;
            } else {
                o.state = 7;
            }
        }
    }
    repeat_mask |= 0xc;  // canvas +68
}

void GuiRuntime::oc_sleep(Widget* w) {  // 0029ecd0
    auto& o = *w->oc;
    base_sleep(w);  // 0033d360: 0041ebf0, vt +68 (0029cba0: nothing)
    // 0044f6b0 on +98 / +b4 / +d0
    o.configs.clear();
    o.servers.clear();
    g.app_2a = 1;
    repeat_mask &= ~0xcu;
    popup_close(o.save.popup);
}

std::uint8_t GuiRuntime::oc_input(Widget* w, const PadEvent& ev) {  // 0029e8c0
    auto& o = *w->oc;
    if (pressed(ev, pad_triangle)) {
        if (o.state == 5 || o.state == 1) {
            oc_back(w, false);
        } else {
            const std::string m = lobby_string(0xe);
            popup_open(5, std::string(), m, w, 0x1e);  // the quit question
        }
        return 1;
    }
    if (pressed(ev, pad_circle)) {
        // PORT-ONLY (132ln2): LAN mode has no PS2 network configuration GUI to launch (LAN_PLAY.md section 4 row 2b)
        if (g.lan_mode) return 1;
        if (o.state == 5) {
            o.state = 3;
            const std::string t = lobby_string(7);
            const std::string m = lobby_string(8);
            popup_open(5, t, m, w, 0x1f);
        }
        return 1;
    }
    const auto scroll = [&](bool up) {
        const auto visible = [](const Widget* gr) { return gr && (gr->flags & flag_visible); };
        Widget* t = nullptr;
        if (visible(o.groups[1])) t = o.policy_text;
        else if (visible(o.groups[2])) t = o.announce_text;
        else return;
        if (!t) unhosted(up ? 0x29eb34 : 0x29ebe4, "the scroll of a null ML text");
        if (up) ml_up(t, false);    // 0043d820
        else ml_down(t, false);     // 0043d760
    };
    if (pressed(ev, pad_r1)) {
        scroll(true);
        return 1;
    }
    if (pressed(ev, pad_l1)) {
        scroll(false);
        return 1;
    }
    // 0033cef0: +78 is 0 (nothing in this class sets it)
    if (o.b78) unhosted(0x33cf54, "net screen input");
    return 0;
}

void GuiRuntime::oc_update(Widget* w, float dt) {  // 0029e1a0
    auto& o = *w->oc;
    base_update(w, dt);  // 0033d310: 0041e3b0, vt +60 0033cca0
    net_poll(w, o.start_button);
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
    if (g.debug_triangle) return;  // byte 0051eeb0
    if (o.state == 0) {
        host_.app_2c82f0();
        o.state = 2;
    }
    if (o.state == 2) {
        host_.app_2c9aa0();
        bool ok = host_.net_session();
        if (!ok) ok = host_.net_start() != 0;  // 002ca710
        if (!ok) {
            o.state = 4;
            const std::string t = lobby_string(1);
            const std::string m = lobby_string(4);
            popup_open(2, t, m, w, 0);
            host_.app_2c9af0(0);
            popup_close(o.save.popup);
            host_.app_2c81d0();
            return;
        }
        oc_list(w);  // 0029d120
        host_.app_2c81d0();
        popup_close(o.save.popup);
        host_.app_2c9af0(0);
    }
    if (o.state == 7) {
        if (oc_policy(w)) oc_next(w);  // 0029dfc0
    }
}

std::uint8_t GuiRuntime::oc_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 0029e440
    auto& o = *w->oc;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x2002) {
        // the DNAS overlay's result: with a session, *a3 = 1 (a3 non-null) for the results -701 / -703 (00452fe0)
        if (host_.net_session()) {
            const std::int32_t r = host_.lobby_dnas_result();
            if ((r == -0x2bd || r == -0x2bf) && a3 != 0) unhosted(0x29e4d0, "the DNAS event's result store *a3 = 1");
        }
        o.dnas_done = 1;
        return 1;
    }
    if (o.state == 5 && t == 0x13) {  // a configuration row chosen (the list's 0x13, t0 = its row)
        o.config = t0;
        oc_show(w, 4);
        if (g.lan_mode) {  // PORT-ONLY (132ln2): Host / Join instead of the connect 0029daf0
            oc_lan_select(w, o.config);
            return 1;
        }
        oc_connect(w, o.config);
        return 1;
    }
    if (t == 0x2001) {  // the DNAS logo
        if (o.dnas) {
            push(o.dnas, 0x7d0);
            if (Widget* c = find(o.dnas, "cancel_grp")) c->flags &= ~flag_visible;
            if (Widget* d = find(o.dnas, "dnas_grp")) d->flags |= flag_visible;
            const std::int32_t prev = o.popup_11c;
            const std::string m = lobby_string(0x82);
            o.popup_11c = popup_open(0, std::string(), m, nullptr, 0);
            if (prev != -1) popup_close(prev);
        }
        return 1;
    }
    if (t == 0x1b || t == 0x1a) {  // popup replies (a3 the popup's argument)
        if (a3 == 0x20) {
            oc_dnas_retry(w);
            return 1;
        }
        if (o.state == 3) {
            if (t == 0x1a)
                unhosted(0x29e664, "the network configuration GUI launch (0037e320, 001c9710, 002b4fa8, 002b4e50, "
                                   "00189540, 0044b970 NTGUI)");
            if (a3 == 0x1f) o.state = 5;
            else oc_back(w, false);
        }
        if (o.state == 4 && t == 0x1a) oc_back(w, false);
        if (a3 == 0x1e && t == 0x1a) oc_back(w, false);
        return 1;
    }
    if (t == 2) {
        if (sender == o.policy_accept || sender == o.policy_decline) {
            if (o.state == 1) {
                oc_show(w, 4);
                if (sender == o.policy_accept) {
                    oc_connect(w, o.config);
                } else {
                    const std::string m = lobby_string(0xe);
                    popup_open(5, std::string(), m, w, 0x1e);
                }
            } else if (sender == o.policy_accept) {
                oc_next(w);
            } else {
                const std::string m = lobby_string(0xe);
                popup_open(5, std::string(), m, w, 0x1e);
            }
            return 1;
        }
        if (sender == o.announce_ok) {
            oc_next(w);
            return 1;
        }
    }
    return net_event(w, sender, type, a3, t0);  // 0033cf90
}

// ---------------------------------------------------------------- PORT-ONLY: the LAN mode (checkpoint 132ln2)

// PORT-ONLY (research/LAN_PLAY.md section 4 rows 2 / 2b / 2c): the configuration group lists HOST LAN GAME /
// JOIN LAN GAME where 0029d120 lists the PS2 network configurations, in the state 0029d120 leaves (5: a row is
// awaited). No wait popup, no network start, no DNAS / policy / announcement / account login; the Circle hint
// (the network GUI launch) and the DNAS logo are hidden.
void GuiRuntime::oc_lan_wake(Widget* w) {
    auto& o = *w->oc;
    o.state = 5;
    if (Widget* t = of_class(find(w, "config_title"), WidgetClass::text)) set_text(t, port_lan_game_title, false);
    for (const char* n : {"buttoncircle", "label_launch_netgui_", "dnas_logo"})
        if (Widget* h = find(w, n)) h->flags &= ~flag_visible;
    if (o.config_list) {  // as 0029d120: 00421070, then 00421290(list, name, row, 0) per row
        list_clear(o.config_list);
        list_add(o.config_list, std::string(port_lan_host_label), 0, 0);
        list_add(o.config_list, std::string(port_lan_join_label), 1, 0);
    }
    oc_show(w, 0);
}

// PORT-ONLY (checkpoint 132ln3, research/LAN_PLAY.md §4 row 5): the LAN stand-in for the navigator's lobby check
// 00442510(lobby, app +248). The original fails for a mission index outside 0 .. lobby +58 (the lobby's mission
// channels) and otherwise joins that mission's Medius channel. LAN has no channels: the index must name one of
// the nine online missions (table 004acb30, the command centre's 004acb60 values 0..8), and no LAN game may be
// hosted already.
bool GuiRuntime::lan_lobby_check() {
    constexpr std::int32_t online_missions = 9;  // 004acb30: 0, 1, 2, 4, 5, 8, 12, 13, 16
    return g.app_248 >= 0 && g.app_248 < online_missions && !host_.lan_hosting();
}

// PORT-ONLY: Host goes to the command centre in online mode exactly as after an online login (the navigator's
// op 1; app +30 = 1 from the LAN entry). Join (checkpoint 132ln4, the coordinator's decision) takes the same op 1,
// with the mission panel's focus on join_mission (g.lan_join_focus, cc_lan_activate): the original reaches
// JOINMIS only from the command centre's join_mission (00295ea0, op 2), whose lobby check needs the chosen mission
// in app +248, and the LAN check keeps that test. The LAN lobby (LanSession) is not here: nothing stands in for a
// lobby result.
void GuiRuntime::oc_lan_select(Widget* w, std::int32_t row) {
    if (row != 0 && row != 1) return;
    g.lan_join_focus = row == 1 ? 1 : 0;
    net_navigate(w, 1);
}

}  // namespace omega::gui
