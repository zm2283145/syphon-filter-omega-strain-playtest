// The command centre's personnel files (checkpoint 132f12), opened from the personnel group of the
// command centre's event 00295f20: the GuiPersonnelScreen base (vtable 004df0b0, ctor 00356ff0) and
// its "statistics" file GuiMissionStatistics (vtable 004dee70, ctor 0034fcb0, STATS.GUI) with the
// per-level table GuiMissionStatList (vtable 004deee0, ctor 00350460 over GuiTextArrayWidget).
// research/FRONTEND.md "Checkpoint 132f12".
#include "gui_runtime.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent_camera.hpp"
#include "appearance.hpp"

namespace omega::gui {
namespace {

[[noreturn]] void unhosted(std::uint32_t address, const std::string& what) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", address);
    throw std::runtime_error("gui runtime: unhosted " + what + " (" + buf + ")");
}

Widget* of_class(Widget* w, WidgetClass c) { return w && w->cls == c ? w : nullptr; }  // vt +58 IsA
Widget* of_text(Widget* w) {  // vt +58 IsA "GuiTextWidget" (its button / edit subclasses answer too)
    return w && (w->cls == WidgetClass::text || w->cls == WidgetClass::button || w->cls == WidgetClass::edit) ? w
                                                                                                              : nullptr;
}

bool pressed(const PadEvent& ev, std::uint32_t mask) {  // +0 == 1, +2 == 0, +8 == mask, +6 == 0
    return ev.kind == 1 && ev.port == 0 && ev.mask == mask && ev.released == 0;
}

using Rgba = std::array<float, 4>;
Rgba rgba_bits(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return {std::bit_cast<float>(r), std::bit_cast<float>(g), std::bit_cast<float>(b), std::bit_cast<float>(a)};
}
const Rgba cc_list_rgba = rgba_bits(0x3eaeaeaf, 0x3f26a6a7, 0x3f800000, 0x3f800000);  // 00587ba0
const Rgba cc_list_c0 = rgba_bits(0x3f50d0d1, 0x3f6eeeef, 0x3f800000, 0x3f800000);    // 00587bb0
const Rgba cc_list_d0 = rgba_bits(0x3ecacacb, 0x3f149495, 0x3f25a5a6, 0x3f800000);    // 00587bc0

// 003fda40: the 16-byte lower-case key 00332fe0 takes.
std::string level_key(const char* name) {
    std::string k = name;
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return k.substr(0, 16);
}

std::string cstr_of(const std::string& s) { return s.substr(0, s.find('\0')); }

// 00128bd0 (sprintf) of the fill's formats.
std::string fmt_time(std::int32_t v) {  // "%d:%2.2d" (004bab40) of v / 60, v % 60 (C truncation)
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%2.2d", v / 60, v % 60);
    return buf;
}
std::string fmt_int(std::int32_t v) {  // "%d" (004bab50)
    char buf[16];
    std::snprintf(buf, sizeof buf, "%d", v);
    return buf;
}
std::string fmt2(const char* f, std::int32_t a, std::int32_t b) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

void show(Widget* w) { w->flags |= flag_visible; }   // the inlined +14 |= 2
void hide(Widget* w) { w->flags &= ~flag_visible; }  // the inlined +14 &= ~2

// 0034d2b0 / 00333260's flag to the list style: 0 classified (style 2), 2 earned (0), otherwise 1.
std::int8_t earned_style(std::uint8_t flag) { return flag == 0 ? 2 : flag == 2 ? 0 : 1; }

constexpr std::size_t stat_levels = 17;  // 0034f670's rows, the 17 levels of 0048c160
constexpr std::uint32_t stat_cols = 11;   // 003503d0 / 00350050 vt +5c (11, count)

}  // namespace

// ---------------------------------------------------------------- GuiPersonnelScreen (vtable 004df0b0)

void GuiRuntime::pe_init(Widget* w) {  // 00356cd0
    base_init(w);  // 0041f150
    auto& p = *w->ps;
    p.item_list = of_class(find(w, "item_list"), WidgetClass::textlist);                  // +48
    p.earned_item = of_class(find(w, "earned_item"), WidgetClass::weapondisplay);         // +5c
    p.earned_bitmap = of_class(find(w, "earned_item_bitmap"), WidgetClass::bitmap);       // +54
    p.unlocked_name = of_text(find(w, "value_unlocked_name"));                             // +58
    p.item_cursor = find(w, "item_list_cursor");                                           // +4c (any class)
    if (p.item_list) {
        auto& l = *p.item_list->list;
        l.cursor = p.item_cursor;  // +88
        l.rgba = cc_list_rgba;     // +b0 = 00587ba0
        l.rgba_c0 = cc_list_c0;    // +c0 = 00587bb0
        l.rgba_d0 = cc_list_d0;    // +d0 = 00587bc0
        l.sb = of_class(find(w, "vscrollbar"), WidgetClass::scrollbar);  // +8c
    }
    p.thumbnail = of_class(find(w, "thumbnail"), WidgetClass::bitmap);  // +50
    if (Widget* t = of_text(find(w, "value_agent")))                      // 0041beb0(t, 00336170(001314f0(-1)), 0)
        set_text(t, cstr_of(host_.profile_name(host_.current_profile()).value_or("")), false);
}

void GuiRuntime::pe_wake(Widget* w) {  // 00356c70
    base_wake(w);  // 0041efa0
    auto& p = *w->ps;
    if (!p.item_list) return;
    list_clear(p.item_list);  // 00421070(+48)
    pe_fill(w);               // vt +5c
    array_select(p.item_list, 0, static_cast<std::uint32_t>(p.sel));  // 00424430(+48, 0, +60)
}

std::uint8_t GuiRuntime::pe_input(Widget* w, const PadEvent& ev) {  // 00356be0
    if (!pressed(ev, pad_triangle)) return 0;
    pe_back(w);  // vt +68
    return 1;
}

void GuiRuntime::pe_back(Widget* w) {  // 00356ad0 (GuiPersonnelMenu's vt +68 is 003549c0)
    if (w->cls == WidgetClass::pmenu) {
        pm_back(w);
        return;
    }
    Widget* next = factory("GuiCommandCenter", std::string("cmdcentr"));  // 004bb340 / 004bb358
    switch_screens(w, next);  // 00414ac0(canvas, this, next)
}

std::uint8_t GuiRuntime::pe_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 00356b30
    const std::uint32_t k = type & 0xffff;
    if (k == 0x14) {
        pe_show(w, t0);  // vt +60
        return 1;
    }
    if (k == 0x10 && a3 == 1) return 1;  // the list's confirm sound is the file's own (vt +64)
    if (k == 0x13) {
        pe_select(w, t0);  // vt +64
        return 1;
    }
    if (k == 0x3000) {
        w->ps->sel = a3;  // +60
        return 1;
    }
    return base_event(w, sender, type, a3, t0);  // 0041ef10
}

// ---------------------------------------------------------------- GuiMissionStatistics (vtable 004dee70)

void GuiRuntime::ms_init(Widget* w) {  // 0034fba0
    pe_init(w);  // 00356cd0
    auto& p = *w->ps;
    p.row = 0;  // +80
    // 0034f670 -> 0034fe40: 17 rows, each 0034f920's zeroed fields and an empty name.
    p.rows.assign(stat_levels, Widget::MissionStatRow{});
    p.list = of_class(find(w, "config_list"), WidgetClass::missionstatlist);  // +6c (00492bd0)
    if (!p.list) return;
    p.list->list->cursor = find(w, "select_config_select");  // list +88 (00492bd8)
    msl_attach(p.list, &p.rows);  // the inlined 003503d0
}

void GuiRuntime::ms_wake(Widget* w) {  // 0034fb50
    ms_refresh(w);  // 0034f3b0
    pe_wake(w);     // 00356c70
    if (w->ps->list) array_select(w->ps->list, 0, static_cast<std::uint32_t>(w->ps->row));  // 00424430(+6c, 0, +80)
}

std::uint8_t GuiRuntime::ms_input(Widget* w, const PadEvent& ev) {  // 0034fa80
    if (pressed(ev, pad_triangle)) {
        Widget* next = factory("GuiCommandCenter", std::string("cmdcentr"));  // 004baae0 / 004baaf8
        switch_screens(w, next);  // 00414ac0(canvas, this, next)
        return 1;
    }
    return pe_input(w, ev);  // 00356be0
}

std::uint8_t GuiRuntime::ms_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 0034fa40
    if ((type & 0xffff) == 0x3000) {
        w->ps->row = a3;  // +80
        return 1;
    }
    return pe_event(w, sender, type, a3, t0);  // 00356b30
}

void GuiRuntime::ms_select(Widget* w, std::int32_t row) {  // 0034f940
    const char* name = row >= 0 && static_cast<std::size_t>(row) < stat_levels ? cc_level_names[static_cast<std::size_t>(row)]
                                                                               : nullptr;  // 0048c160[row]
    if (name && host_.level_done(host_.current_profile(), level_key(name))) {  // 003fda40 + 00332fe0
        send(w, &canvas_, 0x10, 1, 0);  // the confirm sound
        pe_open_menu(w, "CSUBMENU", 6, row, 0, 0);  // 003569f0(this, 004baad0, 6, row, 0, 0)
        return;
    }
    send(w, &canvas_, 0x10, 4, 0);  // the refusal sound
}

void GuiRuntime::ms_refresh(Widget* w) {  // 0034f3b0
    auto& p = *w->ps;
    if (p.rows.size() != stat_levels) return;  // +74 != 0x11
    const std::int32_t prof = host_.current_profile();  // 001314f0(004ffb50, -1)
    const auto records = host_.profile_level_records(prof);  // agent + c4 (count +c8, data +cc)
    if (records.size() < stat_levels) return;
    const std::string classified = cstr_of(host_.get_string("Classified").value_or(""));  // 004baaa8, else 004baab8
    for (std::size_t i = 0; i < p.rows.size(); ++i) {
        auto& r = p.rows[i];
        r.played = host_.level_done(host_.current_profile(), level_key(cc_level_names[i])) ? 1 : 0;  // +c
        if (!r.played) {
            r.name = classified;  // the other fields keep what they held
            continue;
        }
        char key[32];
        std::snprintf(key, sizeof key, "SHORT_LEVEL%d", static_cast<int>(i));  // 004baac0
        r.name = cstr_of(host_.get_string(key).value_or(""));
        const auto& rec = records[i];
        auto timed = [](Widget::MissionStatRow::Field& f, std::int32_t a, std::int32_t b) {
            if (a == 0) a = -1;
            f.ok = a != -1 && !(b < a);
            f.a = a;
            f.b = b;
        };
        timed(r.f[0], rec[0], rec[2]);  // +10 / +14 / +18
        timed(r.f[1], rec[1], rec[3]);  // +1c / +20 / +24
        r.f[2] = {static_cast<std::uint8_t>(rec[4] == 0), rec[4], 0};  // +28 / +2c / +30
        std::int32_t a1 = rec[6], a0 = rec[7];
        if (a1 == 0) a0 = a1 = -1;
        r.f[3] = {static_cast<std::uint8_t>(a0 == a1 && a0 != -1), a0, a1};  // +34 / +38 / +3c
        r.f[4] = {static_cast<std::uint8_t>(rec[5] == 0), rec[5], 0};      // +40 / +44 / +48
    }
    if (p.list) msl_fill(p.list);  // 00350050(+6c)
}

// ---------------------------------------------------------------- GuiMissionStatList (vtable 004deee0)

void GuiRuntime::msl_attach(Widget* w, const std::vector<Widget::MissionStatRow>* rows) {  // 003503d0
    w->msl->rows = rows;  // +c0
    ta_set_dims(w, stat_cols, rows ? static_cast<std::uint32_t>(rows->size()) : 0);  // vt +5c
    array_select(w, 0, 0);  // 00424430
    for (std::uint32_t i = 0; i < stat_cols; ++i)
        array_set_col_width(w, i, static_cast<std::uint32_t>(w->msl->colw[i]));  // 004248c0(w, i, +c4[i])
}

void GuiRuntime::msl_fill(Widget* w) {  // 00350050
    const auto* rows = w->msl->rows;
    if (!rows || rows->empty()) return;
    ta_set_dims(w, stat_cols, static_cast<std::uint32_t>(rows->size()));  // vt +5c
    const std::string none = "---";  // *00492bc8 (004baa70)
    for (std::uint32_t i = 0; i < w->list->rows; ++i) {  // +4c
        if (i >= rows->size()) unhosted(0x3500c4, "statistics row read past the +c0 vector");
        const auto& r = (*rows)[i];
        ta_set_cell(w, 0, i, r.name);
        auto timed = [&](std::int32_t v) { return v == -1 ? none : fmt_time(v); };
        ta_set_cell(w, 1, i, timed(r.f[0].b));  // +18
        ta_set_cell(w, 2, i, timed(r.f[0].a));  // +14
        ta_set_cell(w, 4, i, timed(r.f[1].b));  // +24
        ta_set_cell(w, 5, i, timed(r.f[1].a));  // +20
        ta_set_cell(w, 7, i, r.f[2].a == -1 ? none : fmt_int(r.f[2].a));  // +2c
        if (r.f[3].a == -1) {
            ta_set_cell(w, 9, i, none);
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%d/%d", r.f[3].a, r.f[3].b);  // 004bab58 of +38, +3c
            ta_set_cell(w, 9, i, std::string(buf));
        }
        ta_set_cell(w, 10, i, r.f[4].a == -1 ? none : fmt_int(r.f[4].a));  // +44
    }
}

std::optional<std::array<float, 4>> GuiRuntime::msl_cell_colour(const Widget& w, std::uint32_t col,
                                                                std::uint32_t row) {  // 0034fec0
    const auto* rows = w.msl ? w.msl->rows : nullptr;
    if (!rows || col >= stat_cols || row >= rows->size()) return std::nullopt;
    const auto& r = (*rows)[row];
    if (!r.played) {
        if (col != 0) return std::nullopt;
        return cc_list_d0;  // 00587bc0
    }
    auto pick = [&](std::uint8_t ok) { return ok ? cc_list_rgba : cc_list_c0; };  // 00587ba0 / 00587bb0
    switch (col) {  // the jump table 004bab10
    case 0: return cc_list_rgba;
    case 1:
    case 2: return pick(r.f[0].ok);
    case 4:
    case 5: return pick(r.f[1].ok);
    case 7: return pick(r.f[2].ok);
    case 9: return pick(r.f[3].ok);
    case 10: return pick(r.f[4].ok);
    default: return cc_list_c0;  // columns 3, 6 and 8
    }
}

// ---------------------------------------------------------------- the personnel files (checkpoint 132f13)

void GuiRuntime::pe_fill(Widget* w) {  // vt +5c (the base's 0034d2b0 is empty)
    switch (w->cls) {
    case WidgetClass::dossiers: ds_fill(w); break;
    case WidgetClass::ranks: rk_fill(w); break;
    case WidgetClass::omega: om_fill(w); break;
    case WidgetClass::achieve: ah_fill(w); break;
    case WidgetClass::pmenu: pm_fill(w); break;
    default: break;
    }
}

void GuiRuntime::pe_show(Widget* w, std::int32_t row) {  // vt +60 (the base's 00349ed0 is empty)
    switch (w->cls) {
    case WidgetClass::ranks: rk_show(w, row); break;
    case WidgetClass::omega: om_show(w, row); break;
    case WidgetClass::achieve: ah_show(w, row); break;
    default: break;
    }
}

void GuiRuntime::pe_select(Widget* w, std::int32_t row) {  // vt +64 (the base's 0034d2c0 is empty)
    switch (w->cls) {
    case WidgetClass::missionstats: ms_select(w, row); break;  // 0034f940
    case WidgetClass::dossiers: ds_select(w, row); break;
    case WidgetClass::ranks: rk_select(w, row); break;
    case WidgetClass::omega: om_select(w, row); break;
    case WidgetClass::achieve: ah_select(w, row); break;
    default: break;
    }
}

void GuiRuntime::pe_show_item(Widget* w, std::int32_t id, std::int32_t unlock) {  // 00356850
    auto& p = *w->ps;
    const std::uint32_t zeus = host_.zeus_archive();  // *(*004ffc04 + 25c)
    if (unlock >= 10000) {  // slti 0x2710: a special ability
        if (p.earned_item) hide(p.earned_item);  // +5c
        if (p.earned_bitmap) {                   // +54
            show(p.earned_bitmap);
            bm_set_image(p.earned_bitmap, fmt2("0_%d_%d.tdx", 7, unlock - 9999), zeus);  // 004bb2e8
            if (p.unlocked_name) {  // +58
                show(p.unlocked_name);
                char key[32];
                std::snprintf(key, sizeof key, "AB_SPECIAL_%d", static_cast<int>(unlock - 10000));  // 004bb2f8
                set_text(p.unlocked_name, cstr_of(host_.get_string(key).value_or("")), false);  // else 004bb308
            }
        }
    } else {
        if (p.earned_item) {
            show(p.earned_item);
            wd_set(p.earned_item, unlock);  // 00441060
        }
        if (p.earned_bitmap) hide(p.earned_bitmap);
        if (p.unlocked_name) hide(p.unlocked_name);
    }
    if (p.thumbnail)  // +50: "0_%d_%ds.tdx" (004bb310) of +64, id + +68
        bm_set_image(p.thumbnail, fmt2("0_%d_%ds.tdx", p.kind_img, id + p.offset), zeus);
}

void GuiRuntime::pe_open_menu(Widget* w, const std::string& file, std::int32_t kind, std::int32_t row,
                              std::int32_t a, std::int32_t b) {  // 003569f0
    Widget* n = factory("GuiPersonnelMenu", file);  // 004bb320
    if (!n) return;
    pm_setup(n, widget_class_name(w->cls), w->name, kind, row, a, b);  // 00354e40(n, vt +54, +4, ...)
    switch_screens(w, n);  // 00414ac0
}

Widget* GuiRuntime::pe_achievement(std::int32_t mode) {  // 003534a0
    static constexpr std::array<const char*, 3> files{"SARATING", "COMENDAT", "MEDALS"};
    static constexpr std::array<std::int32_t, 3> kinds{3, 4, 2};      // +64
    static constexpr std::array<std::int32_t, 3> offsets{0x1a, 1, 1};  // +68
    if (mode < 0 || mode > 2) return nullptr;
    const auto m = static_cast<std::size_t>(mode);
    Widget* n = factory("GuiAchievementScreen", std::string(files[m]));
    if (!n) return nullptr;
    auto& p = *n->ps;
    p.mode = static_cast<std::int8_t>(mode);  // +7c
    p.sub_file = "CSUBMENU";                  // +80
    p.kind_img = kinds[m];
    p.offset = offsets[m];
    return n;
}

// ---------------------------------------------------------------- GuiDossiers (vtable 004dec30)

namespace {
// 00332bb0(profile, i): the dossier row exists and the agent's CLP (003305d0) reaches its +8.
bool dossier_open(const std::vector<GuiHost::RankEntry>& rows, std::int32_t clp, std::int32_t i) {
    return i >= 0 && i < static_cast<std::int32_t>(rows.size()) && clp >= rows[static_cast<std::size_t>(i)].threshold;
}
}  // namespace

void GuiRuntime::ds_fill(Widget* w) {  // 00349ee0
    auto& p = *w->ps;
    const std::int32_t prof = host_.current_profile();
    list_add(p.item_list, host_.profile_name(prof), 0, 0);  // 00336170: the agent's own file
    const auto rows = host_.profile_dossiers();  // 00532b70
    const std::int32_t clp = host_.profile_clp(prof);
    for (std::int32_t i = 1; i < static_cast<std::int32_t>(rows.size()); ++i) {
        const auto& key = rows[static_cast<std::size_t>(i)].name;
        const auto text = key ? host_.get_string(*key) : std::nullopt;  // 003fdb50(+0)
        list_add(p.item_list, text, i, dossier_open(rows, clp, i) ? 0 : 1);
    }
}

void GuiRuntime::ds_select(Widget* w, std::int32_t row) {  // 00349fd0
    auto& p = *w->ps;
    const std::int32_t id = list_id(p.item_list, row);
    if (id == -1 || !dossier_open(host_.profile_dossiers(), host_.profile_clp(host_.current_profile()), id)) {
        send(w, &canvas_, 0x10, 4, 0);
        return;
    }
    send(w, &canvas_, 0x10, 0, 0);
    if (id == 0) {  // 0034a070: the agent's own file
        const bool noapp = g.app_loaded == 0;  // app +260
        std::int32_t pid = -2;
        if (noapp) {
            host_.app_2c82f0();
            pid = popup_open(0, std::string(), host_.get_string("LoadCharData"), nullptr, 0);  // 004ba5d0
        }
        Widget* n = factory("GuiAgentInfo", std::string("AGNTINFO"));  // 004ba5e0 / 004ba5f0
        if (noapp) {
            popup_close(pid);
            host_.app_2c81d0();
        }
        if (n) {
            ai_fill(n);             // 003609f0
            switch_screens(w, n);   // 00414ac0
        }
        return;
    }
    Widget* ss = factory("GuiSlideShow", std::string("ZEUSHELL"));  // 004ba600 / 004ba610
    if (!ss) return;
    ss_return(ss, widget_class_name(w->cls), w->name, row);  // 00327c70
    ss_clear(ss);                                            // 00328630
    const std::int32_t cat = ss_add_category(ss, list_text(p.item_list, row));
    ss_add_slide(ss, cat, fmt2("0_%d_%d.tdx", 1, id), host_.zeus_archive(), "");  // 004ba620, voice null
    switch_screens(w, ss);
}

// ---------------------------------------------------------------- GuiRanks (vtable 004ded50)

void GuiRuntime::rk_fill(Widget* w) {  // 0034e4d0
    auto& p = *w->ps;
    const auto cur = host_.profile_rank_row(host_.current_profile());  // *(agent + b50)
    const auto ranks = host_.profile_ranks();                           // 00532b50
    for (std::size_t k = 0; k < ranks.size(); ++k) {
        const auto text = ranks[k].name ? host_.get_string(*ranks[k].name) : std::nullopt;  // 003fdb50(+0)
        list_add(p.item_list, text, static_cast<std::int32_t>(k), ranks[k].index == cur.index ? 0 : 1);
    }
}

void GuiRuntime::rk_show(Widget* w, std::int32_t row) {  // 0034e770
    const std::int32_t id = list_id(w->ps->item_list, row);
    if (id == -1) return;
    const auto ranks = host_.profile_ranks();
    if (id < 0 || static_cast<std::size_t>(id) >= ranks.size()) unhosted(0x34e7c4, "rank row read past 00532b50");
    pe_show_item(w, id, ranks[static_cast<std::size_t>(id)].unlock);  // +c
}

void GuiRuntime::rk_select(Widget* w, std::int32_t row) {  // 0034e5a0
    auto& p = *w->ps;
    const std::int32_t id = list_id(p.item_list, row);
    if (id == -1) return;
    if (host_.profile_rank_row(host_.current_profile()).index < id) {
        send(w, &canvas_, 0x10, 4, 0);
        return;
    }
    send(w, &canvas_, 0x10, 1, 0);
    Widget* ss = factory("GuiSlideShow", std::string("ZEUSHELL"));  // 004ba930 / 004ba940
    if (!ss) return;
    ss_return(ss, widget_class_name(w->cls), w->name, row);
    ss_clear(ss);
    const std::int32_t cat = ss_add_category(ss, list_text(p.item_list, row));
    const auto ranks = host_.profile_ranks();
    if (static_cast<std::size_t>(id) >= ranks.size()) unhosted(0x34e6f0, "rank row read past 00532b50");
    std::string voice;  // the row's +10 (a 16-byte name)
    if (const auto& t = ranks[static_cast<std::size_t>(id)].text) {
        const auto end = std::find(t->begin(), t->end(), '\0');
        if (end == t->end()) unhosted(0x34e6f8, "rank voice name of 16 characters (no NUL)");
        voice.assign(t->begin(), end);
    }
    ss_add_slide(ss, cat, fmt2("0_%d_%d.tdx", p.kind_img, id + 1), host_.zeus_archive(), voice);  // 004ba950
    switch_screens(w, ss);
}

// ---------------------------------------------------------------- GuiRatings (vtable 004decc0)

void GuiRuntime::rt_init(Widget* w) {  // 0034dd20
    pe_init(w);
    auto& p = *w->ps;
    p.ach.clear();
    for (const auto& r : host_.profile_ratings(host_.current_profile()))  // 00333600
        p.ach.push_back({host_.loc_text(r.key), r.key, r.b8, r.unlock, r.percent});
    Widget* grp = find(w, "button_grp");
    if (!grp) return;
    const std::uint32_t zeus = host_.zeus_archive();
    for (std::int32_t i = 0; i < 24; ++i) {
        char name[8];
        std::snprintf(name, sizeof name, "%2.2d", static_cast<int>(i + 1));
        Widget* b = of_class(find(grp, name), WidgetClass::bitmap);
        p.grid[static_cast<std::size_t>(i)] = b;  // +6c + 4i
        if (!b) continue;
        bm_set_image(b, fmt2("0_%d_%ds.tdx", p.kind_img, i + 1), zeus);
        b->id10 = i;
        b->flags |= 0x80;
        const bool dim = i < static_cast<std::int32_t>(p.ach.size()) && p.ach[static_cast<std::size_t>(i)].flag == 0;
        b->bm->rgba = {1.0f, 1.0f, 1.0f, dim ? std::bit_cast<float>(0x3e99999au) : 1.0f};
    }
}

void GuiRuntime::rt_wake(Widget* w) {  // 0034dc20
    pe_wake(w);
    TextObject o;  // 003d0940(o, 0, 0, 10, 10, 00418420("callout"), 00587c80 = (1, 1, 1, 1))
    o.rect = {0, 0, 10, 10};
    o.font = host_.font("callout");
    o.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    host_.text_object(o);
    o.b10 = 2;
    o.b12 = 1;
    w->ps->pct_text = o;  // +dc
}

void GuiRuntime::rt_sleep(Widget* w) {  // 0034db10
    base_sleep(w);
    w->ps->pct_text.reset();
    w->id10 = -1;
}

std::uint8_t GuiRuntime::rt_input(Widget* w, const PadEvent& ev) {  // 0034d2d0
    if (ev.kind == 1 && ev.port == 0 && ev.released == 0) {
        switch (ev.mask) {
        case 0x8000: rt_move(w, 0); return 1;
        case 0x2000: rt_move(w, 1); return 1;
        case pad_up: rt_move(w, 2); return 1;
        case pad_down: rt_move(w, 3); return 1;
        default: break;
        }
    }
    // The original reads the canvas focus' +10 without a null check.
    if (pressed(ev, pad_cross) && canvas_.focus && canvas_.focus->id10 != -1) {
        auto& p = *w->ps;
        std::int32_t s3 = canvas_.focus->id10;
        if (s3 >= static_cast<std::int32_t>(p.ach.size())) unhosted(0x34d460, "rating row read past +d0");
        const auto& r = p.ach[static_cast<std::size_t>(s3)];
        if (r.flag == 0) {
            send(w, &canvas_, 0x10, 4, 0);
            return 1;
        }
        send(w, &canvas_, 0x10, 1, 0);
        Widget* ss = factory("GuiSlideShow", std::string("ZEUSHELL"));  // 004ba818 / 004ba828
        if (!ss) return 1;
        ss_return(ss, widget_class_name(w->cls), w->name, s3);
        ss_clear(ss);
        const std::int32_t cat = ss_add_category(ss, r.text);
        const std::string image = fmt2("0_%d_%d.tdx", p.kind_img, s3 + 1);  // 004ba838
        if (s3 % 4 == 2) --s3;  // the third column shares the second's voice
        const std::string voice = fmt2("0_%d_%d", p.kind_img, s3 + 1);  // 004ba848
        ss_add_slide(ss, cat, image, host_.zeus_archive(), voice);
        switch_screens(w, ss);
        return 1;
    }
    return pe_input(w, ev);  // 00356be0
}

void GuiRuntime::rt_move(Widget* w, std::int32_t dir) {  // 0034d770
    Widget* f = canvas_.focus;
    if (!f || f->id10 == -1) return;
    std::int32_t col = f->id10 % 4, row = f->id10 / 4;
    switch (dir) {
    case 0: --col; break;
    case 1: ++col; break;
    case 2: --row; break;
    case 3: ++row; break;
    default: break;
    }
    if (col < 0) col = 3;
    if (col > 3) col = 0;
    if (row < 0) row = 5;
    if (row > 5) row = 0;
    set_focus(w->ps->grid[static_cast<std::size_t>(row * 4 + col)]);  // 00414b30, even when null
}

std::uint8_t GuiRuntime::rt_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 0034d600
    const std::uint32_t k = type & 0xffff;
    auto& p = *w->ps;
    if (k == 0) {
        send(w, &canvas_, 0x10, 0, 0);  // the focus sound
        if (sender->id10 != -1) {
            if (sender->id10 >= static_cast<std::int32_t>(p.ach.size())) unhosted(0x34d680, "rating row read past +d0");
            pe_show_item(w, sender->id10, p.ach[static_cast<std::size_t>(sender->id10)].unlock);
            send(w, sender, 6, 0x4ba850, 0);  // "onActivate"
            return 1;
        }
    } else if (k == 1 && sender->id10 != -1) {
        send(w, sender, 6, 0x4ba860, 0);  // "onDeactivate"
        return 1;
    } else if (k == 0x3000) {
        if (a3 < 0 || a3 >= 24) unhosted(0x34d714, "rating grid index out of range");
        set_focus(p.grid[static_cast<std::size_t>(a3)]);
        w->id10 = a3;
        return 1;
    }
    return pe_event(w, sender, type, a3, t0);  // 00356b30
}

Widget* GuiRuntime::rt_first(Widget* w) {  // 0034dbe0
    if (w->id10 != -1 && w->id10 >= 0 && w->id10 < 24) return w->ps->grid[static_cast<std::size_t>(w->id10)];
    return v_first_base(w);
}

std::vector<GuiRuntime::RatingCell> GuiRuntime::rt_cells(const Widget* w) const {  // 0034d8c0's loop
    std::vector<RatingCell> out;
    const auto& p = *w->ps;
    if (!p.pct_text) return out;
    const auto n = static_cast<std::int32_t>(p.ach.size());
    for (std::int32_t r = 0; r < 6; ++r) {
        std::int32_t s0 = -1;
        for (std::int32_t c = 0; c < 4; ++c) {
            const std::int32_t i = r * 4 + c;
            if (i < n && p.ach[static_cast<std::size_t>(i)].flag == 0) {
                s0 = i;
                break;
            }
        }
        if (s0 < 0 || !p.grid[static_cast<std::size_t>(s0)]) continue;
        const std::int32_t pct = p.ach[static_cast<std::size_t>(s0)].percent;
        char buf[16];
        std::snprintf(buf, sizeof buf, pct < 10 ? "%1.1d%%" : "%2.2d%%", static_cast<int>(pct));
        out.push_back({p.grid[static_cast<std::size_t>(s0)], buf});
    }
    return out;
}

// ---------------------------------------------------------------- GuiOmegaStrain (vtable 004dede0)
// Each entry rebuilds 00333260(agent, 4, 0, list) as the original does (it steps the award element).

void GuiRuntime::om_wake(Widget* w) {  // 0034ef10
    pe_wake(w);
    Widget* t = of_text(find(w, "value_percentage"));  // 00492bc0
    if (!t) return;
    const auto items = host_.profile_items(host_.current_profile(), 4, 0);
    std::int32_t n2 = 0;
    for (const auto& it : items)
        if (it.b8 == 2) ++n2;
    if (items.empty()) unhosted(0x34f010, "Omega Strain percentage of no requirements (div.s by zero)");
    const float f = 100.0f * (static_cast<float>(n2) / static_cast<float>(items.size()));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%3.0f%%", static_cast<double>(f));  // 0011bd70, 004ba9f8
    set_text(t, buf, false);
}

void GuiRuntime::om_fill(Widget* w) {  // 0034edf0
    const auto items = host_.profile_items(host_.current_profile(), 4, 0);
    const auto classified = host_.get_string("Classified");  // 004ba9e8
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        list_add(w->ps->item_list, it.b8 == 0 ? classified : it.text, static_cast<std::int32_t>(i), earned_style(it.b8));
    }
}

void GuiRuntime::om_show(Widget* w, std::int32_t row) {  // 0034ea00
    const auto items = host_.profile_items(host_.current_profile(), 4, 0);
    if (row < 0 || row >= static_cast<std::int32_t>(items.size())) return;
    pe_show_item(w, row, items[static_cast<std::size_t>(row)].unlock);  // +18
}

void GuiRuntime::om_select(Widget* w, std::int32_t row) {  // 0034ec00
    const auto items = host_.profile_items(host_.current_profile(), 4, 0);
    if (row < 0 || row >= static_cast<std::int32_t>(items.size())) return;
    const auto& it = items[static_cast<std::size_t>(row)];
    if (it.b8 != 2) {
        send(w, &canvas_, 0x10, 4, 0);
        return;
    }
    send(w, &canvas_, 0x10, 1, 0);
    Widget* ss = factory("GuiSlideShow", std::string("ZEUSHELL"));  // 004ba9b8 / 004ba9c8
    if (!ss) return;
    ss_return(ss, widget_class_name(w->cls), w->name, row);
    ss_clear(ss);
    const std::int32_t cat = ss_add_category(ss, list_text(w->ps->item_list, row));
    char image[64];
    std::snprintf(image, sizeof image, "%d_%d_%d.tdx", static_cast<int>(it.a), static_cast<int>(it.b),
                  static_cast<int>(it.c));  // 004ba9d8 of +c / +10 / +14
    ss_add_slide(ss, cat, image, host_.zeus_archive(), "");
    switch_screens(w, ss);
}

// ---------------------------------------------------------------- GuiAchievementScreen (vtable 004def90)

void GuiRuntime::ah_init(Widget* w) {  // 00353850
    pe_init(w);
    auto& p = *w->ps;
    p.mode = 0;
    p.sub_file.clear();
    p.kind_img = 0;
    p.offset = 0;
}

void GuiRuntime::ah_wake(Widget* w) {  // 00353800
    auto& p = *w->ps;
    p.ach.clear();
    for (const auto& r : host_.profile_vec(host_.current_profile(), p.mode))  // 003334c0(agent, +7c, +6c)
        p.ach.push_back({host_.loc_text(r.key), r.key, r.b8, r.unlock, r.percent});
    pe_wake(w);
}

void GuiRuntime::ah_fill(Widget* w) {  // 00353700
    auto& p = *w->ps;
    for (std::size_t i = 0; i < p.ach.size(); ++i)
        list_add(p.item_list, p.ach[i].text, static_cast<std::int32_t>(i), p.ach[i].flag ? 0 : 1);
}

void GuiRuntime::ah_show(Widget* w, std::int32_t row) {  // 003536a0
    auto& p = *w->ps;
    const std::int32_t id = list_id(p.item_list, row);
    if (id == -1) return;
    if (id < 0 || id >= static_cast<std::int32_t>(p.ach.size())) unhosted(0x3536e0, "achievement row read past +6c");
    pe_show_item(w, id, p.ach[static_cast<std::size_t>(id)].unlock);
}

void GuiRuntime::ah_select(Widget* w, std::int32_t row) {  // 003535d0
    auto& p = *w->ps;
    const std::int32_t id = list_id(p.item_list, row);
    if (id == -1 || id >= static_cast<std::int32_t>(p.ach.size())) {
        send(w, &canvas_, 0x10, 4, 0);
        return;
    }
    pe_open_menu(w, p.sub_file, p.mode, row, p.kind_img, id + p.offset);  // 003569f0
    send(w, &canvas_, 0x10, 0, 0);
}

// ---------------------------------------------------------------- GuiPersonnelMenu (vtable 004df020)

void GuiRuntime::pm_init(Widget* w) {  // 003557d0
    pe_init(w);
    auto& p = *w->ps;
    p.ret_class.clear();
    p.ret_file.clear();
    p.pm_kind = 0;
    p.pm_row = -1;
    p.pm_a = p.pm_b = 0;
    p.frame = of_class(find(w, "bitmap_frame"), WidgetClass::bitmap);  // +98
    if (p.frame) p.frame->bm->z = std::bit_cast<float>(0x42b40000u);  // 90.0 at bitmap +70
    p.group1 = find(w, "group1_submenu");  // +9c
    p.group2 = find(w, "group2_bitmap");   // +a0
    p.buttonx = find(w, "buttonx");        // +a4
    p.f4 = 0;
    if (p.item_list) array_set_row_height(p.item_list, p.item_list->list->row_h - 2);  // 00424820(+48, +5c - 2)
    pm_toggle(w, 0);
}

void GuiRuntime::pm_wake(Widget* w) {  // 00355790
    pe_wake(w);
    host_.options_frontend_wake();
}

void GuiRuntime::pm_sleep(Widget* w) {  // 003556f0
    base_sleep(w);
    auto& p = *w->ps;
    p.f4 = 0;
    host_.radio_sound(0, p.voice_type, p.voice_name, 0);
    host_.options_frontend_sleep();
}

std::uint8_t GuiRuntime::pm_input(Widget* w, const PadEvent& ev) {  // 00354a70
    if (pressed(ev, pad_cross)) {
        auto& p = *w->ps;
        if (p.has_detail) {
            send(w, &canvas_, 0x10, 2, 0);
            pm_toggle(w, p.detail == 0 ? 1 : 0);
        }
        return 1;
    }
    return pe_input(w, ev);
}

void GuiRuntime::pm_fill(Widget* w) {  // 00354b50
    auto& p = *w->ps;
    std::vector<GuiHost::BonusItem> items;
    const std::int32_t prof = host_.current_profile();
    if (p.pm_kind == 6) {
        const char* name = p.pm_row >= 0 && p.pm_row < static_cast<std::int32_t>(stat_levels)
                               ? cc_level_names[static_cast<std::size_t>(p.pm_row)]
                               : nullptr;
        if (!name) return;
        const auto short_name = host_.profile_6060(prof, name);  // 00336060
        if (!short_name) return;
        items = host_.profile_level_objectives(prof, *short_name);  // 003353f0
    } else {
        items = host_.profile_items(prof, p.pm_kind, p.pm_row);  // 00333260
    }
    const auto classified = host_.get_string("Classified");
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        list_add(p.item_list, it.b8 == 0 ? classified : it.text, static_cast<std::int32_t>(i), earned_style(it.b8));
    }
}

void GuiRuntime::pm_back(Widget* w) {  // 003549c0
    auto& p = *w->ps;
    Widget* n = pe_achievement(p.pm_kind);
    if (!n) n = factory(p.ret_class, p.ret_file);
    if (!n) return;
    send(w, n, 0x3000, p.pm_row, 0);
    switch_screens(w, n);
}

void GuiRuntime::pm_setup(Widget* w, const std::string& cls, const std::string& file, std::int32_t kind,
                          std::int32_t row, std::int32_t a, std::int32_t b) {  // 00354e40
    auto& p = *w->ps;
    p.ret_class = cls;
    p.ret_file = file;
    p.pm_kind = static_cast<std::int8_t>(kind);
    p.pm_row = row;
    p.pm_a = a;
    p.pm_b = b;
    if (Widget* t = of_text(find(w, "menutitle"))) {
        if (p.pm_kind != 6) {
            const auto rows = host_.profile_vec(host_.current_profile(), p.pm_kind);  // 003334c0
            if (row < 0 || row >= static_cast<std::int32_t>(rows.size()))
                unhosted(0x354ec0, "a sub-menu row past the award list (read unchecked)");
            {
                const auto& r = rows[static_cast<std::size_t>(row)];
                t->b75 = 1;  // 0041c0f0(t, 1)
                set_text(t, cstr_of(host_.loc_text(r.key).value_or("")), false);
                p.has_detail = r.b8 != 0;
                pm_toggle(w, p.has_detail);
            }
        } else {
            const char* name = row >= 0 && row < static_cast<std::int32_t>(stat_levels)
                                   ? cc_level_names[static_cast<std::size_t>(row)]
                                   : nullptr;
            if (name) {
                set_text(t, cstr_of(host_.get_string(name).value_or("")), false);
                t->b75 = 1;
            }
            p.has_detail = 0;
            pm_toggle(w, 0);
        }
    }
    if (p.buttonx) {
        if (p.has_detail) show(p.buttonx);
        else hide(p.buttonx);
    }
}

void GuiRuntime::pm_toggle(Widget* w, std::uint8_t v) {  // 00355490
    auto& p = *w->ps;
    if (v == 1) {
        if (p.group2) show(p.group2);
        if (p.group1) hide(p.group1);
        if (p.frame) {
            bm_set_image(p.frame, fmt2("0_%d_%d.tdx", p.pm_a, p.pm_b), host_.zeus_archive());
            show(p.frame);
            p.f4 = 0;
            host_.radio_sound(0, p.voice_type, p.voice_name, 0);
            p.voice_type = 4;
            p.voice_name = fmt2("0_%d_%d", p.pm_a, p.pm_b);
            p.f4 = 0;
            host_.radio_sound(2, p.voice_type, p.voice_name, 0);
        }
    } else {
        if (p.group2) hide(p.group2);
        if (p.group1) show(p.group1);
        if (p.frame) {
            bm_set_image(p.frame, "", 0);  // 0043c810
            hide(p.frame);
        }
        p.f4 = 0;
        host_.radio_sound(0, p.voice_type, p.voice_name, 0);
    }
    p.detail = v;  // +240
}

// ---------------------------------------------------------------- GuiAgentInfo (vtable 004df350)
// The dossier's row 0 (00349fd0 -> 0034a070): the agent's own file, AGNTINFO.GUI over the net screen
// 0033d580 (offline: its network manager 004294c0 is null). Mode +fc is 0 for the dossier; mode 1
// (another player's agent, from the online lobby's agent info 0033e410: 00360610) since checkpoint 132on2i.

namespace {

// The widget the original dereferences without a null test.
Widget* need(Widget* w, std::uint32_t address, const char* what) {
    if (!w) unhosted(address, std::string("a null ") + what);
    return w;
}

std::string fmt1(const char* f, std::int32_t a) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, a);
    return buf;
}
std::string fmt_s(const char* f, const std::string& s) {
    char buf[128];
    std::snprintf(buf, sizeof buf, f, s.c_str());
    return buf;
}

// EE COP1 (as gui_agent_modify.cpp): denormals flush to a signed zero, x / 0 the signed maximum,
// cvt.w.s saturates.
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
std::int32_t cvt_w(float x) {
    x = ftz(x);
    if (std::isnan(x)) unhosted(0x360b60, "cvt.w.s of NaN");
    if (x >= 2147483647.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    return static_cast<std::int32_t>(x);
}

constexpr float ad_pi = std::bit_cast<float>(0x40490fdbu);
constexpr float ad_two_pi = std::bit_cast<float>(0x40c90fdbu);
constexpr float ad_minus_pi = std::bit_cast<float>(0xc0490fdbu);

// The idle clip's clock (as gui_agent_modify.cpp's preview_clock). PORT-ONLY: 30 Hz pictures.
float ad_clock(float seconds) { return std::floor(seconds * 30.0f) / 30.0f; }

const Rgba ai_stat_name = rgba_bits(0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000);  // 00587be0
const Rgba ai_stat_value = rgba_bits(0x3f3ebebf, 0x3f67e7e8, 0x3f800000, 0x3f800000);  // 00587bd0

}  // namespace

void GuiRuntime::ai_init(Widget* w) {  // 00361aa0
    base_init(w);  // 0033d3f0: 0041f150, then +7c "start_button"
    auto& a = *w->ai;
    a.start_button = find(w, "start_button");
    a.value_agent = of_text(find(w, "value_agent"));                                  // +100
    a.value_rank = of_text(find(w, "value_rank"));                                    // +104
    a.value_cell = of_text(find(w, "value_cell"));                                    // +108
    if (a.value_cell) {  // 0041c0d0(t, 1): +76 = 1, the text object's +5b = 1
        a.value_cell->b76 = 1;
        if (a.value_cell->obj) a.value_cell->obj->b5b = 1;
    }
    a.ratings_list = of_class(find(w, "ratings_list"), WidgetClass::textarray);       // +10c
    a.statistics = of_class(find(w, "statistics"), WidgetClass::textarray);           // +110
    a.button01 = of_class(find(w, "01"), WidgetClass::button);                        // +114
    a.button02 = of_class(find(w, "02"), WidgetClass::button);                        // +118
    a.thumbnail = of_class(find(w, "thumbnail"), WidgetClass::bitmap);                // +11c
    a.display = of_class(find(w, "playercharacter"), WidgetClass::agentdisplay);      // +120
    need(a.ratings_list, 0x361d30, "ratings_list")->flags &= ~0x80;
    need(a.statistics, 0x361d40, "statistics")->flags &= ~0x80;
    array_set_row_height(a.statistics, a.statistics->list->row_h - 2);      // 00424820(+110, +5c - 2)
    array_set_row_height(a.ratings_list, a.ratings_list->list->row_h - 4);  // 00424820(+10c, +5c - 4)
    // 002c9d50(*004ffc04): the front end's character data (app +260), allocated when absent.
    if (!host_.agent_parts()) unhosted(0x2c9d50, "the front end's character data (app +260)");
}

void GuiRuntime::ai_deinit(Widget* w) {  // 00361a70: 0041f090, then 002c9c80(*004ffc04, 0)
    (void)w;
    if (g.app_loaded) host_.app_40f540();
}

void GuiRuntime::ai_wake(Widget* w) {  // 0033d3a0: 0041efa0, vt +64 (0029cb90, empty), +7c's +10 = -1
    base_wake(w);
    if (w->ai->start_button) w->ai->start_button->id10 = -1;
}

void GuiRuntime::ai_sleep(Widget* w) { base_sleep(w); }  // 0033d360: 0041ebf0, vt +68 (0029cba0, empty)

std::uint8_t GuiRuntime::ai_input(Widget* w, const PadEvent& ev) {  // 003617c0
    auto& a = *w->ai;
    if (pressed(ev, pad_triangle)) {
        if (a.mode != 0) {  // 00361890 (132on2i): the lobby shown again when it is on the canvas, this screen popped
            if (Widget* l = a.lobby) {
                if (std::find(canvas_.children.begin(), canvas_.children.end(), l) != canvas_.children.end())
                    l->flags |= flag_visible;
            }
            pop(w);  // the manager's vt +68
            return 1;
        }
        Widget* n = factory("GuiDossiers", std::string("DOSSIERS"));  // 004bb958 / 004bb968
        switch_screens(w, n);                                         // 00414ac0
        return 1;
    }
    const bool pad0 = ev.kind == 1 && ev.port == 0;
    if (pad0 && (ev.mask == 2 || ev.mask == 1)) {  // R2 / L2, held: +4e0 the turn rate
        Widget* d = need(a.display, 0x361998, "playercharacter");
        const float step = ev.mask == 2 ? (ev.released == 0 ? -1.0f : 1.0f) : (ev.released == 0 ? 1.0f : -1.0f);
        d->ad->rate = fadd(d->ad->rate, step);
        return 1;
    }
    // 0033cef0: the net screen's input reads the network manager's byte +78.
    if (a.net78 == 0) return 0;
    unhosted(0x33cef0, "the net screen's input with a network manager");
}

void GuiRuntime::ai_update(Widget* w, float dt) {  // 003616e0
    base_update(w, dt);  // 0033d310: 0041e3b0, vt +60 (0033cca0: the null network manager)
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
    auto& a = *w->ai;
    if (a.display && top() != w) a.display->ad->rate = 0.0f;  // the canvas top is another screen
}

std::uint8_t GuiRuntime::ai_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 00361730
    auto& a = *w->ai;
    if ((type & 0xffff) == 2) {
        if (sender == a.button02) {  // 0033c9e0(this, +f8, the name)
            ai_cell_invite(w, a.target, a.name);
            return 1;
        }
        if (sender == a.button01) {  // 0033c7c0(this, +f8, the name): the contacts' add
            ct_add(w, a.target, a.name);
            return 1;
        }
    }
    return net_event(w, sender, type, a3, t0);  // 0033cf90
}

void GuiRuntime::ai_fill(Widget* w) {  // 003609f0
    auto& a = *w->ai;
    a.words.fill(0);  // 00360510(+88)
    a.rank = 0;
    a.ratings.fill(0);
    a.specials.fill(0);
    const std::int32_t cp = host_.current_profile();  // 001314f0(004ffb50, -1)
    a.name = cstr_of(host_.profile_name(cp).value_or(""));  // 00336170
    // 004294c0 (the network manager) is null offline: no copy of its +e8c to the agent's +8bc.
    a.cell = cstr_of(host_.profile_text(cp, 0x8bc));
    a.rank = host_.profile_rank_row(cp).index;  // *(*(agent + b50) + 4)
    auto s = [&](std::int32_t i) { return host_.profile_statistic(cp, i); };  // 00333ee0
    auto percent = [&](std::int32_t num, std::int32_t den) {  // 100.0 * (float)num / (float)den
        if (s(num) == 0 || s(den) == 0) return 0;
        const float n = static_cast<float>(s(num)), d = static_cast<float>(s(den));
        return cvt_w(fmul(std::bit_cast<float>(0x42c80000u), fdiv(n, d)));
    };
    a.words[0] = s(0xd);
    a.words[1] = s(9);
    a.words[2] = s(0xa);
    a.words[3] = s(2);
    a.words[4] = s(1);
    a.words[5] = s(4);
    a.words[6] = percent(0xc, 0xb);
    a.words[7] = percent(0xa, 9);
    a.words[8] = s(6);
    a.words[9] = s(7);
    a.words[10] = s(8);
    a.words[11] = s(0);
    const auto ratings = host_.profile_ratings(cp);  // 00333600: the 24 rows read unchecked
    if (ratings.size() < a.ratings.size()) unhosted(0x360c78, "fewer than 24 rating rows");
    for (std::size_t i = 0; i < a.ratings.size(); ++i) a.ratings[i] = ratings[i].b8 != 0;
    const auto specials = host_.profile_vec(cp, 0);  // 003334c0(agent, 0)
    for (std::size_t i = 0; i < a.specials.size(); ++i) a.specials[i] = i < specials.size() && specials[i].b8 != 0;
    if (const auto look = host_.profile_appearance(cp))  // *(agent + 9a4) to the display's +a0 (0x440 bytes)
        need(a.display, 0x360da4, "playercharacter")->ad->packing = *look;
    ai_refresh(w);  // 00361000
    a.mode = 0;     // +fc
}

void GuiRuntime::ai_refresh(Widget* w) {  // 00361000
    auto& a = *w->ai;
    set_text(need(a.value_agent, 0x361040, "value_agent"), a.name, false);
    std::string cell = a.cell;
    if (cell.empty()) cell = host_.get_string("NoneOption").value_or("");  // 004bb8b0
    set_text(need(a.value_cell, 0x361098, "value_cell"), cell, false);
    set_text(need(a.value_rank, 0x3610e0, "value_rank"),
             host_.get_string(fmt1("S_RANK_%2.2d", a.rank + 1)).value_or(""), false);  // 004bb8c0
    if (Widget* t = of_text(find(w, "menutitle")))
        set_text(t, host_.get_string(a.mode == 1 ? "AgentInfo" : "Dossier").value_or(""), false);
    // The ratings: column 0 the last rating earned of each four (or "Classified"), column 1 the
    // special ratings earned.
    Widget* rl = need(a.ratings_list, 0x361178, "ratings_list");
    ta_set_dims(rl, 2, 8);
    ta_set_halign(rl, 2);
    array_set_col_width(rl, 0, 0x70);
    array_set_col_width(rl, 1, 0xa);
    const std::int32_t cp = host_.current_profile();
    const auto rows = host_.profile_ratings(cp);  // 00333600
    std::size_t k = 0;
    for (std::uint32_t r = 0; r < 6; ++r) {
        std::int32_t idx = -1;
        for (int j = 0; j < 4; ++j, ++k)
            if (a.ratings[k]) idx = static_cast<std::int32_t>(k);
        std::optional<std::string> text;
        if (idx == -1) text = host_.get_string("Classified");  // 004bb8f8
        else {
            if (static_cast<std::size_t>(idx) >= rows.size()) unhosted(0x361248, "a rating row past the list");
            text = host_.get_string(fmt_s("%s_S", host_.string_key(rows[static_cast<std::size_t>(idx)].key)));
        }
        ta_set_cell(rl, 0, r, text);
    }
    const auto specials = host_.profile_vec(cp, 0);  // 003334c0(agent, 0)
    for (std::size_t r = 0; r < specials.size() && r < 8; ++r)
        if (a.specials[r])
            ta_set_cell(rl, 1, static_cast<std::uint32_t>(r),
                        host_.get_string(fmt_s("%s_S", host_.string_key(specials[r].key))));
    // The statistics: the "AgentInfoStat%d" names, the words ("%d%%" for the two percentages).
    Widget* st = need(a.statistics, 0x361378, "statistics");
    ta_set_dims(st, 2, 0xc);
    array_set_col_width(st, 0, 0xa0);
    array_set_col_width(st, 1, 0xa);
    for (std::uint32_t r = 0; r < 0xc; ++r) {
        ta_set_cell(st, 0, r, host_.get_string(fmt1("AgentInfoStat%d", static_cast<std::int32_t>(r))));
        ta_set_cell(st, 1, r, fmt1(r == 6 || r == 7 ? "%d%%" : "%d", a.words[r]));
        ta_set_cell_rgba(st, 0, r, ai_stat_name);
        ta_set_cell_halign(st, 0, r, 2);
        ta_set_cell_rgba(st, 1, r, ai_stat_value);
        ta_set_cell_halign(st, 1, r, 1);
    }
    bm_set_image(need(a.thumbnail, 0x3614d0, "thumbnail"), fmt1("0_6_%ds.tdx", a.rank + 1), host_.zeus_archive());
    Widget* bx = find(w, "buttonx");
    Widget* bd = find(w, "buttondpad");
    if (bx) a.mode == 1 ? show(bx) : hide(bx);
    if (bd) a.mode == 1 ? show(bd) : hide(bd);
    Widget* b1 = need(a.button01, 0x361698, "01");
    Widget* b2 = need(a.button02, 0x3616a8, "02");
    if (a.mode != 1) {
        hide(b1);
        hide(b2);
        return;
    }
    // Mode 1 (132on2i): "01" (the contact invitation) and "02" (the agency cell's) shown; enabled with a session for
    // another agent: "02" when the own agent leads a cell the agent is not in (0044a0d0), "01" when the agent is not
    // a contact (0044d670).
    show(b1);
    show(b2);
    constexpr auto off = static_cast<std::uint16_t>(~flag_enabled);
    if (!host_.net_session() || a.target == -1) {
        b2->flags &= off;
        b1->flags &= off;
        return;
    }
    if (a.leader && !host_.lobby_cell_has(a.target)) b2->flags |= flag_enabled;
    else b2->flags &= off;
    if (!host_.lobby_is_contact(a.target)) b1->flags |= flag_enabled;
    else b1->flags &= off;
}

// 00360610(this, the lobby record, its agent's record): the online lobby's agent (mode 1).
void GuiRuntime::ai_fill_net(Widget* w, const LobbyRecord& r, const std::array<std::uint8_t, 0xfc>& rec) {
    auto& a = *w->ai;
    a.words.fill(0);  // 00360510(+88)
    a.rank = 0;
    a.ratings.fill(0);
    a.specials.fill(0);
    a.rank = rec[1];
    a.name = cstr_of(r.name);
    if (host_.net_session()) {
        a.leader = host_.lobby_cell_leader() ? 1 : 0;  // 0044a1d0
        if ((host_.lobby_error() & 0xff) == 0) {        // 00452ff0(session, 0)
            if (r.id == host_.lobby_own_id()) {
                a.target = -1;
                a.cell = cstr_of(host_.lobby_cell_name());  // 0044a070
            } else {
                a.target = r.id;
                a.cell = std::string();  // 004bb868
                std::int32_t clan = 0;
                std::memcpy(&clan, rec.data() + 0xe8, 4);
                if (clan != -1) {
                    if (const auto n = host_.lobby_cell_name_of(clan)) a.cell = cstr_of(*n);  // 00448ff0
                }
            }
        }
    }
    auto u16 = [&](std::size_t o) { return static_cast<std::int32_t>(rec[o] | rec[o + 1] << 8); };
    auto u32 = [&](std::size_t o) {
        std::uint32_t v = 0;
        std::memcpy(&v, rec.data() + o, 4);
        return static_cast<std::int32_t>(v);
    };
    if (rec[0] == 0xb) {
        a.words = {u16(4), u32(0x10), u32(0x14), u32(0x18), u16(6), u16(8),
                   rec[2], rec[3], u16(0xa), u16(0xc), u32(0x20), u32(0x1c)};
        const auto bits = static_cast<std::uint32_t>(u32(0x24));
        for (std::size_t k = 0; k < a.ratings.size(); ++k)
            if (bits >> k & 1) a.ratings[k] = 1;
        for (std::size_t k = 0; k < a.specials.size(); ++k)
            if (bits >> (k + 0x18) & 1) a.specials[k] = 1;
        // The 0xc0-byte compact appearance (+28) in a zeroed 0x100 buffer, expanded (00406ec0) over the zeroed packing.
        std::array<std::uint8_t, 0x100> compact{};
        std::copy_n(rec.begin() + 0x28, 0xc0, compact.begin());
        omega::AgentPartCache* cache = host_.agent_parts();
        if (!cache) unhosted(0x3608ac, "the front end's character part cache (0055dda0)");
        auto& p = need(a.display, 0x360890, "playercharacter")->ad->packing;
        p.fill(0);
        omega::agent_packing_expand(*cache, compact, p);
    } else {
        // 003608c0: Skl_Load 00402840 packs MALE.SKL / FEMALE.SKL into 0055d510 / 0055d950 (the bytes the text
        // names), as GuiAgentCreate's 0028f760; the display shows the male default (0055d510).
        static constexpr const char* skl[2] = {"GAMEDATA\\FRONTEND\\MALE.SKL", "GAMEDATA\\FRONTEND\\FEMALE.SKL"};
        g.packing_slot = -1;
        for (std::size_t i = 0; i < 2; ++i) {
            const omega::AppearancePacking ap = omega::pack_appearance(host_.skl_file(skl[i]));
            auto& dst = g.packings[i];
            for (std::size_t k = 0; k < dst.size(); ++k)
                if (ap.initialized[k]) dst[k] = ap.bytes[k];
        }
        g.packing_slot = 0;
        need(a.display, 0x3608dc, "playercharacter")->ad->packing = g.packings[0];
    }
    a.mode = 1;     // +fc
    ai_refresh(w);  // 00361000
}

// 0033c9e0(screen, id, name): the agency cell's invitation.
void GuiRuntime::ai_cell_invite(Widget* w, std::int32_t id, const std::string& name) {
    const std::int32_t wait = popup_open(0, std::string(), lobby_string(0x4a, name), nullptr, 0);
    if (host_.lobby_cell_invite(id)) {
        popup_open(2, std::string(), lobby_string(0x4b, name), nullptr, 0);
    } else {
        const std::int32_t err = host_.lobby_error() & 0xff;
        if (err == 0x3f) {
            const std::string title = lobby_string(0x4c);
            popup_open(5, title, lobby_string(0x55), w, 0x13);
        } else {
            std::uint32_t text = 0x4d;
            switch (err) {
            case 0x2a: text = 0x4f; break;
            case 0x1f: text = 0x4e; break;
            case 0x1d: text = 0x17; break;
            case 0x1e: text = 0x50; break;
            default: break;
            }
            const std::string title = lobby_string(0x4c);
            popup_open(2, title, lobby_string(text, name), nullptr, 0);
        }
    }
    popup_close(wait);
}

// ---------------------------------------------------------------- GuiAgentDisplay (vtable 004df530)

void GuiRuntime::ad_deinit(Widget* w) {  // 003641c0: 0041f090, 002c9c80(*004ffc04, 0), the actor freed
    (void)w;
    if (g.app_loaded) host_.app_40f540();
}

// 00362dc0: 0041efa0; +4e0 = 0; the actor rebuilt from +a0 (the host's preview poses the packing);
// the camera record from the cache's first MANSORT2 row, its location (0.75, 11.5, 0.3).
void GuiRuntime::ad_wake(Widget* w) {
    base_wake(w);
    auto& d = *w->ad;
    d.rate = 0.0f;
    omega::AgentPartCache* cache = host_.agent_parts();
    if (!cache) unhosted(0x362dc0, "GuiAgentDisplay without the front end's character data");
    d.camera = omega::agent_camera_initial(*cache);
    d.camera.loc = {std::bit_cast<float>(0x3f400000u), std::bit_cast<float>(0x41380000u),
                    std::bit_cast<float>(0x3e99999au)};
    d.anim_time = 0.0f;
    d.pose_time = ad_clock(d.anim_time);
}

// 00362d00: 0041e3b0, then +7c += pi * (dt / 30) * +4e0, wrapped into [-pi, pi].
void GuiRuntime::ad_update(Widget* w, float dt) {
    base_update(w, dt);
    auto& d = *w->ad;
    float v = fadd(d.camera.rot[2], fmul(fmul(ad_pi, fdiv(dt, 30.0f)), d.rate));
    if (!(v <= ad_pi)) v = fsub(v, ad_two_pi);
    else if (v < ad_minus_pi) v = fadd(v, ad_two_pi);
    d.camera.rot[2] = v;
    // The draw 00361e70 steps the actor by the manager's dt after drawing (as GuiAgentModify's
    // 00342768): the picture shows the pose before this update's step.
    d.pose_time = ad_clock(d.anim_time);
    d.anim_time += dt / 30.0f;
}

}  // namespace omega::gui
