// The online options screen GuiOnlineOptions (checkpoint 132on2c): vtable 004ddaf0, ctor 002aa110 over the net
// screen 0033d580 (+84 = 5). ONLIPREF.GUI: the agent's universe (region) choice. research/ONLINE.md §9.7.
//
//   init 002a9fe0, wake / sleep / update the net screen's 0033d3a0 / 0033d360 / 0033d310, input 002a98d0, event
//   002a97f0, name 002a9660, IsType 002a9670, +5c (the save flow's end) 002a97e0 -> 002a96a0(this, 0), +64 activate
//   002a9ba0, +68 deactivate 002a9b50; the apply 002a9a20.
//
// The navigator's op 5 opens it: from ONLINECN's MUIS step (0029d390, app +268 = 0), ACNTLOGN's Circle (app +268 =
// 7) and JOINMIS' "online_prefs" (app +268 = 2); its end goes back through the navigator's op app +268. The universe
// list comes from the lobby (00442f40 / 00442d80 / 00442e90, GuiHost::lobby_universes_begin / lobby_universe_list /
// lobby_universes_end); the choice is selected (00442840) and stored in the agent (+9a0 the id, +95c the name), then
// saved (00441a10 mode 1).
#include "gui_runtime.hpp"

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

constexpr std::uint32_t pad_square = 0x80;
constexpr std::uint32_t agent_universe_id = 0x9a0;    // the agent's universe id (word)
constexpr std::uint32_t agent_universe_name = 0x95c;  // the agent's universe name (0x40 bytes)

std::string text_of(const Widget* w) { return cstr_of(w->text); }  // 001692d0(+48)

}  // namespace

void GuiRuntime::opt_init(Widget* w) {  // 002a9fe0
    auto& o = *w->opt;
    base_init(w);  // 0033d3f0: 0041f150, then +7c start_button
    o.start_button = find(w, "start_button");
    o.label = of_text(find(w, "label_muis_server"));                         // *0048bc00
    o.list = of_class(find(w, "config_list"), WidgetClass::textlist);         // *0048bc08 isA "GuiTextListWidget"
    o.value = of_text(find(w, "value_region"));                               // *0048bbf0
    o.select = find(w, "select_config_select");                               // *0048bc10
    if (o.select && o.list) o.list->list->cursor = o.select;  // list +88
}

void GuiRuntime::opt_wake(Widget* w) {  // 0033d3a0: 0041efa0, vt +64, start_button +10 = -1
    base_wake(w);
    opt_activate(w);
    if (w->opt->start_button) w->opt->start_button->id10 = -1;
}

void GuiRuntime::opt_sleep(Widget* w) {  // 0033d360: 0041ebf0, vt +68
    base_sleep(w);
    opt_deactivate(w);
}

void GuiRuntime::opt_net_update(Widget* w, float dt) {  // 0033d310: 0041e3b0, vt +60 0033cca0, byte 00532fe8
    base_update(w, dt);
    net_poll(w, w->opt->start_button);  // 0033cca0
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
}

void GuiRuntime::opt_activate(Widget* w) {  // 002a9ba0
    auto& o = *w->opt;
    if (o.active) return;
    const bool session = host_.net_session();  // 004294c0
    if (session) {
        if (host_.lobby_state() == 4) host_.lobby_channel_leave();  // session +255c; 004426b0
        if (!host_.lobby_universes_begin()) {                       // 00442f40(session, 002cc260, +a0)
            const std::string m = lobby_string(0x72);
            popup_open(2, std::string(), m, w, 0x1e);
            return;
        }
    }
    o.active = 1;
    if (o.label) {
        o.label->b75 = 1;  // 0041c0f0
        if (o.label->obj) o.label->obj->b58 = 1;
        set_text(o.label, cstr_of(host_.get_string("GameTitle").value_or("")), false);  // 004ab810
    }
    const std::int32_t p = host_.current_profile();  // 001314f0(004ffb50, -1)
    if (Widget* t = of_text(find(w, "value_agentsname")))  // *0048bbf8
        set_text(t, cstr_of(host_.profile_name(p).value_or("")), false);  // 00336170
    if (Widget* t = of_text(find(w, "value_current_server")))  // *0048bbe8
        if (o.label) set_text(t, text_of(o.label), false);
    o.picked = -1;
    const std::int32_t p2 = host_.current_profile();  // 002a9d8c: 001314f0(004ffb50, -1) again
    const std::int32_t id = host_.profile_word(p2, agent_universe_id);                // s3
    const std::string region = cstr_of(host_.profile_text(p2, agent_universe_name));  // s4
    if (o.list) {
        list_clear(o.list);  // 00421070
        if (!session) unhosted(0x2a9db0, "the universe list request 00442d80 with no session");
        const bool listed = (host_.lobby_universe_list(o.universes) & 0xff) != 0;  // 00442d80
        if (listed) {
            for (std::size_t i = 0; i < o.universes.size(); ++i) {  // 0044f290(+a0, i)
                std::string name = cstr_of(o.universes[i].name);    // 0013b480(tmp, +4)
                font_text(name);                                     // 003fd740
                list_add(o.list, name, static_cast<std::int32_t>(i), 0);  // 00421290
                if (o.universes[i].id == id && name == region) o.current = static_cast<std::int32_t>(i);  // 00129218
            }
        }
        if (!listed || o.list->list->items.empty()) {  // list +94
            const std::string m = lobby_string(0x72);
            popup_open(2, std::string(), m, w, 0x1e);
            return;
        }
    }
    if (o.current == -1) {
        const std::string m = lobby_string(0x6f);
        popup_open(2, std::string(), m, nullptr, 0);
        if (o.value) set_text(o.value, cstr_of(host_.get_string("NotSet").value_or("")), false);  // 004ab820
        host_.profile_set_word(p2, agent_universe_id, -1);
        host_.profile_set_text(p2, agent_universe_name, std::string());  // 00129360(+95c, "")
    } else {
        if (o.value) set_text(o.value, region, false);
        if (o.list) array_select(o.list, 0, static_cast<std::uint32_t>(o.current));  // 00424430
    }
    o.picked = o.current;
}

void GuiRuntime::opt_deactivate(Widget* w) {  // 002a9b50
    auto& o = *w->opt;
    if (!o.active) return;
    if (host_.net_session()) host_.lobby_universes_end();  // 00442e90
    o.universes.clear();                                   // 0044f6b0(+a0)
    o.active = 0;
}

// 002a96a0(this, a1): with a1 and no universe the "choose one" message, else back through the navigator's op app
// +268 (its failure: the message LOBBY_STR 0x71 and app +268 = 1, CMDCENTR next time).
void GuiRuntime::opt_end(Widget* w, std::uint8_t a1) {
    auto& o = *w->opt;
    if (a1 && o.current == -1) {
        const std::string t = lobby_string(0x73);
        const std::string m = lobby_string(0x74);
        popup_open(2, t, m, nullptr, 0);
        return;
    }
    if (!net_navigate(w, g.app_268)) {  // 0033c320
        const std::string m = lobby_string(0x71);
        popup_open(2, std::string(), m, nullptr, 0);
        g.app_268 = 1;
    }
}

void GuiRuntime::opt_apply(Widget* w) {  // 002a9a20
    auto& o = *w->opt;
    if (host_.net_session()) {
        const auto i = static_cast<std::size_t>(o.picked);
        if (o.picked >= 0 && i < o.universes.size()) {  // 0044f290(+a0, +9c)
            if (!host_.lobby_universe_select(o.picked)) {  // 00442840(session, +9c)
                const std::string m = lobby_string(0x70);
                popup_open(2, std::string(), m, nullptr, 0);
                return;
            }
            const std::int32_t p = host_.current_profile();
            std::string name = cstr_of(o.universes[i].name);
            font_text(name);  // 003fd740
            host_.profile_set_text(p, agent_universe_name, name);  // 00129360(+95c)
            host_.profile_set_word(p, agent_universe_id, o.universes[i].id);
            o.current = o.picked;
        }
    }
    o.save.mode = 1;
    save_start(w);  // 00441a10; its end is vt +5c, 002a96a0(this, 0)
}

std::uint8_t GuiRuntime::opt_input(Widget* w, const PadEvent& ev) {  // 002a98d0
    auto& o = *w->opt;
    if (pressed(ev, pad_triangle)) {
        if (o.current == -1 && o.picked != -1) opt_apply(w);
        else opt_end(w, 1);
        return 1;
    }
    if (pressed(ev, pad_square)) {
        if (o.picked == -1 || o.picked == o.current) opt_end(w, 1);
        else opt_apply(w);
        return 1;
    }
    // 0033cef0: +78 is 0 (nothing in this class sets it)
    if (o.b78) unhosted(0x33cf54, "net screen input");
    return 0;
}

std::uint8_t GuiRuntime::opt_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                   std::int32_t t0) {  // 002a97f0
    auto& o = *w->opt;
    const std::uint32_t t = type & 0xffff;
    if (sender == o.list && o.list && t == 0x13) {  // the list's pick: t0 the row
        o.picked = list_id(o.list, t0);  // 00421480
        if (o.value) set_text(o.value, cstr_of(list_text(o.list, t0).value_or("")), false);  // 00421430
    }
    if (t == 0x1a && a3 == 0x1e) {  // the failure message's OK
        opt_end(w, 0);
        return 1;
    }
    return net_event(w, sender, type, a3, t0);  // 0033cf90
}

}  // namespace omega::gui
