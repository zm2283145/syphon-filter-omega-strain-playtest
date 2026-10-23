#pragma once
// Checkpoint 122b: the GuiInterfaceDecorator's interface element (vtable
// 004e09f0, ctor 00417bc0): the IE element a decorated widget animates and
// draws under itself (the title background, the button brackets, the
// prompt "textbackground" bars, the Navigate / Accept glyphs).
//
//   004178e0  init: the widget's +c, then the IE (00412f80 / 00413370), the
//             node (d+14, else the widget name, + "_root" for a parentless
//             widget), 003ef010 create, 003e8de0(el, 0), 003e8040(el, d+30)
//   00416d10  the named event record (00416e40) applied to the element
//   004173b0  the event switch (types 4..0x12, jump table 004bf030)
//   00417790  update: 003f43d0(el, dt), then the sleep wait
//   00417340  draw: 003e8de0(el, 1), 003e8dc0 (003ecd60), 003e8de0(el, 0),
//             then the widget's own +30
//   00416b30 / 00416b80 / 00416bd0 / 00416c20 / 00416c70 / 00416cc0: the
//             widget's slot, then 00416d10 with "onWake" / "onSleep" /
//             "onGainFocus" / "onLoseFocus" / "onActivate" / "onDeactivate"
//
// The GUI runtime (gui_runtime.*) models the decorator without an element
// (its ie_lookup answers -1). GuiDecor is the element layer beside it: the
// decorator events arrive through on_* calls (a runtime hook, or the
// host-side bridge GuiDecor::bridge_* until one exists, see research/
// FRONTEND.md "Checkpoint 122b GUI decor"), and the element draw is called
// by GuiDrawer before the widget's own draw.

#include "frontend_formats.hpp"
#include "gui_runtime.hpp"
#include "ie_draw.hpp"
#include "ie_screen.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace omega {

// 00416e40(d, name): a null or empty name gives the last record (d+80);
// otherwise the index of the first record whose name equals it (001c43a0),
// or -1.
int gui_deco_find(const std::vector<GuiEvent>& records, int last, const std::string* name);

// 00416d10(d, name) on the element `el` (d+24; null: 0): the record's
// action byte (+c, the file word's low byte) 1 = 003f45a0(a) then
// 003f44e0(b), 2 = 003f45a0(a) then 003f4510(a, b, 1), 3 = 003f45a0(a) then
// 003f4510(a, b, 0), others nothing (a, b = cvt.s.w of the zero-extended
// file words). A found record becomes `last` and the result is 1; none: 0.
std::uint8_t gui_deco_event(IeAnim* el, const std::vector<GuiEvent>& records, int& last, const std::string* name);

// The arguments of a 004173b0 call: a3 / t0 as integers, or as the pointers
// the pointer-taking types read (4: a3 the name, t0 the record out; 6: a3
// the name; 7: a3 the step).
struct GuiDecoArgs {
    std::int32_t a3 = 0;
    std::int32_t t0 = 0;
    const std::string* name = nullptr;  // types 4 / 6 (null: a3 == 0)
    const float* step = nullptr;        // type 7 (null: a3 == 0)
    bool t0_out = false;                // type 4: t0 != 0
};
struct GuiDecoResult {
    // nullopt: the switch falls to the widget's own event (+3c).
    std::optional<std::uint8_t> value;
    int record = -1;  // type 4: the record stored through t0 (-1 none)
    float out = 0.0F; // type 9: *a3 = the element time
};
// 004173b0 (jump table 004bf030 on type & 0xffff): 4, 5, 6, 7, 9, 0xa..0xe
// and 0xf as the original; 8 with a3 and an element reads the element
// duration (vt +c 003e7ed0 -> 003ee740, not hosted: throws); 0x12 is the
// runtime's deco_event (throws here); 0x10 / 0x11 and the types past 0x12
// give nullopt (the widget's +3c).
GuiDecoResult gui_deco_switch(IeAnim* el, const std::vector<GuiEvent>& records, int& last, std::uint32_t type,
                              const GuiDecoArgs& args);

// 004178e0's node name: d+14 when non-empty, else the widget name, with
// "_root" appended (004bf070) when the widget has no parent at init; the
// copy loop stops at 0x7e characters (004179c0 / 00417a50 write "\n" and
// end the name there; not reached by any file on the disc: throws).
std::string gui_deco_node_name(const std::string& widget_name, bool has_parent, const std::string& node);

// 0x0f of 004173b0 (00417400): 0 when the last record (d+80) is set, the
// element is, the record's action byte is 1 and cvt.s.w(b) differs from
// the element time (the onSleep / fade has not arrived); else 1.
std::uint8_t gui_deco_settled(const IeAnim* el, const std::vector<GuiEvent>& records, int last);

// The IE file and texture slots of one interface (the IE manager *00571dc0).
struct GuiDecoIe {
    const IeFile* file = nullptr;
    IeTextureService* textures = nullptr;
};

class GuiDecor {
public:
    // 00412f80(manager, name): the upper-cased name's interface, or
    // nullopt when the manager has none (-1: no element). The file open
    // (00413124 4134b0, 3f9850 / 3f96e0, "%s.IE" 36e1b0) is the resolver's.
    using Resolver = std::function<std::optional<GuiDecoIe>(const std::string& upper_name)>;
    GuiDecor(Resolver resolver, std::uint32_t text_word) : resolver_(std::move(resolver)), text_word_(text_word) {}

    // 004178e0 for every decorated node of a loaded file, in the order the
    // init runs them (the widget's +c first, which inits the children: post-
    // order). The IE name is the runtime's d+8 (the file's, or the manager's
    // +2c when empty). A file node the runtime tree does not hold (a pruned
    // load) still gets its element, hidden, never drawn; its widget is
    // matched by name among the runtime children, in file order.
    // rt.g.iface (the manager's +2c) names the IE of the pruned nodes whose
    // file IE name is empty.
    void attach(const gui::GuiRuntime& rt, const gui::Widget& root, const GuiNode& file_root);

    struct Entry {
        const gui::Widget* widget = nullptr;  // null: not in the runtime tree
        IeScreen* screen = nullptr;
        int element = -1;
        const std::vector<GuiEvent>* records = nullptr;
        int last = -1;  // d+80
        std::array<float, 16> seen{};  // d+30 as last copied into the element (003e8040)
    };
    const Entry* entry(const gui::Widget& w) const;
    Entry* entry(const gui::Widget& w);
    IeAnim* anim(const gui::Widget& w);
    IeScreen* screen(const std::string& upper_name);
    // The pruned-load entries (file node name -> entry).
    const std::vector<std::pair<std::string, Entry>>& unloaded() const { return unloaded_; }

    // The decorator slots (the widget's own slot runs first, in the runtime).
    void on_wake(const gui::Widget& w) { named(w, "onWake"); }
    void on_sleep(const gui::Widget& w) { named(w, "onSleep"); }
    void on_gain(const gui::Widget& w) { named(w, "onGainFocus"); }
    void on_lose(const gui::Widget& w) { named(w, "onLoseFocus"); }
    void on_activate(const gui::Widget& w) { named(w, "onActivate"); }
    void on_deactivate(const gui::Widget& w) { named(w, "onDeactivate"); }
    // 00417790's element step (before the widget's own update).
    void on_update(const gui::Widget& w, float dt);
    // 004173b0 for an event delivered to a decorated widget.
    GuiDecoResult on_event(const gui::Widget& w, std::uint32_t type, const GuiDecoArgs& args);
    // 003e8040(el, m): the element matrix +30.
    void set_matrix(const gui::Widget& w, const std::array<float, 16>& m);
    // 00417340's element part: 003e8de0(el, 1), 003ecd60(node, el, 0),
    // 003e8de0(el, 0). Nothing without an element.
    void draw(GuiDrawList& out, IeDrawState& st, const gui::Widget& w);

    // Host-side bridge (the runtime's owner dispatch o_wake / o_gain /
    // o_lose runs the widget slots only):
    //   - install chains the runtime's send_observer: every observed send
    //     first syncs, then applies 004173b0's element part when the target
    //     is decorated with an element (t0 is not observed: see on_send);
    //     it also sets the runtime's deco_sleep / deco_settled / deco_step
    //     (00416b80's onSleep, 00417400's 0xf answer, 00417790's step), so
    //     a sleep waits for its children's onSleep animations as on the EE;
    //   - sync diffs the state the runtime exposes: the awake bit 0x8 (a
    //     widget whose bit rose, or whose parent's rose, had its onWake;
    //     canvas_push's clear_awake is seen at its 0x18),
    //     the canvas focus (onLoseFocus(old), onGainFocus(new); pop's
    //     order at its 0x19) and d+30 (the 0x12 copy 003e8040);
    //   - begin_update / step bracket GuiRuntime::update (installed, the
    //     screens' update begins at the runtime's stack_observer, after
    //     controller_check 00465870): step syncs, then
    //     runs 00417790 (3f43d0(el, 30 dt)) for every decorated non-root
    //     widget of the enabled screens deco_step has not reached
    //     (update_stack calls the screens' own update: their root
    //     decorators are not stepped).
    // Orders the diff cannot recover throw naming the address whose order
    // is not modelled (see research/FRONTEND.md "Checkpoint 122b GUI decor").
    void bridge_install(gui::GuiRuntime& rt);
    void bridge_sync(const gui::GuiRuntime& rt);
    void bridge_begin_update(const gui::GuiRuntime& rt);
    void bridge_step(const gui::GuiRuntime& rt, float dt);

private:
    bool named(const gui::Widget& w, const char* name);  // false: no element (nothing changed)
    Entry make_entry(const gui::Widget* w, const GuiNode& n, const std::string& iface, bool root);
    void attach_node(const gui::Widget* w, const GuiNode& n, const std::string& iface, bool root);
    void copy_matrix(const gui::Widget& w, Entry& e);
    // w: the widget whose element changed. Installed, a change inside the screens' update is ordered like
    // on_send's (the runtime steps the element in its o_update; bridge_step throws if it only catches it up).
    void note_change(const gui::GuiRuntime& rt, bool canvas_send, const gui::Widget* w = nullptr);
    void on_send(const gui::GuiRuntime& rt, const gui::Widget* sender, const gui::Widget* target, std::uint32_t type,
                 std::int32_t a3);
    void sync(const gui::GuiRuntime& rt, bool canvas_send);
    void sync_awake(const gui::GuiRuntime& rt, bool canvas_send);  // the sleeps / wakes before bridge_install
    void sync_focus(const gui::GuiRuntime& rt, bool canvas_send);  // the focus records and the matrices
    void collect(const gui::Widget* w, std::vector<const gui::Widget*>& out) const;
    Resolver resolver_;
    std::uint32_t text_word_ = 0;
    std::map<std::string, std::unique_ptr<IeScreen>> screens_;
    std::map<const gui::Widget*, Entry> entries_;
    // sync's matrix pass skip (port bookkeeping): the runtime's deco_matrix_serial and entries_serial_ at the last
    // pass, and whether a matrix then still compared unequal (NaN).
    std::uint64_t entries_serial_ = 0;
    const gui::GuiRuntime* matrix_rt_ = nullptr;
    std::uint64_t matrix_serial_ = 0;
    std::uint64_t matrix_entries_ = 0;
    bool matrix_unsettled_ = true;
    std::vector<std::pair<std::string, Entry>> unloaded_;
    std::vector<const gui::Widget*> roots_;
    std::map<const gui::Widget*, bool> awake_;
    const gui::Widget* focus_ = nullptr;
    // pop 00414be0 seen at its 0x19: the focus before it and the popped
    // screen's +20.
    bool pop_pending_ = false;
    const gui::Widget* pop_old_ = nullptr;
    const gui::Widget* pop_focus_ = nullptr;
    bool in_update_ = false;
    bool update_pending_ = false;  // the canvas's +50 at begin_update
    bool hooked_ = false;          // bridge_install set the runtime's deco_* hooks
    std::set<const gui::Widget*> stepped_;  // stepped by deco_step this update
    std::set<const gui::Widget*> changed_;  // sent an element event during this update (installed)
};

}  // namespace omega
