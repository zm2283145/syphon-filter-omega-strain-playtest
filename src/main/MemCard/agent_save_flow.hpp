#pragma once
// The agent save manager GuiSaveAgentWidget (research/FRONTEND.md §6): the
// memory-card popup flow that writes a profile (create, manual save,
// autosave, rename) or the options file SFO.CFG. Start 00441a10, rename
// 00441c20, popup handler 00441ca0 and their helpers 00441960 (format check),
// 004415a0 (prepare), 00441780 (space check), 00441370 (save), 004422d0 (find
// the file of a slot).
//
// The card, the popup service, the agent and the save back ends are host
// callbacks, called in the EE's order. The poll callback *0055a838 (00441c10
// while a flow runs, else 0) is the polling flag.
#include "agent_card.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omega {

enum class SaveAgentMode : std::uint8_t { create = 0, manual = 1, autosave = 2, rename = 3, options = 4 };

// 002cae30 kinds: 0 no buttons (progress), 2 OK, 4 Yes/No, 5 Yes/No focused on No.
inline constexpr std::int32_t save_popup_progress = 0;
inline constexpr std::int32_t save_popup_ok = 2;
inline constexpr std::int32_t save_popup_yes_no = 4;
inline constexpr std::int32_t save_popup_yes_no_focus_no = 5;
inline constexpr std::int32_t save_popup_none = -2;
inline constexpr std::uint32_t save_message_yes = 0x1a;  // Yes / OK (a2 & 0xffff)
inline constexpr std::uint32_t save_message_no = 0x1b;   // No / Back
inline constexpr std::uint32_t agent_save_bytes = 16000;  // 00334100
inline constexpr std::uint32_t options_save_bytes = 1000;
inline constexpr std::size_t save_flow_old_name_size = 0x23;  // +55 .. +77

struct SaveCardStatus {  // mc +59d present, +59e formatted, +59f directory (bytes), +5a4 free KB, +5a8 id
    std::uint8_t present = 0;
    std::uint8_t formatted = 0;
    std::uint8_t directory = 0;
    std::int32_t free_kb = 0;
    std::uint32_t id = 0;
};

struct SaveAgentHost {
    // The card wrapper W = *0055a830. status() is read where the EE reads mc;
    // only refresh() is expected to change it.
    std::function<void()> refresh;                                   // 0040d3c0(&W)
    std::function<SaveCardStatus()> status;
    std::function<std::uint32_t()> make_directory;                   // 0040d740(W), tested as a word
    std::function<std::uint32_t()> format;                           // 0040db10(W), tested as a word
    std::function<std::vector<std::string>()> list;                  // 0040d600(W, vec), directory order
    std::function<void(const std::string&)> remove;                  // 0040dc80(W, name), result ignored
    std::function<std::int32_t(std::uint32_t bytes)> kilobytes;      // 0040d580(W, bytes)
    std::function<std::uint32_t(const std::string&)> exists;         // 0040d690(W, name), tested as a word
    // The popup service *004ffc04 and the owner.
    std::function<std::string(const std::string& key)> text;         // 003fdb50 GetString
    // 002cae30(mgr, kind, title, text, this, tag) -> the popup id.
    std::function<std::int32_t(std::int32_t kind, const std::string& title, const std::string& text, std::int32_t tag)> open;
    std::function<void(std::int32_t id)> close;                      // 002cac80(mgr, id)
    std::function<void(std::int32_t result)> done;                   // vtable +5c (00293890 in the base: nothing)
    std::function<std::int32_t(std::uint32_t a1, std::uint32_t a2, std::int32_t tag, std::uint32_t t0)> default_event;  // 0041ef10
    // The agent 001314f0(0x4ffb50, -1) and the save back ends.
    std::function<std::string()> agent_name;                         // agent +8ac
    std::function<std::int32_t()> agent_slot;                        // agent +4
    std::function<void(const std::string&)> set_agent_name;          // 003340b0
    std::function<std::uint32_t()> save_agent;                       // 00334110
    std::function<std::uint32_t(std::int32_t slot, std::uint32_t nosave)> create_agent;  // 00333f60
    std::function<std::uint32_t()> save_options;                     // 0045a2a0(*004ffd2c)
    std::function<std::optional<AgentFileName>(const std::string& file)> decode;  // 003fd380(file, out, &slot, 0x10)
};

struct SaveAgentGlobals {
    std::uint8_t card_seen = 0;           // byte 004ffbb8
    std::uint32_t last_card_id = 0;       // 004ffbbc (0xffffffff: none)
    bool polling = false;                 // *0055a838 == 00441c10
};

class SaveAgentFlow {
public:
    SaveAgentFlow(const SaveAgentHost& host, SaveAgentGlobals& globals) : host_(host), g_(globals) {}

    // The 0x78-byte widget's fields. The owner sets mode (and slot for create)
    // before start(). Modes above 4 throw.
    std::int32_t popup = save_popup_none;  // +48
    std::uint8_t mode = 1;                 // +4c
    std::int32_t slot = 0;                 // +50
    std::uint8_t no_remove_shown = 0;      // +54
    std::string old_name;                  // +55 (0x23 bytes with its NUL)

    void start();                                   // 00441a10
    void rename(const std::string& new_name);       // 00441c20
    std::int32_t on_event(std::uint32_t a1, std::uint32_t a2, std::int32_t tag, std::uint32_t t0);  // 00441ca0

private:
    void check_format();
    void prepare();
    void overwrite_or_save();
    bool space_check();
    void save();
    bool find_slot_file(std::int32_t s, const std::string* exclude, std::string* out);
    void show_no_remove();
    void open(std::int32_t kind, const char* title, const char* key, std::int32_t tag);
    void close_and_cancel();
    SaveAgentMode checked_mode() const;

    const SaveAgentHost& host_;
    SaveAgentGlobals& g_;
};

}  // namespace omega
