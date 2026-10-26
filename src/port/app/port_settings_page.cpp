#include "port_settings_page.hpp"

#include "display_settings.hpp"
#include "input_bindings.hpp"
#include "mod_manager.hpp"
#include "mouse_look.hpp"
#include "pad_connection.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace omega {

std::optional<std::string> port_setting_value(const PortSettings& s, const std::string& key) {
    std::optional<std::string> v;
    for (const auto& [k, value] : s.other)
        if (k == key) v = value;
    return v;
}

void set_port_setting(PortSettings& s, const std::string& key, const std::string& value) {
    bool set = false;
    for (auto it = s.other.begin(); it != s.other.end();) {
        if (it->first != key) {
            ++it;
            continue;
        }
        if (!set) {
            it->second = value;
            set = true;
            ++it;
        } else {
            it = s.other.erase(it);
        }
    }
    if (!set) s.other.emplace_back(key, value);
}

std::size_t erase_port_settings(PortSettings& s, const std::string& prefix) {
    const auto before = s.other.size();
    s.other.erase(std::remove_if(s.other.begin(), s.other.end(),
                                 [&](const auto& kv) { return kv.first.rfind(prefix, 0) == 0; }),
                  s.other.end());
    return before - s.other.size();
}

namespace {
constexpr std::int32_t rows_max = gui::PortSettingsScreen::rows;
constexpr std::array<const char*, PortSettingsPage::page_count> page_names{
    "DISPLAY", "INPUT", "PLAY KEYS 1", "PLAY KEYS 2", "PLAY KEYS 3", "PLAY KEYS 4", "MENU KEYS 1", "MENU KEYS 2", "MODS"};
constexpr std::array<const char*, 8> display_rows{"FULLSCREEN",    "VSYNC",       "ASPECT",   "RENDER SCALE",
                                                 "FRAME OVERLAY", "OVERLAY KEY", "BG AUDIO", "HUD MARGIN"};
constexpr std::array<const char*, 7> input_rows{"MOUSE LOOK", "SENSITIVITY", "INVERT Y", "PAD PROMPT", "RESET KEYS",
                                                "FIELD OF VIEW", "MOVIES"};  // 132ws3 / 132vt2a2: the DISPLAY page is full
constexpr std::array<MovieFit, 3> movie_fits{MovieFit::fill, MovieFit::bars, MovieFit::stretch};  // 132vt2a2: movies=
constexpr std::array<float, 8> render_scales{0.5F, 0.75F, 1.0F, 1.25F, 1.5F, 2.0F, 3.0F, 4.0F};
// 132vp4: the Vita's (the scene scale; press steps down: 100 -> 75 -> 50 -> 100 %)
constexpr std::array<float, 3> vita_render_scales{0.5F, 0.75F, 1.0F};
constexpr std::array<float, 14> sensitivities{0.25F, 0.5F, 0.75F, 1.0F, 1.25F, 1.5F, 2.0F, 2.5F, 3.0F, 4.0F, 5.0F, 6.0F, 8.0F, 10.0F};
// 132ws: the ASPECT row's values: the aspect settings, then wide with movie_fill=zoom (WIDE ZOOM).
struct AspectChoice {
    FrameAspect aspect;
    bool zoom;
    const char* label;
};
constexpr std::array<AspectChoice, 5> aspects{{{FrameAspect::pillarbox, false, "PILLARBOX"},
                                              {FrameAspect::strict, false, "4:3"},
                                              {FrameAspect::stretch, false, "STRETCH"},
                                              {FrameAspect::wide, false, "WIDE"},
                                              {FrameAspect::wide, true, "WIDE ZOOM"}}};
std::size_t aspect_choice(const DisplaySettings& d) {
    for (std::size_t i = 0; i < aspects.size(); ++i)
        if (aspects[i].aspect == d.aspect && (d.aspect != FrameAspect::wide || aspects[i].zoom == d.movie_zoom)) return i;
    return 0;
}
constexpr std::array<unsigned, 6> hud_margins{0, 2, 4, 6, 8, 10};  // 132ws: percent
constexpr std::array<unsigned, 9> fovs{70, 80, 90, 100, 110, 120, 130, 140, 150};  // 132ws3: percent
constexpr std::uint8_t vk_escape = 0x1b, vk_f11 = 0x7a, vk_f3 = 0x72;
constexpr std::uint16_t page_rows = 8;  // binding rows per key page
constexpr std::size_t mod_window_max = 7;  // 132md4: mod rows on the MODS page (rows 2..8)
constexpr std::size_t mod_window = 6;      // with more mods: 6 a time and MORE MODS

// The buttons in the order shown (ds2_button_name's names) and their labels.
constexpr std::array<std::pair<const char*, const char*>, 16> button_rows{{
    {"cross", "CROSS"}, {"circle", "CIRCLE"}, {"square", "SQUARE"}, {"triangle", "TRIANGLE"},
    {"l1", "L1"}, {"r1", "R1"}, {"l2", "L2"}, {"r2", "R2"},
    {"l3", "L3"}, {"r3", "R3"}, {"select", "SELECT"}, {"start", "START"},
    {"up", "D-PAD UP"}, {"down", "D-PAD DOWN"}, {"left", "D-PAD LEFT"}, {"right", "D-PAD RIGHT"}}};
constexpr std::array<const char*, 4> directions{"up", "down", "left", "right"};
constexpr std::array<const char*, 4> direction_labels{"UP", "DOWN", "LEFT", "RIGHT"};

const char* on_off(bool on) { return on ? "ON" : "OFF"; }
std::string number(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", static_cast<double>(v));
    return b;
}
template <std::size_t N>
std::size_t nearest(const std::array<float, N>& list, float v) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < N; ++i)
        if (std::fabs(list[i] - v) < std::fabs(list[best] - v)) best = i;
    return best;
}
template <std::size_t N>
std::size_t step(std::size_t i, bool forward) {
    return forward ? (i + 1) % N : (i + N - 1) % N;
}
std::string join_keys(const std::vector<std::uint8_t>& keys) {
    if (keys.empty()) return "NONE";
    std::string s = key_name(keys[0]);
    if (keys.size() > 1) s += ", " + key_name(keys[1]);
    if (keys.size() > 2) s += " +" + std::to_string(keys.size() - 2);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}
std::size_t button_bit(const char* name) {
    for (std::size_t k = 0; k < 16; ++k)
        if (std::string(ds2_button_name(k)) == name) return k;
    throw std::logic_error(std::string("port settings page: no ds2 button ") + name);
}
// A key the page can bind: named (key_name / key_code round trip), not Esc (cancel) nor F11 (the window's).
bool bindable(std::uint8_t vk) {
    if (vk == 0 || vk == vk_escape || vk == vk_f11) return false;
    const std::string n = key_name(vk);
    if (n.rfind("VK", 0) == 0) return false;
    const auto back = key_code(n);
    return back && *back == vk;
}
}  // namespace

PortSettingsPage::PortSettingsPage(std::shared_ptr<PortSettingsStore> store, PortSettingsHooks hooks)
    : store_(std::move(store)), hooks_(std::move(hooks)) {
    if (!store_) throw std::invalid_argument("port settings page: no settings store");
}

std::vector<PortSettingsPage::Binding> PortSettingsPage::bindings_on(std::int32_t page) const {
    std::vector<Binding> all;
    const bool play = page >= static_cast<std::int32_t>(Page::play_keys_1) && page <= static_cast<std::int32_t>(Page::play_keys_4);
    const bool menu = page >= static_cast<std::int32_t>(Page::menu_keys_1) && page <= static_cast<std::int32_t>(Page::menu_keys_2);
    if (!play && !menu) return all;
    const std::string profile = play ? "play" : "menu";
    for (const auto& [name, label] : button_rows) all.push_back({"bind." + profile + "." + name, label});
    if (play) {
        for (std::size_t d = 0; d < 4; ++d)
            all.push_back({std::string("bind.play.stick_left.") + directions[d], std::string("MOVE ") + direction_labels[d]});
        for (std::size_t d = 0; d < 4; ++d)
            all.push_back({std::string("bind.play.stick_right.") + directions[d], std::string("LOOK ") + direction_labels[d]});
        all.push_back({"bind.play.walk", "WALK"});
        for (std::size_t d = 0; d < 4; ++d)
            all.push_back({std::string("bind.play.menu_dpad.") + directions[d], std::string("MENU ") + direction_labels[d]});
    }
    const std::int32_t first_page = play ? static_cast<std::int32_t>(Page::play_keys_1) : static_cast<std::int32_t>(Page::menu_keys_1);
    const std::size_t from = static_cast<std::size_t>(page - first_page) * page_rows;
    std::vector<Binding> out;
    for (std::size_t i = from; i < all.size() && i < from + page_rows; ++i) out.push_back(all[i]);
    return out;
}

namespace {
// Port-only (Vita): the PC rows set_pc_rows(false) hides, by their place in the page's full row list.
constexpr std::array<std::size_t, 1> display_pc_rows{5};           // OVERLAY KEY
constexpr std::array<std::size_t, 4> input_pc_rows{0, 1, 2, 4};    // MOUSE LOOK, SENSITIVITY, INVERT Y, RESET KEYS
}  // namespace

std::int32_t PortSettingsPage::rows_on_page() const {
    switch (static_cast<Page>(page_)) {
    case Page::display:
        return 1 + static_cast<std::int32_t>(display_rows.size() - (pc_rows_ ? 0 : display_pc_rows.size()));
    case Page::input:
        return 1 + static_cast<std::int32_t>(input_rows.size() - (pc_rows_ ? 0 : input_pc_rows.size()));
    case Page::mods: return 2 + mod_rows();
    default: return 1 + static_cast<std::int32_t>(bindings_on(page_).size());
    }
}

bool PortSettingsPage::page_shown(std::int32_t p) const {
    if (p < 0 || p >= page_count) return false;
    return pc_rows_ || bindings_on(p).empty();
}

std::int32_t PortSettingsPage::step_page(bool forward) const {
    auto p = static_cast<std::size_t>(page_);
    do p = step<page_count>(p, forward);
    while (!page_shown(static_cast<std::int32_t>(p)));
    return static_cast<std::int32_t>(p);
}

std::size_t PortSettingsPage::row_id(std::size_t r) const {
    if (pc_rows_) return r;
    auto skip = [r](const auto& hidden) {
        std::size_t id = 0;
        for (std::size_t shown = 0;; ++id) {
            if (std::find(hidden.begin(), hidden.end(), id) != hidden.end()) continue;
            if (shown++ == r) return id;
        }
    };
    if (page_ == static_cast<std::int32_t>(Page::display)) return skip(display_pc_rows);
    if (page_ == static_cast<std::int32_t>(Page::input)) return skip(input_pc_rows);
    return r;
}

void PortSettingsPage::set_pc_rows(bool shown) {
    if (shown == pc_rows_) return;
    pc_rows_ = shown;
    capture_.reset();
    if (!page_shown(page_)) page_ = static_cast<std::int32_t>(Page::display);
    ++revision_;
}

void PortSettingsPage::set_page(std::int32_t p) {
    if (p < 0 || p >= page_count) throw std::out_of_range("port settings page: page out of range");
    if (!page_shown(p)) throw std::out_of_range("port settings page: page hidden (no PC rows)");
    if (p == page_) return;
    page_ = p;
    capture_.reset();
    mod_list_.reset();
    mod_offset_ = 0;
    ++revision_;
}

std::string PortSettingsPage::title() const { return "PORT SETTINGS"; }

std::string PortSettingsPage::label(std::int32_t row) const {
    if (row < 0 || row >= rows_max || row >= rows_on_page()) return {};
    if (row == 0) return "PAGE";
    const auto r = row_id(static_cast<std::size_t>(row - 1));
    switch (static_cast<Page>(page_)) {
    case Page::display: return display_rows[r];
    case Page::input: return input_rows[r];
    case Page::mods: {
        if (r == 0) return "ALL MODS";
        const auto& list = mod_list();
        if (list.empty()) return "NO MODS FOUND";
        const auto shown = static_cast<std::size_t>(list.size() > mod_window_max ? mod_window : list.size());
        if (r - 1 == shown) return "MORE MODS";
        if (mod_offset_ + r - 1 >= list.size()) return {};
        const auto& m = list[mod_offset_ + r - 1];
        std::string name = m.manifest ? m.manifest->name : m.folder;
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return name.size() > 17 ? name.substr(0, 16) + "." : name;
    }
    default: return bindings_on(page_)[r].label;
    }
}

std::vector<std::uint8_t> PortSettingsPage::keys_of(const PortSettings& s, const Binding& b) const {
    if (b.overlay_key) {
        const auto d = display_settings(s);
        return d.frame_overlay_key ? std::vector<std::uint8_t>{*d.frame_overlay_key} : std::vector<std::uint8_t>{};
    }
    const BindingSet set = parse_input_bindings(s);
    const bool play = b.key.rfind("bind.play.", 0) == 0;
    const InputBindings& in = play ? set.play : set.menu;
    const std::string input = b.key.substr(10);  // after "bind.play." / "bind.menu."
    for (std::size_t d = 0; d < 4; ++d) {
        if (input == std::string("stick_left.") + directions[d]) return in.stick_left[d];
        if (input == std::string("stick_right.") + directions[d]) return in.stick_right[d];
        if (input == std::string("menu_dpad.") + directions[d]) return in.menu_dpad[d];
    }
    if (input == "walk") return in.walk;
    return in.buttons[button_bit(input.c_str())];
}

std::string PortSettingsPage::binding_value(const PortSettings& s, const Binding& b) const {
    if (capture_ && capture_->target.key == b.key) return capture_->append ? "ADD A KEY" : "PRESS A KEY";
    return join_keys(keys_of(s, b));
}

// Checkpoint 132win12: a key list as the bind.* value ("Space,MouseLeft"; "none" for an empty list).
std::string PortSettingsPage::keys_text(const std::vector<std::uint8_t>& keys) {
    if (keys.empty()) return "none";
    std::string v;
    for (const auto k : keys) v += (v.empty() ? "" : ",") + key_name(k);
    return v;
}

std::string PortSettingsPage::value(std::int32_t row) const {
    if (row < 0 || row >= rows_max || row >= rows_on_page()) return {};
    if (row == 0) {
        std::int32_t at = 0, shown = 0;
        for (std::int32_t p = 0; p < page_count; ++p) {
            if (!page_shown(p)) continue;
            ++shown;
            if (p <= page_) at = shown;
        }
        return std::string(page_names[static_cast<std::size_t>(page_)]) + "  " + std::to_string(at) + "/" +
               std::to_string(shown);
    }
    const PortSettings s = store_->read();
    const auto r = row_id(static_cast<std::size_t>(row - 1));
    switch (static_cast<Page>(page_)) {
    case Page::display: {
        const auto d = display_settings(s);
        switch (r) {
        case 0: return on_off(hooks_.fullscreen_get ? hooks_.fullscreen_get() : s.fullscreen);
        case 1: return on_off(s.vsync);
        case 2: return aspects[aspect_choice(d)].label;
        case 3:
            if (capture_ && capture_->text) return *capture_->text + "_";
            if (!pc_rows_)   // 132vp4: the Vita's scene scale (display_settings.hpp vita_render_scale)
                return std::to_string(std::lround(vita_render_scale(d.render_scale) * 100.0F)) + "%";
            return number(d.render_scale) + "X";
        case 4: return on_off(d.frame_overlay);
        case 5: return binding_value(s, {"frame_overlay_key", "OVERLAY KEY", true});
        case 6: return on_off(background_audio_setting(s));
        default: return std::to_string(d.hud_margin) + "%";
        }
    }
    case Page::input: {
        const auto m = mouse_look_settings(s);
        switch (r) {
        case 0: return on_off(m.enabled);
        case 1: return capture_ && capture_->text ? *capture_->text + "_" : number(m.sensitivity);
        case 2: return on_off(m.invert_y);
        case 3: return on_off(controller_prompt_setting(s));
        case 4: {
            const auto n = parse_input_bindings(s).overridden;
            return n == 0 ? std::string("DEFAULT") : "CUSTOM (" + std::to_string(n) + ")";
        }
        case 5: return std::to_string(display_settings(s).fov) + "%";  // 132ws3
        default: {  // 132vt2a2
            std::string v = frame_movie_fit_name(display_settings(s).movie_fit);
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return v;
        }
        }
    }
    case Page::mods: return mod_value(r);
    default: return binding_value(s, bindings_on(page_)[r]);
    }
}

void PortSettingsPage::commit(PortSettings& s, const std::string& what) {
    // Each value is checked by the readers before it is written (a bad value would throw at the next start).
    (void)display_settings(s);
    (void)mouse_look_settings(s);
    (void)parse_input_bindings(s);
    (void)controller_prompt_setting(s);
    (void)background_audio_setting(s);
    (void)mods_setting(s);
    (void)mod_choices_setting(s);
    store_->write(s);
    mod_list_.reset();
    ++revision_;
    log_.push_back("Port settings page (port-only): " + what + ", written to " + store_->where());
    if (hooks_.changed) hooks_.changed(s);
    const auto listeners = listeners_;  // a listener may unlisten
    for (const auto& [id, f] : listeners) f(s);
}

std::size_t PortSettingsPage::listen(std::function<void(const PortSettings&)> f) {
    listeners_.emplace(next_listener_, std::move(f));
    return next_listener_++;
}

void PortSettingsPage::unlisten(std::size_t id) { listeners_.erase(id); }

bool PortSettingsPage::holds_input(bool input_down) {
    if (capture_) return true;
    if (wait_release_ && input_down) return true;
    wait_release_ = false;
    return false;
}

bool PortSettingsPage::act(std::int32_t row, Action a) {
    if (a == Action::page_previous || a == Action::page_next) {
        set_page(step_page(a == Action::page_next));
        return true;
    }
    if (row < 0 || row >= rows_on_page()) return false;
    if (capture_) return false;  // the hosts give the GUI no buttons while a key is awaited
    const bool forward = a != Action::previous;
    if (row == 0) {
        set_page(step_page(forward));
        return true;
    }
    PortSettings s = store_->read();
    const auto r = row_id(static_cast<std::size_t>(row - 1));
    auto start_capture = [&](Binding b, bool append = false) {
        capture_ = Capture{std::move(b), std::nullopt, append, std::nullopt};
        ++revision_;
        log_.push_back("Port settings page (port-only): " + capture_->target.label +
                       (append ? " waits for a key to add" : " waits for a key"));
        return true;
    };
    // Checkpoint 132win12: Cross on a number row types its value (digits, ".", Backspace; Enter sets it, Esc cancels).
    auto start_entry = [&](Binding b) {
        capture_ = Capture{std::move(b), std::nullopt, false, std::string()};
        ++revision_;
        log_.push_back("Port settings page (port-only): " + capture_->target.label + " waits for a typed value");
        return true;
    };
    switch (static_cast<Page>(page_)) {
    case Page::display: {
        const auto d = display_settings(s);
        switch (r) {
        case 0: {
            const bool on = !(hooks_.fullscreen_get ? hooks_.fullscreen_get() : s.fullscreen);
            s.fullscreen = on;
            commit(s, std::string("FULLSCREEN ") + on_off(on));
            if (hooks_.fullscreen_set) hooks_.fullscreen_set(on);
            ++revision_;
            return true;
        }
        case 1:
            s.vsync = !s.vsync;
            commit(s, std::string("VSYNC ") + on_off(s.vsync));
            return true;
        case 2: {
            const AspectChoice& next = aspects[step<aspects.size()>(aspect_choice(d), forward)];
            set_port_setting(s, "aspect", frame_aspect_name(next.aspect));
            if (next.aspect == FrameAspect::wide || d.movie_zoom) set_port_setting(s, "movie_fill", next.zoom ? "zoom" : "bars");
            commit(s, std::string("ASPECT ") + frame_aspect_name(next.aspect) + (next.zoom ? " (movie_fill zoom)" : ""));
            return true;
        }
        case 3: {
            if (!pc_rows_) {   // 132vp4: the Vita's 100 / 75 / 50 %, cycled (no keyboard for a typed value)
                const float v = vita_render_scales[step<vita_render_scales.size()>(
                    nearest(vita_render_scales, vita_render_scale(d.render_scale)), a == Action::press ? false : forward)];
                set_port_setting(s, "render_scale", number(v));
                commit(s, "RENDER SCALE " + std::to_string(std::lround(v * 100.0F)) + "%");
                return true;
            }
            if (a == Action::press) return start_entry({"render_scale", "RENDER SCALE"});
            const float v = render_scales[step<render_scales.size()>(nearest(render_scales, d.render_scale), forward)];
            set_port_setting(s, "render_scale", number(v));
            commit(s, "RENDER SCALE " + number(v));
            return true;
        }
        case 4:
            set_port_setting(s, "frame_overlay", d.frame_overlay ? "0" : "1");
            commit(s, std::string("FRAME OVERLAY ") + on_off(!d.frame_overlay));
            return true;
        case 5:
            if (a == Action::press) return start_capture({"frame_overlay_key", "OVERLAY KEY", true});
            set_port_setting(s, "frame_overlay_key", d.frame_overlay_key ? "none" : key_name(vk_f3));
            commit(s, std::string("OVERLAY KEY ") + (d.frame_overlay_key ? "none" : key_name(vk_f3)));
            return true;
        case 6: {
            const bool on = !background_audio_setting(s);
            set_port_setting(s, "background_audio", on ? "1" : "0");
            commit(s, std::string("BG AUDIO ") + on_off(on));
            return true;
        }
        default: {  // 132ws: the wide HUD's safe-area margin
            const auto at = static_cast<std::size_t>(
                std::find(hud_margins.begin(), hud_margins.end(), d.hud_margin) - hud_margins.begin());
            const unsigned v = hud_margins[at == hud_margins.size() ? 0 : step<hud_margins.size()>(at, forward)];
            set_port_setting(s, "hud_margin", std::to_string(v));
            commit(s, "HUD MARGIN " + std::to_string(v) + "%");
            return true;
        }
        }
    }
    case Page::input: {
        const auto m = mouse_look_settings(s);
        switch (r) {
        case 0:
            set_port_setting(s, "mouse_look", m.enabled ? "0" : "1");
            commit(s, std::string("MOUSE LOOK ") + on_off(!m.enabled));
            return true;
        case 1: {
            if (a == Action::press) return start_entry({"mouse_sensitivity", "SENSITIVITY"});
            const float v = sensitivities[step<sensitivities.size()>(nearest(sensitivities, m.sensitivity), forward)];
            set_port_setting(s, "mouse_sensitivity", number(v));
            commit(s, "SENSITIVITY " + number(v));
            return true;
        }
        case 2:
            set_port_setting(s, "mouse_invert_y", m.invert_y ? "0" : "1");
            commit(s, std::string("INVERT Y ") + on_off(!m.invert_y));
            return true;
        case 3: {
            const bool on = !controller_prompt_setting(s);
            set_port_setting(s, "controller_prompt", on ? "1" : "0");
            commit(s, std::string("PAD PROMPT ") + on_off(on));
            return true;
        }
        case 4: {
            if (a != Action::press) return false;
            const auto n = erase_port_settings(s, "bind.");
            commit(s, "RESET KEYS (" + std::to_string(n) + " bind.* keys removed)");
            return true;
        }
        case 5: {  // 132ws3: the wide view's field of view (draw only; the original 100%)
            const unsigned f = display_settings(s).fov;
            const auto at = static_cast<std::size_t>(std::find(fovs.begin(), fovs.end(), f) - fovs.begin());
            const unsigned v = fovs[at == fovs.size() ? 3 : step<fovs.size()>(at, forward)];
            set_port_setting(s, "fov", std::to_string(v));
            commit(s, "FIELD OF VIEW " + std::to_string(v) + "%");
            return true;
        }
        default: {  // 132vt2a2: the wide movies' fit (fill, bars, stretch)
            const MovieFit f = display_settings(s).movie_fit;
            const auto at = static_cast<std::size_t>(std::find(movie_fits.begin(), movie_fits.end(), f) - movie_fits.begin());
            const MovieFit v = movie_fits[step<movie_fits.size()>(at, forward)];
            set_port_setting(s, "movies", frame_movie_fit_name(v));
            commit(s, std::string("MOVIES ") + frame_movie_fit_name(v));
            return true;
        }
        }
    }
    case Page::mods: return mod_act(r, a);
    default: {
        const Binding b = bindings_on(page_)[r];
        if (a == Action::press) return start_capture(b);
        if (a == Action::next) return start_capture(b, true);  // 132win12: Right adds a key to the list
        auto keys = keys_of(s, b);                              // 132win12: Left removes the list's last key
        if (keys.empty()) return false;
        const std::string gone = key_name(keys.back());
        keys.pop_back();
        set_port_setting(s, b.key, keys_text(keys));
        commit(s, b.label + " - " + gone + " = " + keys_text(keys) + " (" + b.key + ")");
        return true;
    }
    }
}

void PortSettingsPage::bind_key(std::uint8_t vk) {
    PortSettings s = store_->read();
    const Binding target = capture_->target;
    const bool append = capture_->append;
    capture_.reset();
    wait_release_ = true;
    if (append) {
        auto keys = keys_of(s, target);
        if (std::find(keys.begin(), keys.end(), vk) != keys.end()) {
            log_.push_back("Port settings page (port-only): " + target.label + " already has " + key_name(vk));
            ++revision_;
            return;
        }
        keys.push_back(vk);
        set_port_setting(s, target.key, keys_text(keys));
        commit(s, target.label + " + " + key_name(vk) + " = " + keys_text(keys) + " (" + target.key + ")");
        return;
    }
    set_port_setting(s, target.key, key_name(vk));
    commit(s, target.label + " = " + key_name(vk) + " (" + target.key + ")");
}

// Checkpoint 132win12: one key of a typed value. Digits (the row's or the keypad's) and "." add a character (8 at
// most), Backspace takes one back, Esc cancels, Enter writes the value when its reader takes it (in range) and
// cancels otherwise; other keys are ignored.
bool PortSettingsPage::feed_number(std::uint8_t vk) {
    constexpr std::uint8_t vk_back = 0x08, vk_return = 0x0d, vk_period = 0xbe, vk_decimal = 0x6e;
    std::string& text = *capture_->text;
    const std::string label = capture_->target.label;
    const auto end = [&](const std::string& why) {
        log_.push_back("Port settings page (port-only): " + label + " " + why);
        capture_.reset();
        wait_release_ = true;
        ++revision_;
        return true;
    };
    if (vk == vk_escape) return end("cancelled");
    if (vk == vk_return) {
        if (text.empty()) return end("cancelled (nothing typed)");
        PortSettings s = store_->read();
        const std::string key = capture_->target.key, typed = text;
        set_port_setting(s, key, typed);
        try {
            (void)display_settings(s);
            (void)mouse_look_settings(s);
        } catch (const std::exception& e) {
            return end("not set: " + typed + " (" + e.what() + ")");
        }
        capture_.reset();
        wait_release_ = true;
        commit(s, label + " " + typed + " (typed)");
        return true;
    }
    if (vk == vk_back) {
        if (!text.empty()) text.pop_back();
    } else if (text.size() < 8 && ((vk >= 0x30 && vk <= 0x39) || (vk >= 0x60 && vk <= 0x69))) {
        text += static_cast<char>('0' + (vk >= 0x60 ? vk - 0x60 : vk - 0x30));
    } else if (text.size() < 8 && (vk == vk_period || vk == vk_decimal) && text.find('.') == std::string::npos) {
        text += text.empty() ? "0." : ".";
    } else {
        return false;
    }
    ++revision_;
    return false;
}

bool PortSettingsPage::feed_keys(const KeyboardState& keys) {
    if (!capture_) return false;
    if (!test_keys_.empty()) {
        const std::uint8_t vk = test_keys_.front();
        test_keys_.pop_front();
        if (capture_->text) return feed_number(vk);
        if (vk == vk_escape || !bindable(vk) || (capture_->target.overlay_key && vk < 0x08)) {
            log_.push_back("Port settings page (port-only): " + capture_->target.label + " cancelled (test key " +
                           key_name(vk) + ")");
            capture_.reset();
            wait_release_ = true;
            ++revision_;
            return true;
        }
        bind_key(vk);
        return true;
    }
    if (!capture_->held) {
        capture_->held = keys;
        return false;
    }
    KeyboardState& held = *capture_->held;
    for (std::size_t vk = 1; vk < keys.size(); ++vk) {
        if (!keys[vk]) {
            held[vk] = false;
            continue;
        }
        if (held[vk]) continue;
        held[vk] = true;
        if (capture_->text) {
            if (feed_number(static_cast<std::uint8_t>(vk))) return true;
            continue;
        }
        if (vk == vk_escape) {
            log_.push_back("Port settings page (port-only): " + capture_->target.label + " cancelled (Esc)");
            capture_.reset();
            wait_release_ = true;
            ++revision_;
            return true;
        }
        if (!bindable(static_cast<std::uint8_t>(vk)) || (capture_->target.overlay_key && vk < 0x08)) continue;  // the overlay key is a keyboard key
        bind_key(static_cast<std::uint8_t>(vk));
        return true;
    }
    return false;
}

void PortSettingsPage::opened() {
    capture_.reset();
    mod_list_.reset();
    ++revision_;
}

void PortSettingsPage::closed() {
    if (capture_) {
        capture_.reset();
        ++revision_;
    }
}

// Checkpoint 132md4: the MODS page (the mods folder's mods, the player's mod.<folder>=0|1 over their manifests).
void PortSettingsPage::set_mods_folder(std::optional<std::filesystem::path> folder) {
    mods_folder_ = std::move(folder);
    mod_list_.reset();
    ++revision_;
}

std::filesystem::path PortSettingsPage::mods_folder() const {
    if (mods_folder_) return *mods_folder_;
    try {
        const std::string folder = mods_folder_setting(store_->read());
        if (!folder.empty()) return std::filesystem::path(std::u8string(folder.begin(), folder.end()));
    } catch (const std::exception&) {
    }
    return "mods";
}

const std::vector<mods::ModListing>& PortSettingsPage::mod_list() const {
    if (!mod_list_) {
        mods::ModChoices choices;
        try {
            for (const auto& [folder, on] : mod_choices_setting(store_->read())) choices[folder] = on;
        } catch (const std::exception&) {
        }
        mod_list_ = mods::list_mods(mods_folder(), choices);
    }
    return *mod_list_;
}

std::int32_t PortSettingsPage::mod_rows() const {
    const auto n = mod_list().size();
    if (n == 0) return 1;
    return static_cast<std::int32_t>(n > mod_window_max ? mod_window + 1 : n);
}

std::string PortSettingsPage::mod_value(std::size_t r) const {
    PortSettings s;
    try {
        s = store_->read();
    } catch (const std::exception&) {
        return "?";
    }
    if (r == 0) {
        try {
            return on_off(mods_setting(s));
        } catch (const std::exception&) {
            return "?";
        }
    }
    const auto& list = mod_list();
    if (list.empty()) return mods_folder().filename().string();
    if (list.size() > mod_window_max && r - 1 == mod_window) {
        const auto last = std::min(list.size(), mod_offset_ + mod_window);
        return std::to_string(mod_offset_ + 1) + "-" + std::to_string(last) + " OF " + std::to_string(list.size());
    }
    if (mod_offset_ + r - 1 >= list.size()) return {};
    const std::size_t index = mod_offset_ + r - 1;
    const auto& m = list[index];
    if (!m.manifest) return "INVALID";
    // 132md14: the place in the load order (#1 wins a key two mods override), and whether the running set differs:
    // the mod on or off, or (on in both) its place among the enabled mods.
    std::optional<std::size_t> running;
    if (const auto* set = mods::active())
        for (std::size_t k = 0; k < set->mods().size(); ++k)
            if (mods::upper(set->mods()[k].folder) == mods::upper(m.folder)) running = k;
    std::size_t enabled_place = 0;
    for (std::size_t k = 0; k < index; ++k)
        if (list[k].enabled) ++enabled_place;
    std::string v = "#" + std::to_string(index + 1) + " " + on_off(m.enabled);
    if (m.enabled != running.has_value() || (m.enabled && *running != enabled_place)) v += " (NEXT)";   // 132md15: the next level (or start)
    return v;
}

bool PortSettingsPage::mod_act(std::size_t r, Action a) {
    PortSettings s = store_->read();
    if (r == 0) {
        const bool on = !mods_setting(s);
        mods::set_mods_on(s, on);
        commit(s, std::string("ALL MODS ") + on_off(on) + " (mods=" + (on ? "1" : "0") + "; applies at the next level)");
        return true;
    }
    const auto& list = mod_list();
    if (list.empty()) return false;
    if (list.size() > mod_window_max && r - 1 == mod_window) {
        const auto pages = (list.size() + mod_window - 1) / mod_window;
        auto at = mod_offset_ / mod_window;
        at = a == Action::previous ? (at + pages - 1) % pages : (at + 1) % pages;
        mod_offset_ = at * mod_window;
        ++revision_;
        return true;
    }
    if (mod_offset_ + r - 1 >= list.size()) return false;
    const std::size_t index = mod_offset_ + r - 1;
    const auto m = list[index];
    if (!m.manifest) {
        log_.push_back("Port settings page (port-only): mod " + m.folder + " has no valid manifest (" + m.problem + ")");
        ++revision_;
        return false;
    }
    if (a != Action::press) {
        // Checkpoint 132md14: Left moves the mod up the load order (toward #1, which wins a key two mods override),
        // Right down: new manifest priority= values, as the launcher's Move up / down and omega_mod_tool move write.
        const int delta = a == Action::previous ? -1 : 1;
        const auto changes = mods::move_mod(list, index, delta);
        if (changes.empty()) return false;
        try {
            mods::apply_priority_changes(changes);
        } catch (const std::exception& e) {
            log_.push_back("Port settings page (port-only): mod " + m.folder + " not moved (" + e.what() + ")");
            mod_list_.reset();
            ++revision_;
            return false;
        }
        std::string what;
        for (const auto& c : changes)
            what += (what.empty() ? "" : ", ") + c.folder + " priority " + std::to_string(c.from) + " -> " + std::to_string(c.to);
        log_.push_back("Port settings page (port-only): MOD " + m.folder + (delta < 0 ? " UP" : " DOWN") + " (" + what +
                       "; applies at the next level)");
        mod_list_.reset();
        // Past seven mods the window follows the mod when it moves out of it.
        const auto& moved = mod_list();
        if (moved.size() > mod_window_max)
            for (std::size_t k = 0; k < moved.size(); ++k)
                if (moved[k].folder == m.folder && (k < mod_offset_ || k >= mod_offset_ + mod_window))
                    mod_offset_ = k / mod_window * mod_window;
        ++revision_;
        return true;
    }
    const bool on = !m.enabled;
    const std::string key = "mod." + m.folder;
    try {
        // 132md6: the panel's and omega_mod_tool's write: the folder's earlier mod.<folder>= lines of any case go.
        mods::set_mod_choice(s, m.folder, on);
    } catch (const std::exception&) {
        log_.push_back("Port settings page (port-only): mod folder '" + m.folder + "' cannot be named in the settings");
        ++revision_;
        return false;
    }
    commit(s, "MOD " + m.folder + " " + on_off(on) + " (" + key + "=" + (on ? "1" : "0") + "; applies at the next level)");
    return true;
}

std::vector<std::string> PortSettingsPage::take_log() { return std::exchange(log_, {}); }

}  // namespace omega
