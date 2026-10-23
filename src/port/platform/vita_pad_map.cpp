#include "vita_pad_map.hpp"

#include <cstdlib>
#include <utility>

namespace omega {
namespace {

void press(Ds2Pad& pad, std::uint16_t button) {
    pad.held |= button;
    for (std::size_t k = 0; k < ds2::pressure_mask.size(); ++k)
        if (ds2::pressure_mask[k] == button) pad.pressure[k] = 0xff;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

std::uint16_t vita_touch_button(const VitaTouchPanel& panel, const VitaTouchPoint& point, float edge_dead_zone) {
    const int w = panel.max_x - panel.min_x + 1, h = panel.max_y - panel.min_y + 1;
    if (w <= 0 || h <= 0) return 0;
    const int x = point.x - panel.min_x, y = point.y - panel.min_y;
    const float mx = edge_dead_zone * float(w), my = edge_dead_zone * float(h);
    if (float(x) < mx || float(y) < my || float(x) >= float(w) - mx || float(y) >= float(h) - my) return 0;
    const bool right_half = 2 * x >= w, bottom_half = 2 * y >= h;
    if (!bottom_half) return right_half ? ds2::r2 : ds2::l2;
    return right_half ? ds2::r3 : ds2::l3;
}

Ds2Pad ds2_from_vita(const VitaPadInput& in, const VitaTouchConfig& config) {
    Ds2Pad pad;
    const auto b = in.buttons;
    const std::pair<std::uint32_t, std::uint16_t> map[] = {
        {vita_ctrl::up, ds2::up},          {vita_ctrl::down, ds2::down},     {vita_ctrl::left, ds2::left},
        {vita_ctrl::right, ds2::right},    {vita_ctrl::start, ds2::start},   {vita_ctrl::select, ds2::select},
        {vita_ctrl::l3, ds2::l3},          {vita_ctrl::r3, ds2::r3},         {vita_ctrl::ltrigger, ds2::l1},
        {vita_ctrl::rtrigger, ds2::r1},    {vita_ctrl::cross, ds2::cross},   {vita_ctrl::circle, ds2::circle},
        {vita_ctrl::square, ds2::square},  {vita_ctrl::triangle, ds2::triangle}};
    for (const auto& [from, to] : map)
        if (b & from) press(pad, to);
    auto touch = [&](const VitaTouchPanel& panel) {
        for (int k = 0; k < panel.count && k < int(panel.points.size()); ++k)
            if (const auto button = vita_touch_button(panel, panel.points[std::size_t(k)], config.edge_dead_zone)) press(pad, button);
    };
    if (config.front) touch(in.front);
    if (config.rear) touch(in.rear);
    pad.left_x = in.lx;
    pad.left_y = in.ly;
    pad.right_x = in.rx;
    pad.right_y = in.ry;
    return pad;
}

VitaTouchConfig parse_vita_touch_config(std::string_view text, std::string& problems) {
    VitaTouchConfig config;
    int number = 0;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++number;
        if (line.empty() || line.front() == '#') continue;
        const auto eq = line.find('=');
        const std::string_view key = trim(line.substr(0, eq)), value = eq == std::string_view::npos ? std::string_view{} : trim(line.substr(eq + 1));
        auto bad = [&] { problems += "vita_input.ini line " + std::to_string(number) + ": ignored '" + std::string(line) + "'\n"; };
        if (eq == std::string_view::npos) {
            bad();
        } else if (key == "touch_front" || key == "touch_rear") {
            if (value != "0" && value != "1") {
                bad();
                continue;
            }
            (key == "touch_front" ? config.front : config.rear) = value == "1";
        } else if (key == "touch_edge_dead_zone") {
            const std::string s(value);
            char* stop = nullptr;
            const float v = std::strtof(s.c_str(), &stop);
            if (s.empty() || stop != s.c_str() + s.size() || !(v >= 0.0F && v <= 0.45F)) {
                bad();
                continue;
            }
            config.edge_dead_zone = v;
        } else {
            bad();
        }
    }
    return config;
}

}  // namespace omega
