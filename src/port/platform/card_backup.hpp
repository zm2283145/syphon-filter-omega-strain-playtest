#pragma once
// Port-only (checkpoint 132win13, not the original): rolling backups of the PC memory card's save directory
// (FolderAgentCard's <card root>/BASCUS-97264, agent_card.hpp). Before a save changes the directory (the card's
// before-change hook: write, remove, rename), its files are copied as they are on disk to
// <card root>/backups/<YYYYMMDD-HHMMSS-mmm>/BASCUS-97264/ (local time). No copy is made when backups are off
// (keep 0), the directory is missing or empty, its files equal the newest backup's, or the previous change was
// under group_window ago (one save writes several files: the copy holds the card as it was before the save).
// The oldest backups past `keep` are deleted. The count is the port setting card_backups=<n> (0..100, default 10).
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace omega {

struct PortSettings;

inline constexpr unsigned card_backups_default = 10;
inline constexpr unsigned card_backups_max = 100;

// card_backups=<n> (kept in PortSettings::other): 0 (off) .. 100, default 10. Throws on another value.
unsigned card_backups_setting(const PortSettings& s);

class CardBackups {
public:
    // save_directory: the card's BASCUS-97264 folder; backups go to its parent's backups/ folder.
    CardBackups(std::filesystem::path save_directory, unsigned keep,
                std::chrono::milliseconds group_window = std::chrono::milliseconds(2000));

    struct Result {
        std::optional<std::filesystem::path> made;  // the new backup's folder (<stamp>/BASCUS-97264's parent)
        std::string skipped;                        // why none was made (empty when one was)
        std::vector<std::filesystem::path> pruned;  // backups deleted past `keep`
    };
    // Before a change; `now` names the folder, `mono` groups a save's changes.
    Result before_change(std::chrono::system_clock::time_point now, std::chrono::steady_clock::time_point mono);
    Result before_change() { return before_change(std::chrono::system_clock::now(), std::chrono::steady_clock::now()); }

    // The backups' folders (<stamp>), oldest first.
    std::vector<std::filesystem::path> backups() const;
    const std::filesystem::path& root() const { return root_; }
    unsigned keep() const { return keep_; }

private:
    std::filesystem::path save_directory_, root_;
    unsigned keep_;
    std::chrono::milliseconds group_window_;
    std::optional<std::chrono::steady_clock::time_point> last_change_;
};

// The regular files of a folder (names on disk, sorted) and their bytes; "*.tmp~" files are left out.
std::vector<std::pair<std::string, std::vector<std::uint8_t>>> card_folder_files(const std::filesystem::path& folder);

}  // namespace omega
