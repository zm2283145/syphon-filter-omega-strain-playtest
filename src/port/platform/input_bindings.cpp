#include "input_bindings.hpp"

#include "gamepad_reports.hpp"
#include "port_settings.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#endif
#endif

namespace omega {
namespace {

using Keys = InputBindings::Keys;

constexpr std::array<const char*, 16> button_names{"l2",    "r2",     "l1", "r1",    "triangle", "circle", "cross", "square",
                                                    "select", "l3",   "r3", "start", "up",       "right",  "down",  "left"};
constexpr std::array<const char*, 4> direction_names{"up", "down", "left", "right"};
constexpr std::uint8_t vk_f11 = 0x7a;

struct NamedKey {
    const char* name;
    std::uint8_t vk;
};
// Named keys (letters, digits and F1-F24 are generated in key_code / key_name).
constexpr NamedKey named_keys[] = {
    {"MouseLeft", 0x01}, {"MouseRight", 0x02}, {"MouseMiddle", 0x04}, {"Mouse4", 0x05}, {"Mouse5", 0x06},
    {"Backspace", 0x08}, {"Tab", 0x09},        {"Enter", 0x0d},       {"Shift", 0x10},  {"Ctrl", 0x11},
    {"Alt", 0x12},       {"Esc", 0x1b},        {"Space", 0x20},       {"PageUp", 0x21}, {"PageDown", 0x22},
    {"End", 0x23},       {"Home", 0x24},       {"Left", 0x25},        {"Up", 0x26},     {"Right", 0x27},
    {"Down", 0x28},      {"Insert", 0x2d},     {"Delete", 0x2e},      {"NumMul", 0x6a}, {"NumAdd", 0x6b},
    {"NumSub", 0x6d},    {"NumDecimal", 0x6e}, {"NumDiv", 0x6f},      {"LShift", 0xa0}, {"RShift", 0xa1},
    {"LCtrl", 0xa2},     {"RCtrl", 0xa3},      {"Semicolon", 0xba},   {"Equals", 0xbb}, {"Comma", 0xbc},
    {"Minus", 0xbd},     {"Period", 0xbe},     {"Slash", 0xbf},       {"Grave", 0xc0},  {"LBracket", 0xdb},
    {"Backslash", 0xdc}, {"RBracket", 0xdd},   {"Quote", 0xde},
};

std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t");
    return std::string(s.substr(b, e - b + 1));
}

Keys keys(std::initializer_list<std::uint8_t> list) { return Keys(list); }

std::size_t bit_index(std::uint16_t mask) {
    std::size_t k = 0;
    while (k < 16 && (mask >> k) != 1U) ++k;
    return k;
}

bool any_down(const Keys& list, const KeyboardState& state) {
    for (const auto vk : list)
        if (state[vk]) return true;
    return false;
}

Keys parse_keys(const std::string& setting, const std::string& value) {
    Keys out;
    if (lower(trim(value)) == "none") return out;
    std::size_t at = 0;
    while (at <= value.size()) {
        const auto end = std::min(value.find(',', at), value.size());
        const std::string name = trim(std::string_view(value).substr(at, end - at));
        at = end + 1;
        const auto vk = key_code(name);
        if (!vk) throw std::runtime_error("port settings: " + setting + ": unknown key '" + name + "'");
        if (*vk == vk_f11) throw std::runtime_error("port settings: " + setting + ": F11 is the fullscreen toggle (reserved)");
        if (std::find(out.begin(), out.end(), *vk) == out.end()) out.push_back(*vk);
    }
    return out;
}

std::optional<std::size_t> direction(std::string_view name) {
    for (std::size_t i = 0; i < direction_names.size(); ++i)
        if (name == direction_names[i]) return i;
    return std::nullopt;
}

}  // namespace

const char* ds2_button_name(std::size_t bit) { return bit < button_names.size() ? button_names[bit] : "?"; }

std::optional<std::uint8_t> key_code(std::string_view name) {
    const std::string n = lower(trim(name));
    if (n.size() == 1 && ((n[0] >= 'a' && n[0] <= 'z') || (n[0] >= '0' && n[0] <= '9')))
        return static_cast<std::uint8_t>(std::toupper(static_cast<unsigned char>(n[0])));
    if (n.size() >= 2 && n[0] == 'f' && std::all_of(n.begin() + 1, n.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const int f = std::stoi(n.substr(1));
        if (f >= 1 && f <= 24) return static_cast<std::uint8_t>(0x70 + f - 1);
        return std::nullopt;
    }
    if (n.size() == 4 && n.rfind("num", 0) == 0 && n[3] >= '0' && n[3] <= '9') return static_cast<std::uint8_t>(0x60 + (n[3] - '0'));
    for (const auto& k : named_keys)
        if (lower(k.name) == n) return k.vk;
    return std::nullopt;
}

std::string key_name(std::uint8_t vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= 0x70 && vk <= 0x87) return "F" + std::to_string(vk - 0x70 + 1);
    if (vk >= 0x60 && vk <= 0x69) return "Num" + std::to_string(vk - 0x60);
    for (const auto& k : named_keys)
        if (k.vk == vk) return k.name;
    return "VK" + std::to_string(vk);
}

InputBindings default_input_bindings(BindProfile profile) {
    InputBindings b;
    auto set = [&](std::uint16_t mask, Keys k) { b.buttons[bit_index(mask)] = std::move(k); };
    if (profile == BindProfile::menu) {
        // The front end's keys since checkpoint 126 (keyboard_pad.hpp), plus L3 / R3.
        set(ds2::up, keys({0x26}));
        set(ds2::right, keys({0x27}));
        set(ds2::down, keys({0x28}));
        set(ds2::left, keys({0x25}));
        set(ds2::cross, keys({0x0d, 0x20}));
        set(ds2::triangle, keys({0x1b, 0x08}));
        set(ds2::square, keys({0x10}));
        set(ds2::circle, keys({0x11}));
        set(ds2::l1, keys({'Q'}));
        set(ds2::r1, keys({'E'}));
        set(ds2::l2, keys({'1'}));
        set(ds2::r2, keys({'3'}));
        set(ds2::l3, keys({'Z'}));
        set(ds2::r3, keys({'C'}));
        set(ds2::select, keys({0x09}));
        set(ds2::start, keys({'P'}));
        return b;
    }
    // The play sandbox's keys (play_main.cpp before checkpoint 132win6), plus the mouse buttons and L3 / R3.
    set(ds2::cross, keys({0x20}));
    set(ds2::triangle, keys({'R'}));
    set(ds2::square, keys({'J', 0x01}));
    set(ds2::circle, keys({'K'}));
    set(ds2::l1, keys({'I', 0x02}));
    set(ds2::r1, keys({'U'}));
    set(ds2::l2, keys({'Q'}));
    set(ds2::r2, keys({'E'}));
    set(ds2::l3, keys({'X'}));
    set(ds2::r3, keys({'M'}));
    set(ds2::select, keys({'Z'}));
    set(ds2::start, keys({'P'}));
    set(ds2::up, keys({'Y'}));
    set(ds2::down, keys({'B'}));
    set(ds2::left, keys({'V'}));
    set(ds2::right, keys({'N'}));
    b.stick_left = {keys({'W', 0x26}), keys({'S', 0x28}), keys({'A', 0x25}), keys({'D', 0x27})};
    b.stick_right = {keys({'T'}), keys({'G'}), keys({'F'}), keys({'H'})};
    b.walk = keys({0x10});
    b.menu_dpad = {keys({0x26}), keys({0x28}), keys({0x25}), keys({0x27})};
    return b;
}

BindingSet default_binding_set() { return {default_input_bindings(BindProfile::menu), default_input_bindings(BindProfile::play), 0}; }

BindingSet parse_input_bindings(const PortSettings& settings) {
    BindingSet set = default_binding_set();
    // Buttons set explicitly; a key they name leaves the other buttons' default lists (a key is one button).
    std::array<std::array<bool, 16>, 2> explicit_button{};
    for (const auto& [key, value] : settings.other) {
        if (key.rfind("bind.", 0) != 0) continue;
        const std::string rest = lower(key.substr(5));
        const auto dot = rest.find('.');
        if (dot == std::string::npos) throw std::runtime_error("port settings: " + key + ": needs bind.<menu|play>.<input>");
        const std::string profile = rest.substr(0, dot), input = rest.substr(dot + 1);
        InputBindings* b = profile == "menu" ? &set.menu : profile == "play" ? &set.play : nullptr;
        if (!b) throw std::runtime_error("port settings: " + key + ": the profile is menu or play");
        Keys parsed = parse_keys(key, value);
        Keys* target = nullptr;
        auto& explicit_here = explicit_button[b == &set.menu ? 0 : 1];
        for (std::size_t k = 0; k < button_names.size(); ++k)
            if (input == button_names[k]) {
                target = &b->buttons[k];
                explicit_here[k] = true;
                for (std::size_t other = 0; other < b->buttons.size(); ++other) {
                    if (other == k || explicit_here[other]) continue;
                    auto& list = b->buttons[other];
                    for (const auto vk : parsed) list.erase(std::remove(list.begin(), list.end(), vk), list.end());
                }
            }
        if (!target && input == "walk") target = &b->walk;
        for (const auto& [group, list] : {std::pair<const char*, std::array<Keys, 4>*>{"stick_left.", &b->stick_left},
                                          {"stick_right.", &b->stick_right},
                                          {"menu_dpad.", &b->menu_dpad}}) {
            if (target || input.rfind(group, 0) != 0) continue;
            if (const auto d = direction(std::string_view(input).substr(std::char_traits<char>::length(group)))) target = &(*list)[*d];
        }
        if (!target) throw std::runtime_error("port settings: " + key + ": unknown input '" + input + "'");
        *target = std::move(parsed);
        ++set.overridden;
    }
    return set;
}

std::uint16_t bound_buttons(const InputBindings& b, const KeyboardState& state) {
    std::uint16_t m = 0;
    for (std::size_t k = 0; k < b.buttons.size(); ++k)
        if (any_down(b.buttons[k], state)) m = static_cast<std::uint16_t>(m | (1U << k));
    return m;
}

BoundInput bound_input(const InputBindings& b, const KeyboardState& state) {
    BoundInput in;
    in.held = bound_buttons(b, state);
    auto stick = [&](const std::array<Keys, 4>& s) {
        std::array<float, 2> v{};
        if (any_down(s[0], state)) v[1] += 1;
        if (any_down(s[1], state)) v[1] -= 1;
        if (any_down(s[3], state)) v[0] += 1;
        if (any_down(s[2], state)) v[0] -= 1;
        return v;
    };
    in.left = stick(b.stick_left);
    in.right = stick(b.stick_right);
    if (any_down(b.walk, state)) {
        in.left[0] *= 0.6F;
        in.left[1] *= 0.6F;
    }
    return in;
}

std::uint16_t bound_menu_buttons(const InputBindings& play, const KeyboardState& state) {
    return static_cast<std::uint16_t>(bound_buttons(play, state) | bound_menu_dpad(play, state));
}

std::uint16_t bound_menu_dpad(const InputBindings& play, const KeyboardState& state) {
    std::uint16_t m = 0;
    constexpr std::array<std::uint16_t, 4> dpad{ds2::up, ds2::down, ds2::left, ds2::right};
    for (std::size_t d = 0; d < 4; ++d)
        if (any_down(play.menu_dpad[d], state)) m = static_cast<std::uint16_t>(m | dpad[d]);
    return m;
}

bool any_bound_key(const InputBindings& b, const KeyboardState& state) {
    if (bound_buttons(b, state) || any_down(b.walk, state)) return true;
    for (const auto* s : {&b.stick_left, &b.stick_right, &b.menu_dpad})
        for (const auto& k : *s)
            if (any_down(k, state)) return true;
    return false;
}

std::vector<std::string> describe_input_bindings(const InputBindings& b, BindProfile profile) {
    std::vector<std::string> lines;
    auto list = [](const Keys& k) {
        if (k.empty()) return std::string("none");
        std::string s;
        for (const auto vk : k) s += (s.empty() ? "" : ", ") + key_name(vk);
        return s;
    };
    for (std::size_t k = 0; k < b.buttons.size(); ++k) lines.push_back(std::string(button_names[k]) + ": " + list(b.buttons[k]));
    if (profile == BindProfile::play) {
        for (std::size_t d = 0; d < 4; ++d) lines.push_back(std::string("stick_left.") + direction_names[d] + ": " + list(b.stick_left[d]));
        for (std::size_t d = 0; d < 4; ++d) lines.push_back(std::string("stick_right.") + direction_names[d] + ": " + list(b.stick_right[d]));
        lines.push_back("walk: " + list(b.walk));
        for (std::size_t d = 0; d < 4; ++d) lines.push_back(std::string("menu_dpad.") + direction_names[d] + ": " + list(b.menu_dpad[d]));
    }
    return lines;
}

KeyboardState host_keyboard_state() {
    KeyboardState keys{};
#ifdef _WIN32
    for (int vk = 1; vk < 256; ++vk) keys[static_cast<std::size_t>(vk)] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    // Port-only (checkpoint 132win): Alt+Enter toggles fullscreen (host_window.hpp), so its Enter is no Cross.
    if (keys[0x12]) keys[0x0d] = false;
#endif
    return keys;
}

}  // namespace omega
