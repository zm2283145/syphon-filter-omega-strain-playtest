#pragma once
// The agent card layer (research/FRONTEND.md §6): the card file name decoder
// 003fd380, the AGENTOPN slot list builder 00292e80 (GuiAgentLoad /
// GuiAgentSave) and the PC replacement for the PS2 memory card wrapper
// (W = *0055a830): a folder of card files.
#include "agent_load.hpp"

#include <filesystem>

namespace omega {

inline constexpr std::size_t agent_card_slots = 8;
inline constexpr std::size_t agent_card_max_entries = 0x12;  // the card struct's directory (0040d600 / 0040e090)
inline constexpr std::size_t agent_card_name_limit = 0x20;   // PS2 card names: 31 bytes and a NUL
inline constexpr std::string_view agent_card_directory = "BASCUS-97264";

// 003fd380(name, out, &slot, max): fails (false) when the font is not ready
// (byte 0055d450 or word 0055ca90 zero), a pointer is null, strlen < 2, the
// length is even or (length - 1) / 2 >= max (signed). Each pair hi lo needs
// hi in '0'..'4', lo >= 0x40 and lo - 0x40 < 0x3e; index = (hi - '0') * 0x3e
// + lo - 0x40 must be below map_count (unsigned) and is written as
// (u8)(index + 0x20). A bad pair returns with the pairs before it written and
// no NUL; then the NUL is written and the last character must be '0'..'9'
// (else fail with out terminated). slot is written only on success. name is
// read to its NUL (none throws); writes past out throw.
bool decode_agent_file_name(std::optional<std::span<const std::uint8_t>> name,
    std::optional<std::span<std::uint8_t>> out, std::int32_t* slot, std::int32_t max, bool font_ready,
    std::int32_t map_count);

struct AgentFileName {
    std::string name;
    std::int32_t slot = 0;
};

// The decoder on a card file name, with a scratch of max + 1 bytes.
std::optional<AgentFileName> decode_agent_file_name(std::string_view file_name, std::int32_t max, bool font_ready,
    std::int32_t map_count);

// One button "0".."7" of the AGENTOPN layout as 00292e80 finds it.
struct AgentSlotButton {
    bool found = true;      // 0041db80 finds the child
    bool is_button = true;  // its IsA (vtable +58) answers GuiButtonWidget
    std::uint16_t flags = 0;  // +14; bit 2 is the selectable bit the builder sets or clears
};

struct AgentSlotListScreen {
    std::array<AgentSlotButton, agent_card_slots> buttons;
    bool empty_selectable = false;  // screen +7c: empty slots keep bit 2 (and do not move the focus)
    // The focus at entry (004147a0() + 20): agent_slot_focus_other, agent_slot_focus_screen or a button index.
    std::int32_t focus = -2;
};

inline constexpr std::int32_t agent_slot_focus_other = -2;
inline constexpr std::int32_t agent_slot_focus_screen = -1;

enum class AgentSlotText { none, name, empty };  // no SetText; the decoded name; GetString("MC_EMPTY")

struct AgentSlotListResult {
    std::array<AgentSlotText, agent_card_slots> text_kind{};
    std::array<std::string, agent_card_slots> text;  // the decoded name for AgentSlotText::name
    std::vector<std::pair<std::string, std::string>> renames;  // 0040ddc0(W, old, new), in order
    // The "Updating agents to new format" popup (002cae30 / 002cac80 around the
    // renames; *0055a838 = 00293650 while open, 0 after).
    bool update_popup = false;
    // 00414b30(004147a0(), screen->vtable[+44](screen)): the focus was on an
    // empty slot that lost bit 2, or on the screen itself with a filled slot.
    bool refocus = false;
};

// 0040ddc0(W, old, new); its result is ignored.
using AgentCardRename = std::function<void(std::string_view from, std::string_view to)>;

// 00292e80(screen) over the card directory (0040d600's order). Pass 1 places
// each name without '.' that starts with a digit and decodes (max 0x10) at its
// slot (a later name wins); a slot 8 or 9 writes past the 8-entry vector and
// throws. Pass 2 renames each "<prefix>.agt?" (strncmp ignores case, a
// character must follow) into the first free slot: the popup opens on the
// first one, the prefix is encoded (max 0x20) into one stack buffer that every
// renamed slot points at (an original bug: they all show the last name), and
// a failed encode skips the rename after writing its leading pairs into that
// buffer. Then each button gets the decoded name (bit 2 set) or MC_EMPTY
// (bit 2 set when empty_selectable, else cleared). Names of 0x20 bytes or
// more and a renamed name that runs into unwritten stack throw.
AgentSlotListResult build_agent_slot_list(std::span<const std::string> directory, AgentSlotListScreen& screen,
    bool font_ready, std::int32_t map_count, const AgentCardRename& rename);

// PORT-ONLY (checkpoint 132cc10, off by default): a simulated PS2 card state
// over the folder card, so the original's offline card popups are reachable
// on PC. The fields are what the card-info step 0040efa0 leaves in W:
// - no card: +59d 0 (an mcGetInfo error other than the low half 0x2f), and
//   the error path's +59e 0, +59f 0, +560 0 (no entries), +5a4 0, +5a8 -1;
// - unformatted: +59d 1 (type 2), +59e 0 (+3f8 != 1); the directory read
//   002b56e0 fails, so +59f 0, no entries, +5a8 -1 (and +5a4 0 here);
// - free_kb: +5a4 (the card's free KB) instead of the disk's free space;
// - wrong_id: +5a8 one above the folder's id (another card, same files);
// - read_fail / write_fail / mkdir_fail / format_fail: 0040df00 / 0040e090 /
//   0040d740 / 0040db10 report a card error;
// - corrupt: 0040df00 returns the file with one byte inverted (the agent
//   load's checksum fails: 00334790 result 2, MC_CORRUPT).
struct AgentCardSimulation {
    bool present = true;
    bool formatted = true;
    std::optional<std::uint32_t> free_kb;
    bool wrong_id = false;
    bool read_fail = false;
    bool write_fail = false;
    bool mkdir_fail = false;
    bool format_fail = false;
    bool corrupt = false;
};

// "normal" (a simulated healthy card) or tokens joined by ',' or '+': nocard,
// unformatted, full (free 0 KB), free=<KB>, wrongcard, readfail, writefail,
// mkdirfail, formatfail, corrupt. "off" (or "") is nullopt: no simulation.
// Unknown tokens throw.
std::optional<AgentCardSimulation> parse_agent_card_simulation(std::string_view spec);
std::string describe_agent_card_simulation(const std::optional<AgentCardSimulation>& sim);

// The PC card: <root>/BASCUS-97264/ holds one file per entry, like the PS2
// card's save directory. The card is always present and formatted (unless a
// PORT-ONLY simulation says otherwise); its id is the number of a "#<id>"
// marker file (0040d740 writes one) or 0.
class FolderAgentCard {
public:
    explicit FolderAgentCard(std::filesystem::path root);

    const std::filesystem::path& directory() const { return directory_; }
    bool save_directory_present() const;  // W +59f
    // 0040d740 on PC: creates the directory (no icon files).
    bool create_save_directory();
    std::uint32_t card_id() const;        // W +5a8
    std::uint64_t free_bytes() const;     // W +5a4 (bytes, not the card's KB)

    // 0040d600: the entry names, sorted (the PS2 order is creation order).
    std::vector<std::string> list() const;
    // 0040df00(W, name, buffer, max): false when the entry is missing or
    // larger than buffer; else the file is copied to the front of buffer and
    // the rest is left alone.
    bool read(std::string_view name, std::span<std::uint8_t> buffer) const;
    // 0040e090(W, name, data, size, 1): needs the directory; a new entry needs
    // fewer than 0x12 entries.
    bool write(std::string_view name, std::span<const std::uint8_t> data);
    bool remove(std::string_view name);                           // 0040dc80
    bool rename(std::string_view from, std::string_view to);      // 0040ddc0
    // 0040dbb0(W, name): the entry's size, 0 when there is none.
    std::uint32_t size(std::string_view name) const;

    AgentCardState state(bool font_ready, std::int32_t map_count) const;
    AgentSaveWriter writer();
    AgentLoadReader reader() const;
    AgentCardRename renamer();

    // Port-only (checkpoint 132win13, not the original): called before write / remove / rename change the folder
    // (card_backup.hpp's rolling backups of the save directory).
    void set_before_change(std::function<void()> hook) { before_change_ = std::move(hook); }
    // PORT-ONLY (checkpoint 132cc10): the simulated card state (nullopt: none).
    void simulate(std::optional<AgentCardSimulation> sim) { sim_ = std::move(sim); }
    const std::optional<AgentCardSimulation>& simulation() const { return sim_; }
    // 0040db10 on a simulated card: false with no card or format_fail; else
    // the card reads formatted and empty (the save directory, if any, is
    // moved aside to <root>/BASCUS-97264.formatted-<n> rather than deleted).
    // Without a simulation the folder card is never unformatted: throws.
    bool format();

private:
    std::optional<std::filesystem::path> find(std::string_view name) const;
    bool usable() const { return !sim_ || (sim_->present && sim_->formatted); }
    std::filesystem::path directory_;
    std::function<void()> before_change_;
    std::optional<AgentCardSimulation> sim_;
};

}  // namespace omega
