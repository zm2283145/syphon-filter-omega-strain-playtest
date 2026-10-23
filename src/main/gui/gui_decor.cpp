#include "gui_decor.hpp"
#include "zone_profile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>

namespace omega {

using gui::Widget;
using gui::WidgetClass;

namespace {

[[noreturn]] void fail(std::uint32_t address, const std::string& what) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", address);
    throw std::runtime_error("gui decor: unhosted " + what + " (" + buf + ")");
}

// cvt.s.w as the EE interpreter models it (round to nearest even; the
// goldens hold 16777217 / -16777219 rows).
float cvt_s_w(std::int32_t v) { return static_cast<float>(v); }

std::string upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// The 0x6 names the runtime's senders pass as a3 (string addresses).
const std::string* event_name(std::int32_t a3) {
    static const std::map<std::int32_t, std::string> names = {
        {0x4b8cb8, "onGainFocus"}, {0x4b8cc8, "onLoseFocus"},  // GuiKeyboard 0032c...
        {0x4aa2c8, "onStateOn"},   {0x4aa2d8, "onLoseFocus"},  {0x4aa2e8, "onStateOff"},
        {0x4aa2f8, "onGainFocus"}, {0x4aa308, "onDeactivate"},  // GuiCommandCenter
        {0x4bbc58, "onGainFocus"},                              // GuiObjectives 00367b20
        {0x4ac360, "onPressed"},                                // GuiEquipmentModify 002bcb50 / 002bd560
        {0x4c21f8, "onWake"},      {0x4c2200, "onSleep"},
        {0x4c2278, "onActivate"},                               // GuiGameScreen 0045ca20 / 0045d960
        {0x4c2470, "onDeactivate"}, {0x4c2498, "onActivate"},  // GuiEndGameAwards 0045f4d0
        {0x4ba850, "onActivate"},  {0x4ba860, "onDeactivate"},  // GuiRatings 0034d600
        {0x4b9c68, "onPressed"},                                // GuiLobbyScreen 0033e2a0 (132ln4)
        {0x4b9b58, "onActivate"},  {0x4b9b68, "onWake"},        // the net screens' poll 0033cca0 (132on2b)
        {0x4ba378, "onWake"},                                   // GuiNetMessages' buttons 00346fb0 (132on2h)
    };
    const auto it = names.find(a3);
    return it == names.end() ? nullptr : &it->second;
}

}  // namespace

int gui_deco_find(const std::vector<GuiEvent>& records, int last, const std::string* name) {  // 00416e40
    if (!name || name->empty()) return last;
    for (std::size_t i = 0; i < records.size(); ++i)
        if (records[i].name == *name) return static_cast<int>(i);
    return -1;
}

std::uint8_t gui_deco_event(IeAnim* el, const std::vector<GuiEvent>& records, int& last, const std::string* name) {
    if (!el) return 0;  // 00416d24
    const int r = gui_deco_find(records, last, name);
    if (r < 0) return 0;
    const GuiEvent& e = records[static_cast<std::size_t>(r)];
    const float a = cvt_s_w(e.a), b = cvt_s_w(e.b);
    switch (e.action & 0xff) {  // lbu +c
    case 1:
        ie_anim_evaluate(*el, a);
        ie_anim_fade(*el, b);
        break;
    case 2:
        ie_anim_evaluate(*el, a);
        ie_anim_range(*el, a, b, 1);
        break;
    case 3:
        ie_anim_evaluate(*el, a);
        ie_anim_range(*el, a, b, 0);
        break;
    default: break;
    }
    last = r;  // 00416e08
    return 1;
}

std::uint8_t gui_deco_settled(const IeAnim* el, const std::vector<GuiEvent>& records, int last) {  // 00417400
    if (last < 0 || !el) return 1;
    const GuiEvent& e = records[static_cast<std::size_t>(last)];
    if ((e.action & 0xff) != 1) return 1;
    return cvt_s_w(e.b) == el->t ? 1 : 0;
}

GuiDecoResult gui_deco_switch(IeAnim* el, const std::vector<GuiEvent>& records, int& last, std::uint32_t type,
                              const GuiDecoArgs& args) {
    GuiDecoResult r;
    const std::uint32_t t = type & 0xffff;
    switch (t) {
    case 4: {  // 00417520
        const int f = gui_deco_find(records, last, args.name);
        if (f >= 0 && args.t0_out) {
            r.record = f;
            r.value = std::uint8_t{1};
        } else {
            r.value = std::uint8_t{0};
        }
        return r;
    }
    case 5:  // 00417468: *a3 = d+24 (the caller's element pointer)
        r.value = std::uint8_t{1};
        return r;
    case 6:  // 00417458
        gui_deco_event(el, records, last, args.name);
        r.value = std::uint8_t{0};
        return r;
    case 7:  // 00417480
        if (!args.step || *args.step == 0.0F) {
            r.value = std::uint8_t{1};
            return r;
        }
        if (el) ie_anim_update(*el, *args.step);
        r.value = std::uint8_t{0};
        return r;
    case 8:  // 004174f0: *a3 = element vt +c (003e7ed0 -> 003ee740)
        if (args.a3 != 0 && el) fail(0x3ee740, "the element duration (decorator event 8)");
        r.value = std::uint8_t{1};
        return r;
    case 9:  // 004174c8
        if (args.a3 != 0 && el) r.out = el->t;
        r.value = std::uint8_t{1};
        return r;
    case 0xa:
    case 0xb:
    case 0xc:
    case 0xd:
    case 0xe: {  // 00417550
        if (!el) return r;  // the widget's +3c
        const float a = cvt_s_w(args.a3);
        if (t == 0xa) {
            ie_anim_evaluate(*el, a);
            ie_anim_fade(*el, a);
        } else if (t == 0xb) {
            ie_anim_fade(*el, a);
        } else if (t == 0xc) {
            ie_anim_evaluate(*el, a);
            ie_anim_fade(*el, cvt_s_w(args.t0));
        } else {
            const float b = cvt_s_w(args.t0);
            if (el->t < a || !(el->t <= b)) ie_anim_evaluate(*el, a);
            ie_anim_range(*el, a, b, t == 0xe ? 1 : 0);
        }
        last = -1;  // 004176b0
        r.value = std::uint8_t{0};
        return r;
    }
    case 0xf: r.value = gui_deco_settled(el, records, last); return r;
    case 0x12: fail(0x4176c0, "decorator event 0x12 (the runtime's deco_event)");
    default: return r;  // 00417758: the widget's +3c
    }
}

std::string gui_deco_node_name(const std::string& widget_name, bool has_parent, const std::string& node) {  // 004178e0
    std::string name = node.empty() ? widget_name : node;
    // 004179b4 / 00417a44: a name reaching 0x7e characters is cut with "\n".
    if (name.size() >= 0x7d) fail(0x4179c0, "a decorator node name of 0x7d characters or more");
    if (!has_parent) {
        if (name.size() + 5 >= 0x7e) fail(0x417a50, "a decorator root node name cut by the 0x7e limit");
        name += "_root";  // 004bf070
    }
    return name;
}

// ---------------------------------------------------------------- GuiDecor

IeScreen* GuiDecor::screen(const std::string& upper_name) {
    const auto it = screens_.find(upper_name);
    if (it != screens_.end()) return it->second.get();
    const auto ie = resolver_(upper_name);
    if (!ie || !ie->file) return screens_.emplace(upper_name, nullptr).first->second.get();
    auto s = std::make_unique<IeScreen>(IeScreen::load(*ie->file, text_word_, ie->textures));
    return screens_.emplace(upper_name, std::move(s)).first->second.get();
}

GuiDecor::Entry GuiDecor::make_entry(const Widget* w, const GuiNode& n, const std::string& iface, bool root) {
    Entry e;
    e.widget = w;
    e.records = &n.decorator->events;
    const auto& m = n.decorator->matrix;  // 00416f70 (d+30)
    e.seen = {m[0], m[1], m[2], 0.0F, m[3], m[4], m[5], 0.0F, m[6], m[7], m[8], 0.0F, m[9], m[10], m[11], 1.0F};
    // 00412f80(00571dc0, d+8) / 00413370: the interface.
    // The runtime's d+8 (load_node: the file's name, else the manager's +2c
    // at load); for a pruned node the same rule with the current +2c.
    const std::string ie = w && w->deco ? w->deco->ie : n.decorator->ie.empty() ? iface : n.decorator->ie;
    IeScreen* s = screen(upper(ie));
    if (!s) return e;
    const int node = s->find_node(s->root(), gui_deco_node_name(n.name, !root, n.decorator->node));  // 003ee840
    if (node < 0) return e;
    e.screen = s;
    e.element = s->create_element(node);  // 003ef010
    s->show_element(e.element, 0);        // 003e8de0(el, 0)
    s->element_at(e.element).matrix = e.seen;  // 003e8040(el, d+30)
    return e;
}

void GuiDecor::attach_node(const Widget* w, const GuiNode& n, const std::string& iface, bool root) {
    // 004178e0 runs the widget's +c first: the children's decorators init
    // before the node's own.
    std::size_t next = 0;
    for (const GuiNode& c : n.children) {
        const Widget* cw = nullptr;
        if (w) {
            for (std::size_t k = next; k < w->children.size(); ++k) {
                if (w->children[k]->name == c.name) {
                    cw = w->children[k];
                    next = k + 1;
                    break;
                }
            }
        }
        attach_node(cw, c, iface, false);
    }
    if (n.decorator_class.empty()) return;
    if (n.decorator_class != "GuiInterfaceDecorator" || !n.decorator)
        fail(0x418b10, "decorator class '" + n.decorator_class + "'");
    if (w && !w->deco) throw std::runtime_error("gui decor: runtime widget '" + w->name + "' has no decorator");
    Entry e = make_entry(w, n, iface, root);
    if (w) {
        entries_[w] = e;
        ++entries_serial_;
    }
    else unloaded_.emplace_back(n.name, e);
}

void GuiDecor::attach(const gui::GuiRuntime& rt, const Widget& root, const GuiNode& file_root) {
    if (root.name != file_root.name) throw std::runtime_error("gui decor: attach root name mismatch");
    attach_node(&root, file_root, rt.g.iface, true);
    roots_.push_back(&root);
    std::vector<const Widget*> all;
    collect(&root, all);
    for (const Widget* w : all) awake_[w] = (w->flags & gui::flag_awake) != 0;
}

const GuiDecor::Entry* GuiDecor::entry(const Widget& w) const {
    const auto it = entries_.find(&w);
    return it == entries_.end() ? nullptr : &it->second;
}
GuiDecor::Entry* GuiDecor::entry(const Widget& w) {
    const auto it = entries_.find(&w);
    return it == entries_.end() ? nullptr : &it->second;
}
IeAnim* GuiDecor::anim(const Widget& w) {
    Entry* e = entry(w);
    if (!e || e->element < 0) return nullptr;
    return &e->screen->element_at(e->element).anim;
}

bool GuiDecor::named(const Widget& w, const char* name) {
    Entry* e = entry(w);
    if (!e || e->element < 0) return false;  // 00416d24: no element
    const std::string n = name;
    gui_deco_event(anim(w), *e->records, e->last, &n);
    return true;
}

void GuiDecor::on_update(const Widget& w, float dt) {  // 00417790
    OMEGA_ZONE("decor.on_update");
    if (IeAnim* a = anim(w)) ie_anim_update(*a, dt);
}

GuiDecoResult GuiDecor::on_event(const Widget& w, std::uint32_t type, const GuiDecoArgs& args) {
    Entry* e = entry(w);
    if (!e) throw std::runtime_error("gui decor: event to an undecorated widget");
    return gui_deco_switch(anim(w), *e->records, e->last, type, args);
}

void GuiDecor::set_matrix(const Widget& w, const std::array<float, 16>& m) {  // 003e8040
    Entry* e = entry(w);
    if (!e || e->element < 0) return;
    e->screen->element_at(e->element).matrix = m;
}

void GuiDecor::draw(GuiDrawList& out, IeDrawState& st, const Widget& w) {  // 00417340
    Entry* e = entry(w);
    if (!e || e->element < 0) return;
    copy_matrix(w, *e);
    IeScreen& s = *e->screen;
    s.show_element(e->element, 1);
    ie_draw_element(out, st, s, s.element_at(e->element).node, e->element, 0);  // 003e8dc0 -> 003ecd60
    s.show_element(e->element, 0);
}

void GuiDecor::copy_matrix(const Widget& w, Entry& e) {
    // 004176c0 moves d+30's translation and copies it into the element
    // (003e8040); the runtime models the first half (deco_event 0x12).
    if (!w.deco || w.deco->matrix == e.seen) return;
    e.seen = w.deco->matrix;
    if (e.element >= 0) e.screen->element_at(e.element).matrix = e.seen;
}

// ---------------------------------------------------------------- bridge

void GuiDecor::collect(const Widget* w, std::vector<const Widget*>& out) const {  // post-order
    if (!w) return;
    for (const Widget* c : w->children) collect(c, out);
    out.push_back(w);
}

void GuiDecor::bridge_install(gui::GuiRuntime& rt) {
    auto previous = rt.send_observer;
    const gui::GuiRuntime* r = &rt;
    rt.send_observer = [this, previous, r](Widget* sender, Widget* target, std::uint32_t type, std::int32_t a3) {
        if (previous) previous(sender, target, type, a3);
        on_send(*r, sender, target, type, a3);
    };
    // 0041df00 sends to each child in turn (a slider's frame event 0xa).
    auto previous_broadcast = rt.broadcast_observer;
    rt.broadcast_observer = [this, previous_broadcast, r](Widget* sender, Widget* target, std::uint32_t type,
                                                          std::int32_t a3) {
        if (previous_broadcast) previous_broadcast(sender, target, type, a3);
        on_send(*r, sender, target, type, a3);
    };
    // The decorator slots the diff cannot order: 00416b30's onWake (after the
    // widget's vt +1c: a slot button's onWake runs inside its screen's
    // base_wake, before the screen's own wake sends such as
    // GuiCommandCenter's onStateOff from 002973b0), 00416b80's onSleep (its
    // check_sleep 0041ed20 queries the children's 0xf right after their
    // onSleep), 00417400's 0xf answer and 00417790's step (before each
    // widget's sleep wait and update, so base_update's check_sleep sees
    // this frame's step). Each first syncs the diffed records.
    hooked_ = true;
    rt.stack_observer = [this, r] {
        sync(*r, false);
        in_update_ = true;
        update_pending_ = r->pending != 0;
    };
    rt.deco_wake = [this, r](Widget* w) {
        sync(*r, false);
        named(*w, "onWake");
    };
    rt.deco_sleep = [this, r](Widget* w) {
        sync(*r, false);
        named(*w, "onSleep");
    };
    rt.deco_settled = [this, r](Widget* w) -> std::uint8_t {
        sync(*r, false);
        const Entry* e = entry(*w);
        if (!e || e->element < 0) return 1;
        return gui_deco_settled(anim(*w), *e->records, e->last);
    };
    rt.deco_step = [this, r](Widget* w, float dt) {
        sync(*r, false);
        on_update(*w, ie_ftz(dt));
        stepped_.insert(w);
    };
    // GuiWidget_OnActivate 00416c70 / GuiWidget_OnDeactivate 00416cc0 (the decorator's vt +14 / +18 after the
    // widget's): GuiAgentModify's 00340850.
    rt.deco_named = [this, r](Widget* w, const char* record) {
        sync(*r, false);
        named(*w, record);
    };
    // 004174c8 (event 9 with a pointer): the element's clock +4 (GuiGameScreen 0045ca20 / 0045d960).
    rt.deco_time = [this, r](Widget* w) -> std::optional<float> {
        sync(*r, false);
        const Entry* e = entry(*w);
        if (!e || e->element < 0) return std::nullopt;
        return anim(*w)->t;
    };
}

void GuiDecor::bridge_sync(const gui::GuiRuntime& rt) { sync(rt, false); }

void GuiDecor::bridge_begin_update(const gui::GuiRuntime& rt) {
    sync(rt, false);
    // Installed, the screens' update starts at the runtime's stack_observer (after controller_check 00465870,
    // whose popup open / close precede every element step).
    in_update_ = !hooked_;
    update_pending_ = rt.pending != 0;
}

void GuiDecor::bridge_step(const gui::GuiRuntime& rt, float dt) {
    sync(rt, false);
    in_update_ = false;
    const Widget& canvas = rt.canvas();
    if (!(canvas.flags & gui::flag_enabled)) {  // 00415950
        stepped_.clear();
        changed_.clear();
        return;
    }
    const float f = ie_ftz(dt * 30.0F);
    // update_stack's v_update of each enabled screen; base_update runs
    // o_update (00417790 on a decorator: the element step first) on every
    // child, each before its own children.
    std::vector<const Widget*> order;
    std::function<void(const Widget*)> pre = [&](const Widget* w) {
        for (const Widget* c : w->children) {
            if (c->deleted) continue;
            order.push_back(c);
            pre(c);
        }
    };
    for (const Widget* s : canvas.children)
        if (s->flags & gui::flag_enabled) pre(s);
    // Widgets the runtime's o_update reached were stepped there (deco_step).
    for (const Widget* w : order) {
        if (stepped_.count(w)) continue;
        if (changed_.count(w))
            fail(0x415950, "an element event inside the screens' update to an element the runtime did not step");
        on_update(*w, f);
    }
    stepped_.clear();
    changed_.clear();
}

void GuiDecor::on_send(const gui::GuiRuntime& rt, const Widget* sender, const Widget* target, std::uint32_t type,
                       std::int32_t a3) {
    const std::uint32_t t = type & 0xffff;
    const bool canvas_send = sender == &rt.canvas();
    if (t == 0x18 && target && awake_.count(target) && !hooked_) {
        // canvas_push 00414f00: clear_awake 0041ee50 then o_wake ran on the
        // pushed subtree since the last observed send.
        std::vector<const Widget*> sub;
        collect(target, sub);
        for (const Widget* w : sub) awake_[w] = false;
    }
    sync(rt, canvas_send);
    if (t == 0x19 && target) {
        // pop 00414be0: the focus change after this send loses the old
        // focus only when it is the popped screen's +20.
        pop_pending_ = true;
        pop_old_ = rt.canvas().focus;
        pop_focus_ = target->focus;
        return;
    }
    if (!target) return;
    Entry* e = entry(*target);
    if (!e || e->element < 0) return;
    GuiDecoArgs args;
    args.a3 = a3;
    switch (t) {
    case 6: {
        if (a3 == 0) {
            args.name = nullptr;
        } else {
            args.name = event_name(a3);
            if (!args.name) fail(0x417458, "decorator event 6 with the name at " + std::to_string(a3));
        }
        break;
    }
    case 0xa:
    case 0xb: break;
    case 0xc:
        // The send's t0 (the keyboard's 0xc is (0xd, 0); GuiGameScreen 0045ca20's close passes 00497acc[+dc]).
        args.t0 = rt.observed_t0;
        break;
    case 0xd:
        // push 00465570's (0, 300) to "tempbackground".
        if (!canvas_send) fail(0x4173b0, "decorator event 0xd from a widget");
        args.t0 = 300;
        break;
    case 0xe: fail(0x4173b0, "decorator event 0xe (t0 not observed)");
    case 4: return;  // 00417520 only looks the named record up (the runtime answers it)
    case 0xf: {
        if (hooked_) return;  // the runtime asks 00417400 (deco_settled)
        const IeAnim* el = anim(*target);
        if (!gui_deco_settled(el, *e->records, e->last))
            fail(0x417400, "decorator event 0xf before the element settles (the runtime answers 1)");
        return;
    }
    default: return;  // 5 / 7 / 8 / 9 without pointers, 0x12 (copied at sync / draw), the widget's own
    }
    if (hooked_ && in_update_) {
        // Installed, the runtime steps each element in its o_update (deco_step), so an event sent during
        // the screens' update lands in the original order unless the element is only caught up after it.
        changed_.insert(target);
    } else {
        note_change(rt, canvas_send);
    }
    gui_deco_switch(anim(*target), *e->records, e->last, t, args);
}

void GuiDecor::note_change(const gui::GuiRuntime& rt, bool canvas_send, const Widget* w) {
    if (!in_update_) return;
    if (hooked_ && w) {
        changed_.insert(w);
        return;
    }
    if (canvas_send && update_pending_) return;  // update_stack's pending pop / push, before any screen update
    bool titles = true;
    for (const Widget* s : rt.canvas().children)
        if ((s->flags & gui::flag_enabled) && s->cls != WidgetClass::title) titles = false;
    // 00288060 (GuiTitleScreen update) changes the focus before its
    // base_update: the element steps follow every change it makes.
    if (titles) return;
    fail(0x415950, "an element event inside the screens' update (its order against the element steps)");
}

void GuiDecor::sync(const gui::GuiRuntime& rt, bool canvas_send) {
    OMEGA_ZONE("decor.sync");
    // Installed (hooked_, never cleared), the runtime runs the sleeps and wakes itself and nothing reads awake_
    // (every reader below and on_send's 0x18 is behind !hooked_), so the tree walk and the awake record are skipped.
    if (!hooked_) sync_awake(rt, canvas_send);
    sync_focus(rt, canvas_send);
}

void GuiDecor::sync_awake(const gui::GuiRuntime& rt, bool canvas_send) {
    std::vector<const Widget*> order;
    for (const Widget* r : roots_) collect(r, order);
    auto now = [](const Widget* w) { return !w->deleted && (w->flags & gui::flag_awake) != 0; };
    auto was = [&](const Widget* w) {
        const auto it = awake_.find(w);
        return it != awake_.end() && it->second;
    };
    auto fell = [&](const Widget* w) { return w && was(w) && !now(w); };
    auto rose = [&](const Widget* w) { return w && !was(w) && now(w); };
    auto has_element = [&](const Widget* w) {
        const Entry* e = entry(*w);
        return e && e->element >= 0;
    };
    // Sleeps (base_sleep 0041e7c0 under the decorator slot 00416b80): the
    // 0xf query of a widget that was awake precedes everything below it;
    // then, children first, check_sleep 0041ed20's 0xf on the children and
    // the "onSleep" record. Installed, the runtime runs these itself
    // (deco_sleep / deco_settled) and the awake bit is only recorded.
    for (const Widget* w : order) {
        if (hooked_) break;
        if (!fell(w) || !has_element(w)) continue;
        const Entry& e = *entry(*w);
        if (!gui_deco_settled(&e.screen->element_at(e.element).anim, *e.records, e.last))
            fail(0x417400, "a sleeping decorator whose element has not settled (the runtime answers 1)");
    }
    for (const Widget* w : order) {
        if (hooked_) break;
        const Widget* p = w->parent;
        const bool p_tracked = p && awake_.count(p);
        if (!(fell(w) || (p_tracked && fell(p)))) continue;
        if (fell(w)) {
            for (const Widget* c : w->children) {
                if (!has_element(c)) continue;
                const Entry& ce = *entry(*c);
                if (!gui_deco_settled(&ce.screen->element_at(ce.element).anim, *ce.records, ce.last))
                    fail(0x41ed20, "check_sleep before a child element settles (the runtime answers 1)");
            }
        }
        if (named(*w, "onSleep")) note_change(rt, canvas_send);
    }
    // Wakes (base_wake under 00416b30): every widget whose parent woke, and
    // a pushed root, gets "onWake", children first. Installed, the runtime
    // runs them itself (deco_wake).
    for (const Widget* w : order) {
        if (hooked_) break;
        const Widget* p = w->parent;
        const bool p_tracked = p && awake_.count(p);
        if (!(rose(w) || (p_tracked && rose(p)))) continue;
        if (named(*w, "onWake")) note_change(rt, canvas_send);
    }
    for (const Widget* w : order) awake_[w] = now(w);
}

void GuiDecor::sync_focus(const gui::GuiRuntime& rt, bool canvas_send) {
    // Focus (set_focus 00414b30, canvas_push, pop): onLoseFocus(old) then
    // onGainFocus(new).
    const Widget* cur = rt.canvas().focus;
    if (pop_pending_) {
        pop_pending_ = false;
        if (pop_old_ && pop_focus_ == pop_old_ && entry(*pop_old_))
            if (named(*pop_old_, "onLoseFocus")) note_change(rt, canvas_send, pop_old_);
        if (cur && entry(*cur))
            if (named(*cur, "onGainFocus")) note_change(rt, canvas_send, cur);
    } else if (cur != focus_) {
        if (focus_ && !focus_->deleted && entry(*focus_))
            if (named(*focus_, "onLoseFocus")) note_change(rt, canvas_send, focus_);
        if (cur && entry(*cur))
            if (named(*cur, "onGainFocus")) note_change(rt, canvas_send, cur);
    }
    focus_ = cur;
    // Port bookkeeping: the copies only change something after a decorator matrix write (the runtime counts them),
    // a new entry, or while a matrix compares unequal to itself (a NaN), so an unchanged pass is skipped.
    if (&rt == matrix_rt_ && rt.deco_matrix_serial == matrix_serial_ && entries_serial_ == matrix_entries_ &&
        !matrix_unsettled_)
        return;
    OMEGA_ZONE("decor.sync.matrices");
    matrix_unsettled_ = false;
    for (auto& [w, e] : entries_) {
        copy_matrix(*w, e);
        if (w->deco && !(w->deco->matrix == e.seen)) matrix_unsettled_ = true;
    }
    matrix_rt_ = &rt;
    matrix_serial_ = rt.deco_matrix_serial;
    matrix_entries_ = entries_serial_;
}

}  // namespace omega
