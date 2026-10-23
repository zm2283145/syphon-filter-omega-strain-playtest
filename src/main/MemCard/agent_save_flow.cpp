#include "agent_save_flow.hpp"

#include <stdexcept>

namespace omega {
namespace {

constexpr std::size_t nospace_kb_buffer = 8;       // sp+158 .. sp+160 of 00441780
constexpr std::size_t nospace_text_buffer = 0x108;  // sp+50 .. sp+158
constexpr std::size_t slot_file_buffer = 0x20;      // sp+60 .. sp+80 of 00441370
constexpr std::size_t decoded_name_buffer = 0x10;   // sp+80 .. sp+90 of 004422d0

}  // namespace

SaveAgentMode SaveAgentFlow::checked_mode() const {
    if (mode > 4) throw std::runtime_error("save flow: mode above 4 (no caller sets one)");
    return static_cast<SaveAgentMode>(mode);
}

void SaveAgentFlow::open(std::int32_t kind, const char* title, const char* key, std::int32_t tag) {
    // the title's lookup first, as the EE calls do (argument evaluation order is unspecified)
    const std::string t = host_.text(title);
    const std::string k = host_.text(key);
    popup = host_.open(kind, t, k, tag);
}

void SaveAgentFlow::show_no_remove() {
    if (no_remove_shown) return;
    no_remove_shown = 1;
    open(save_popup_progress, "MC_ALERT", "MC_NOREMOVE", 0);
}

// 00441a10
void SaveAgentFlow::start() {
    const auto m = checked_mode();
    if (m != SaveAgentMode::create && m != SaveAgentMode::options) slot = host_.agent_slot();
    no_remove_shown = 0;
    g_.polling = true;
    host_.refresh();
    const auto card = host_.status();
    if (!card.present) {
        if (m == SaveAgentMode::autosave && !g_.card_seen) {
            g_.polling = false;
            host_.done(0);
        } else if (m == SaveAgentMode::options) {
            open(save_popup_ok, "MC_ALERT", "MC_OPTNOCARD", 0xa);
        } else {
            open(save_popup_yes_no_focus_no, "MC_ALERT", m == SaveAgentMode::create ? "MC_NOCARDCONT" : "MC_AUTONOCARD", 3);
        }
        return;
    }
    g_.card_seen = 1;
    if (m != SaveAgentMode::create && g_.last_card_id != card.id) {
        open(save_popup_yes_no_focus_no, "MC_ALERT", "MC_WRONGCARD", 5);
        return;
    }
    check_format();
}

// 00441960
void SaveAgentFlow::check_format() {
    const auto card = host_.status();
    if (!card.present) return start();
    if (!card.formatted) return open(save_popup_yes_no_focus_no, "MC_ALERT", "MC_ASKFORMAT", 0);
    prepare();
}

// 004415a0
void SaveAgentFlow::prepare() {
    if (!host_.status().directory) {
        if (!space_check()) return;
        show_no_remove();
        if (!host_.make_directory()) {
            const auto previous = popup;
            open(save_popup_ok, "MC_ERROR", "MC_BADMAKEDIR", 2);
            host_.close(previous);
            return;
        }
    }
    overwrite_or_save();
}

// The tail of 004415a0 (004416a8), repeated in the format path (00442040).
void SaveAgentFlow::overwrite_or_save() {
    const auto m = checked_mode();
    if (m == SaveAgentMode::options) return save();
    std::optional<std::string> exclude;
    if (m != SaveAgentMode::create && host_.status().id == g_.last_card_id)
        exclude = m == SaveAgentMode::rename ? old_name : host_.agent_name();
    if (find_slot_file(slot, exclude ? &*exclude : nullptr, nullptr))
        open(save_popup_yes_no_focus_no, "MC_ALERT", "MC_OVERWRITE", 1);
    else
        save();
}

// 00441780
bool SaveAgentFlow::space_check() {
    std::uint32_t need = 0;
    if (checked_mode() == SaveAgentMode::options)
        need = host_.exists("SFO.CFG") ? 0 : options_save_bytes;
    else
        need = find_slot_file(slot, nullptr, nullptr) ? 0 : agent_save_bytes;
    const std::int32_t kb = host_.kilobytes(need);
    const auto card = host_.status();
    if (!card.present || !card.formatted) {
        host_.close(popup);
        start();
        return false;
    }
    if (card.free_kb >= kb) return true;
    const auto number = std::to_string(kb);
    if (number.size() >= nospace_kb_buffer) throw std::runtime_error("save flow: free space text overruns its buffer");
    auto text = host_.text("MC_NOSPACE");
    if (const auto at = text.find("1000"); at != std::string::npos)
        text = text.substr(0, at) + number + text.substr(at + 4);
    if (text.size() >= nospace_text_buffer) throw std::runtime_error("save flow: MC_NOSPACE text overruns its buffer");
    host_.close(popup);
    popup = host_.open(save_popup_ok, host_.text("MC_ERROR"), text, 7);
    return false;
}

// 00441370
void SaveAgentFlow::save() {
    if (!space_check()) return;
    show_no_remove();
    const auto m = checked_mode();
    std::uint32_t ok = 0;
    if (m == SaveAgentMode::options) {
        ok = host_.save_options() & 0xffu;
    } else {
        const auto name = host_.agent_name();
        std::string file;
        if (find_slot_file(slot, &name, &file)) host_.remove(file);
        ok = (m == SaveAgentMode::create ? host_.create_agent(slot, 0) : host_.save_agent()) & 0xffu;
    }
    const auto previous = popup;
    if (ok) {
        g_.polling = false;
        host_.done(1);
        g_.last_card_id = host_.status().id;
    } else if (m == SaveAgentMode::autosave) {
        open(save_popup_yes_no, "MC_ERROR", "MC_BADAUTOSAVE", 2);
    } else {
        open(save_popup_ok, "MC_ERROR", "MC_BADSAVE", 2);
    }
    host_.close(previous);
}

// 004422d0
bool SaveAgentFlow::find_slot_file(std::int32_t s, const std::string* exclude, std::string* out) {
    for (const auto& file : host_.list()) {
        if (file.find('.') != std::string::npos) continue;
        const auto first = file.empty() ? 0u : static_cast<unsigned char>(file[0]);
        // isdigit through the ctype table at 00499c71 with a signed byte.
        if (first >= 0x80) throw std::runtime_error("save flow: card file name starts with a byte >= 0x80");
        if (first < '0' || first > '9') continue;
        const auto decoded = host_.decode(file);
        if (!decoded) continue;
        if (exclude) {
            if (decoded->name.size() >= decoded_name_buffer)
                throw std::runtime_error("save flow: decoded name fills its 16-byte buffer");
            if (decoded->name == *exclude) continue;  // 00129218 strcmp
        }
        if (decoded->slot != s) continue;
        if (out) {
            if (file.size() >= slot_file_buffer) throw std::runtime_error("save flow: card file name overruns its buffer");
            *out = file;
        }
        return true;
    }
    return false;
}

// 00441c20
void SaveAgentFlow::rename(const std::string& new_name) {
    mode = static_cast<std::uint8_t>(SaveAgentMode::rename);
    const auto name = host_.agent_name();
    if (name.size() >= save_flow_old_name_size) throw std::runtime_error("save flow: agent name overruns the old name");
    old_name = name;
    host_.set_agent_name(new_name);
    start();
}

void SaveAgentFlow::close_and_cancel() {
    host_.close(popup);
    if (checked_mode() == SaveAgentMode::autosave) open(save_popup_yes_no_focus_no, "MC_ALERT", "MC_CANCELSAVE", 9);
}

// 00441ca0
std::int32_t SaveAgentFlow::on_event(std::uint32_t a1, std::uint32_t a2, std::int32_t tag, std::uint32_t t0) {
    const auto m = checked_mode();
    const auto message = a2 & 0xffffu;
    if (message == save_message_no) {
        switch (tag) {
        case 3:
        case 9:
            host_.close(popup);
            start();
            return 1;
        case 2:
            host_.close(popup);
            g_.polling = false;
            host_.done(0);
            return 1;
        case 0:
        case 1:
        case 5:
            close_and_cancel();
            return 1;
        default:
            return host_.default_event(a1, a2, tag, t0);
        }
    }
    if (message != save_message_yes) return host_.default_event(a1, a2, tag, t0);
    switch (tag) {
    case 3:
        host_.close(popup);
        if (m == SaveAgentMode::create) host_.create_agent(slot, 1);
        if (m == SaveAgentMode::create || m == SaveAgentMode::rename) g_.last_card_id = 0xffffffffu;
        g_.polling = false;
        g_.card_seen = 0;
        host_.done(0);
        return 1;
    case 0: {
        host_.close(popup);
        const auto card = host_.status();
        if (!card.present) {
            start();
            return 1;
        }
        if (card.formatted) {
            open(save_popup_ok, "MC_ERROR", "MC_CARDCHANGE", 8);
            return 1;
        }
        open(save_popup_progress, "MC_ALERT", "MC_FORMATTING", 0);
        const auto formatted = host_.format();
        host_.close(popup);
        if (!formatted) {
            open(save_popup_ok, "MC_ERROR", "MC_BADFORMAT", 2);
            return 1;
        }
        show_no_remove();
        if (!host_.make_directory()) {
            host_.close(popup);
            open(save_popup_ok, "MC_ERROR", "MC_BADMAKEDIR", 2);
            return 1;
        }
        overwrite_or_save();
        return 1;
    }
    case 1: {
        host_.close(popup);
        host_.refresh();
        const auto card = host_.status();
        if (card.present && card.formatted && card.directory)
            save();
        else
            start();
        return 1;
    }
    case 2:
        host_.close(popup);
        start();
        return 1;
    case 0xa:
        host_.done(0);
        return 1;
    case 7:
        close_and_cancel();
        return 1;
    case 5:
        host_.close(popup);
        host_.refresh();
        check_format();
        return 1;
    case 9:
        host_.close(popup);
        g_.polling = false;
        host_.done(0);
        return 1;
    default:
        return 1;
    }
}

}  // namespace omega
