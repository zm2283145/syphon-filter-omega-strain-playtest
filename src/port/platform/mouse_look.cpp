#include "mouse_look.hpp"

#include "port_settings.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

bool flag(const std::string& key, const std::string& value) {
    if (value != "0" && value != "1") throw std::runtime_error("port settings: " + key + " must be 0 or 1");
    return value == "1";
}

// One axis: the base byte's deflection outside the dead zone plus the motion's offset.
std::uint8_t axis(std::uint8_t base, int counts, float sensitivity) {
    if (counts == 0) return base;
    const float m = std::clamp(static_cast<float>(counts) * sensitivity / mouse_look_full_counts, -1.0F, 1.0F);
    const float offset = std::copysign(mouse_look_dead_bytes + (127.5F - mouse_look_dead_bytes) * std::fabs(m), m);
    float held = static_cast<float>(base) - 127.5F;
    if (std::fabs(held) <= 56.0F) held = 0.0F;  // inside the game's dead zone: the stick is idle
    const float byte = std::round(std::clamp(127.5F + held + offset, 0.0F, 255.0F));
    return static_cast<std::uint8_t>(byte);
}

int parse_int(const std::string& text, const std::string& item) {
    if (text.empty() || text.find_first_not_of("-0123456789") != std::string::npos || text.find('-', 1) != std::string::npos)
        throw std::runtime_error("--test-mouse: bad motion '" + item + "' (F:dx/dy or F-G:dx/dy)");
    return std::stoi(text);
}

std::uint64_t parse_frame(const std::string& text, const std::string& item) {
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("--test-mouse: bad frame in '" + item + "' (F:dx/dy or F-G:dx/dy)");
    return std::stoull(text);
}

}  // namespace

MouseLookSettings mouse_look_settings(const PortSettings& settings) {
    MouseLookSettings s;
    for (const auto& [key, value] : settings.other) {
        if (key == "mouse_look") s.enabled = flag(key, value);
        else if (key == "mouse_invert_y") s.invert_y = flag(key, value);
        else if (key == "mouse_sensitivity") {
            char* end = nullptr;
            const float v = std::strtof(value.c_str(), &end);
            if (value.empty() || end != value.c_str() + value.size() || !(v >= 0.05F && v <= 20.0F))
                throw std::runtime_error("port settings: mouse_sensitivity must be a number from 0.05 to 20");
            s.sensitivity = v;
        }
    }
    return s;
}

StickBytes mouse_look_stick(StickBytes base, int dx, int dy, const MouseLookSettings& settings) {
    if (!settings.enabled) return base;
    return {axis(base.x, dx, settings.sensitivity), axis(base.y, settings.invert_y ? -dy : dy, settings.sensitivity)};
}

StickBytes stick_bytes_from_deflection(float x, float y) {
    const auto byte = [](float v) {
        return static_cast<std::uint8_t>(std::round(std::clamp(127.5F + std::clamp(v, -1.0F, 1.0F) * 127.5F, 0.0F, 255.0F)));
    };
    return {byte(x), byte(-y)};
}

std::vector<MouseMotionEvent> parse_mouse_motion(std::string_view list) {
    std::vector<MouseMotionEvent> out;
    std::size_t at = 0;
    while (at < list.size()) {
        const auto end = std::min(list.find(',', at), list.size());
        std::string item(list.substr(at, end - at));
        at = end + 1;
        item.erase(0, item.find_first_not_of(' '));
        item.erase(item.find_last_not_of(' ') + 1);
        if (item.empty()) continue;
        const auto colon = item.find(':');
        const auto slash = item.find('/', colon == std::string::npos ? 0 : colon);
        if (colon == std::string::npos || slash == std::string::npos)
            throw std::runtime_error("--test-mouse: bad motion '" + item + "' (F:dx/dy or F-G:dx/dy)");
        const std::string frames = item.substr(0, colon);
        MouseMotionEvent e;
        const auto dash = frames.find('-');
        e.first = parse_frame(frames.substr(0, dash), item);
        e.last = dash == std::string::npos ? e.first : parse_frame(frames.substr(dash + 1), item);
        e.dx = parse_int(item.substr(colon + 1, slash - colon - 1), item);
        e.dy = parse_int(item.substr(slash + 1), item);
        if (e.last < e.first) throw std::runtime_error("--test-mouse: empty frame range in '" + item + "'");
        if (!out.empty() && e.first <= out.back().last)
            throw std::runtime_error("--test-mouse: frames out of order at '" + item + "'");
        out.push_back(e);
    }
    return out;
}

}  // namespace omega
