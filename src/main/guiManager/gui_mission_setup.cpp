// The mission setup screen GuiMissionSetup (checkpoint 132ln3): vtable 004dd730, ctor 002a19c0 over the net screen
// 0033d580 (MSNSETUP.GUI; destroy 002a0cf0, init / wake / sleep / update the net screen's 0033d3f0 / 0033d3a0 /
// 0033d360 / 0033d310, input 002a12f0, event 002a1140, name 002a0d80, IsType 002a0d90, +64 activate 002a1780, +68
// deactivate 002a1700), with begin_mission 002a0f60 and its results 002a0dc0 (created) / 002a0e80 (failed).
// research/FRONTEND.md "Checkpoint 132ln3".
//
// The original reaches it from the online command centre's play_mission (the navigator 0033c320's op 6). Checkpoint
// 132on2b (stage B3, research/ONLINE.md §9.4): with a DME session (004294c0, GuiHost::net_session) activate /
// deactivate move the lobby to and from the game room and begin_mission creates the game (0044e960) and waits for
// the lobby's callback (mset_create); the lobby calls are GuiHost's, unhosted at their addresses unless the port's
// lobby answers them. With no session begin_mission only clears the other players' +b70 and copies the agent count.
//
// PORT-ONLY (research/LAN_PLAY.md §4 row 6b, the user's decision of 2026-10-18): with g.lan_mode set,
// begin_mission keeps the original's wait popup and result bytes 005064a8 / 005064b0 / 005064b8 but hosts the game
// through the LAN session (GuiHost::lan_host: LanTransport::host and the LanDiscoveryHost advertisement) in place
// of the PS2 network start (001698c0 / 00165bc0) and the Medius create game 0044e960. The LAN session completes
// the lobby callback at once: the event 0x1e (a3 1 "created", t0 0) the Medius callback sends to this screen, so
// the original's event handler stores the result and its success path 002a0dc0 runs unchanged. The LAN branch is
// mset_begin's `g.lan_mode` block; everything else is the original's.
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

Widget* of_text(Widget* w) { return w && w->cls == WidgetClass::text ? w : nullptr; }  // vt +58 "GuiTextWidget"

bool pressed(const PadEvent& ev, std::uint32_t mask) {  // +0 == 1, +2 == 0, +8 == mask, +6 == 0
    return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
}

constexpr std::uint32_t pad_left = 0x8000, pad_right = 0x2000, pad_square = 0x80;

// 002a1a50(v, lo, hi, up): one step up or down, wrapping past either end.
std::int32_t step_wrap(std::int32_t v, std::int32_t lo, std::int32_t hi, bool up) {
    v = up ? v + 1 : v - 1;
    if (v < lo) v = hi;
    if (hi < v) v = lo;
    return v;
}

// 004acb30: the online missions' level indices (the command centre's app +248 indexes it).
constexpr std::array<std::int32_t, 9> online_mission_levels = {0, 1, 2, 4, 5, 8, 12, 13, 16};

}  // namespace

void GuiRuntime::mset_init(Widget* w) {  // 0033d3f0: 0041f150, then +7c start_button
    base_init(w);
    w->ms->start_button = find(w, "start_button");
}

void GuiRuntime::mset_wake(Widget* w) {  // 0033d3a0: 0041efa0, vt +64, start_button +10 = -1
    auto& m = *w->ms;
    base_wake(w);
    mset_activate(w);
    if (m.start_button) m.start_button->id10 = -1;
}

void GuiRuntime::mset_sleep(Widget* w) {  // 0033d360: 0041ebf0, vt +68
    base_sleep(w);
    mset_deactivate(w);
}

void GuiRuntime::mset_update(Widget* w, float dt) {  // 0033d310: 0041e3b0, vt +60 0033cca0, byte 00532fe8
    base_update(w, dt);
    net_poll(w, w->ms->start_button);  // 0033cca0
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
}

void GuiRuntime::mset_activate(Widget* w) {  // 002a1780
    auto& m = *w->ms;
    if (m.active) return;
    m.num_agents = find(w, "num_agents");                // 0048ba40
    m.join_filter = find(w, "join_filter");              // 0048ba48
    m.begin = find(w, "begin_mission");                  // 0048ba60
    m.num_agents_txt = of_text(find(w, "num_agents_txt"));    // 0048ba50
    m.join_filter_txt = of_text(find(w, "join_filter_txt"));  // 0048ba58
    if (Widget* t = of_text(find(w, "mission_name"))) set_text(t, cstr_of(g.app_1c8), false);  // 0048ba68, app +1c8
    mset_texts(w);
    m.room = 0;
    if (host_.net_session()) {  // 002a18c8: the lobby's main channel left, else the game room
        if (host_.lobby_state() == 4) {
            host_.lobby_channel_leave();  // 004426b0
        } else if (host_.lobby_state() != 5) {
            m.room = host_.lobby_room_open() ? 1 : 0;  // 004518f0(session, 002cc260, +88, +a4, +c0)
            if (!m.room) popup_open(2, std::string(), lobby_string(0x5a), w, 0xb);  // 002cae30
        }
    }
    g.r_64a8 = 0;
    g.r_64b0 = 0;
    g.r_64b8 = 0;
    m.active = 1;
}

void GuiRuntime::mset_deactivate(Widget* w) {  // 002a1700
    auto& m = *w->ms;
    if (!m.active) return;
    if (m.room) {
        if (host_.net_session()) host_.lobby_room_leave();  // 00451610
        m.room = 0;
    }
    // 0044f6b0 on +88 / +a4 / +c0: the lists are empty (see Widget::MissionSetup)
    m.active = 0;
}

std::uint8_t GuiRuntime::mset_input(Widget* w, const PadEvent& ev) {  // 002a12f0
    auto& m = *w->ms;
    // LEFT / RIGHT step the focused setting (the pointers are compared as they are, null included); the event
    // then goes on to the tests below and, unmatched, to the net screen input, which leaves it unhandled.
    if (pressed(ev, pad_left) || pressed(ev, pad_right)) {
        const bool up = ev.mask == pad_right;
        if (w->focus == m.num_agents) mset_agents(w, up);
        else if (w->focus == m.join_filter) mset_filter(w, up);
    }
    if (pressed(ev, pad_triangle)) {
        // back to a waiting join screen (00418980 "GuiNetJoinMission"), else the navigator's op 1 (CMDCENTR)
        if (Widget* j = lookup("GuiNetJoinMission")) {
            pop(w);  // canvas vt +68
            j->flags |= flag_visible;
        } else {
            net_navigate(w, 1);
        }
        return 1;
    }
    if (pressed(ev, pad_square)) {
        mset_begin(w);
        return 1;
    }
    // 0033cef0: +78 is 0 (nothing in this class sets it)
    if (m.b78) unhosted(0x33cf54, "net screen input");
    return 0;
}

std::uint8_t GuiRuntime::mset_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 002a1140
    auto& m = *w->ms;
    const std::uint32_t t = type & 0xffff;
    if ((t == 0x1b || t == 0x1a) && a3 == 0xb) {  // the room error popup's reply: back to the command centre
        net_navigate(w, 1);
        return 1;
    }
    if (t == 2) {
        if (!sender) unhosted(0x2a11c0, "a mission setup activation without a sender (reads sender +4)");
        if (sender->name == "begin_mission") {
            mset_begin(w);
            return 1;
        }
        if (sender->name == "num_agents") {
            mset_agents(w, true);
            return 1;
        }
        if (sender->name == "join_filter") {
            mset_filter(w, true);
            return 1;
        }
    }
    if (t == 0x10 && a3 == 1 && (sender == m.join_filter || sender == m.num_agents)) return 1;
    if (t == 0x1e && !g.app_32) {  // the lobby's create-game callback (002a126c)
        g.r_64b0 = a3;
        g.r_64b8 = t0;
        g.r_64a8 = 1;
        return 1;
    }
    return net_event(w, sender, type, a3, t0);  // 0033cf90
}

void GuiRuntime::mset_agents(Widget* w, bool up) {  // 002a1590: app +180 in 2..4
    g.app_180 = step_wrap(g.app_180, 2, 4, up);
    mset_texts(w);
    send(w->ms->num_agents, &canvas_, 0x10, 0, 0);  // 0041e0f0
}

void GuiRuntime::mset_filter(Widget* w, bool up) {  // 002a1600: app +185 in 0..2
    g.app_185 = static_cast<std::uint8_t>(step_wrap(g.app_185, 0, 2, up));
    mset_texts(w);
    send(w->ms->join_filter, &canvas_, 0x10, 0, 0);
}

void GuiRuntime::mset_texts(Widget* w) {  // 002a1670
    auto& m = *w->ms;
    char buf[0x20];
    std::snprintf(buf, sizeof buf, "FilterType%d", static_cast<int>(g.app_185));  // 004ab018
    const auto s = host_.get_string(buf);  // 003fdb50, "" (004aaff0) for NULL
    if (!m.join_filter_txt) unhosted(0x2a16c0, "0041beb0 on a missing join_filter_txt");
    set_text(m.join_filter_txt, s ? cstr_of(*s) : std::string(), false);
    std::snprintf(buf, sizeof buf, "%d", static_cast<int>(g.app_180));  // 004ab028
    if (!m.num_agents_txt) unhosted(0x2a16e8, "0041beb0 on a missing num_agents_txt");
    set_text(m.num_agents_txt, buf, false);
}

void GuiRuntime::mset_begin(Widget* w) {  // 002a0f60
    host_.profiles_clear_b70();  // 0012ee30(004ffb50)
    g.app_254 = g.app_180;
    if (g.app_32) return;
    if (host_.net_session()) {
        mset_create(w);
        return;
    }
    if (!g.lan_mode) return;  // the original with no session: nothing more
    // PORT-ONLY (132ln3, LAN_PLAY.md §4 row 6b): the LAN game in place of the PS2 network start and 0044e960.
    const std::int32_t mission = g.app_248;
    const std::string wait = lobby_string(0x16);
    const std::int32_t popup = popup_open(0, std::string(), wait, nullptr, 0);
    g.r_64a8 = 0;
    g.r_64b0 = 0;
    g.r_64b8 = 0;
    GuiHost::LanHostRequest request;
    request.host_name = cstr_of(host_.profile_name(host_.current_profile()).value_or(""));
    request.mission = mission;
    request.num_agents = g.app_180;
    request.join_filter = g.app_185;
    const bool created = host_.lan_host(request);
    // 0044e960 -> 0042a870(0): Net_StartSession's create stores the host role 005721c0 = 1 on success (0042aac8)
    if (created) g.b_21c0 = 1;
    if (!created) {  // as 0044e960 failing with 00452ff0's 0x31: 002a0e80(this, 1), LOBBY_STR89
        mset_failed(w, true);
        popup_close(popup);
        return;
    }
    send(nullptr, w, 0x1e, 1, 0);  // the lobby callback, completed at once with "created"
    // The original's wait loop (002cc260 + 004536b0 until 005064a8) finds the result already set.
    if (!g.r_64a8) unhosted(0x2a10a8, "the LAN create-game result did not arrive");
    if (g.r_64b0 != 0 && created) {
        mset_created(w);
        popup_close(popup);
        return;
    }
    mset_failed(w, g.r_64b0 != 0);
    popup_close(popup);
}

void GuiRuntime::mset_create(Widget* w) {  // 002a0fc8: begin_mission with a session
    const std::int32_t mission = g.app_248;
    const std::int32_t popup = popup_open(0, std::string(), lobby_string(0x16), nullptr, 0);
    host_.lobby_net_start();  // 001698c0 / 00165bc0
    g.r_64a8 = 0;
    g.r_64b0 = 0;
    g.r_64b8 = 0;
    const std::int32_t sent = host_.lobby_create(mission, g.app_180, g.app_185);  // 0044e960
    // 0044e960 -> 0042a870(0): the session's create stores the host role 005721c0 = 1 (0042aac8)
    if (sent) g.b_21c0 = 1;
    if (!sent && (host_.lobby_error() & 0xff) == 0x31) {
        mset_failed(w, true);
        popup_close(popup);
        return;
    }
    // Until the callback (event 0x1e) arrives: 002cc260(0, 0) and the lobby update 004536b0. The original waits
    // on with no end when the request was not sent (0x32); a host's pump completes every request it takes.
    while (!g.r_64a8) {
        host_.card_poll();
        if (const auto cb = host_.lobby_pump()) send(nullptr, w, 0x1e, cb->a3, cb->t0);
    }
    if (g.r_64b0 != 0 && sent == 1) mset_created(w);
    else mset_failed(w, g.r_64b0 != 0);
    popup_close(popup);
}

void GuiRuntime::mset_created(Widget*) {  // 002a0dc0
    g.app_state_prev = g.app_state;
    g.app_state = 3;
    g.app_35 = 1;
    g.app_33 = 1;
    const std::int32_t mission = g.app_248;
    if (mission != -1) {
        if (mission < 0 || mission >= static_cast<std::int32_t>(online_mission_levels.size()))
            unhosted(0x2a0e40, "app +248 outside the online mission table 004acb30");
        const std::int32_t p = host_.current_profile();
        host_.level_progress(p, online_mission_levels[static_cast<std::size_t>(mission)] + 1, 6, 1);  // 00332a10
    }
    g.r_64a8 = 0;
    g.r_64b0 = 0;
    g.r_64b8 = 0;
}

void GuiRuntime::mset_failed(Widget*, bool lobby_text) {  // 002a0e80
    if (lobby_text) {
        const std::string m = lobby_string(0x59);
        popup_open(2, std::string(), m, nullptr, 0);
    } else {
        popup_open(2, std::string(), host_.get_string(lobby_callback_key(g.r_64b8)), nullptr, 0);  // 003fdb50
    }
    if (g.r_64b0 != 0) host_.lobby_game_cleanup();  // 0042b1f0
    g.r_64a8 = 0;
    g.r_64b0 = 0;
    g.r_64b8 = 0;
}

}  // namespace omega::gui
