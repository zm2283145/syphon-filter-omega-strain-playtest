// The mission debrief's screens (checkpoint 132f11), run by 00134110 after a level ends:
// GuiEndGameObjectives (SUMMARY.GUI, vtable over GuiObjectives 003688c0), GuiEndGameStatistics
// (SCOREONE.GUI / SCORES.GUI over GuiScoresEquip 0035cd00, with the ENDBACK.GUI backdrop),
// GuiEndGameAwards (AWARDS.GUI, the ZEUS award bitmaps) and GuiAutoSaver (the agent autosave that ends
// the debrief). Each of the first three owns the voice queue 00460ac0 / 004610f0 that plays the
// handler's quips in turn. research/LEVEL_END.md "Checkpoint 132f11".
#include "gui_runtime.hpp"

#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace omega::gui {
namespace {

// EE COP1 semantics (as gui_runtime.cpp): denormals flush to a signed zero, cvt.w.s saturates.
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

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsA

bool pressed(const PadEvent& ev, std::uint32_t mask) {  // +0 == 1, +2 == 0, +8 == mask, +6 == 0
    return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
}

constexpr std::uint32_t pad_start = 0x800;

// The decorator event names 0045f4d0 sends (event 6).
constexpr std::int32_t on_deactivate = 0x4c2470;  // "onDeactivate"
constexpr std::int32_t on_activate = 0x4c2498;    // "onActivate"

std::string fmt2(const char* f, std::int32_t a, std::int32_t b) {  // 00128bd0 (sprintf)
    char buf[32];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

}  // namespace

// The rows per line of the award layout (0x497b20, three words per award count 1..15).
const std::array<std::array<std::int32_t, 3>, 15> end_award_rows = {{
    {1, 0, 0}, {2, 0, 0}, {3, 0, 0}, {2, 2, 0}, {2, 3, 0}, {3, 3, 0}, {2, 3, 2}, {2, 3, 3},
    {3, 3, 3}, {3, 4, 3}, {3, 4, 4}, {4, 4, 4}, {4, 5, 4}, {4, 5, 5}, {5, 5, 5},
}};

void GuiRuntime::debrief_session_end_popup(std::uint32_t mode, const std::string& message_key,
                                           const std::optional<std::string>& inline_message) {  // 00134220..00134318
    switch (mode & 0xffu) {
    case 1: {
        std::optional<std::string> title;
        std::optional<std::string> message;
        if (inline_message && !inline_message->empty()) {  // lb 005721f0
            title = host_.get_string(message_key);  // 00134240: test the selected title
            if (title) title = host_.get_string(message_key);  // 00134260: original repeats the lookup
            else title = host_.get_string("QuitTitle");
            message = *inline_message;  // 00134290: pointer to the inline 005721f0 bytes
        } else {
            title = host_.get_string("QuitTitle");
            message = host_.get_string(message_key);  // 0013429c
        }
        popup_open(2, title, message, nullptr, 0);  // 002cae30(app, 2, title, message, 0, 0)
        break;
    }
    case 2: {
        const auto title = host_.get_string("QuitTitle");
        const auto message = host_.get_string("HOST_LEFTGAME");
        popup_open(2, title, message, nullptr, 0);
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- the voice queue

void GuiRuntime::vq_push(Widget* w, const std::string& name, float f) {  // 00460ac0
    // 00129a70(entry, name, 8) copies 8 bytes: a longer name would lose its terminator.
    if (name.size() >= 8) unhosted(0x460adc, "a debrief voice name of 8 or more characters ('" + name + "')");
    Widget::VoiceQueue::Entry e;
    e.name = name;
    e.delay = cvt_w(fmul(30.0f, f), 0x460afc);  // 0x41f00000 * f
    w->vq->entries.push_back(e);  // 00461770
}

void GuiRuntime::vq_fire(Widget* w) {  // 004610f0
    auto& q = *w->vq;
    if (q.cursor >= static_cast<std::int32_t>(q.entries.size())) return;
    const Widget::VoiceQueue::Entry e = q.entries[static_cast<std::size_t>(q.cursor++)];
    host_.radio_sound(0, q.type, q.sound, 0);  // 0020ecb0(msg, +20, 0); 003c9420
    if (!e.garbage) {
        // 001ca010(+20, 4, name, 0, 0): the in-place record (strcpy into its 16 bytes).
        if (e.name.size() >= 16) unhosted(0x1ca0c4, "a debrief voice name of 16 or more characters ('" + e.name + "')");
        q.type = 4;
        q.sound = e.name;
        host_.radio_sound(2, q.type, q.sound, 0);  // 0020f2a0; 003c9420
    }
    q.timer = static_cast<float>(e.delay);  // cvt.s.w of the entry's +8
}

void GuiRuntime::vq_update(Widget* w, float dt) {  // the head of 00462270 / 004637d0 / 00461020
    auto& q = *w->vq;
    if (q.cursor >= static_cast<std::int32_t>(q.entries.size())) return;
    if (q.cursor != 0) {
        const auto& prev = q.entries[static_cast<std::size_t>(q.cursor - 1)];
        if (!prev.garbage && host_.quip_playing(prev.name)) return;  // 001c95d0
    }
    q.timer = fsub(q.timer, dt);  // stored whether or not it fires (the swc1 in the branch's delay slot)
    if (q.timer < 0.0f) vq_fire(w);
}

// ---------------------------------------------------------------- GuiEndGameObjectives (SUMMARY)

void GuiRuntime::eo_wake(Widget* w) {  // 004624e0
    ob_wake(w);  // 00368760
    const std::int32_t cp = host_.current_profile();  // 001314f0(G, -1)
    // *004ffd28 = G+1d8, the active agent index: 0 offline (00136870 swaps the local agent to slot 0).
    const std::int32_t player = g.b_21c8 ? host_.debrief_local_slot() : 0;
    Widget::VoiceQueue::Entry e;
    if ((g.b_21c8 ? host_.debrief_statistic(player, 0x19) : host_.scores_statistic(0, 0x19)) > 0)
        e.name = fmt2("21_13_%d", player + 1, 0);  // 0x4c2698
    else e.garbage = true;  // the stack buffer stays uninitialised (port: plays nothing)
    e.delay = 0x1e;         // 00462650(entry, name, 0x1e)
    w->vq->entries.push_back(e);  // 00462620
    Widget* missed = find(w, "label_objectives_missed");
    if (host_.profile_byte(cp, 0xb71)) {
        if (missed) missed->flags &= ~flag_visible;
        const std::int32_t par = host_.par_time();  // 00330c30(004ffc90, b_21c8)
        const float time = std::bit_cast<float>(host_.profile_word(cp, 0xd4));
        if (!(par < cvt_w(time, 0x4625a8))) {
            Widget::VoiceQueue::Entry t;
            t.name = "21_10_1";  // 0x4c26c8
            t.delay = 0x1e;
            w->vq->entries.push_back(t);
        }
    } else if (missed) {
        missed->flags |= flag_visible;
    }
}

std::uint8_t GuiRuntime::eo_input(Widget* w, const PadEvent& ev) {  // 00462380
    if (pressed(ev, pad_cross)) {
        pop(w);
        push(factory("GuiEndGameStatistics", std::string(g.b_21c8 ? "SCORES" : "SCOREONE")), 1);
        return 1;
    }
    if (pressed(ev, pad_triangle)) return 1;
    return ob_input(w, ev);  // 00368300
}

// ---------------------------------------------------------------- GuiEndGameStatistics (SCOREONE)

void GuiRuntime::es_wake(Widget* w) {  // 00463a20
    auto& s = *w->es;
    s.back = load_root("ENDBACK");  // 004191d0(mgr, "ENDBACK", 0)
    s.b254 = {0, 0, 0};
    if (s.back) push(s.back, 0);
    se_wake(w);  // 0035cbf0
    es_voices(w);
}

void GuiRuntime::es_voices(Widget* w) {  // 00463390
    if (g.b_21c8) {  // 004633cc..00463590: highest two percentages, first player wins ties
        std::array<std::int32_t, 2> best{};
        std::array<std::int32_t, 2> winner{-1, -1};
        for (std::int32_t p = 0; p < 4; ++p) {
            const std::int32_t cp = host_.debrief_profile_slot(p);
            if (cp < 0 || !host_.profile_byte(cp, 0xb70) || !host_.profile_byte(cp, 0xb74)) continue;
            for (std::int32_t k = 0; k < 2; ++k) {
                const std::uint32_t numerator = k == 0 ? 0x1d : 0x1b;
                const std::int32_t count = host_.debrief_statistic(p, numerator);
                const std::int32_t denominator = host_.debrief_statistic(p, numerator + 1);
                // EE div uses the wrapped 32-bit numerator. Compute the quotient in 64 bits so
                // INT_MIN / -1 cannot overflow C++, then retain the EE's low 32 bits.
                const std::int32_t value = denominator == 0 ? 0 : std::bit_cast<std::int32_t>(
                    static_cast<std::uint32_t>(static_cast<std::int64_t>(
                        std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(count) * 100u)) / denominator));
                if (value > best[static_cast<std::size_t>(k)]) {
                    best[static_cast<std::size_t>(k)] = value;
                    winner[static_cast<std::size_t>(k)] = p;
                }
            }
        }
        for (std::int32_t k = 0; k < 2; ++k) {
            if (w->es->b254[static_cast<std::size_t>(k)] || winner[static_cast<std::size_t>(k)] < 0) continue;
            w->es->b254[static_cast<std::size_t>(k)] = 1;
            vq_push(w, fmt2(k == 0 ? "21_3_%d" : "21_4_%d", winner[static_cast<std::size_t>(k)] + 1, 0), 1.0f);
        }
    }
    // The players list holds each active, completed agent whose statistic 0x1a is 0.
    std::vector<std::int32_t> players;
    for (std::int32_t p = 0; p < (g.b_21c8 ? 4 : 1); ++p) {
        const std::int32_t cp = g.b_21c8 ? host_.debrief_profile_slot(p) : host_.current_profile();
        if (cp < 0) continue;
        if ((!g.b_21c8 || host_.profile_byte(cp, 0xb70)) && host_.profile_byte(cp, 0xb74) &&
            (g.b_21c8 ? host_.debrief_statistic(p, 0x1a) : host_.scores_statistic(0, 0x1a)) == 0) players.push_back(p);
    }
    if (players.empty()) return;
    if (players.size() >= 2) {
        for (std::int32_t p : players) vq_push(w, fmt2("21_2_%d", p + 1, 0), 0.0f);  // 0x4c2610
        vq_push(w, fmt2("21_%d_5", 5, 0), 1.0f);                                   // 0x4c2608
        return;
    }
    vq_push(w, fmt2("21_%d_%d", 5, players[0] + 1), 1.0f);  // 0x4c25f8
}

void GuiRuntime::es_awards(Widget* w) {
    pop(w->es->back);  // the manager's +68 on +250
    switch_screens(w, factory("GuiEndGameAwards", std::string("AWARDS")));
}

std::uint8_t GuiRuntime::es_input(Widget* w, const PadEvent& ev) {  // 00463160
    if (pressed(ev, pad_start)) return 1;
    if (pressed(ev, pad_triangle)) return 1;
    if (pressed(ev, pad_cross)) {
        es_awards(w);
        return 1;
    }
    return se_input(w, ev);
}

std::uint8_t GuiRuntime::es_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 00463980
    if ((type & 0xffff) == 2) {
        es_awards(w);
        return 1;
    }
    return se_event(w, sender, type, a3, t0);
}

// ---------------------------------------------------------------- GuiEndGameAwards (AWARDS)

void GuiRuntime::aw_init(Widget* w) {  // 00460bb0
    base_init(w);  // 0041f150
    auto& a = *w->aw;
    if (host_.zeus_archive() != 0) {  // 002c9ee0(*004ffc04)
        Widget* grp = find(w, "award_grp");
        if (grp && grp->children.size() == a.entries.size())
            for (std::size_t i = 0; i < a.entries.size(); ++i)
                a.entries[i].bitmap = of_class(grp->children[i], WidgetClass::bitmap);  // isA "GuiBitmapWidget"
    }
    a.border = find(w, "border");
}

void GuiRuntime::aw_add(Widget* w, const std::string& file, const std::optional<std::string>& text) {
    auto& a = *w->aw;
    if (a.count >= static_cast<std::int32_t>(a.entries.size())) return;
    auto& e = a.entries[static_cast<std::size_t>(a.count)];
    e.sound.clear();  // 00129360(+8, "")
    if (e.bitmap) bm_set_image(e.bitmap, file, host_.zeus_archive());  // 0043c890
    e.text = text;
    ++a.count;
}

void GuiRuntime::aw_build(Widget* w) {  // 0045fab0
    auto& a = *w->aw;
    a.count = 0;
    if (host_.zeus_archive() == 0) return;  // *(*004ffc04 + 25c)
    const std::int32_t cp = host_.current_profile();
    // The rank: only when +b54 is set and +b50 (the new rank row) differs from it.
    const std::int32_t b54 = host_.profile_word(cp, 0xb54);
    if (b54 != 0 && host_.profile_word(cp, 0xb50) != b54) {
        const GuiHost::RankRow row = host_.profile_rank_row(cp);
        if (!row.name) unhosted(0x45fb44, "a rank row without a name");
        const auto title = host_.get_string(*row.name);  // 003fdb50
        aw_add(w, fmt2("0_%d_%d", 6, row.index + 1) + "s" + ".tdx", title);
        aw_add(w, fmt2("0_%d_%d", 7, 7) + ".tdx", host_.get_string("NewParts"));
    }
    // The six perks: unlocked (00332f90, +b08) and not yet seen (00332f40, +b2c).
    for (std::int32_t i = 0; i < 6; ++i) {
        const auto bit = [&](std::uint32_t base) {
            const std::uint32_t word = static_cast<std::uint32_t>(host_.profile_word(cp, base + 4u * (static_cast<std::uint32_t>(i) >> 5)));
            return ((word >> (i & 31)) & 1u) != 0;
        };
        if (!bit(0xb08) || bit(0xb2c)) continue;
        const auto text = host_.get_string(std::string("AB_SPECIAL_") + static_cast<char>('0' + i));  // 0x4c24d8, byte +b
        aw_add(w, fmt2("0_%d_%d", 7, i + 1) + ".tdx", text);
    }
    const auto rows = [&](const std::vector<GuiHost::BonusRow>& v, std::int32_t k, std::int32_t j0) {
        for (std::size_t j = 0; j < v.size(); ++j) {
            if (v[j].b8 != 1) continue;
            if (a.count >= static_cast<std::int32_t>(a.entries.size())) continue;
            aw_add(w, fmt2("0_%d_%d", k, static_cast<std::int32_t>(j) + j0) + "s" + ".tdx", host_.loc_text(v[j].key));
        }
    };
    rows(host_.profile_vec(cp, 2), 2, 1);    // 003334c0(agent, 2)
    rows(host_.profile_vec(cp, 1), 4, 1);    // 003334c0(agent, 1)
    rows(host_.profile_ratings(cp), 3, 1);   // 00333600(agent)
    rows(host_.profile_vec(cp, 0), 3, 26);   // 003334c0(agent, 0)
}

std::uint8_t GuiRuntime::aw_next(Widget* w) {  // 0045f4d0
    auto& a = *w->aw;
    if (a.count == 0 || a.cur >= a.count - 1) return 0;
    if (a.cur >= 0) send(w, a.entries[static_cast<std::size_t>(a.cur)].bitmap, 6, on_deactivate);
    ++a.cur;
    const auto& e = a.entries[static_cast<std::size_t>(a.cur)];
    if (Widget* t = of_class(find(w, "value_awardstring"), WidgetClass::text))  // isA "GuiTextWidget"
        set_text(t, e.text.value_or(std::string()), false);                    // 0041beb0 (NULL: empty)
    if (!e.sound.empty()) unhosted(0x45f5c4, "an award with a voice");
    if (e.bitmap) {
        e.bitmap->flags |= flag_visible;
        send(w, e.bitmap, 6, on_activate);
    }
    return 1;
}

void GuiRuntime::aw_layout(Widget* w) {  // 0045f860
    auto& a = *w->aw;
    for (auto& e : a.entries) {
        if (!e.bitmap) return;
        e.bitmap->flags &= ~flag_visible;
        send(w, e.bitmap, 0xa, 0, 0);
    }
    if (a.count <= 0 || a.count >= 16 || !a.border) return;
    const float bx = a.border->rect[0];
    const float by = a.border->rect[1];
    const auto& table = end_award_rows[static_cast<std::size_t>(a.count - 1)];
    std::int32_t lines = 0;
    for (std::int32_t n : table)
        if (n != 0) ++lines;
    float yoff = fdiv(fmul(static_cast<float>(3 - lines), -120.0f), 2.0f);
    std::size_t k = 0;
    for (std::int32_t r = 0; r < lines; ++r) {
        const std::int32_t n = table[static_cast<std::size_t>(r)];
        float xoff = fdiv(fmul(static_cast<float>(5 - n), 120.0f), 2.0f);
        const float y = fadd(by, yoff);
        for (std::int32_t i = 0; i < n; ++i) {
            set_pos(a.entries[k++].bitmap, fadd(bx, xoff), y);  // 0041eba0
            xoff = fadd(xoff, 120.0f);
        }
        yoff = fadd(yoff, -120.0f);
    }
}

void GuiRuntime::aw_voices(Widget* w) {  // 00460450
    // The loop covers player 0 offline and all four agent slots in a network game (001314f0(G, p)).
    // A the rank changed, B a rating flag of 1 (00333600), C a kind-1 row flag of 1, D a kind-2 one.
    std::array<std::vector<std::int32_t>, 4> lists;
    for (std::int32_t p = 0; p < (g.b_21c8 ? 4 : 1); ++p) {
        const std::int32_t cp = g.b_21c8 ? host_.debrief_profile_slot(p) : host_.current_profile();
        if (cp < 0) continue;
        if (!host_.profile_byte(cp, 0xb70)) continue;
        const std::int32_t b54 = host_.profile_word(cp, 0xb54);
        if (b54 != 0 && host_.profile_word(cp, 0xb50) != b54) lists[0].push_back(p);
        const auto any = [](const std::vector<GuiHost::BonusRow>& v) {
            for (const auto& r : v)
                if (r.b8 == 1) return true;
            return false;
        };
        if (any(host_.profile_ratings(cp))) lists[1].push_back(p);
        if (any(host_.profile_vec(cp, 1))) lists[2].push_back(p);
        if (any(host_.profile_vec(cp, 2))) lists[3].push_back(p);
    }
    for (std::size_t l = 0; l < lists.size(); ++l) {
        const auto& v = lists[l];
        const std::int32_t k = 6 + static_cast<std::int32_t>(l);
        if (v.empty()) continue;
        if (v.size() >= 2) {
            for (std::int32_t p : v) vq_push(w, fmt2("21_2_%d", p + 1, 0), 0.0f);  // 0x4c2610
            vq_push(w, fmt2("21_%d_5", k, 0), 1.0f);                             // 0x4c2608
        } else {
            vq_push(w, fmt2("21_%d_%d", k, v[0] + 1), 1.0f);  // 0x4c25f8
        }
    }
}

void GuiRuntime::aw_autosave(Widget* w) {  // 0041f960("GuiAutoSaver") + 00419530 + 00414ac0
    Widget* s = new_widget("GuiAutoSaver");  // 0x4c2460
    if (!s) return;
    add_root(s);
    switch_screens(w, s);
}

void GuiRuntime::aw_wake(Widget* w) {  // 004603a0
    aw_build(w);
    if (w->aw->count == 0) {
        aw_autosave(w);
        return;
    }
    base_wake(w);  // 0041efa0
    aw_voices(w);
    w->aw->b204 = 0;
    aw_layout(w);
    aw_next(w);
}

std::uint8_t GuiRuntime::aw_input(Widget* w, const PadEvent& ev) {  // 0045f3f0
    if (!pressed(ev, pad_cross)) return 0;
    if (!aw_next(w)) aw_autosave(w);
    return 1;
}

// ---------------------------------------------------------------- GuiAutoSaver

void GuiRuntime::asv_wake(Widget* w) {  // 0045f340
    base_wake(w);  // 0041efa0
    save_fields(w).mode = 2;  // +4c: the autosave
    save_start(w);            // 00441a10; its end (0045f300) leaves the debrief
}

}  // namespace omega::gui
