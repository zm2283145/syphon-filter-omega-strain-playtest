// The online join screen (checkpoint 132ln4): GuiNetJoinMission (vtable 004dda50, ctor 002a9340; JOINMIS.GUI) over
// GuiLobbyScreen (vtable 004de8a0, ctor 0033f480) over the net screen 0033d580, and the list widget GuiMissionList
// (vtable 004ddb80, ctor 002acea0) over GuiArrayWidget. research/FRONTEND.md "Checkpoint 132ln4".
//
//   GuiNetJoinMission  init 002a9240, event 002a8d60, +64 activate 002a9030, +68 deactivate 002a8fd0, +78 002a8b50
//   GuiLobbyScreen     init 0033f250, wake 0033f110, sleep 0033f050, input 0033da00, update 0033de00, event
//                      0033df00, +6c chat 0033ee40, +70 agent info 0033e410, +74 join 0033e5b0; the helpers
//                      0033d800 / 0033d8c0 / 0033d8e0 / 0033e2a0 / 0033ef10 / 0033f0c0
//   GuiMissionList     init 002accf0, wake 002abd50, sleep 002abc90, update 002acca0, +6c 002ac300, the cell /
//                      row draws +68 002aad40 / +64 002ab2a0 (gui_draw.cpp), 002abf00 / 002abf40 / 002ac070 /
//                      002ac400 / 002ac470 / 002ac6f0 (the sort keys 002ac7e0 / 002ac900 / 002ac980 / 002aca00) /
//                      002acb70 / 002acc80
//
// The lists the screen shows (+ac / +c8 / +e4, SFOLobby lists 0044fa00) are filled by the lobby (SFOLobby_Lobby,
// registered by the activate's 004518f0) on a DME session. With no session (004294c0 null, the port's Online) every
// session call is an unhosted throw at its call site and the lists stay empty, as in the original without a
// network: GuiMissionList's data paths (002ac070, 002ac470) return at once.
//
// PORT-ONLY (research/LAN_PLAY.md §4 rows 7 / 7b, with g.lan_mode set by the LAN PLAY entry):
//   - region_name reads "LAN" (port_lan_region) instead of the agent's online region.
//   - The lobby screen's update fills +c8 and +e4 with one game record per discovered LAN game
//     (GuiHost::lan_games, a LanDiscoveryBrowser started at wake and stopped at sleep): lb_lan_refresh.
//   - GuiMissionList's data paths treat the LAN session as the session (lobby_data): the rows are rebuilt and
//     002ac470's join test runs; the friends / agency-cell filters (0044d670 / 0044a230) answer "no" (LAN has no
//     buddies or clans). A game of another build is listed greyed (not selectable) in every mode, its mission column
//     reading "VERSION MISMATCH" (port_lan_version_mismatch).
//   - The join 0033e5b0 keeps the original's popups, checks and wait loop: 0044e060 is the browse entry, the
//     network start (001698c0 / 00165bc0) is skipped, 0044e430 is GuiHost::lan_join (LanTransport::connect, JOIN_REQ),
//     the loop's 004536b0 / 00429d30 pump is GuiHost::lan_pump and its clock the LAN session's, and the lobby's
//     callback is the event 0x1e the LAN session's answer becomes: JOIN_ACK (a3 1), JOIN_NAK (a3 0, F_TO_JOINGAME)
//     or no answer (a3 0, F_TO_CONNECT, the DME connect failure's key). The cleanup 0042b1f0 / 0044e340 is
//     GuiHost::lan_join_cancel.
//   - START (0033cef0: the navigator's op 9, NETMESS) does nothing: LAN has no messages (§4 row 4b).
//
// Checkpoint 132on2b (research/ONLINE.md §9.5): with the online session (004294c0, GuiHost::net_session over
// omega_server) the activate opens the game room (004426b0 / 004518f0), the list switches request their kind
// (004515f0), the deactivate leaves the room (00451610), GuiMissionList's own id is the account's (session +2480)
// and the join 0033e5b0 runs its session branch (lb_join_session: 0044e060, the network start, 0044e430 and the
// 004536b0 / 00429d30 wait, 00452ff0's error texts). PORT-ONLY (132on2b):
//   - The game list callback 00447790 runs over GuiHost::lobby_game_list at the update's start
//     (lb_session_refresh), and (132on2c) the player list callback 00447a30 over GuiHost::lobby_player_list and the
//     chat callbacks 00447bb0 / 00447c70 over GuiHost::lobby_chat_take; the chat prompt 0033ee40 and its send
//     00451050 (GuiHost::lobby_chat_send) and the chat display 002aec90 are the original's.
//   - 0044fa40 (the paged game list), the friends check 0044d670 and the agency cell check 0044a0d0 answer 0.
//   - The another-host prompt (0033e870) stays unhosted: JOINMIS never sets the list's +b5.
//   - PORT-ONLY (132on2c, research/ONLINE.md §5.3 GamePassword): a join omega_server refuses for its game password
//     opens a keyboard for the password and joins again with it (lb_join_session).
#include "gui_runtime.hpp"

#include <algorithm>
#include <bit>
#include <array>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsType (exact)

bool pressed(const PadEvent& ev, std::uint16_t mask) {  // +0 == 1, +2 == 0, +8 == mask, +6 == 0
    return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
}

constexpr std::uint16_t pad_left = 0x8000, pad_right = 0x2000, pad_l2 = 0x1, pad_r2 = 0x2, pad_select = 0x100,
                        pad_start = 0x800;

constexpr std::int32_t a3_on_pressed = 0x4b9c68;  // "onPressed" (0033e2a0's event 6)

constexpr std::int32_t key_f_to_connect = lobby_key_f_to_connect;
constexpr std::int32_t key_f_to_joingame = lobby_key_f_to_joingame;
std::string callback_key(std::int32_t t0) { return lobby_callback_key(t0); }

// 004acb30: the online missions' level indices, as far as 002ac470 reads it (+40 < 0x11; entries 9.. are the next
// table's words). 004acb30[15] is -1, so 0048c160[-1] would be read: not modelled.
constexpr std::array<std::int32_t, 17> mission_levels = {0, 1, 2, 4, 5, 8, 12, 13, 16, 0, 0, 0, 0, 1, 2, -1, 3};

// 003fda40: the 16-byte lower-case key 00332fe0 takes.
std::string level_key(const char* name) {
    std::string k = name;
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return k.substr(0, 16);
}

// 0048bc60: per column layout (+ac), the column count and the column types; 0048bc40: per column type the width.
struct ColumnLayout {
    std::int32_t count;
    std::array<std::int32_t, 3> type;
};
constexpr std::array<ColumnLayout, 5> column_layouts = {{{3, {3, 4, 5}},
                                                          {3, {0, 1, 2}},
                                                          {1, {0, -1, -1}},
                                                          {3, {0, 1, 5}},
                                                          {0, {-1, -1, -1}}}};
constexpr std::array<std::int32_t, 6> column_widths = {224, 140, 60, 224, 80, 70};

// 001273c8: the executable's qsort (BSD form), as character_shadows.cpp ports it, over the rows.
template <class Row, class Cmp> void game_qsort(std::vector<Row>& v, std::ptrdiff_t a, std::ptrdiff_t n, Cmp compare) {
    using Index = std::ptrdiff_t;
    auto at = [&](Index i) -> Row& { return v[static_cast<std::size_t>(i)]; };
    auto cmp = [&](Index i, Index j) { return compare(at(i), at(j)); };
    auto swp = [&](Index i, Index j) { std::swap(at(i), at(j)); };
    auto vecswap = [&](Index i, Index j, Index count) {
        for (Index k = 0; k < count; ++k) swp(i + k, j + k);
    };
    auto insertion = [&](Index b, Index m) {
        for (Index pm = b + 1; pm < b + m; ++pm)
            for (Index pl = pm; pl > b && cmp(pl - 1, pl) > 0; --pl) swp(pl, pl - 1);
    };
    auto med3 = [&](Index x, Index y, Index z) {
        return cmp(x, y) < 0 ? (cmp(y, z) < 0 ? y : (cmp(x, z) < 0 ? z : x))
                             : (cmp(y, z) > 0 ? y : (cmp(x, z) < 0 ? x : z));
    };
    for (;;) {
        bool swapped = false;
        if (n < 7) {
            insertion(a, n);
            return;
        }
        Index pm = a + n / 2;
        if (n > 7) {
            Index pl = a, pn = a + n - 1;
            if (n > 40) {
                const Index d = n / 8;
                pl = med3(pl, pl + d, pl + 2 * d);
                pm = med3(pm - d, pm, pm + d);
                pn = med3(pn - 2 * d, pn - d, pn);
            }
            pm = med3(pl, pm, pn);
        }
        swp(a, pm);
        Index pa = a + 1, pb = a + 1, pc = a + n - 1, pd = a + n - 1;
        for (;;) {
            int r = 0;
            while (pb <= pc && (r = cmp(pb, a)) <= 0) {
                if (r == 0) {
                    swapped = true;
                    swp(pa, pb);
                    ++pa;
                }
                ++pb;
            }
            while (pb <= pc && (r = cmp(pc, a)) >= 0) {
                if (r == 0) {
                    swapped = true;
                    swp(pc, pd);
                    --pd;
                }
                --pc;
            }
            if (pb > pc) break;
            swp(pb, pc);
            swapped = true;
            ++pb;
            --pc;
        }
        if (!swapped) {
            insertion(a, n);
            return;
        }
        const Index pn = a + n;
        Index r = std::min(pa - a, pb - pa);
        vecswap(a, pb - r, r);
        r = std::min(pd - pc, pn - pd - 1);
        vecswap(pb, pn - r, r);
        if ((r = pb - pa) > 1) game_qsort(v, a, r, compare);
        if ((r = pd - pc) > 1) {
            a = pn - r;
            n = r;
            continue;
        }
        return;
    }
}

// 00129218 (strcmp) over the records' +4 names.
int name_cmp(const LobbyRecord& a, const LobbyRecord& b) {
    const std::string x = cstr_of(a.name), y = cstr_of(b.name);
    const std::size_t n = std::min(x.size(), y.size());
    for (std::size_t i = 0; i < n; ++i)
        if (x[i] != y[i]) return static_cast<int>(static_cast<unsigned char>(x[i])) - static_cast<unsigned char>(y[i]);
    if (x.size() == y.size()) return 0;
    return x.size() < y.size() ? -static_cast<int>(static_cast<unsigned char>(y[n]))
                               : static_cast<int>(static_cast<unsigned char>(x[n]));
}

// PORT-ONLY (132on2c): the a3 of the game password keyboard lb_join_session opens on a refused join (no a3 the
// original's lobby screens send).
constexpr std::int32_t port_password_arg = 0x13a0;

// PORT-ONLY (132ln4): a LAN game's record id (the record's +0 and its +48 world id): the session id's low word,
// never 0 (002ac070 renumbers an id-0 record only when its name is empty).
std::int32_t lan_record_id(std::uint64_t session) {
    auto v = static_cast<std::int32_t>(static_cast<std::uint32_t>(session));
    if (v == 0) v = static_cast<std::int32_t>(static_cast<std::uint32_t>(session >> 32) | 1U);
    return v;
}
}  // namespace

std::string lobby_callback_key(std::int32_t t0) {
    switch (t0) {
    case lobby_key_f_to_connect: return "F_TO_CONNECT";
    case lobby_key_f_to_joingame: return "F_TO_JOINGAME";
    case lobby_key_f_to_creategame: return "F_TO_CREATEGAME";
    default: break;
    }
    unhosted(0x33ec18, "the lobby callback's error key " + std::to_string(t0));
}

namespace {

}  // namespace

// 0033ce30(screen, id, a1, NULL, NULL, NULL): as lobby_string, with "$1" replaced by 0x11, the argument and 0x10
// (0033d6a0); "$2".."$4" (NULL) drop their two characters.
std::string GuiRuntime::lobby_string(std::uint32_t id, const std::optional<std::string>& a1) {
    if (!a1) return lobby_string(id);
    return lobby_string(id, a1, std::nullopt);
}

// 0033ce30(screen, id, a1, a2, NULL, NULL): "$n" becomes 0x10 + n, the argument and 0x10; a NULL argument drops the
// two characters (checkpoint 132on2c: GuiNetMessages' message texts).
std::string GuiRuntime::lobby_string(std::uint32_t id, const std::optional<std::string>& a1,
                                     const std::optional<std::string>& a2) {
    char key[32];
    std::snprintf(key, sizeof key, "LOBBY_STR%d", static_cast<int>(id & 0xff));
    const auto s = host_.get_string(key);
    if (!s) return std::string();
    const std::string in = cstr_of(*s);
    std::string out;
    for (std::size_t i = 0; i < in.size() && out.size() < 0x3ff;) {
        if (in[i] == '$' && i + 1 < in.size() && in[i + 1] >= '1' && in[i + 1] <= '4') {
            const auto& arg = in[i + 1] == '1' ? a1 : in[i + 1] == '2' ? a2 : std::optional<std::string>();
            if (arg) {
                out.push_back(static_cast<char>(in[i + 1] - 0x20));
                const std::string v = cstr_of(*arg);
                for (std::size_t k = 0; k < v.size() && out.size() < 0x3ff; ++k) out.push_back(v[k]);
                if (out.size() >= 0x3ff) unhosted(0x33d778, "a lobby string argument reaching the 0x400-byte buffer's end");
                out.push_back(0x10);
            }
            i += 2;
            continue;
        }
        out.push_back(in[i++]);
    }
    return out;
}

// ---------------------------------------------------------------- GuiLobbyScreen / GuiNetJoinMission

void GuiRuntime::lb_init(Widget* w) {  // 002a9240: 0033f250, then the three buttons
    auto& l = *w->lb;
    base_init(w);  // 0033d3f0: 0041f150, then +7c start_button
    l.start_button = find(w, "start_button");
    l.list = of_class(find(w, "mission_list"), WidgetClass::missionlist);  // *004928b0
    l.chat = of_class(find(w, "chat_display"), WidgetClass::chatwin);      // *004928b8
    Widget* sb = of_class(find(w, "vscrollbar2"), WidgetClass::scrollbar);
    if (!l.chat) unhosted(0x33f338, "the lobby screen's +7c store into a missing chat_display");
    l.chat->ml->scroll = sb;              // ML +7c
    ml_set_font(l.chat, "callout");       // 0043e200
    l.chat->ml->max_lines = 0x40;         // ML +60
    l.join_game = of_class(find(w, "join_game"), WidgetClass::button);        // *004928c0
    l.agent_chat = of_class(find(w, "agent_chat"), WidgetClass::button);      // *004928c8
    l.agent_info = of_class(find(w, "agent_info"), WidgetClass::button);      // *004928d0
    l.agent_remove = of_class(find(w, "agent_remove"), WidgetClass::button);  // *004928d8
    if (w->cls == WidgetClass::netcontacts) {  // 002adf00's own part (checkpoint 132on2h)
        l.invite_contact = of_class(find(w, "invite_contact"), WidgetClass::button);  // *0048bcb8
        l.remove_contact = of_class(find(w, "remove_contact"), WidgetClass::button);  // *0048bcc0
        l.b94 = 0;
        return;
    }
    if (w->cls == WidgetClass::netagencycell) {  // 002afdc0's own part (checkpoint 132on2i)
        l.resign_cell = of_class(find(w, "resign_cell"), WidgetClass::button);     // *0048bcd8
        l.invite_agent = of_class(find(w, "invite_agent"), WidgetClass::button);   // *0048bce0
        l.set_message = of_class(find(w, "set_message"), WidgetClass::button);     // *0048bce8
        l.agency_message = of_class(find(w, "agency_message"), WidgetClass::text);  // *0048bcf8
        if (l.agency_message) {  // 0041c0d0(t, 1): +76 = 1, the text object's +5b = 1
            l.agency_message->b76 = 1;
            if (l.agency_message->obj) l.agency_message->obj->b5b = 1;
        }
        return;
    }
    // 002a9240's own part
    l.show_toggle = of_class(find(w, "show_toggle"), WidgetClass::button);    // *0048bbb0
    l.online_prefs = of_class(find(w, "online_prefs"), WidgetClass::button);  // *0048bbb8
    l.host_game = of_class(find(w, "host_game"), WidgetClass::button);        // *0048bbc0
}

void GuiRuntime::lb_wake(Widget* w) {  // 0033f110
    auto& l = *w->lb;
    base_wake(w);  // 0033d3a0: 0041efa0, vt +64, start_button +10 = -1
    if (w->cls == WidgetClass::netcontacts) ct_activate(w);  // 002ade60 (132on2h)
    else jm_activate(w);
    if (l.start_button) l.start_button->id10 = -1;
    l.source = nullptr;
    l.w100 = -1;
    l.s104.clear();
    if (l.list) {
        lb_show(w, 1);
        l.list->mls->b8 = l.w84;
        l.list->list->paging = 1;  // list +80
    }
    l.b78 = 1;
    if (Widget* t = of_class(find(w, "agent_name"), WidgetClass::text)) {  // *004928e0
        const std::int32_t p = host_.current_profile();
        set_text(t, cstr_of(host_.profile_name(p).value_or("")), false);  // 00336170
    }
    g.r_2ff0 = 0;
    g.r_2ff8 = 0;
    g.r_3000 = 0;
    repeat_mask |= 0xc;  // canvas +68
    repeat_mask |= 0x3;
    if (g.lan_mode) {  // PORT-ONLY (132ln4): the discovery browser lists the LAN games while the screen is up
        host_.lan_browse(true);
        if (l.start_button) l.start_button->flags &= static_cast<std::uint16_t>(~(flag_visible | flag_enabled));
    }
}

void GuiRuntime::lb_sleep(Widget* w) {  // 0033f050
    auto& l = *w->lb;
    base_sleep(w);  // 0033d360: 0041ebf0, vt +68
    if (w->cls == WidgetClass::netcontacts) ct_deactivate(w);  // 002ade00 (132on2h)
    else jm_deactivate(w);
    l.source = nullptr;
    if (l.list) mlist_attach(l.list, nullptr);
    l.chat_ac.items.clear();  // 0044f6b0
    l.players_c8.items.clear();
    l.games_e4.items.clear();
    l.b78 = 0;
    repeat_mask &= ~0x3u;
    if (g.lan_mode) host_.lan_browse(false);  // PORT-ONLY (132ln4)
}

void GuiRuntime::lb_off(Widget* w) {  // 0033f0c0
    auto& l = *w->lb;
    if (l.active) {
        l.source = nullptr;
        if (l.list) mlist_attach(l.list, nullptr);
    }
    repeat_mask |= 0xc;
}

void GuiRuntime::lb_update(Widget* w, float dt) {  // 0033de00
    auto& l = *w->lb;
    if (host_.net_session()) lb_session_refresh(w);  // PORT-ONLY (132on2b): the lobby's game list callback
    base_update(w, dt);  // 0033d310: 0041e3b0, vt +60 0033cca0, byte 00532fe8
    net_poll(w, w->lb->start_button);  // 0033cca0
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
    if (top() == w) repeat_mask |= 0x3;
    else repeat_mask &= ~0x3u;
    // 0033de68: each chat record (+ac) into the chat window (002aec90(+8c, +4 name, +24 text, +64 private)), then
    // the list emptied (0044f7e0)
    for (const LobbyRecord& c : l.chat_ac.items) chat_line(l.chat, c.name, c.text, c.b64 != 0);
    if (!l.chat_ac.items.empty()) l.chat_ac.changed = 1;
    l.chat_ac.items.clear();
    if (g.lan_mode) lb_lan_refresh(w);  // PORT-ONLY (132ln4)
}

// 002aec90 (GuiChatWindow): a chat line appended ("(%c%s%c): %s\n" 004abc20 private, else "%c%s%c: %s\n" 004abc30,
// the name between 0x11 and 0x10) through 0043dce0; a window at its last page follows the new lines.
void GuiRuntime::chat_line(Widget* chat, const std::string& name, const std::string& text, bool whisper) {
    const std::string n = cstr_of(name), s = cstr_of(text);
    if (n.empty() || s.empty()) return;
    if (n.size() >= 0x10 || s.size() > 0x40) return;
    auto& m = *chat->ml;
    const bool bottom = !(m.top < static_cast<std::int32_t>(m.lines.size()) - m.per_page);
    std::string line;
    if (whisper) line = "(\x11" + n + "\x10): " + s + "\n";
    else line = "\x11" + n + "\x10: " + s + "\n";
    ml_set_text(chat, line);  // 0043dce0
    if (bottom) {
        m.top = static_cast<std::int32_t>(m.lines.size()) - m.per_page;
        if (m.top < 0) m.top = 0;
        ml_sync(chat);  // 0043e290
    }
}

std::uint8_t GuiRuntime::lb_input(Widget* w, const PadEvent& ev) {  // 0033da00
    auto& l = *w->lb;
    const bool triangle = pressed(ev, pad_triangle);
    if (triangle || pressed(ev, pad_left)) {
        if (canvas_.focus == l.list) {
            Widget* b = nullptr;
            switch (l.mode) {
            case 3: b = l.agent_remove; break;
            case 2: b = l.join_game; break;
            case 1: b = l.agent_info; break;
            case 0: b = l.agent_chat; break;
            default: break;
            }
            if (b) {
                set_focus(b);  // 00414b30
                send(canvas_.focus, &canvas_, 0x10, 0, 0);
                return 1;
            }
        } else if (triangle) {
            net_navigate(w, 1);
            return 1;
        }
    }
    if (pressed(ev, pad_r2)) {
        if (!l.chat) unhosted(0x33dc58, "the chat scroll of a missing chat_display");
        ml_up(l.chat, false);  // 0043d820(+8c, 0)
        return 1;
    }
    if (pressed(ev, pad_l2)) {
        if (!l.chat) unhosted(0x33dcbc, "the chat scroll of a missing chat_display");
        ml_down(l.chat, false);  // 0043d760(+8c, 0)
        return 1;
    }
    if (pressed(ev, pad_select)) {  // 002aec60: to the last page
        if (!l.chat) unhosted(0x33dd1c, "the chat scroll of a missing chat_display");
        auto& m = *l.chat->ml;
        m.top = static_cast<std::int32_t>(m.lines.size()) - m.per_page;
        if (m.top < 0) m.top = 0;
        ml_sync(l.chat);  // 0043e290
        return 1;
    }
    if (pressed(ev, pad_right) && lb_pick(w, canvas_.focus, false)) {
        send(canvas_.focus, &canvas_, 0x10, 0, 0);
        return 1;
    }
    // 0033cef0: with +78 set, START opens the messages (00532fd0 = +84, the navigator's op 9)
    if (l.b78 && pressed(ev, pad_start)) {
        if (g.lan_mode) return 0;  // PORT-ONLY (132ln4, §4 row 4b): LAN has no messages
        g.b_2fd0 = l.w84;
        net_navigate(w, 9);
        return 1;
    }
    return 0;
}

std::uint8_t GuiRuntime::lb_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 0033df00
    auto& l = *w->lb;
    const std::uint32_t t = type & 0xffff;
    if (t == 0 && l.list) {  // a child took the focus: the list's action follows the button
        if (sender == l.agent_chat) {
            mlist_action(l.list, 0);
            return 1;
        }
        if (sender == l.join_game) {
            mlist_action(l.list, 2);
            return 1;
        }
        if (sender == l.agent_info) {
            mlist_action(l.list, 1);
            return 1;
        }
        if (sender == l.agent_remove) {
            mlist_action(l.list, 3);
            return 1;
        }
        if (sender != l.list) {
            mlist_action(l.list, 1);
            return 1;
        }
    }
    if (t == 2 && lb_pick(w, sender, true)) return 1;
    if (sender == l.list && t == 0x13) {  // a row chosen (t0)
        if (!l.source) return 1;
        LobbyRecord* r = mlist_entry(l.list, t0);
        if (!r || !(mlist_flags(l.list, r) & 1)) return 1;
        lb_action(w, r);
        return 1;
    }
    if (t == 0x1a || t == 0x1b) {
        if (a3 == 0x1d) {  // the chat prompt's reply: t0 the keyboard's text (0 on a cancel)
            const std::string text = t == 0x1a && sender && sender->kb ? sender->kb->result.value_or("") : "";
            if (l.w100 != -1 && !text.empty()) {
                // 00451050(session, t0, +100); no session or a failed send: the popup (2, "", LOBBY_STR 0x65)
                if (!host_.net_session() || !host_.lobby_chat_send(text, l.w100))
                    popup_open(2, std::string(), lobby_string(0x65), nullptr, 0);
            }
            return 1;
        }
        if (a3 == 0x1e && t == 0x1a) {  // the password prompt's OK: vt +74 (this, +110 the waiting entry, 0)
            lb_join(w, &l.pending, 0);
            return 1;
        }
        if (a3 == port_password_arg) {  // PORT-ONLY (132on2c): the game password keyboard's reply
            if (t == 0x1a && sender && sender->kb && sender->kb->result) {
                host_.lobby_set_join_password(*sender->kb->result);
                lb_join(w, &l.pending, 0);
            }
            return 1;
        }
    }
    if (t == 0x1e && !g.app_32) {  // the lobby's join callback
        if (!(w->flags & flag_visible)) return 0;
        g.r_2ff8 = a3;
        g.r_3000 = t0;
        g.r_2ff0 = 1;
        return 1;
    }
    return net_event(w, sender, type, a3, t0);  // 0033cf90
}

void GuiRuntime::lb_action(Widget* w, LobbyRecord* r) {  // vt +6c / +70 / +74 / +78 by +90
    switch (w->lb->mode) {
    case 0: lb_chat(w, r); break;
    case 1: lb_info(w, r); break;
    case 2: lb_join(w, r, 1); break;
    case 3:  // 002a8b50 (GuiNetJoinMission): nothing; 002ada10 (GuiNetContacts): the remove prompt
        if (w->cls == WidgetClass::netcontacts) ct_remove(w, r);
        break;
    default: break;
    }
}

std::uint8_t GuiRuntime::lb_pick(Widget* w, Widget* sender, bool now) {  // 0033e2a0
    auto& l = *w->lb;
    if (sender == l.agent_chat) {
        if (l.b94 && now) {
            if (lb_mode(w, 0, false)) lb_chat(w, nullptr);
        } else {
            lb_mode(w, 0, true);
            send(w, l.agent_chat, 6, a3_on_pressed, 0);
        }
        return 1;
    }
    if (sender == l.join_game) {
        lb_mode(w, 2, true);
        send(w, l.join_game, 6, a3_on_pressed, 0);
        return 1;
    }
    if (sender == l.agent_info) {
        lb_mode(w, 1, true);
        send(w, l.agent_info, 6, a3_on_pressed, 0);
        return 1;
    }
    if (sender == l.agent_remove) {
        lb_mode(w, 3, true);
        send(w, l.agent_remove, 6, a3_on_pressed, 0);
        return 1;
    }
    return 0;
}

std::uint8_t GuiRuntime::lb_mode(Widget* w, std::uint8_t mode, bool focus) {  // 0033ef10
    auto& l = *w->lb;
    l.mode = mode;
    if (!l.list) return 0;
    mlist_action(l.list, mode);
    if (!l.list->mls->rows.empty()) {
        if (focus) set_focus(l.list);
        return 1;
    }
    std::uint32_t id = 0;
    switch (mode) {
    case 0: id = 0x11; break;
    case 1: id = 0x12; break;
    case 2: id = 0x10; break;
    case 3: id = 0x13; break;
    default: return 1;  // 0033efd8: none of the four, nothing shown
    }
    const std::string m = lobby_string(id);
    popup_open(2, std::string(), m, nullptr, 0);
    return 0;
}

void GuiRuntime::lb_show(Widget* w, std::uint8_t kind) {  // 0033d8e0
    auto& l = *w->lb;
    if (kind == 1 || kind == 3 || kind == 2) {
        if (l.source != &l.players_c8 && host_.net_session()) host_.lobby_list_kind(0);  // 004515f0(lobby, 0)
        l.source = &l.players_c8;
    } else if (kind == 0) {
        if (l.source != &l.games_e4) {
            if (host_.net_session()) host_.lobby_list_kind(1);  // 004515f0(lobby, 1)
            l.source = &l.games_e4;
        }
    }
    if (l.list) {
        array_select(l.list, 0, 0);  // 00424430
        mlist_kind(l.list, kind);
        mlist_attach(l.list, l.source);
    }
}

std::uint8_t GuiRuntime::lb_kind(const Widget* w) const {  // 0033d8c0
    const auto& l = *w->lb;
    return l.list ? l.list->mls->kind : 1;
}

void GuiRuntime::lb_host_game(Widget*) {  // 0033d800: with a session in state 5 only (session +255c)
    if (host_.net_session()) unhosted(0x33d820, "the lobby's host game (session +255c, GuiAgentInfo / AGNTINFO)");
}

void GuiRuntime::lb_chat(Widget* w, LobbyRecord* r) {  // vt +6c 0033ee40: with +a8 and a session only
    auto& l = *w->lb;
    if (!l.source) return;
    if (!host_.net_session()) return;
    l.w100 = r ? r->id : 0;  // the target: the record's agent, or 0 the room
    Widget* kb = factory("GuiKeyboard", std::string("KEYBOARD"));
    if (kb) kb_open(kb, lobby_string(0x64), std::string(), w, 0x40, 0x1d, 0);  // 0032b4f0 (text "" 004b9c60)
}

void GuiRuntime::lb_info(Widget* w, LobbyRecord* r) {  // vt +70 0033e410: with +a8 and a session only
    if (!w->lb->source) return;
    if (!host_.net_session()) return;
    // Checkpoint 132on2i: 00450c60(session, the record's agent, buf): the agent's record, else (2, "", LOBBY_STR 0x67).
    std::array<std::uint8_t, 0xfc> rec{};
    if (!host_.lobby_agent_stats(r->id, rec)) {
        const std::string m = lobby_string(0x67);
        popup_open(2, std::string(), m, nullptr, 0);
        return;
    }
    const std::int32_t wait = popup_open(0, std::string(), lobby_string(0x7d, cstr_of(r->name)), nullptr, 0);
    host_.app_2c82f0();
    Widget* n = factory("GuiAgentInfo", std::string("AGNTINFO"));  // 004b9c78 / 004b9c88
    host_.app_2c81d0();
    if (n) {
        n->ai->lobby = w;      // +10
        ai_fill_net(n, *r, rec);  // 00360610
    }
    w->flags &= static_cast<std::uint16_t>(~flag_visible);  // +14 &= ~2
    push(n, 0x14);   // the manager's vt +64
    popup_close(wait);
}

void GuiRuntime::lb_join(Widget* w, LobbyRecord* r, std::int32_t prompt) {  // vt +74 0033e5b0
    auto& l = *w->lb;
    if (!l.source) return;
    if (host_.net_session()) {
        lb_join_session(w, r, prompt);
        return;
    }
    if (!g.lan_mode) return;  // the original with no session: nothing
    // PORT-ONLY (132ln4, LAN_PLAY.md §4 row 7b) from here: the original's steps with the LAN session behind them.
    const std::int32_t popup = popup_open(0, std::string(), lobby_string(0x15), nullptr, 0);
    auto fail = [&](std::uint32_t text_id) {  // (2, LOBBY_STR102, LOBBY_STR<id> with $1 the entry's name)
        const std::string title = lobby_string(0x66);
        const std::string m = lobby_string(text_id, cstr_of(r->name));
        popup_open(2, title, m, nullptr, 0);
    };
    // The entry's level must be the command centre's (app +24c, or none chosen): the level name into G+140.
    const char* name = nullptr;
    const std::int32_t mission = r->at(0x40);
    if (mission >= 0 && mission < 9) {
        const std::int32_t level = mission_levels[static_cast<std::size_t>(mission)];
        if (level != -1 && (g.app_24c == -1 || level == g.app_24c)) name = cc_level_names[static_cast<std::size_t>(level)];
    }
    if (!name || !*name) {
        fail(0x68);
        popup_close(popup);
        return;
    }
    g.level_name = std::string(name).substr(0, 0x80);  // 00129a70(004ffc90, name, 0x80)
    // 0044e060(lobby, +48): the game's information; the LAN browse entry of the record's session.
    std::optional<GuiHost::LanGame> game;
    for (const auto& gm : host_.lan_games())
        if (gm.session_id == r->lan_session) game = gm;
    bool bad = !game;
    if (!bad && game->mission != r->at(0x40)) bad = true;  // +38 the mission
    if (!bad && lan_record_id(game->session_id) != r->id) bad = true;  // +34 the record (no password on LAN)
    (void)prompt;
    if (bad) {
        fail(0x68);
        popup_close(popup);
        return;
    }
    std::int32_t agents = game->agents_max;  // +24
    if (agents <= 0 || agents >= 5) agents = 4;
    g.app_254 = agents;
    host_.profiles_clear_b70();  // 0012ee30(004ffb50)
    // 001698c0 / 00165bc0 (the network start) and session +2560 = +255c: none on LAN
    const std::int32_t old_level = g.app_24c;
    cc_level_block(mission_levels[static_cast<std::size_t>(mission)]);  // 002cc820(app + 188, level)
    if (host_.lan_join(game->session_id)) {  // 0044e430
        const std::int64_t deadline = host_.lan_clock_ms() + 60000;  // 003698b0() + 0x7fff + 0x6a61
        while (!g.r_2ff0 && host_.lan_clock_ms() < deadline) {
            host_.card_poll();  // 002cc260(0, 0)
            host_.lan_pump();   // 004536b0 / 00429d30
            switch (host_.lan_join_state()) {  // the lobby's callback, event 0x1e
            case GuiHost::LanJoin::accepted: send(nullptr, w, 0x1e, 1, 0); break;
            case GuiHost::LanJoin::rejected: send(nullptr, w, 0x1e, 0, key_f_to_joingame); break;
            case GuiHost::LanJoin::failed: send(nullptr, w, 0x1e, 0, key_f_to_connect); break;
            default: break;
            }
        }
        const bool timeout = !g.r_2ff0;
        if (!timeout && g.r_2ff8 != 0) {  // joined: the game flow's state 3 (its multiplayer path)
            g.app_state_prev = g.app_state;
            g.app_state = 3;
            g.app_35 = 1;
        } else {
            const std::string title = lobby_string(0x66);
            const auto m = host_.get_string(timeout ? "F_TO_CONNECT" : callback_key(g.r_3000));  // 003fdb50
            std::optional<std::string> text = m ? std::optional<std::string>(cstr_of(*m)) : std::nullopt;
            if (!timeout && g.r_3000 == key_f_to_joingame)  // PORT-ONLY (132md5): the host's mod policy refusal
                if (auto notice = host_.lan_join_notice()) text = std::move(notice);
            popup_open(2, title, text, nullptr, 0);
            host_.lan_join_cancel();  // 0042b1f0 (005721a8 kept around it on a timeout), 0044e340
            cc_level_block(old_level);
        }
    } else {  // 00452ff0's error code: none on LAN, the default text
        fail(0x68);
        cc_level_block(old_level);
    }
    popup_close(popup);
    g.r_2ff0 = 0;
    g.r_2ff8 = 0;
    g.r_3000 = 0;
}

// 0033e5b0's session branch (132on2b, research/ONLINE.md §9.5): the record's game joined through the lobby.
void GuiRuntime::lb_join_session(Widget* w, LobbyRecord* r, std::int32_t prompt) {
    auto& l = *w->lb;
    const std::int32_t popup = popup_open(0, std::string(), lobby_string(0x15), nullptr, 0);
    auto fail = [&](std::uint32_t text_id) {  // (2, LOBBY_STR102, LOBBY_STR<id> with $1 the entry's name)
        const std::string title = lobby_string(0x66);
        const std::string m = lobby_string(text_id, cstr_of(r->name));
        popup_open(2, title, m, nullptr, 0);
    };
    const char* name = nullptr;
    const std::int32_t mission = r->at(0x40);
    if (mission >= 0 && mission < 9) {
        const std::int32_t level = mission_levels[static_cast<std::size_t>(mission)];
        if (level != -1 && (g.app_24c == -1 || level == g.app_24c)) name = cc_level_names[static_cast<std::size_t>(level)];
    }
    if (!name || !*name) {
        fail(0x68);
        popup_close(popup);
        return;
    }
    g.level_name = std::string(name).substr(0, 0x80);  // 00129a70(004ffc90, name, 0x80)
    const auto info = host_.lobby_game_info(r->at(0x48));  // 0044e060(session, +48)
    bool bad = !info || info->mission != r->at(0x40);       // +38
    if (!bad && info->owner != r->id) {                     // +34: another agent's game
        // 0033e7f0: with the list's +b5 set (CONTACTS' 002aded4) and a game name (+134): without the prompt flag
        // (a2) the join; with it (checkpoint 132on2i) the name's first tab field (strchr 9, else strlen) of 1..15
        // characters, the record into +110, and the prompt (4, LOBBY_STR 0x89, LOBBY_STR 0x8a with $1 the field;
        // the reply 0x1e joins +110 without the flag); the wait popup closed and 0033ee10 (+2ff0 / +2ff8 / +3000
        // left as they are). JOINMIS never sets +b5, so there this is the failure.
        if (l.list && l.list->mls->b5 && info->game_name) {
            if (prompt) {
                const std::string game = cstr_of(info->name);
                const std::size_t tab = game.find('\t');
                const std::size_t len = tab == std::string::npos ? game.size() : tab;
                if (len >= 1 && len < 0x10) {
                    if (r != &l.pending) l.pending = *r;
                    const std::string title = lobby_string(0x89);
                    const std::string text = lobby_string(0x8a, game.substr(0, len));
                    popup_open(4, title, text, w, 0x1e);  // 002cae30
                    popup_close(popup);
                    return;
                }
                bad = true;
            }
        } else {
            bad = true;
        }
    }
    if (bad) {
        fail(0x68);
        popup_close(popup);
        return;
    }
    std::int32_t agents = info->max_players;  // +24
    if (agents <= 0 || agents >= 5) agents = 4;
    g.app_254 = agents;
    host_.profiles_clear_b70();  // 0012ee30(004ffb50)
    host_.lobby_net_start();     // 001698c0 / 00165bc0
    const std::int32_t saved = host_.lobby_state();  // session +2560 = +255c
    const std::int32_t old_level = g.app_24c;
    cc_level_block(mission_levels[static_cast<std::size_t>(mission)]);  // 002cc820(app + 188, level)
    if (host_.lobby_join(r->at(0x48))) {  // 0044e430(session, +48)
        const std::int64_t deadline = host_.lobby_clock_ms() + 60000;  // 003698b0() + 0x7fff + 0x6a61
        while (!g.r_2ff0 && host_.lobby_clock_ms() < deadline) {
            host_.card_poll();  // 002cc260(0, 0)
            if (const auto cb = host_.lobby_pump()) send(nullptr, w, 0x1e, cb->a3, cb->t0);  // 004536b0
            host_.net_update();  // 00429d30
        }
        const bool timeout = !g.r_2ff0;
        if (!timeout && g.r_2ff8 != 0) {  // joined: the game flow's state 3 (its multiplayer path)
            g.app_state_prev = g.app_state;
            g.app_state = 3;
            g.app_35 = 1;
        } else {
            // PORT-ONLY (132on2c, research/ONLINE.md §5.3 GamePassword): a join refused for its game password asks
            // for the password (the keyboard's OK joins the entry again, prompt flag 0) instead of the popup.
            const bool password = !timeout && host_.lobby_join_password_refused();
            if (!password) {
                const std::string title = lobby_string(0x66);
                const auto m = host_.get_string(timeout ? "F_TO_CONNECT" : callback_key(g.r_3000));  // 003fdb50
                popup_open(2, title, m ? std::optional<std::string>(cstr_of(*m)) : std::nullopt, nullptr, 0);
            }
            host_.lobby_game_cleanup();  // 0042b1f0 (005721a8 kept around it on a timeout)
            host_.lobby_join_end();      // 0044e340
            host_.lobby_set_state(saved);  // +255c = +2560
            cc_level_block(old_level);
            if (password) {
                if (r != &l.pending) l.pending = *r;
                if (Widget* kb = factory("GuiKeyboard", std::string("KEYBOARD")))
                    kb_open(kb, host_.get_string("password"), std::string(), w, 0x1f, port_password_arg, 0);
            }
        }
    } else {  // 00452ff0's error code
        std::uint32_t id = 0x68;
        switch (host_.lobby_error() & 0xff) {
        case 0x39: id = 0x76; break;
        case 0x37: id = 0x6d; break;
        case 0x36: id = 0x6c; break;
        case 0x35: id = 0x6b; break;
        case 0x33: id = 0x6e; break;
        case 2: id = 0x6a; break;
        default: break;
        }
        fail(id);
        cc_level_block(old_level);
    }
    popup_close(popup);
    g.r_2ff0 = 0;
    g.r_2ff8 = 0;
    g.r_3000 = 0;
}

// 0044eff0 / 0044f0c0's record fields from a game cache entry (checkpoint 132on2i): +30 the world status (+24), +34
// players (+4), +38 max players (+8), +3c the game's seconds (+1c), +40 mission (+10), +44 filter (+18).
void GuiRuntime::lb_cache_fill(LobbyRecord& rec, const GuiHost::LobbyCachedGame& e) {
    rec.at(0x30) = e.world_status;
    rec.at(0x34) = e.players;
    rec.at(0x38) = e.max_players;
    rec.at(0x3c) = e.stats[4];
    rec.at(0x40) = e.stats[1];
    rec.at(0x44) = e.stats[3];
}

// 00447790 (SFOLobby's game list callback) over GuiHost::lobby_game_list, as the lobby's +a8 list that 0044f180
// copies into the screen's +e4 (the activate's 004518f0 registered it) at the list's end (+6c): +e4 emptied (changed
// when it held records) and the records appended (each marks it changed). PORT-ONLY (132on2b): run at the update's
// start for each list completed since the last (its serial; the original's callback runs from the lobby's update
// 004536b0, before the screens' update). Checkpoint 132on2c: the player list (00447a30 into +c8, the same way) and
// the chat records (00447bb0 / 00447c70 into +ac) likewise.
void GuiRuntime::lb_session_refresh(Widget* w) {
    auto& l = *w->lb;
    // 0044edc0's 0044eff0 (132on2i): each game cache entry added or changed since the last update, applied to the
    // registered lists' records of its world (+48): +30..+44 set, the list marked changed (0044f280(list, 1)).
    for (const auto& e : host_.lobby_game_cache_take()) {
        for (LobbyList* list : {&l.games_e4, &l.players_c8}) {
            for (auto& rec : list->items) {
                if (rec.at(0x48) != e.world) continue;
                lb_cache_fill(rec, e);
                list->changed = 1;
            }
        }
    }
    // 00447bb0 / 00447c70 (the TextFilter echo and ChatFwd callbacks): the room's chat records appended to +ac
    for (const auto& c : host_.lobby_chat_take()) {
        LobbyRecord rec;
        rec.id = c.id;
        rec.name = c.name.substr(0, 0x1f);
        rec.text = c.text.substr(0, 0x3f);
        rec.b64 = c.whisper ? 1 : 0;
        // 0044f470 on the list 004518f0 set up with the session's +1ea4 (MAX_CHAT, at least 5: 0044f880) records and
        // no growth: a full list drops its oldest record (0044f2e0(list, 0)) first
        const std::size_t cap = static_cast<std::size_t>(std::max<std::int32_t>(5, host_.lobby_max_chat()));
        if (l.chat_ac.items.size() >= cap) l.chat_ac.items.erase(l.chat_ac.items.begin());
        l.chat_ac.items.push_back(std::move(rec));
        l.chat_ac.changed = 1;
    }
    if (w->cls == WidgetClass::netcontacts) {
        // 00446830 (GetBuddyListExtraInfo's callback, checkpoint 132on2h): each response with status +18 0 a record
        // (0044f470) in the session's work list +e0: +0 the id, +4 the name, +24 the kind by the player status +40
        // (3 in a game -> 1 with +48 the world, 2 online -> 2, else 3), +28 2, +2c the empty string 004c0cc8, +30 0,
        // +34..+40 -1, +44 5; at EndOfList (+cc) copied (0044f180) into +23e8 (this screen's +c8) and +150.
        // Checkpoint 132on2i: a record in a game takes its game's cache entry (0044f0c0(session, +48)) as 0044eff0
        // does: +30 the world status, +34 players, +38 max players, +3c the game's seconds, +40 mission, +44 filter.
        const GuiHost::LobbyContactList contacts = host_.lobby_contact_list();
        if (contacts.serial == l.port_player_serial) return;
        l.port_player_serial = contacts.serial;
        std::vector<LobbyRecord> records;
        for (const auto& row : contacts.rows) {
            LobbyRecord rec;
            rec.id = row.id;
            rec.name = cstr_of(row.name).substr(0, 0x1f);
            rec.at(0x24) = row.status == 3 ? 1 : row.status == 2 ? 2 : 3;
            rec.at(0x28) = 2;
            rec.at(0x2c) = 0;
            rec.at(0x30) = 0;
            rec.at(0x34) = rec.at(0x38) = rec.at(0x3c) = rec.at(0x40) = -1;
            rec.at(0x44) = 5;
            if (row.status == 3) {
                rec.at(0x48) = row.world;
                if (const auto e = host_.lobby_game_cached(row.world)) lb_cache_fill(rec, *e);
            }
            records.push_back(std::move(rec));
        }
        if (!l.players_c8.items.empty() || !records.empty()) l.players_c8.changed = 1;
        l.players_c8.items = std::move(records);
        return;
    }
    // 00447a30 (the player list callback): the response's agent (+1c id, +20 name) by its status +40 (3 -> kind 1,
    // 2 -> kind 2, 1 / 0 not listed, else kind 3), copied into +c8 (0044f180) at EndOfList
    const GuiHost::LobbyPlayerList players = host_.lobby_player_list();
    if (players.serial != l.port_player_serial) {
        l.port_player_serial = players.serial;
        std::vector<LobbyRecord> records;
        for (const auto& row : players.rows) {
            if (row.status == 0 || row.status == 1) continue;
            LobbyRecord rec;
            rec.id = row.id;
            rec.name = cstr_of(row.name).substr(0, 0x1f);
            rec.at(0x24) = row.status == 3 ? 1 : row.status == 2 ? 2 : 3;
            rec.at(0x28) = 2;
            rec.at(0x2c) = 0;
            rec.at(0x30) = 0;
            rec.at(0x34) = rec.at(0x38) = rec.at(0x3c) = rec.at(0x40) = -1;
            rec.at(0x44) = 5;
            records.push_back(std::move(rec));
        }
        if (!l.players_c8.items.empty() || !records.empty()) l.players_c8.changed = 1;
        l.players_c8.items = std::move(records);
    }
    const GuiHost::LobbyGameList list = host_.lobby_game_list();
    if (list.serial == l.port_list_serial) return;
    l.port_list_serial = list.serial;
    std::vector<LobbyRecord> records;
    for (const auto& row : list.rows) {  // the host's rows are the responses with status +18 0
        // +20 with spaces as 0xff up to the tab, then sscanf "%32s\t%d\t%d\t%d\t%d" (5 fields), 0xff back to spaces
        std::string s = cstr_of(row.name);
        for (std::size_t i = 0; i < s.size() && i < 64 && s[i] != '\t'; ++i)
            if (s[i] == ' ') s[i] = static_cast<char>(0xff);
        std::size_t p = 0;
        auto space = [&] {
            while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) ++p;
        };
        space();
        std::string name;
        while (p < s.size() && !std::isspace(static_cast<unsigned char>(s[p])) && name.size() < 32) name.push_back(s[p++]);
        if (name.empty()) continue;
        std::array<std::int32_t, 4> v{};
        bool ok = true;
        for (auto& x : v) {
            space();
            std::size_t q = p;
            const bool neg = q < s.size() && s[q] == '-';
            if (q < s.size() && (s[q] == '-' || s[q] == '+')) ++q;
            const std::size_t digits = q;
            std::uint32_t acc = 0;
            while (q < s.size() && std::isdigit(static_cast<unsigned char>(s[q])))
                acc = acc * 10U + static_cast<std::uint32_t>(s[q++] - '0');
            if (q == digits) {
                ok = false;
                break;
            }
            x = static_cast<std::int32_t>(neg ? 0U - acc : acc);
            p = q;
        }
        if (!ok) continue;
        for (auto& ch : name)
            if (ch == static_cast<char>(0xff)) ch = ' ';
        // lobby +58 the mission count: 9, the table 004acb30's online missions
        if (name.size() >= 0x20 || v[0] < 1 || v[0] > 4 || v[1] < 0 || v[1] >= 9 || v[2] < 0 || v[2] > 5) continue;
        // 00450880(lobby, d, name, 0, 0), the lobby's player cache: not ported (the player list is not served)
        LobbyRecord rec;
        rec.id = v[3];
        rec.name = name;
        rec.at(0x24) = 0;
        rec.at(0x30) = row.status;   // +60 the world status (1 staging: open to join)
        rec.at(0x34) = row.players;  // +68
        rec.at(0x38) = v[0];
        rec.at(0x40) = v[1];
        rec.at(0x44) = v[2];
        rec.at(0x48) = row.world;
        records.push_back(std::move(rec));
    }
    if (!l.games_e4.items.empty() || !records.empty()) l.games_e4.changed = 1;
    l.games_e4.items = std::move(records);
}

// PORT-ONLY (132ln4): the discovered LAN games as game records (+24 0) in the lobby's lists +c8 and +e4 (JOINMIS
// shows +c8, which the original's join mode reads games from); a list that changes is marked changed (+18), so
// GuiMissionList's update rebuilds its rows (002acca0).
void GuiRuntime::lb_lan_refresh(Widget* w) {
    auto& l = *w->lb;
    std::vector<LobbyRecord> records;
    for (const auto& gm : host_.lan_games()) {
        LobbyRecord r;
        r.id = lan_record_id(gm.session_id);
        r.name = gm.host_name.substr(0, 0x1f);
        r.at(0x24) = 0;                                          // a game
        r.at(0x30) = gm.state == 0 && !gm.mismatch ? 1 : 0;      // open to join
        r.at(0x34) = gm.agents_now;
        r.at(0x38) = gm.agents_max;
        r.at(0x40) = gm.mission;
        r.at(0x44) = gm.join_filter;
        r.at(0x48) = r.id;
        r.lan_session = gm.session_id;
        r.lan_mismatch = gm.mismatch;
        records.push_back(std::move(r));
    }
    auto same = [](const std::vector<LobbyRecord>& a, const std::vector<LobbyRecord>& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].w != b[i].w ||
                a[i].lan_session != b[i].lan_session || a[i].lan_mismatch != b[i].lan_mismatch)
                return false;
        return true;
    };
    for (LobbyList* list : {&l.players_c8, &l.games_e4}) {
        if (same(list->items, records)) continue;
        list->items = records;
        list->changed = 1;
    }
}

std::uint8_t GuiRuntime::jm_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 002a8d60
    auto& l = *w->lb;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x1a && a3 == 0xb) {  // the room error popup's reply: back to the command centre
        net_navigate(w, 1);
        return 1;
    }
    if (t == 2) {
        if (sender == l.online_prefs) {
            g.app_268 = 2;
            net_navigate(w, 5);
            return 1;
        }
        if (sender == l.show_toggle) {
            if (lb_kind(w) == 2) {
                lb_show(w, 0);
                if (host_.net_session()) host_.lobby_list_kind(1);  // 004515f0(lobby, 1)
                jm_toggle_text(w, "ShowGames");  // *0048bbd8
            } else {
                lb_show(w, 2);
                if (host_.net_session()) host_.lobby_list_kind(0);  // 004515f0(lobby, 0)
                jm_toggle_text(w, "ShowPlayers");  // *0048bbe0
            }
            return 1;
        }
        if (sender == l.host_game) {
            lb_host_game(w);
            return 1;
        }
    }
    if (t == 0) {
        if (sender == l.agent_chat) lb_show(w, 2);
        else if (sender == l.join_game) lb_show(w, 0);
        else if (sender == l.agent_info) lb_show(w, 2);
        else if (sender == l.online_prefs) lb_show(w, 4);
        else if (sender == l.host_game) lb_show(w, 0);
    }
    return lb_event(w, sender, type, a3, t0);  // 0033df00
}

void GuiRuntime::jm_toggle_text(Widget* w, const char* key) {  // 0041beb0(+160, 003fdb50(key), 0)
    const auto s = host_.get_string(key);
    if (s && w->lb->show_toggle) set_text(w->lb->show_toggle, cstr_of(*s), false);
}

void GuiRuntime::jm_activate(Widget* w) {  // 002a9030
    auto& l = *w->lb;
    if (l.active) return;
    if (host_.net_session()) {  // 002a9060: the main channel left, then the game room (its failure only reported)
        if (host_.lobby_state() == 4) host_.lobby_channel_leave();  // 004426b0
        if (host_.lobby_state() == 9) {
            // 004518f0 in state 9 sets the three lists up (0044f880: emptied, +18 changed), and frees them
            // (0044f6b0) when the room is refused
            for (LobbyList* x : {&l.chat_ac, &l.players_c8, &l.games_e4}) {
                x->items.clear();
                x->changed = 1;
            }
        }
        if (!host_.lobby_room_open())  // 004518f0(session, 002cc260, +ac, +c8, +e4)
            popup_open(2, std::string(), lobby_string(0x5a), w, 0xb);  // 002cae30
        // PORT-ONLY (132on2b): only the lists completed from now on reach +e4 / +c8 (004518f0 registers the lists)
        l.port_list_serial = host_.lobby_game_list().serial;
        l.port_player_serial = host_.lobby_player_list().serial;
    }
    if (Widget* t = of_class(find(w, "mission_name"), WidgetClass::text)) set_text(t, cstr_of(g.app_1c8), false);
    if (Widget* t = of_class(find(w, "region_name"), WidgetClass::text)) {
        if (g.lan_mode) {
            set_text(t, port_lan_region, false);  // PORT-ONLY (132ln4)
        } else {
            const std::int32_t p = host_.current_profile();
            set_text(t, host_.profile_text(p, 0x95c), false);  // the agent's +95c
        }
    }
    lb_show(w, 0);
    if (host_.net_session()) host_.lobby_list_kind(1);  // 004515f0(lobby, 1): the games
    jm_toggle_text(w, "ShowGames");  // *0048bbd8
    if (l.list) l.list->mls->b4 = 0;
    l.active = 1;
}

void GuiRuntime::jm_deactivate(Widget* w) {  // 002a8fd0
    auto& l = *w->lb;
    if (!l.active) return;
    lb_off(w);  // 0033f0c0
    if (host_.net_session()) host_.lobby_room_leave();  // 00451610
    l.active = 0;
}

// ---------------------------------------------------------------- GuiNetContacts (vtable 004ddc20, checkpoint 132on2h)

std::uint8_t GuiRuntime::ct_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 002adab0
    auto& l = *w->lb;
    const std::uint32_t t = type & 0xffff;
    if (t == 2 && sender == l.invite_contact) {  // the invite: the name keyboard (0032b4f0, reply a3 0xc)
        Widget* kb = factory("GuiKeyboard", std::string("KEYBOARD"));  // 004abb10 / 004abb20
        if (kb) kb_open(kb, lobby_string(0x40), std::string(), w, 0xf, 0xc, 0);
        return 1;
    }
    if (t == 0x1a && a3 == 0xd) {  // the remove prompt's yes: 0044d9c0(session, +100) under a wait popup
        if (!host_.net_session()) return 1;
        const std::int32_t wait = popup_open(0, std::string(), lobby_string(0x36, l.s104), nullptr, 0);
        if (host_.lobby_contact_remove(l.w100))
            popup_open(2, std::string(), lobby_string(0x37, l.s104), nullptr, 0);
        else
            popup_open(2, std::string(), lobby_string(0x38, l.s104), nullptr, 0);
        popup_close(wait);
        return 1;
    }
    if (t == 0x1a && a3 == 0xc) {  // the name keyboard's OK (t0 its text): 00451370, then 0033c7c0
        const std::string text = sender && sender->kb ? cstr_of(sender->kb->result.value_or("")) : std::string();
        if (text.empty() || !host_.net_session()) return 1;
        const std::int32_t id = host_.lobby_account_id(text);
        if (host_.lobby_error() & 0xff) popup_open(2, std::string(), lobby_string(0x17, text), nullptr, 0);
        else ct_add(w, id, text);
        return 1;
    }
    if (t == 0 && sender != l.list) {  // a child took the focus: the list's columns follow it (0033d8e0)
        if (sender == l.join_game) lb_show(w, 3);
        else if (sender == l.invite_contact) lb_show(w, 4);
        else lb_show(w, 1);
    }
    return lb_event(w, sender, type, a3, t0);  // 0033df00
}

void GuiRuntime::ct_activate(Widget* w) {  // 002ade60
    auto& l = *w->lb;
    if (l.active) return;
    if (host_.net_session()) {  // the main channel left, then 0044df80(session, 002cc260, +ac, +c8)
        if (host_.lobby_state() == 4) host_.lobby_channel_leave();  // 004426b0
        const std::int32_t state = host_.lobby_state();
        if (state == 9 || state == 0) {
            // 0044df80 in state 9 / 0 sets the two lists up (0044f880: emptied, +18 changed)
            for (LobbyList* x : {&l.chat_ac, &l.players_c8}) {
                x->items.clear();
                x->changed = 1;
            }
        }
        host_.lobby_contacts_open();  // its result is not read
        // PORT-ONLY (132on2h): only the contact lists completed from now on reach +c8 (0044df80 registers the list)
        l.port_player_serial = host_.lobby_contact_list().serial;
    }
    if (l.list) {
        l.list->mls->b4 = 1;
        l.list->mls->b5 = 1;
    }
    l.active = 1;
}

void GuiRuntime::ct_deactivate(Widget* w) {  // 002ade00
    auto& l = *w->lb;
    if (!l.active) return;
    lb_off(w);  // 0033f0c0
    if (host_.net_session()) {
        const std::int32_t state = host_.lobby_state();
        host_.lobby_contacts_close();  // 0044dd20
        if (state == 6 || state == 10) {
            // 0044dd20 in state 6 / 10 frees the lists 0044df80 registered (0044f6b0: emptied, changed if they held
            // records)
            for (LobbyList* x : {&l.chat_ac, &l.players_c8}) {
                if (!x->items.empty()) x->changed = 1;
                x->items.clear();
            }
        }
    }
    l.active = 0;
}

void GuiRuntime::ct_remove(Widget* w, LobbyRecord* r) {  // vt +78 002ada10: the remove prompt (5, reply a3 0xd)
    auto& l = *w->lb;
    l.w100 = r->id;
    l.s104 = cstr_of(r->name);
    popup_open(5, std::string(), lobby_string(0x35, l.s104), w, 0xd);
}

void GuiRuntime::ct_add(Widget*, std::int32_t id, const std::string& name) {  // 0033c7c0
    if (!host_.net_session()) return;
    const std::int32_t wait = popup_open(0, std::string(), lobby_string(0x39, name), nullptr, 0);
    if (host_.lobby_contact_add(id)) {
        popup_open(2, std::string(), lobby_string(0x3a, name), nullptr, 0);
    } else {
        std::uint32_t text = 0x3f;
        switch (host_.lobby_error() & 0xff) {
        case 0x2a: text = 0x3e; break;
        case 0x1d: text = 0x17; break;
        case 0x25: text = 0x3d; break;
        case 0x1e: text = 0x3c; break;
        default: break;
        }
        const std::string title = lobby_string(0x3b);  // looked up before the text (the original's order)
        popup_open(2, title, lobby_string(text, name), nullptr, 0);
    }
    popup_close(wait);
}

// ---------------------------------------------------------------- GuiMissionList (vtable 004ddb80)

void GuiRuntime::mlist_init(Widget* w) {  // 002accf0
    auto& m = *w->mls;
    array_init(w);  // 00424bc0
    m.header_w = of_class(find(w, "column_header"), WidgetClass::textarray);  // *0048bc28
    char key[32];
    for (int i = 0; i < 4; ++i) {
        std::snprintf(key, sizeof key, "AgentStatus%d", i);
        m.status[static_cast<std::size_t>(i)] = cstr_of(host_.get_string(key).value_or(""));
    }
    for (int i = 0; i < 5; ++i) {
        std::snprintf(key, sizeof key, "PingResult%d", i);
        m.ping[static_cast<std::size_t>(i)] = cstr_of(host_.get_string(key).value_or(""));
    }
    for (int i = 0; i < 5; ++i) {
        std::snprintf(key, sizeof key, "FilterType%d", i);
        m.filter[static_cast<std::size_t>(i)] = cstr_of(host_.get_string(key).value_or(""));
    }
    for (int i = 0; i < 6; ++i) {
        std::snprintf(key, sizeof key, "MisListHeader%d", i);
        const auto s = host_.get_string(key);
        m.header[static_cast<std::size_t>(i)] = s ? std::optional<std::string>(cstr_of(*s)) : std::nullopt;
    }
    w->list->row_h -= 4;  // +5c
}

void GuiRuntime::mlist_wake(Widget* w) {  // 002abd50
    auto& m = *w->mls;
    array_wake(w);  // 00424a90 (its vt +6c is mlist_entered)
    TextObject o;  // 003d0940(o, 0, 0, w, h, font "callout", 00587c40)
    o.rect = {0, 0, cvt_w(w->rect[2], 0x2abdf0), cvt_w(w->rect[3], 0x2abdf4)};
    o.font = host_.font("callout");
    o.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    host_.text_object(o);
    m.obj = o;
    m.obj->b12 = 1;
    m.obj->b10 = 2;
    const std::string stars(0x4f, '*');  // 001260f0(buf, '*', 0x50), buf[0x4f] = 0
    host_.set_text(stars);  // 003d08b0
    m.obj->text = stars;
    m.sel_row = m.sel_index = m.sel_id = -1;
    // session +2480, the account id (GuiHost::lobby_own_id)
    m.own_id = host_.net_session() ? host_.lobby_own_id() : -1;
    if (!m.header_w) unhosted(0x2abea8, "the column header's +90 read of a missing column_header");
    if (m.header_w->ta->obj) {  // +90, the header's text object (from its wake)
        m.header_w->ta->obj->b11 = 1;
        m.header_w->ta->obj->b58 = 1;
    }
    // session && 0044fa40 (*00585fa8 ? !session +17c : 0, the paged game list). PORT-ONLY (132on2b): omega_server
    // sends whole lists, 0044fa40 answers 0.
    m.bc = 0;
}

void GuiRuntime::mlist_sleep(Widget* w) {  // 002abc90
    base_sleep(w);  // 00424a50: 0041ebf0, then vt +70
    send_up(w, 0x15, static_cast<std::int32_t>(w->list->col), static_cast<std::int32_t>(w->list->row));
    w->mls->obj.reset();  // the string object +14, then 0036b130
}

void GuiRuntime::mlist_update(Widget* w, float dt) {  // 002acca0
    base_update(w, dt);  // 0041e3b0
    if (w->mls->source && w->mls->source->changed) mlist_refresh(w);
}

void GuiRuntime::mlist_action(Widget* w, std::uint8_t action) {  // 002abf00
    auto& m = *w->mls;
    if (m.action == action) return;
    m.action = action;
    m.sel_row = m.sel_index = m.sel_id = -1;
    mlist_rebuild(w);
}

void GuiRuntime::mlist_kind(Widget* w, std::uint8_t kind) {  // 002abf40
    auto& m = *w->mls;
    if (kind >= column_layouts.size()) unhosted(0x2abf88, "a column layout past the five of 0048bc60");
    const auto& lay = column_layouts[kind];
    const auto n = static_cast<std::uint32_t>(lay.count);
    array_set_dims(w, n, 0);  // vt +5c
    if (!m.header_w) unhosted(0x2abf9c, "the column header's vt +5c on a missing column_header");
    ta_set_dims(m.header_w, n, 1);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::int32_t type = lay.type[i];
        if (type < 0 || type >= static_cast<std::int32_t>(column_widths.size()))
            unhosted(0x2abfd8, "a column type past the width table 0048bc40");
        const auto width = static_cast<std::uint32_t>(column_widths[static_cast<std::size_t>(type)]);
        array_set_col_width(w, i, width);              // 004248c0
        array_set_col_width(m.header_w, i, width);
        const auto& h = m.header[static_cast<std::size_t>(type)];
        ta_set_cell(m.header_w, i, 0, h ? *h : std::string());  // 00426510, "" (004ab9d0) for NULL
    }
    m.kind = kind;
}

std::int32_t GuiRuntime::mlist_flags(Widget* w, const LobbyRecord* r) {  // 002ac470
    auto& m = *w->mls;
    if (!r) return 0;
    if (!lobby_data()) return 0;
    if (g.lan_mode && r->lan_mismatch) return 2;  // PORT-ONLY (132ln4, §5.3): another build, listed greyed
    const bool own = r->id == m.own_id;
    std::int32_t s1 = 0;
    switch (m.action) {
    case 3: s1 = !own ? 1 : 0; return s1 ? 3 : 0;
    case 1: return 3;
    case 0: s1 = !own && r->at(0x24) == 2 ? 1 : 0; return s1 ? 3 : 0;
    case 2: break;
    default: return 0;
    }
    bool check = (m.b5 && r->at(0x24) == 1) || (r->at(0x24) == 0 && r->at(0x30) == 1);
    if (check) {
        const std::int32_t mission = r->at(0x40);
        if (mission >= 0 && mission < 0x11 && (g.app_248 == -1 || mission == g.app_248)) {
            bool access = true;
            if (g.b_bb5) {  // byte 004ffbb5: the agent must have the level (00332fe0 of 003fda40's key)
                const std::int32_t level = mission_levels[static_cast<std::size_t>(mission)];
                if (level < 0) unhosted(0x2ac5f0, "0048c160[-1] for the online mission 15");
                const std::int32_t p = host_.current_profile();
                access = host_.level_done(p, level_key(cc_level_names[static_cast<std::size_t>(level)])) != 0;
            }
            if (access && r->at(0x34) != 0 && r->at(0x34) < r->at(0x38)) {
                const std::int32_t filter = r->at(0x44);
                if (filter == 2) {
                    s1 = 0;  // PORT-ONLY (132ln4 LAN, 132on2b omega_server): no agency cells, 0044a0d0 answers 0
                } else if (filter == 1) {
                    s1 = 0;  // PORT-ONLY (132ln4 LAN, 132on2b omega_server): no friends lists, 0044d670 answers 0
                } else {
                    s1 = 1;
                }
            }
        }
    }
    if (m.bc && m.kind == 0 && !s1) return cstr_of(r->name).empty() ? 6 : 2;
    return s1 ? 3 : 0;
}

void GuiRuntime::mlist_rebuild(Widget* w) {  // 002ac070
    auto& m = *w->mls;
    m.rows.clear();  // 002ad770(+10c)
    if (!m.source || !lobby_data()) return;
    for (std::size_t i = 0; i < m.source->items.size(); ++i) {
        LobbyRecord& r = m.source->items[i];
        const std::int32_t f = mlist_flags(w, &r);
        if (!(f & 2)) continue;
        if ((f & 4) && r.id == 0) r.id = g.ml_next_id--;  // 0048bcb0
        m.rows.push_back({r.id, static_cast<std::int32_t>(i), f});  // 002ad1f0 at the end
    }
    mlist_sort(w);
    std::int32_t found = -1;
    if (m.sel_id != -1)
        for (std::size_t j = 0; j < m.rows.size(); ++j)
            if (m.rows[j].id == m.sel_id) {
                m.sel_row = static_cast<std::int32_t>(j);
                found = static_cast<std::int32_t>(j);
                break;
            }
    array_set_dims(w, w->list->cols, static_cast<std::uint32_t>(m.rows.size()));  // vt +5c
    const auto count = static_cast<std::int32_t>(m.rows.size());
    if (found == -1) {
        if (m.sel_row == -1) m.sel_row = 0;
        if (!(m.sel_row < count)) m.sel_row = count - 1;
    }
    if (m.sel_row == -1) {
        m.sel_row = m.sel_index = m.sel_id = -1;
    } else {
        const auto& row = m.rows[static_cast<std::size_t>(m.sel_row)];
        const LobbyRecord* r =
            row.index >= 0 && static_cast<std::size_t>(row.index) < m.source->items.size() ? &m.source->items[static_cast<std::size_t>(row.index)] : nullptr;
        if (r) {
            m.sel_id = r->id;
            m.sel_index = row.index;
        } else {
            m.sel_row = m.sel_index = m.sel_id = -1;
        }
    }
    if (!(m.sel_row < count)) m.sel_row = count - 1;
    if (m.sel_row < 0) m.sel_row = 0;
}

void GuiRuntime::mlist_sort(Widget* w) {  // 002ac6f0 (qsort 001273c8; the keys read the source, 005065c0)
    auto& m = *w->mls;
    if (m.rows.empty() || !m.source) return;
    const auto rec = [&](const Widget::MissionList::Row& row) -> const LobbyRecord* {
        return row.index >= 0 && static_cast<std::size_t>(row.index) < m.source->items.size()
                   ? &m.source->items[static_cast<std::size_t>(row.index)]
                   : nullptr;
    };
    using Row = Widget::MissionList::Row;
    switch (m.kind) {
    case 4: return;
    case 3:  // 002ac7e0: games before players' kinds, then the maximum, the agents, the name
        game_qsort(m.rows, 0, static_cast<std::ptrdiff_t>(m.rows.size()), [&](const Row& x, const Row& y) {
            const LobbyRecord* a = rec(x);
            const LobbyRecord* b = rec(y);
            if (!a || !b) return 0;
            std::int32_t ka = a->at(0x24) != 0 ? 3 : 1, kb = b->at(0x24) != 0 ? 3 : 1;
            if (a->at(0x24) == 1) ka = 2;
            if (b->at(0x24) == 1) kb = 2;
            if (ka != kb) return ka - kb;
            ka += a->at(0x38) - 4;
            kb += b->at(0x38) - 4;
            if (ka != kb) return ka - kb;
            ka += a->at(0x34) - 4;
            kb += b->at(0x34) - 4;
            if (ka != kb) return ka - kb;
            return name_cmp(*a, *b);
        });
        return;
    case 2:  // 002ac900
    case 1:  // 002ac980
        game_qsort(m.rows, 0, static_cast<std::ptrdiff_t>(m.rows.size()), [&](const Row& x, const Row& y) {
            const LobbyRecord* a = rec(x);
            const LobbyRecord* b = rec(y);
            if (!a || !b) return 0;
            return name_cmp(*a, *b);
        });
        return;
    case 0:
        if (m.bc) return;
        // 002aca00: by join filter (friends 1, agency cell 2, others 5), kind, maximum, agents, name
        game_qsort(m.rows, 0, static_cast<std::ptrdiff_t>(m.rows.size()), [&](const Row& x, const Row& y) {
            const LobbyRecord* a = rec(x);
            const LobbyRecord* b = rec(y);
            if (!a || !b) return 0;
            std::int32_t ka = 5, kb = 5;
            if (a->at(0x44) == 1) ka = 1;
            if (b->at(0x44) == 1) kb = 1;
            if (a->at(0x44) == 2) ka = 2;
            if (b->at(0x44) == 2) kb = 2;
            if (ka != kb) return ka - kb;
            if (a->at(0x24) == 0) ka = 3;
            if (b->at(0x24) == 0) kb = 3;
            if (a->at(0x24) == 1) ka = 4;
            if (b->at(0x24) == 1) kb = 4;
            if (ka != kb) return ka - kb;
            ka += a->at(0x38) - 4;
            kb += b->at(0x38) - 4;
            if (ka != kb) return ka - kb;
            ka += a->at(0x34) - 4;
            kb += b->at(0x34) - 4;
            if (ka != kb) return ka - kb;
            return name_cmp(*a, *b);
        });
        return;
    default: return;
    }
}

void GuiRuntime::mlist_refresh(Widget* w) {  // 002acb70
    auto& m = *w->mls;
    auto& l = *w->list;
    const std::uint32_t off = l.top < l.row ? l.row - l.top : 0;
    mlist_rebuild(w);
    array_set_dims(w, l.cols, static_cast<std::uint32_t>(m.rows.size()));  // vt +5c
    array_select(w, 0, static_cast<std::uint32_t>(m.sel_row));  // 00424430
    if (!(l.row < l.top)) {
        const std::uint32_t d = l.row - l.top;
        if (d != off) {
            auto top = static_cast<std::int32_t>(d - off + l.top);
            const auto limit = static_cast<std::int32_t>(l.rows - l.vis_rows);
            if (!(top < limit)) top = limit;
            if (top < 0) top = 0;
            l.top = static_cast<std::uint32_t>(top);
        }
    }
    if (!array_cell_visible(w, l.col, l.row)) array_scroll(w, l.col, l.row);  // 004245d0 / 00424670
    if (m.source) m.source->changed = 0;  // 0044f280(+11c, 0)
}

void GuiRuntime::mlist_attach(Widget* w, LobbyList* source) {  // 002acc80
    auto& m = *w->mls;
    m.source = source;
    m.sel_row = m.sel_index = m.sel_id = -1;
    mlist_refresh(w);
}

void GuiRuntime::mlist_entered(Widget* w, std::uint32_t col, std::uint32_t row) {  // vt +6c 002ac300
    auto& m = *w->mls;
    send_up(w, 0x14, static_cast<std::int32_t>(col), static_cast<std::int32_t>(row));  // 004235e0
    m.sel_row = static_cast<std::int32_t>(row);
    if (m.sel_row >= 0 && m.sel_row < static_cast<std::int32_t>(m.rows.size())) {
        const auto& r = m.rows[static_cast<std::size_t>(m.sel_row)];
        m.sel_id = r.id;
        m.sel_index = r.index;
        m.sel_flags = r.flags;
    }
    if (m.bc && m.kind == 0 && host_.net_session()) unhosted(0x2ac3d8, "the lobby's game list page request 00450590");
}

LobbyRecord* GuiRuntime::mlist_entry(Widget* w, std::int32_t row) {  // 002ac400
    auto& m = *w->mls;
    if (!m.source || row < 0 || row >= static_cast<std::int32_t>(m.rows.size())) return nullptr;
    const std::int32_t index = m.rows[static_cast<std::size_t>(row)].index;  // 0044f290(+11c, rows[row] +4)
    if (index < 0 || static_cast<std::size_t>(index) >= m.source->items.size()) return nullptr;
    return &m.source->items[static_cast<std::size_t>(index)];
}

}  // namespace omega::gui

namespace omega::gui {

// 002aad40 up to the draw: 0048bc64's column type of (+ac, col) picks the text.
std::optional<GuiRuntime::MissionListCell> GuiRuntime::mlist_cell(const Widget& w, std::uint32_t col,
                                                                  std::uint32_t row) const {
    const auto& m = *w.mls;
    if (!m.obj || !m.source) return std::nullopt;
    if (row >= m.rows.size()) return std::nullopt;
    const auto& rw = m.rows[row];
    if (rw.index < 0 || static_cast<std::size_t>(rw.index) >= m.source->items.size()) return std::nullopt;
    const LobbyRecord& r = m.source->items[static_cast<std::size_t>(rw.index)];
    std::int32_t agents = r.at(0x34), max = r.at(0x38);
    if (agents == -1) agents = 0;
    if (max == -1) max = 0;
    const bool game_or_one = r.at(0x24) == 0 || r.at(0x24) == 1;
    MissionListCell c;
    const std::array<float, 4> white = {1.0F, 1.0F, 1.0F, 1.0F};                    // 00587c40
    const float half = std::bit_cast<float>(0x3f008081U);
    const std::array<float, 4> grey = {half, half, half, 1.0F};                      // 00587c70
    const float red_gb = std::bit_cast<float>(0x3ea0a0a1U);
    const std::array<float, 4> red = {1.0F, red_gb, red_gb, 1.0F};                   // 00587c50
    c.rgba = (rw.flags & 1) ? white : grey;
    if (m.kind >= column_layouts.size()) return std::nullopt;  // 002aafc0 (the pushed top is left: unreachable)
    if (rw.flags & 4) {
        c.text = "---";  // *0048bc30
        return c;
    }
    if (col >= 4) unhosted(0x2aaffc, "a column past the four of 0048bc64");
    const std::int32_t type = col < 3 ? column_layouts[m.kind].type[col] : -1;  // 0048bc64 + kind * 16 + col * 4
    if (type < 0 || type >= 6) return c;  // the jump table's range (unsigned): the buffer's ""
    switch (type) {
    case 0:
    case 3:  // the name
        c.text = cstr_of(r.name);
        c.b5b = 1;
        break;
    case 1: {  // the mission (+b4), else the record kind's AgentStatus
        bool done = false;
        if (g.lan_mode && r.lan_mismatch) {  // PORT-ONLY (132ln4, §5.3)
            c.text = port_lan_version_mismatch;
            break;
        }
        if (m.b4 && game_or_one && r.at(0x40) >= 0 && r.at(0x40) < 9) {
            const std::int32_t level = mission_levels[static_cast<std::size_t>(r.at(0x40))];
            char key[32];
            std::snprintf(key, sizeof key, "SHORT_LEVEL%d", static_cast<int>(level));  // 0051ef10 (002cc980)
            if (const auto s = host_.get_string(key)) {  // 003fdb80
                c.text = cstr_of(*s);
                done = true;
            }
            if (r.at(0x30) != 1) c.rgba = red;
        }
        if (!done) {
            const std::int32_t k = r.at(0x24);
            if (k < 0 || k >= 4) unhosted(0x2ab0e4, "an AgentStatus index past the four of +d0");
            c.text = m.status[static_cast<std::size_t>(k)];
        }
        break;
    }
    case 2: {  // the player's rank (00450cf0, session only)
        // 00450cf0 reads the lobby's record cache (+fc, filled by 00450880): an entry answers only with its record
        // (+39, stored only from a record whose byte 0 is 0x0b) younger than 00497848 ms; its rank byte (+1) below
        // the rank registry's count (00532b50 +4) draws the row's title (Loc_GetTextById(+20), "S_RANK_%2.2d" of its
        // index + 1); else "---" (0048bc30). Checkpoint 132on2d: the cache is the lobby's (lobby_player_rank).
        c.text = "---";
        if (!host_.net_session()) break;  // 004294c0
        const auto rank = host_.lobby_player_rank(r.id);
        if (!rank || *rank < 0) break;
        const auto ranks = host_.profile_ranks();
        if (static_cast<std::size_t>(*rank) >= ranks.size()) break;
        char key[16];
        std::snprintf(key, sizeof key, "S_RANK_%2.2d",
                      static_cast<int>(ranks[static_cast<std::size_t>(*rank)].index + 1));
        if (const auto s = host_.get_string(key)) c.text = cstr_of(*s);
        break;
    }
    case 4: {  // the join filter
        const auto v = static_cast<std::uint32_t>(r.at(0x44));
        if (v < 6) {
            if (v >= 5) unhosted(0x2ab1a8, "+f4[5] (past the five FilterType texts)");
            c.text = m.filter[v];
        } else {
            c.text = "---";
        }
        break;
    }
    case 5:  // the agents "%d / %d"
        if (r.at(0x24) == 3 || !game_or_one) {
            c.text = "---";
        } else {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%d / %d", static_cast<int>(agents), static_cast<int>(max));  // 004ab980
            c.text = buf;
        }
        break;
    default: break;
    }
    return c;
}

}  // namespace omega::gui
