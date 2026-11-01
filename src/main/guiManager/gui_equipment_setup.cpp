// The network game's level-start roster GuiEquipmentSetup "eqsetup" (checkpoint 134eq1): vtable 004dd7c0, ctor
// 002a47e0 over GuiWidget 0041f690 (EQSETUP.GUI), name 002a1ce0, IsType 002a1cf0, input 002a1d20, event 002a2170,
// render 002a2ec0, update 002a2f00, wake 002a3bb0, init 002a3bf0, dtor 002a45a0; the start level 002a3a10, the
// group move 002a5470 and the figure callback ES_ManRenderWrapper 002a5460 -> 002a2590 (the play's figure draw).
// Every function is the decomp's matched C (src/SFOGame/Source/GuiEquipmentSetup_*.c), except the ctor 002a47e0,
// which is unmatched there and follows the disassembly. GuiEquipmentModify's network paths (002bcd48 / 002bce20,
// 002c03c0's tail 002c0400, 002c0e2c) are in gui_runtime.cpp.
//
// The roster's players are 003fb8d0(i) (the network game's client map) and their status is the network block's
// byte +3584 +32 (cHuman_GetStatus / SetStatus 00184c80 / 00184c60), which the DME library replicates as a field of
// the cNetHuman object (net_registrations: field +0x32, size 1). 0: joined, 1: equipping (EQS_STAT_EQUIP), 2: ready
// (EQS_STAT_READY), 3: the host's start countdown, 4: in the game (EQS_STAT_INGAME).
//
// Not repeated here: 002a2f00 calls cHuman_CheckForDisconnectedNetworkedPlayer 001b2e90 on each remote player and
// cHuman_CheckForChangedWeapons 001b2d20 on each player every frame. The port's LanNetCore runs both natively on every
// remote human each frame in every game state (net_frame, a PORT-ONLY caller), so the screen does not run them again.
#include "gui_runtime.hpp"

#include <algorithm>
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

constexpr float es_pi = 3.1415927f;   // 002a2f00 / 002a3bb0
constexpr std::uint32_t es_render_wrapper = 0x2a5460;   // ES_ManRenderWrapper (the GuiCharacterDisplay +48)

bool es_text(const Widget* w) {   // isA "GuiTextWidget" (004a7520): the text widget and its subclasses
    return w && (w->cls == WidgetClass::text || w->cls == WidgetClass::button || w->cls == WidgetClass::edit);
}

// sprintf(buf, fmt, v) with the countdown's one %d (EQS_COUNTDOWN / 0048ba80 "Starting in %d Seconds...").
std::string es_format(const std::string& fmt, std::int32_t v, std::uint32_t at) {
    std::string out;
    bool used = false;
    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out += fmt[i];
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == 'd' && !used) {
            out += std::to_string(v);
            used = true;
            ++i;
            continue;
        }
        unhosted(at, "sprintf of the countdown format '" + fmt + "' (only one %d is hosted)");
    }
    return out;
}

}  // namespace

// 0035cb30 GetCodename: sprintf("PlayerName%d"), then "[%c%s%c]" (0x11, the string, 0x10) upper-cased into 00533790.
std::string GuiRuntime::eqs_codename(std::int32_t i) {
    const std::string key = "PlayerName" + std::to_string(i);
    const auto name = host_.get_string(key);
    if (!name) unhosted(0x35cb60, "sprintf %s of the missing string '" + key + "'");
    std::string code = std::string("[") + '\x11' + *name + '\x10' + "]";
    for (char& c : code) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80) unhosted(0x12b290, "toupper of a byte >= 0x80 (reads below the ctype table 00499c71)");
        if (u >= 'a' && u <= 'z') c = static_cast<char>(u - 0x20);
    }
    return code;
}

// 002a47e0 (the disassembly; the decomp has no matched C for it). The display actors, the camera object +58 and the
// figure state (turns +60, camera +70, translations +80..+b0) are Widget::EquipSetup's defaults. Then, past the
// four actors (+2c71 = 1 each, 00506518[i] = 0, 00506520[i] = 0):
//   - 0x002a51bc..0x002a5230: the four names of 0048baa0 get bytes 0 / 1 = 22 / 10 and 22 - 22 / 10 (2, 20) and
//     byte 3 = '1';
//   - g_LocalPlayer +30's SetStatus(1) (vt +74), the roster sound of the local player's slot (001ca210(req, 4,
//     0048baa0[slot], 0, 0), 0020f190, +20 = 1, 003c9420(msg, G, 0)) and SetStatus(1) again;
//   - +c0 = +c8 = 0, 00506508 = 0, 00506510 = 0, +d0 = 0, +d8 = 0, +dc = 0; then the names' byte 3 = '2'.
// The front end's 002c9d50 (004ffba0 == 2) is not reached in play.
void GuiRuntime::eqs_construct(Widget* w) {
    auto& e = *w->eqs;
    w->flags &= ~flag_releasable;  // +14 &= 0xffdf
    e.cc = host_.es_disguise_count();  // +cc = *0055de94
    for (std::size_t i = 0; i < 4; ++i) {
        e.figures[i].b2c71 = 1;
        g.es_506518[i] = 0;
        g.es_506520[i] = 0;
    }
    for (auto& name : g.es_48baa0) {
        name[0] = static_cast<char>(22 / 10);
        name[1] = static_cast<char>(22 - 22 / 10);
        name[3] = '1';
    }
    host_.es_set_local_status(1);
    std::optional<std::uint32_t> local_key;
    for (std::int32_t i = 0; i < 4 && !local_key; ++i)
        if (const auto p = host_.es_player(i); p && p->local) local_key = p->key;
    if (!local_key) unhosted(0x2a5254, "g_LocalPlayer +30 +c (the local player is not in the network game's map)");
    const auto slot = static_cast<std::int32_t>((*local_key & 0x7f000000u) >> 24) - 1;
    if (slot < 0 || slot > 3) unhosted(0x2a5288, "a local player slot outside 0048baa0's four names");
    host_.es_sound(g.es_48baa0[static_cast<std::size_t>(slot)], *local_key);
    host_.es_set_local_status(1);
    e.ready_c0 = 0;
    e.c8 = 0;
    g.es_506508 = 0;
    g.es_506510 = 0.0f;
    e.timer = nullptr;
    e.bd8 = 0;
    e.spin_dc = 0.0f;
    for (auto& name : g.es_48baa0) name[3] = '2';
}

// 002a3bf0.
void GuiRuntime::eqs_init(Widget* w) {
    auto& e = *w->eqs;
    base_init(w);  // 0041f150
    if (Widget* cd = find(w, "playercharacter"); cd && cd->cls == WidgetClass::chardisplay) {
        cd->cd_screen = w;  // 00506550 = self
        cd->cd_callback = es_render_wrapper;
    }
    Widget* ready = find(w, "readybutton");
    if (ready && ready->cls != WidgetClass::button) ready = nullptr;  // isA GuiButtonWidget
    auto t = host_.get_string("EQS_START");
    if (g.b_21c8) t = host_.get_string(g.b_21c0 ? "EQS_WAITING" : "EQS_EQUIP");
    if (host_.es_local_status() == 3) t = host_.get_string("EQS_WAITING");
    if (t && ready) set_text(ready, *t, false);
    t = host_.get_string("EQS_STAT_EQUIP");
    for (const char* name : {"1value_status", "2value_status", "3value_status", "4value_status"}) {
        Widget* s = find(w, name);
        if (t && es_text(s)) set_text(s, *t, false);
    }
    for (std::int32_t i = 0; i < 4; ++i) {
        Widget* c = find(w, std::to_string(i + 1) + "value_codename");
        if (!es_text(c)) unhosted(0x2a3d9c, "0041beb0 on the missing \"" + std::to_string(i + 1) + "value_codename\"");
        set_text(c, eqs_codename(i), false);
    }
    std::array<Widget*, 4> slots{};
    for (std::size_t i = 0; i < 4; ++i) slots[i] = find(w, std::to_string(i + 1) + "player_stats_grp");
    const std::int32_t n = g.app_254;  // *004ffc04 +254
    for (std::int32_t j = n; j < 4; ++j)
        if (j >= 0 && slots[static_cast<std::size_t>(j)]) slots[static_cast<std::size_t>(j)]->flags &= ~flag_visible;
    // 002a5470: the group and every descendant moved by (dx, 0).
    auto move = [&](Widget* s, float dx) {
        auto rec = [&](auto& self, Widget* x) -> void {
            if (!x) return;
            set_pos(x, x->rect[0] + dx, x->rect[1] + 0.0f);
            for (Widget* c : x->children) self(self, c);
        };
        rec(rec, s);
    };
    switch (n) {
    case 1:
        e.pos_80[0] = {0.0f, 5.0f, -2.0f, 1.0f};
        move(slots[0], 244.5f);
        break;
    case 2:
        e.pos_80[0] = {-1.4f, 5.0f, -2.0f, 1.0f};
        e.pos_80[1] = {1.4f, 5.0f, -2.0f, 1.0f};
        move(slots[0], 163.0f);
        move(slots[1], 163.0f);
        break;
    case 3:
        e.pos_80[0] = {-2.8f, 5.0f, -2.0f, 1.0f};
        e.pos_80[1] = {0.0f, 5.0f, -2.0f, 1.0f};
        e.pos_80[2] = {2.8f, 5.0f, -2.0f, 1.0f};
        move(slots[0], 81.5f);
        move(slots[1], 81.5f);
        move(slots[2], 81.5f);
        break;
    default:
        break;
    }
    for (const auto& [key, field] : {std::pair{"level_techname1", "levelname1"}, std::pair{"level_techname2", "levelname2"}}) {
        const auto s = host_.get_string(key);
        Widget* f = find(w, field);
        if (s && es_text(f)) {
            set_text(f, *s, false);
            f->flags |= flag_visible;
        }
    }
    e.timer = find(w, "waitingforhost");
    if (!es_text(e.timer)) e.timer = nullptr;
    if (!g.es_506508) {
        if (!e.timer) unhosted(0x2a4510, "EQSETUP without its GuiTextWidget \"waitingforhost\" (+d0 +14 of null)");
        e.timer->flags &= ~flag_visible;
    }
    if (Widget* b = find(w, "equipbutton")) {
        if (!g.app_251) b->flags |= flag_enabled;
        else b->flags &= ~flag_enabled;
    }
    const auto fmt = host_.get_string("EQS_COUNTDOWN");
    e.timer_fmt = fmt ? *fmt : std::string("Starting in %d Seconds...");  // 0048ba80
}

// 002a3bb0: 0041efa0, then +dc = 0 and the angle +68 = pi.
void GuiRuntime::eqs_wake(Widget* w) {
    base_wake(w);
    w->eqs->spin_dc = 0.0f;
    w->eqs->rot_60[2] = es_pi;
}

// 002a1d20.
std::uint8_t GuiRuntime::eqs_input(Widget* w, const PadEvent& ev) {
    auto& e = *w->eqs;
    const bool down = ev.kind == 1 && ev.port == 0;
    auto press = [&](std::uint32_t m) { return down && ev.mask == m && ev.released == 0; };
    if (press(0x1000)) return 1;  // Up
    if (press(0x4000)) return 1;  // Down
    if (press(0x8000) || press(0x2000)) {  // Left / Right: the other button (vt +44 Next(focus, 1))
        Widget* cur = canvas_.focus;
        send(cur, &canvas_, 0x10, 0, 0);
        set_focus(v_next(w, canvas_.focus, true));
        return 1;
    }
    if (press(0x10)) {  // Triangle: the quit popup
        const auto title = host_.get_string("QuitTitle");
        const auto message = host_.get_string("QuitMessage");
        if (title && message) popup_open(5, title, message, w, 0);  // 002cae30(*004ffc04, 5, ...)
        return 1;
    }
    if (down && ev.mask == 2) {
        e.spin_dc += ev.released == 0 ? -1.0f : 1.0f;
        return 1;
    }
    if (down && ev.mask == 1) {
        e.spin_dc += ev.released == 0 ? 1.0f : -1.0f;
        return 1;
    }
    return 0;
}

// 002a2170.
std::uint8_t GuiRuntime::eqs_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    auto& e = *w->eqs;
    eqs_ready_level(w);
    const std::uint32_t m = type & 0xffff;
    if (m == 2 && sender) {
        if (sender->name == "readybutton") {
            if (g.b_21c8) {
                Widget* b = sender->cls == WidgetClass::button ? sender : nullptr;  // isA GuiButtonWidget
                auto label = [&](const char* key, std::uint32_t at) {
                    const auto t = host_.get_string(key);
                    if (!t) return;
                    if (!b) unhosted(at, "0041beb0 on a ready button that is not a GuiButtonWidget");
                    set_text(b, *t, false);
                };
                if (g.b_21c0) {  // the host (005721c0)
                    if (host_.es_local_status() == 3) {
                        if (e.ready_c0 == 2) {
                            host_.eqm_start();           // g_LoadoutPending (004ffc29) = 0; 0036a0f0(0055a300)
                            host_.es_send_play_game();   // 00429b30
                            switch_screens(w, nullptr);  // 00414ac0(canvas, self, 0)
                            host_.es_set_local_status(4);
                        }
                    } else if (e.ready_c0 > 0) {
                        host_.es_set_local_status(3);
                        host_.es_set_byte_5723f8(1);
                        if (!e.bd8) {
                            e.bd8 = 1;
                            host_.es_lobby_started();   // cDME_GetLobby() +26a0 = 1
                        }
                    }
                } else if (host_.es_play_game()) {  // 00572400: the host already started the game
                    host_.eqm_start();  // 004ffc29 = 0; 0036a0f0(0055a300)
                    switch_screens(w, nullptr);
                    host_.es_set_local_status(4);
                } else if (host_.es_local_status() == 1) {
                    host_.es_set_local_status(2);
                    label("EQS_WAITING", 0x2a2440);
                } else {
                    host_.es_set_local_status(1);
                    label("EQS_EQUIP", 0x2a2490);
                }
            } else {
                host_.eqm_start();  // 004ffc29 = 0; 0036a0f0(0055a300)
                switch_screens(w, nullptr);
            }
            return 1;
        }
        if (sender->name == "equipbutton") {
            Widget* pg = factory("GuiEquipmentModify", std::string("eqmodify"));  // 00418a80(00414790(), ...)
            if (!pg) unhosted(0x2a24f0, "00418a80 GuiEquipmentModify \"eqmodify\" failed");
            host_.es_set_local_status(1);
            switch_screens(w, pg);
            return 1;
        }
    }
    if (m == 0x1a && a3 == 0) {  // the quit popup's yes
        switch_screens(w, nullptr);
        host_.app_12fa60(3);  // cGameSystem_Restart(G, 3)
        return 1;
    }
    return base_event(w, sender, type, a3, t0);  // 0041ef10
}

// 002a3a10: +c0 = 2 (solo); in a network game the host sets 0 while the session has more valid clients than the
// client map holds players (002a3ba0), while a client's world send is pending (0042b4e0) or with fewer than two
// clients; any player with status 0 makes it 0, status 1 at most 1.
void GuiRuntime::eqs_ready_level(Widget* w) {
    auto& e = *w->eqs;
    e.ready_c0 = 2;
    if (!g.b_21c8) return;
    if (g.b_21c0) {
        if (host_.es_valid_clients() > host_.es_player_map_count()) e.ready_c0 = 0;
        if (host_.es_admission_pending()) e.ready_c0 = 0;
        if (host_.es_valid_clients() < 2) e.ready_c0 = 0;
    }
    for (std::int32_t i = 0; i < 4; ++i) {
        const auto p = host_.es_player(i);
        if (!p) continue;
        if (p->status == 0) e.ready_c0 = 0;
        else if (p->status == 1 && 1 < e.ready_c0) e.ready_c0 = 1;
    }
}

// 002a2f00.
void GuiRuntime::eqs_update(Widget* w, float dt) {
    auto& e = *w->eqs;
    ++e.frames_506578;
    std::optional<std::int32_t> me;   // 00437600(*00582cf0), read where a headset needs it (no side effects)
    eqs_ready_level(w);
    std::string roster;
    for (std::int32_t i = 0; i < 4; ++i) {
        const std::string digit(1, static_cast<char>('1' + i));
        Widget* name = find(w, digit + "value_agentname");
        if (!es_text(name)) name = nullptr;
        if (name) {
            name->b75 = 1;  // 0041c0f0(w, 1)
            if (name->obj) name->obj->b58 = 1;
            name->b76 = 1;  // 0041c0d0(w, 1)
            if (name->obj) name->obj->b5b = 1;
        }
        Widget* rank = find(w, digit + "value_rank");
        if (!es_text(rank)) rank = nullptr;
        Widget* status = find(w, digit + "value_status");
        if (!es_text(status)) status = nullptr;
        Widget* icon = find(w, digit + "headset");  // isA GuiWidget
        auto text = [&](Widget* f, const std::string& s, std::uint32_t at) {
            if (!f) unhosted(at, "0041beb0 on a missing EQSETUP text field of player " + digit);
            set_text(f, s, false);
        };
        auto need_icon = [&](std::uint32_t at) {
            if (!icon) unhosted(at, "the missing \"" + digit + "headset\" (+14 of null)");
            return icon;
        };
        const auto p = host_.es_player(i);  // 003fb8d0(i)
        if (!p) {
            e.figures[static_cast<std::size_t>(i)].b2c3a = -1;
            need_icon(0x2a3488)->flags &= ~flag_visible;
            text(name, "", 0x2a349c);
            text(rank, "", 0x2a34b0);
            text(status, "", 0x2a34c4);
            g.es_506518[static_cast<std::size_t>(i)] = 0;
            g.es_506520[static_cast<std::size_t>(i)] = 0;
            roster += " " + digit + ":-";
            continue;
        }
        std::int32_t k = static_cast<std::int32_t>((p->key & 0x7f000000u) >> 24) - 1;
        if (k < 0) k = 0;
        if (k > 3) unhosted(0x2a3080, "a player slot above 3 (00506518 / display +48 index)");
        const auto uk = static_cast<std::size_t>(k);
        if (!g.es_506518[uk]) {
            g.es_506518[uk] = 1;
            if (e.cc) unhosted(0x2a30d8, "cMan_AddDisguise 0038f... of a level with DISGUISE_ skeletons (0055de94)");
            if (k) host_.es_sound(g.es_48baa0[uk], p->key);
            host_.es_send_agent_data();  // 0012eee0(G, 0, 0)
        }
        // 003927a0(display[k], player +60): the player's model copied (its attach table +2410), +2c71 = 1.
        auto& fig = e.figures[uk];
        fig.attach = host_.scores_attach(i);
        fig.b2c71 = 1;
        fig.copied = 1;
        fig.b2c3a = 0;  // the copy's +2c3a is the player's model's (+2bda), not negative for a drawn player
        if (host_.scores_agent_b70(i)) {  // 00185c70(p) && +b70
            auto& c = g.es_506520[uk];
            if (c > 10) {
                text(name, host_.scores_agent_name(i), 0x2a3170);  // 00336170
                text(rank, host_.es_agent_rank(i), 0x2a3190);      // 00336110
            } else {
                text(name, "", 0x2a31b0);
                text(rank, "", 0x2a31c4);
            }
            ++c;
        } else {
            text(name, "", 0x2a31e0);
            text(rank, "", 0x2a31f4);
        }
        const std::uint8_t st = p->status;  // vt +78
        std::optional<std::string> t;
        if (st == 0) {
            t = host_.get_string("EQS_STAT_EQUIP");
            e.ready_c0 = 0;
        } else if (st == 1) {
            t = host_.get_string("EQS_STAT_EQUIP");
            if (e.ready_c0 > 1) e.ready_c0 = 1;
        } else if (st == 2) {
            t = host_.get_string("EQS_STAT_READY");
        } else {
            t = host_.get_string("EQS_STAT_INGAME");
        }
        if (!e.timer) unhosted(0x2a3270, "the countdown widget +d0 is null (002a3bf0 found no \"waitingforhost\")");
        if (st == 3) {
            if (e.ready_c0) {
                ++g.es_506508;
                e.timer->flags |= flag_visible;
            } else {
                g.es_506508 = 0;
                e.ready_c0 = 0;
                host_.es_set_status(i, 1);  // vt +74 SetStatus(1)
                e.timer->flags &= ~flag_visible;
                if (host_.es_byte_5723f8()) host_.es_set_byte_5723f8(0);
            }
        } else if (i == 0 && (e.timer->flags & flag_visible)) {
            g.es_506508 = 0;
            e.timer->flags &= ~flag_visible;
        }
        if (!t) unhosted(0x2a32f0, "0041beb0 of a null status string (EQS_STAT_*)");
        text(status, *t, 0x2a32f0);
        if (p->local) host_.es_write_headset(i);  // +3584 +33 = byte 00582b88
        if (p->headset) {  // vt +70
            need_icon(0x2a3330)->flags |= flag_visible;
            if (!me) me = host_.es_headset_owner();
            send(w, icon, 10, i == *me ? (p->local ? 1 : 2) : 0, 0);
        } else {
            need_icon(0x2a3398)->flags &= ~flag_visible;
        }
        if (p->local) g.es_506500 = i;
        char line[48];
        std::snprintf(line, sizeof line, " %s:%08x/%u%s", digit.c_str(), static_cast<unsigned>(p->key),
                      static_cast<unsigned>(p->status), p->local ? "L" : "");
        roster += line;
    }
    if (g.b_21c8) {
        if (g.es_506508) {
            g.es_506510 += dt;
            if (!e.timer) unhosted(0x2a3500, "the countdown widget +d0 is null");
            set_text(e.timer, es_format(e.timer_fmt, 10 - static_cast<std::int32_t>(g.es_506510 / 30.0f), 0x2a34f0),
                     false);
            if (g.b_21c0 && g.es_506510 > 300.0f) host_.es_send_play_game();
        } else {
            g.es_506510 = 0.0f;
        }
    }
    if (g.b_21c8 && g.b_21c0) {
        e.shown_c4 = e.ready_c0;
        const auto t = host_.get_string((e.ready_c0 == 0 || e.ready_c0 >= 3) ? "EQS_WAITING" : "EQS_START");
        Widget* b = find(w, "readybutton");
        if (b && b->cls != WidgetClass::button) b = nullptr;
        if (t && b) set_text(b, *t, false);
    }
    if (canvas_.children.empty() || top() != w) {
        e.spin_dc = 0.0f;
    } else {
        const float r = es_pi * (dt / 30.0f);
        e.rot_60[2] += r * e.spin_dc;
        if (e.rot_60[2] > es_pi) e.rot_60[2] -= 6.2831855f;
        else if (e.rot_60[2] < -es_pi) e.rot_60[2] += 6.2831855f;
    }
    base_update(w, dt);  // 0041e3b0
    roster += " start level " + std::to_string(e.ready_c0) + (g.es_506508 ? " countdown running" : "");
    if (roster != eqs_last_roster) {  // PORT-ONLY log: on a change of a status, the start level or the countdown
        eqs_last_roster = roster;
        eqs_log.push_back("002a2f00 roster:" + roster + " (00506508 = " + std::to_string(g.es_506508) + ")");
    }
}

// 002a45a0: the display actors and the camera object freed, 00506500 = 0.
void GuiRuntime::eqs_destroy(Widget*) { g.es_506500 = 0; }

// 00168550's network tail (0x00168838..0x001688d0) while 004ffc29 is set (loadout_state.cpp): 00438770(0) (the
// headset voice; it returns at once without lgaud, byte 00582b88 clear), then with 00572400 set the pending byte
// cleared, eqsetup popped (the manager's vt +68) and unloaded at once (00418e00(loader, screen, 1)), eqmodify
// popped, the popups closed (002cac80(app, -1) until it answers 0) and the local player's SetStatus(4).
bool GuiRuntime::eqs_level_start_tail() {
    if (!g.b_21c8 || !host_.es_play_game()) return false;
    host_.es_clear_pending();
    if (Widget* s = lookup("GuiEquipmentSetup")) {
        pop(s);
        unload(s, true);
    }
    if (Widget* s = lookup("GuiEquipmentModify")) pop(s);
    while (popup_close(-1)) {
    }
    host_.es_set_local_status(4);
    return true;
}

// 002bcb20 .. 002c03c0's network tail 0x002c0400 (GuiEquipmentModify_onAdvanceTime after 0041e3b0).
void GuiRuntime::em_net_update(Widget* w, float dt) {
    auto& e = *w->em;
    for (std::int32_t i = 0; i < 4; ++i) {
        const auto p = host_.es_player(i);
        if (!p) continue;
        // 001b2e90 / 001b2d20: LanNetCore's per-frame pass runs them (the file header of gui_equipment_setup.cpp).
        if (!e.waiting) unhosted(0x2c0478, "EQMODIFY's +958 \"waitingforhost\" is null");
        if (p->status == 3) {
            ++g.es_506508;
            e.waiting->flags |= flag_visible;
        } else if (i == 0 && (e.waiting->flags & flag_visible)) {
            g.es_506508 = 0;
            g.es_506510 = 0.0f;
            e.waiting->flags &= ~flag_visible;
            if (host_.es_byte_5723f8()) host_.es_set_byte_5723f8(0);
        }
        const auto ui = static_cast<std::size_t>(i);
        if (g.es_506518[ui]) continue;
        g.es_506518[ui] = 1;
        if (host_.es_disguise_count()) unhosted(0x2c0560, "cMan_AddDisguise of a level with DISGUISE_ skeletons (+80)");
        if (i != 0) host_.es_sound(g.es_48baa0[ui], p->key);
        host_.es_send_agent_data();
    }
    if (g.es_506508) {
        g.es_506510 += dt;
        const auto s = es_format(e.countdown, 10 - static_cast<std::int32_t>(g.es_506510 / 30.0f), 0x2c0650);
        if (e.waiting) set_text(e.waiting, s, false);
        if (g.b_21c0 && !(g.es_506510 <= 300.0f)) host_.es_send_play_game();
    }
}

}  // namespace omega::gui
