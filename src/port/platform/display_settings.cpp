#include "display_settings.hpp"

#include "input_bindings.hpp"
#include "port_settings.hpp"
#include "worker_pool.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace omega {

std::optional<FrameAspect> parse_frame_aspect(std::string_view text) {
    if (text == "pillarbox") return FrameAspect::pillarbox;
    if (text == "4:3" || text == "4x3" || text == "strict") return FrameAspect::strict;
    if (text == "stretch") return FrameAspect::stretch;
    if (text == "wide" || text == "16:9" || text == "16x9") return FrameAspect::wide;  // 132ws
    return std::nullopt;
}

std::optional<unsigned> parse_hud_margin(std::string_view text) {
    if (text.empty() || text.size() > 2) return std::nullopt;
    unsigned v = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        v = v * 10 + static_cast<unsigned>(ch - '0');
    }
    if (v > frame_hud_margin_max) return std::nullopt;
    return v;
}

std::optional<unsigned> parse_fov(std::string_view text) {
    if (text.empty() || text.size() > 3) return std::nullopt;
    unsigned v = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return std::nullopt;
        v = v * 10 + static_cast<unsigned>(ch - '0');
    }
    if (v < frame_fov_min || v > frame_fov_max) return std::nullopt;
    return v;
}

std::optional<bool> parse_movie_fill(std::string_view text) {
    if (text == "bars") return false;
    if (text == "zoom") return true;
    return std::nullopt;
}

std::optional<MovieFit> parse_movie_fit(std::string_view text) {
    if (text == "fill") return MovieFit::fill;
    if (text == "bars") return MovieFit::bars;
    if (text == "stretch") return MovieFit::stretch;
    return std::nullopt;
}

std::optional<float> parse_render_scale(std::string_view text) {
    const std::string value(text);
    char* end = nullptr;
    const float v = std::strtof(value.c_str(), &end);
    if (value.empty() || end != value.c_str() + value.size() || !(v >= render_scale_min && v <= render_scale_max))
        return std::nullopt;
    return v;
}

float vita_render_scale(float render_scale) {
    if (render_scale >= 0.875F) return 1.0F;
    if (render_scale >= 0.625F) return 0.75F;
    return 0.5F;
}

DisplaySettings display_settings(const PortSettings& settings) {
    DisplaySettings s;
    for (const auto& [key, value] : settings.other) {
        if (key == "aspect") {
            const auto a = parse_frame_aspect(value);
            if (!a) throw std::runtime_error("port settings: aspect must be pillarbox, 4:3, stretch or wide");
            s.aspect = *a;
        } else if (key == "hud_margin") {  // 132ws
            const auto v = parse_hud_margin(value);
            if (!v) throw std::runtime_error("port settings: hud_margin must be a whole percent from 0 to 20");
            s.hud_margin = *v;
        } else if (key == "fov") {  // 132ws3
            const auto v = parse_fov(value);
            if (!v) throw std::runtime_error("port settings: fov must be a whole percent from 70 to 150");
            s.fov = *v;
        } else if (key == "movie_fill") {  // 132ws
            const auto v = parse_movie_fill(value);
            if (!v) throw std::runtime_error("port settings: movie_fill must be bars or zoom");
            s.movie_zoom = *v;
        } else if (key == "movies") {  // 132vt2a2
            const auto v = parse_movie_fit(value);
            if (!v) throw std::runtime_error("port settings: movies must be fill, bars or stretch");
            s.movie_fit = *v;
        } else if (key == "render_scale") {
            const auto v = parse_render_scale(value);
            if (!v) throw std::runtime_error("port settings: render_scale must be a number from 0.25 to 4");
            s.render_scale = *v;
        } else if (key == "frame_overlay") {
            if (value != "0" && value != "1") throw std::runtime_error("port settings: frame_overlay must be 0 or 1");
            s.frame_overlay = value == "1";
        } else if (key == "render_thread") {
            if (value != "0" && value != "1") throw std::runtime_error("port settings: render_thread must be 0 or 1");
            s.render_thread = value == "1";
        } else if (key == "worker_threads") {
            try {
                s.worker_threads = parse_worker_threads(value);
            } catch (const std::exception&) {
                throw std::runtime_error("port settings: worker_threads must be auto or 0..64");
            }
        } else if (key == "frame_overlay_key") {
            if (value == "none") {
                s.frame_overlay_key.reset();
                continue;
            }
            const auto code = key_code(value);
            if (!code || *code < 0x08 || *code == 0x7a)
                throw std::runtime_error("port settings: frame_overlay_key must be a keyboard key name (not F11) or none");
            s.frame_overlay_key = static_cast<std::uint8_t>(*code);
        }
    }
    return s;
}

}  // namespace omega
