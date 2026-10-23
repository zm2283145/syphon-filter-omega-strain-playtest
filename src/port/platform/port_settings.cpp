#include "port_settings.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace omega {
namespace {
std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return std::string(s.substr(b, e - b + 1));
}
bool flag_value(const std::string& key, const std::string& v) {
    if (v == "1") return true;
    if (v == "0") return false;
    throw std::runtime_error("port settings: " + key + " takes 0 or 1, not '" + v + "'");
}
std::array<int, 4> rect_value(const std::string& key, const std::string& v) {
    std::array<int, 4> r{};
    std::size_t at = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto end = i < 3 ? v.find(',', at) : v.size();
        std::size_t used = 0;
        try {
            if (end == std::string::npos || end == at) throw std::invalid_argument("empty");
            const std::string part = trim(std::string_view(v).substr(at, end - at));
            r[i] = std::stoi(part, &used);
            if (used != part.size()) throw std::invalid_argument("trailing");
        } catch (const std::logic_error&) {
            throw std::runtime_error("port settings: " + key + " takes left,top,right,bottom, not '" + v + "'");
        }
        at = end + 1;
    }
    if (r[2] <= r[0] || r[3] <= r[1])
        throw std::runtime_error("port settings: " + key + " needs right > left and bottom > top, not '" + v + "'");
    return r;
}
}

PortSettings parse_port_settings(std::string_view text) {
    PortSettings s;
    // Checkpoint 132win8: a hand-edited file saved with a UTF-8 byte-order mark (README-PLAY.md) reads the same.
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    std::size_t line_no = 0;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const std::string line = trim(text.substr(0, nl));
        text = nl == std::string_view::npos ? std::string_view{} : text.substr(nl + 1);
        ++line_no;
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            throw std::runtime_error("port settings: line " + std::to_string(line_no) + " is not key=value");
        const std::string key = trim(std::string_view(line).substr(0, eq));
        const std::string value = trim(std::string_view(line).substr(eq + 1));
        if (key == "coop_partner") s.coop_partner = flag_value(key, value);
        else if (key == "fullscreen") s.fullscreen = flag_value(key, value);
        else if (key == "vsync") s.vsync = flag_value(key, value);
        else if (key == "window") s.window = rect_value(key, value);
        else s.other.emplace_back(key, value);
    }
    return s;
}

std::string format_port_settings(const PortSettings& s) {
    std::ostringstream out;
    out << "# Port-only settings (omega_play; research/PLAY_SANDBOX.md \"Port settings\")\n";
    out << "coop_partner=" << (s.coop_partner ? 1 : 0) << "\n";
    out << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n";
    out << "vsync=" << (s.vsync ? 1 : 0) << "\n";
    if (s.window)
        out << "window=" << (*s.window)[0] << "," << (*s.window)[1] << "," << (*s.window)[2] << "," << (*s.window)[3] << "\n";
    for (const auto& [k, v] : s.other) out << k << "=" << v << "\n";
    return out.str();
}

std::string update_port_settings_text(std::string_view existing, const PortSettings& s) {
    if (existing.substr(0, 3) == "\xEF\xBB\xBF") existing.remove_prefix(3);
    if (trim(existing).empty()) return format_port_settings(s);
    std::ostringstream out;
    bool coop = false, full = false, sync = false, rect = false;
    std::vector<bool> used(s.other.size(), false);
    // Checkpoint 132win11: a key the file only has as a commented example ("# mouse_look=1") is written on the line
    // after that example instead of at the end (the keys already set in the file stay where they are).
    std::vector<std::string> active;
    for (std::string_view rest = existing; !rest.empty();) {
        const auto nl = rest.find('\n');
        const std::string line = trim(rest.substr(0, nl));
        rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
        const auto eq = line.find('=');
        if (!line.empty() && line[0] != '#' && eq != std::string::npos && eq != 0)
            active.push_back(trim(std::string_view(line).substr(0, eq)));
    }
    const auto is_active = [&](const std::string& k) { return std::find(active.begin(), active.end(), k) != active.end(); };
    const auto after_example = [&](const std::string& comment) {
        if (comment.empty() || comment[0] != '#') return;
        const std::string body = trim(std::string_view(comment).substr(1));
        const auto eq = body.find('=');
        if (eq == std::string::npos || eq == 0) return;
        const std::string key = trim(std::string_view(body).substr(0, eq));
        if (key.find_first_of(" \t") != std::string::npos || is_active(key)) return;
        if (key == "window" && !rect && s.window) {
            out << "window=" << (*s.window)[0] << "," << (*s.window)[1] << "," << (*s.window)[2] << "," << (*s.window)[3] << "\n";
            rect = true;
        }
        for (std::size_t i = 0; i < s.other.size(); ++i)
            if (!used[i] && s.other[i].first == key) {
                out << key << "=" << s.other[i].second << "\n";
                used[i] = true;
                break;
            }
    };
    while (!existing.empty()) {
        const auto nl = existing.find('\n');
        const std::string raw(existing.substr(0, nl));
        existing = nl == std::string_view::npos ? std::string_view{} : existing.substr(nl + 1);
        const std::string line = trim(raw);
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos || eq == 0) {
            out << (raw.empty() || raw.back() != '\r' ? raw : raw.substr(0, raw.size() - 1)) << "\n";
            after_example(line);
            continue;
        }
        const std::string key = trim(std::string_view(line).substr(0, eq));
        if (key == "coop_partner") {
            if (!coop) out << "coop_partner=" << (s.coop_partner ? 1 : 0) << "\n";
            coop = true;
        } else if (key == "fullscreen") {
            if (!full) out << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n";
            full = true;
        } else if (key == "vsync") {
            if (!sync) out << "vsync=" << (s.vsync ? 1 : 0) << "\n";
            sync = true;
        } else if (key == "window") {
            if (!rect && s.window)
                out << "window=" << (*s.window)[0] << "," << (*s.window)[1] << "," << (*s.window)[2] << "," << (*s.window)[3] << "\n";
            rect = true;
        } else {
            for (std::size_t i = 0; i < s.other.size(); ++i)
                if (!used[i] && s.other[i].first == key) {
                    out << key << "=" << s.other[i].second << "\n";
                    used[i] = true;
                    break;
                }
        }
    }
    if (!coop) out << "coop_partner=" << (s.coop_partner ? 1 : 0) << "\n";
    if (!full) out << "fullscreen=" << (s.fullscreen ? 1 : 0) << "\n";
    if (!sync) out << "vsync=" << (s.vsync ? 1 : 0) << "\n";
    if (!rect && s.window)
        out << "window=" << (*s.window)[0] << "," << (*s.window)[1] << "," << (*s.window)[2] << "," << (*s.window)[3] << "\n";
    for (std::size_t i = 0; i < s.other.size(); ++i)
        if (!used[i]) out << s.other[i].first << "=" << s.other[i].second << "\n";
    return out.str();
}

namespace {
// The game window's thread writes the fullscreen key (Alt+Enter / F11) while the main thread may read.
std::mutex& file_mutex() {
    static std::mutex m;
    return m;
}
}  // namespace

bool background_audio_setting(const PortSettings& s) {
    bool on = false;
    for (const auto& [key, value] : s.other)
        if (key == "background_audio") on = flag_value(key, value);
    return on;
}

bool lan_play_setting(const PortSettings& s) {
    bool on = true;
    for (const auto& [key, value] : s.other)
        if (key == "lan_play") on = flag_value(key, value);
    return on;
}

bool mods_setting(const PortSettings& s) {
    bool on = true;
    for (const auto& [key, value] : s.other)
        if (key == "mods") on = flag_value(key, value);
    return on;
}

std::string mods_folder_setting(const PortSettings& s) {
    std::string folder;
    for (const auto& [key, value] : s.other)
        if (key == "mods_folder") folder = value;
    return folder;
}

std::optional<std::uint32_t> mods_texture_cap_setting(const PortSettings& s) {
    std::optional<std::uint32_t> cap;
    for (const auto& [key, value] : s.other) {
        if (key != "mods_texture_cap") continue;
        unsigned long n = 0;
        std::size_t used = 0;
        try {
            n = std::stoul(value, &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (value.empty() || used != value.size() || n > 65536)
            throw std::runtime_error("port settings: mods_texture_cap '" + value + "' is not 0..65536 (MB)");
        cap = static_cast<std::uint32_t>(n);
    }
    return cap;
}

std::vector<std::pair<std::string, bool>> mod_choices_setting(const PortSettings& s) {
    std::vector<std::pair<std::string, bool>> choices;
    for (const auto& [key, value] : s.other) {
        if (key.size() <= 4 || key.rfind("mod.", 0) != 0) continue;
        std::string folder = key.substr(4);
        for (auto& c : folder) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const bool on = flag_value(key, value);
        const auto it = std::find_if(choices.begin(), choices.end(), [&](const auto& c) { return c.first == folder; });
        if (it != choices.end()) it->second = on;
        else choices.emplace_back(folder, on);
    }
    return choices;
}

std::vector<std::pair<std::string, unsigned>> mod_volumes_setting(const PortSettings& s) {
    static constexpr std::string_view prefix = "mod_volume.";
    std::vector<std::pair<std::string, unsigned>> volumes;
    for (const auto& [key, value] : s.other) {
        if (key.size() <= prefix.size() || key.rfind(prefix, 0) != 0) continue;
        std::string folder = key.substr(prefix.size());
        for (auto& c : folder) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const auto text = trim(value);
        unsigned percent = 0;
        const bool digits = !text.empty() && text.size() <= 3 &&
                            std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
        if (digits) percent = static_cast<unsigned>(std::stoul(text));
        if (!digits || percent > 400)
            throw std::runtime_error("port settings: " + key + " takes a percent 0..400, not '" + value + "'");
        const auto it = std::find_if(volumes.begin(), volumes.end(), [&](const auto& v) { return v.first == folder; });
        if (it != volumes.end()) it->second = percent;
        else volumes.emplace_back(folder, percent);
    }
    return volumes;
}

bool partner_friendly_fire_setting(const PortSettings& s) {
    bool on = false;
    for (const auto& [key, value] : s.other)
        if (key == "partner_friendly_fire") on = flag_value(key, value);
    return on;
}

OnlineServerAddress online_server_setting(const PortSettings& s) {
    OnlineServerAddress a;
    for (const auto& [key, value] : s.other) {
        if (key != "online_server") continue;
        const auto colon = value.rfind(':');
        OnlineServerAddress next;
        next.host = trim(std::string_view(value).substr(0, colon));
        if (colon != std::string::npos) {
            const std::string port = trim(std::string_view(value).substr(colon + 1));
            unsigned long n = 0;
            std::size_t used = 0;
            try {
                n = std::stoul(port, &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (port.empty() || used != port.size() || n < 1 || n > 65535)
                throw std::runtime_error("port settings: online_server port '" + port + "' is not 1..65535");
            next.port = static_cast<unsigned short>(n);
        }
        if (next.host.empty()) throw std::runtime_error("port settings: online_server needs a host, not '" + value + "'");
        a = next;
    }
    return a;
}

bool online_platform_badges_setting(const PortSettings& s) {
    bool on = true;
    for (const auto& [key, value] : s.other)
        if (key == "online_platform_badges") on = flag_value(key, value);
    return on;
}

std::optional<bool> online_chat_filter_setting(const PortSettings& s) {
    std::optional<bool> on;
    for (const auto& [key, value] : s.other)
        if (key == "online_chat_filter") on = flag_value(key, value);
    return on;
}

std::string online_game_password_setting(const PortSettings& s) {
    std::string pw;
    for (const auto& [key, value] : s.other)
        if (key == "online_game_password") {
            if (value.size() > 31)
                throw std::runtime_error("port settings: online_game_password is longer than 31 characters");
            pw = value;
        }
    return pw;
}

std::string online_relay_setting(const PortSettings& s) {
    std::string mode = "auto";
    for (const auto& [key, value] : s.other)
        if (key == "online_relay") {
            if (value != "auto" && value != "off" && value != "force")
                throw std::runtime_error("port settings: online_relay must be auto, off or force, not " + value);
            mode = value;
        }
    return mode;
}

unsigned short online_game_port_setting(const PortSettings& s) {
    unsigned short port = 3658;
    for (const auto& [key, value] : s.other) {
        if (key != "online_game_port") continue;
        unsigned long n = 0;
        std::size_t used = 0;
        try {
            n = std::stoul(value, &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (value.empty() || used != value.size() || n > 65535)
            throw std::runtime_error("port settings: online_game_port '" + value + "' is not 0..65535");
        port = static_cast<unsigned short>(n);
    }
    return port;
}

bool online_upnp_setting(const PortSettings& s) {
    bool on = true;
    for (const auto& [key, value] : s.other)
        if (key == "online_upnp") on = flag_value(key, value);
    return on;
}

std::string lan_mod_policy_setting(const PortSettings& s) {
    std::string policy = "gameplay";
    for (const auto& [key, value] : s.other)
        if (key == "lan_mod_policy") {
            if (value != "gameplay" && value != "any" && value != "cosmetic" && value != "none" && value != "equal")
                throw std::runtime_error("Port settings: lan_mod_policy must be gameplay, any, cosmetic, none or equal, not " + value);
            policy = value;
        }
    return policy;
}

std::string partner_loadout_setting(const PortSettings& s) {
    std::string mode = "auto";
    for (const auto& [key, value] : s.other)
        if (key == "partner_loadout") {
            if (value != "auto" && value != "copy" && value != "record")
                throw std::runtime_error("Port settings: partner_loadout must be auto, copy or record, not " + value);
            mode = value;
        }
    return mode;
}

bool partner_pickups_setting(const PortSettings& s) {
    bool on = true;
    for (const auto& [key, value] : s.other)
        if (key == "partner_pickups") on = flag_value(key, value);
    return on;
}

std::size_t coop_partners_setting(const PortSettings& s) {
    std::size_t count = 1;
    for (const auto& [key, value] : s.other)
        if (key == "coop_partners") {
            if (value != "1" && value != "2" && value != "3")
                throw std::runtime_error("Port settings: coop_partners must be 1, 2 or 3, not " + value);
            count = static_cast<std::size_t>(value[0] - '0');
        }
    return count;
}

bool window_units_pixels(const PortSettings& s) {
    bool pixels = false;
    for (const auto& [key, value] : s.other)
        if (key == "window_units") {
            if (value != "pixels") throw std::runtime_error("Port settings: window_units must be pixels, not " + value);
            pixels = true;
        }
    return pixels;
}

void set_window_units_pixels(PortSettings& s) {
    for (auto& [key, value] : s.other)
        if (key == "window_units") {
            value = "pixels";
            return;
        }
    s.other.emplace_back("window_units", "pixels");
}

PortSettings read_port_settings(const std::filesystem::path& file) {
    std::lock_guard lock(file_mutex());
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::ostringstream text;
    text << in.rdbuf();
    return parse_port_settings(text.str());
}

void write_port_settings(const std::filesystem::path& file, const PortSettings& s) {
    std::lock_guard lock(file_mutex());
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    std::string existing;
    if (std::ifstream in(file, std::ios::binary); in) {
        std::ostringstream text;
        text << in.rdbuf();
        existing = text.str();
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("port settings: cannot write " + file.string());
    out << update_port_settings_text(existing, s);
    if (!out) throw std::runtime_error("port settings: write failed " + file.string());
}

}  // namespace omega
