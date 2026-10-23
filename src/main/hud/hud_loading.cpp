#include "hud_loading.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace omega {
namespace {
void remove_if_set(IeScreen& screen, int& element) {  // 00272280
    if (element >= 0 && !screen.element_at(element).freed) screen.remove_element(element);
    element = -1;
}
int anim_element(IeScreen& screen, int group, std::string_view name, float lo, float hi) {
    const int e = screen.find_element(group, name);
    if (e >= 0) ie_anim_range(screen.element_at(e).anim, lo, hi, 0);  // 003f4510(+0, lo, hi, 0)
    return e;
}
void show(IeScreen& screen, int element, std::uint8_t v) {
    if (element >= 0) screen.show_element(element, v);  // 003e8de0 (the callers test the slot first)
}
}  // namespace

LoadingScreen loading_screen_create(const IeFile& file, std::uint32_t text_word, IeTextureService* textures) {
    LoadingScreen s{IeScreen::load(file, text_word, textures)};
    s.screen.set_visible(s.screen.root(), 0, true);  // 003eebe0(root, 0, 1)
    return s;                                        // 00272170: +4..+38 null
}

int loading_text_element(IeScreen& screen, std::string_view name, const std::optional<std::string>& text,
                         std::uint32_t font_word) {
    const int e = screen.find_element(screen.root(), name);
    if (e < 0 || !text) return e;
    IeElement& el = screen.element_at(e);
    if (el.text < 0) return e;
    screen.set_text(el.text, *text);  // 003e7ec0 -> 003d08b0
    el.flag_19 = 0;
    IeTextObject& obj = screen.text_object(el.text);
    ie_text_colour(obj, loading_text_colour);  // 003d0810(+74, 0x4a8da0)
    obj.word_20 = font_word;                   // *(1698c0() + 0x280)
    return e;
}

void loading_screen_setup(LoadingScreen& s, const LoadingStrings& strings, const std::string& level,
                          std::uint32_t font_word) {
    IeScreen& sc = s.screen;
    const int root = sc.root();
    sc.set_visible(root, 1, true);
    if (const int g = sc.find_child(root, "background_panels_group"); g >= 0) {
        s.filmstrip_0c = anim_element(sc, g, "filmstrip", 0.0F, 300.0F);
        s.dotsbottom_14 = anim_element(sc, g, "dotsbottom", 0.0F, 60.0F);
        s.bar_10 = anim_element(sc, g, "bar", 0.0F, 60.0F);
        s.background_text_08 = anim_element(sc, g, "background_text", 0.0F, 60.0F);
    }
    s.pressx_24 = sc.find_element(root, "pressx");
    show(sc, s.pressx_24, 0);
    if (const auto c = strings("Continue")) {
        s.loading_data_text_38 = loading_text_element(sc, "loading_data_text", c, font_word);
        show(sc, s.loading_data_text_38, 0);
    }
    s.host_28 = sc.find_element(root, "host");
    show(sc, s.host_28, 0);
    s.whiteflash_18 = anim_element(sc, root, "whiteflash", 0.0F, 17.0F);
    s.backgroundmap1_1c = anim_element(sc, root, "backgroundmap1", 0.0F, 300.0F);
    s.backgroundmap2_20 = anim_element(sc, root, "backgroundmap2", 0.0F, 300.0F);
    std::optional<std::string> name = strings("level_techname1");
    if (!name) name = strings("level_name");
    if (!name) name = level;  // 0x4ffc90
    s.levelname1_30 = loading_text_element(sc, "levelname1", name, font_word);
    if (const auto t2 = strings("level_techname2")) s.levelname2_34 = loading_text_element(sc, "levelname2", t2, font_word);
    if (const auto b = strings("level_briefing")) s.briefing_2c = loading_text_element(sc, "briefing", b, font_word);
}

void loading_screen_progress(LoadingScreen& s, float p) {
    p = std::clamp(p, 0.0F, 1.0F);
    if (s.progress_04 < 0) return;
    IeAnim& a = s.screen.element_at(s.progress_04).anim;
    ie_anim_fade(a, ie_ftz(10.0F + ie_ftz(7.0F * p)));
    ie_anim_update(a, 7.0F);
}

void loading_screen_update(LoadingScreen& s, float dt) { s.screen.update(dt); }

void loading_screen_draw(GuiDrawList& out, IeDrawState& st, LoadingScreen& s) {
    hud_screen_draw_003e7670(out, st, s.screen, HudScreenDrawGlobals{});
}

void loading_screen_show_continue(LoadingScreen& s, std::uint8_t v) {
    show(s.screen, s.pressx_24, v);
    show(s.screen, s.loading_data_text_38, v);
    show(s.screen, s.host_28, 0);
}

void loading_screen_show_host(LoadingScreen& s, std::uint8_t v) {
    show(s.screen, s.host_28, v);
    show(s.screen, s.pressx_24, 0);
    show(s.screen, s.loading_data_text_38, 0);
}

void loading_screen_destroy(LoadingScreen& s) {
    if (s.destroyed) return;
    IeScreen& sc = s.screen;
    for (int* e : {&s.progress_04, &s.background_text_08, &s.filmstrip_0c, &s.bar_10, &s.dotsbottom_14,
                   &s.whiteflash_18, &s.backgroundmap1_1c, &s.backgroundmap2_20, &s.pressx_24, &s.host_28,
                   &s.briefing_2c, &s.levelname1_30, &s.levelname2_34, &s.loading_data_text_38})
        remove_if_set(sc, *e);
    sc.set_visible(sc.root(), 0, true);
    s.destroyed = true;
}

bool loading_vblank_1667c0(LoadingThreadGlobals& g, bool thread_exists) {
    ++g.vblanks;
    if (!thread_exists) return false;
    if (g.countdown == 0) return true;
    --g.countdown;
    return false;
}

std::uint64_t loading_clock_ticks(std::uint32_t t0_count, std::uint32_t t0_base, std::uint64_t ticks_533878) {
    const std::uint32_t count = t0_count + t0_base;
    float f = 0.0F;
    if (static_cast<std::int32_t>(count) >= 0) {
        f = static_cast<float>(static_cast<std::int32_t>(count));
    } else {  // 0x166c14: halve with the low bit folded in, convert, double
        f = static_cast<float>(static_cast<std::int32_t>((count >> 1) | (count & 1)));
        f = f + f;
    }
    const std::int32_t step = ie_cvt_w(ie_ftz(f / 5.76F));  // 0x40b851ec
    return ticks_533878 + static_cast<std::uint32_t>(step);   // dsll32 / dsrl32: zero extended
}

float loading_text_alpha(std::uint64_t ticks) {
    const std::int32_t low = static_cast<std::int32_t>(static_cast<std::uint32_t>(ticks));  // dsll32 / dsra32
    const std::int32_t rest = low % 100000;                                                 // div, mfhi
    const float f = ie_ftz(static_cast<float>(rest) / 100000.0F);  // 001002d0, / 0x47c35000
    if (f <= 0.5F) return ie_ftz(2.0F * f);
    return ie_ftz(2.0F * ie_ftz(1.0F - f));
}

void loading_frame_166850(GuiDrawList& out, IeDrawState& st, LoadingThreadGlobals& g,
                          const LoadingFrameServices& services) {
    if (!services.ticks) throw std::invalid_argument("loading frame: no clock");
    if (!g.started_528) {
        g.started_528 = 1;
        g.last_ticks = services.ticks();
    }
    g.countdown = loading_frame_skip + 1;
    st.mode = 0;  // *005392d0
    std::array<float, 4> rgba{1.0F, 1.0F, 1.0F, 1.0F};
    bool text = true;
    if (g.continue_4f0 && g.screen) {
        if (g.host_4e0) {
            if (g.screen->host_28 >= 0) {
                loading_screen_show_host(*g.screen, 1);
                text = false;
            }
        } else if (g.screen->pressx_24 >= 0) {
            loading_screen_show_continue(*g.screen, 1);
            text = false;
        }
    }
    if (text && g.loading_text) {
        const std::uint64_t now = services.ticks();
        if (!g.continue_4f0) rgba[3] = loading_text_alpha(now);
        const GuiFont* font = services.font ? services.font(g.font) : nullptr;
        if (!font) throw std::runtime_error("loading frame: no font for handle " + std::to_string(g.font));
        std::vector<std::uint8_t> s(g.loading_text->begin(), g.loading_text->end());
        s.push_back(0);
        gui_font_draw(out, *font, s, rgba, -270.0F, -190.0F, 0.001F, 0.0F, 0.0F, 0.0F);  // 0036a300
    }
    if (!g.screen) return;
    const std::uint64_t now = services.ticks();
    const float dt = ie_ftz(static_cast<float>(static_cast<std::int64_t>(now - g.last_ticks)) / 100000.0F);
    loading_screen_update(*g.screen, dt);
    g.last_ticks = now;
    loading_screen_progress(*g.screen,
                            ie_ftz(static_cast<float>(g.vblanks) / static_cast<float>(g.frames)));
    loading_screen_draw(out, st, *g.screen);
    // 003d0c10(003d1020()): each listed object's 003cfab0 (its state, then the draw).
    IeScreen& sc = g.screen->screen;
    const std::vector<int> listed = sc.drawn_texts();
    for (const int index : listed) {
        IeTextObject& obj = sc.text_object(index);
        if (obj.freed || obj.word_20 == 0) continue;  // 003cfaf8: no font, nothing
        const std::int32_t now_ms = ie_text_clock_ms(0, 0, static_cast<std::int64_t>(services.ticks()));
        ie_text_draw_state(obj, now_ms);
        const GuiFont* font = services.font ? services.font(obj.word_20) : nullptr;
        if (!font) throw std::runtime_error("loading frame: no font for text handle " + std::to_string(obj.word_20));
        gui_text_draw(out, gui_text_from_ie(obj, font));
    }
}

}  // namespace omega
