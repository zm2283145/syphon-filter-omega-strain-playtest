#include "pad_connection.hpp"

#include "port_settings.hpp"

#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {

std::string trim(std::string_view s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(" \t");
    return std::string(s.substr(b, e - b + 1));
}

// The XInput slot of an alias, else -1 for a Sony HID pad; throws on an unknown alias.
int xinput_slot(const std::string& alias) {
    if (alias.size() == 7 && alias.rfind("xinput", 0) == 0 && alias[6] >= '0' && alias[6] <= '3') return alias[6] - '0';
    (void)simulated_pad_source(alias);
    return -1;
}

}  // namespace

bool controller_prompt_setting(const PortSettings& settings) {
    bool on = true;
    for (const auto& [key, value] : settings.other) {
        if (key != "controller_prompt") continue;
        if (value != "0" && value != "1") throw std::runtime_error("port settings: controller_prompt must be 0 or 1");
        on = value == "1";
    }
    return on;
}

std::optional<std::string> PadConnection::update(const std::string& source, bool key_pressed) {
    if (!started_) {
        started_ = true;
        source_ = source;
        return source.empty() ? std::string("Controller: none at the start; the keyboard is the controller (pad state 6)")
                              : "Controller: " + source + " (DualShock 2 mapping)";
    }
    if (source == source_) {
        if (removed_ && key_pressed) {
            removed_ = false;
            return std::string("Controller: a bound key was pressed; the keyboard is the controller (pad state 6, the "
                               "controller check 00465870 closes its prompt)");
        }
        return std::nullopt;
    }
    const std::string was = source_;
    source_ = source;
    if (source.empty()) {
        ++disconnects_;
        if (!prompt_) return "Controller: " + was + " disconnected; the keyboard is the controller (controller_prompt=0)";
        removed_ = true;
        return "Controller: " + was + " disconnected; no controller (pad state 0: the pad keeps its last values, "
               "00369c50, and the controller check 00465870 prompts after 3 frames)";
    }
    ++connects_;
    const bool was_removed = removed_;
    removed_ = false;
    if (was.empty())
        return "Controller: " + source + " connected (DualShock 2 mapping)" +
               (was_removed ? std::string("; pad state 6") : std::string());
    return "Controller: " + source + " now active (was " + was + ")";
}

std::string simulated_pad_source(const std::string& alias) {
    std::string name;
    if (alias.size() == 7 && alias.rfind("xinput", 0) == 0 && alias[6] >= '0' && alias[6] <= '3')
        name = "XInput pad " + std::string(1, alias[6]);
    else if (alias == "ds3") name = "DualShock 3 (USB)";
    else if (alias == "ds4" || alias == "vigem") name = "DualShock 4 (USB)";
    else if (alias == "ds4bt") name = "DualShock 4 (Bluetooth)";
    else if (alias == "dualsense") name = "DualSense (USB)";
    else if (alias == "dualsensebt") name = "DualSense (Bluetooth)";
    else throw std::runtime_error("simulated pads: unknown pad '" + alias +
                                  "' (xinput0..xinput3, ds3, ds4, ds4bt, dualsense, dualsensebt, vigem)");
    return name + " [simulated " + alias + "]";
}

std::vector<PadPresenceEvent> parse_pad_events(std::string_view text) {
    std::vector<PadPresenceEvent> events;
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto end = std::min(text.find(',', at), text.size());
        const std::string item = trim(text.substr(at, end - at));
        at = end + 1;
        if (item.empty()) continue;
        const auto colon = item.find(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= item.size())
            throw std::runtime_error("simulated pads: '" + item + "' is not <frame>:+<pad>, <frame>:-<pad> or <frame>:key");
        PadPresenceEvent e;
        std::size_t used = 0;
        try {
            e.frame = std::stoull(item.substr(0, colon), &used);
        } catch (const std::logic_error&) {
            used = 0;
        }
        if (used != colon) throw std::runtime_error("simulated pads: '" + item + "' has no frame number");
        const std::string what = item.substr(colon + 1);
        if (what == "key") e.kind = PadPresenceEvent::Kind::key;
        else if (what[0] == '+' || what[0] == '-') {
            e.kind = what[0] == '+' ? PadPresenceEvent::Kind::connect : PadPresenceEvent::Kind::disconnect;
            e.pad = what.substr(1);
            (void)xinput_slot(e.pad);
        } else throw std::runtime_error("simulated pads: '" + item + "' is not <frame>:+<pad>, <frame>:-<pad> or <frame>:key");
        if (!events.empty() && e.frame < events.back().frame)
            throw std::runtime_error("simulated pads: the events must be in frame order ('" + item + "')");
        events.push_back(e);
    }
    return events;
}

SimulatedPads::Frame SimulatedPads::at(std::uint64_t frame) {
    Frame f;
    for (; next_ < events_.size() && events_[next_].frame <= frame; ++next_) {
        const auto& e = events_[next_];
        if (e.kind == PadPresenceEvent::Kind::key) {
            f.key = f.key || e.frame == frame;
            f.log.push_back("key press");
            continue;
        }
        const int slot = xinput_slot(e.pad);
        const bool connect = e.kind == PadPresenceEvent::Kind::connect;
        if (slot >= 0) {
            if (connect) xinput_.insert(slot);
            else xinput_.erase(slot);
        } else {
            std::erase(sony_, e.pad);
            if (connect) sony_.push_back(e.pad);
        }
        f.log.push_back(std::string(connect ? "connected " : "disconnected ") + e.pad);
    }
    if (!sony_.empty()) {
        f.source = simulated_pad_source(sony_.back());
        return f;
    }
    if (xinput_slot_ < 0 || !xinput_.count(xinput_slot_)) xinput_slot_ = xinput_.empty() ? -1 : *xinput_.begin();
    if (xinput_slot_ >= 0) f.source = simulated_pad_source("xinput" + std::to_string(xinput_slot_));
    return f;
}

}  // namespace omega
