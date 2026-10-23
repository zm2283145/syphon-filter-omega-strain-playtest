#include "card_backup.hpp"

#include "port_settings.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace omega {
namespace fs = std::filesystem;

unsigned card_backups_setting(const PortSettings& s) {
    unsigned n = card_backups_default;
    for (const auto& [key, value] : s.other) {
        if (key != "card_backups") continue;
        std::size_t used = 0;
        unsigned long v = 0;
        try {
            v = std::stoul(value, &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used == 0 || used != value.size() || v > card_backups_max)
            throw std::runtime_error("Port settings: card_backups=" + value + " (needs 0.." +
                                     std::to_string(card_backups_max) + ")");
        n = static_cast<unsigned>(v);
    }
    return n;
}

std::vector<std::pair<std::string, std::vector<std::uint8_t>>> card_folder_files(const fs::path& folder) {
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> out;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) return out;
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().filename().string();
        if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".tmp~") == 0) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        out.emplace_back(name, std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

CardBackups::CardBackups(fs::path save_directory, unsigned keep, std::chrono::milliseconds group_window)
    : save_directory_(std::move(save_directory)), root_(save_directory_.parent_path() / "backups"), keep_(keep),
      group_window_(group_window) {}

std::vector<fs::path> CardBackups::backups() const {
    static const std::regex stamp(R"(\d{8}-\d{6}-\d{3}(-\d+)?)");
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) return out;
    for (const auto& entry : fs::directory_iterator(root_))
        if (entry.is_directory() && std::regex_match(entry.path().filename().string(), stamp)) out.push_back(entry.path());
    // The stamps sort by time; a same-millisecond "-<n>" suffix sorts after its stamp.
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        const auto x = a.filename().string(), y = b.filename().string();
        const auto base = [](const std::string& s) { return s.substr(0, 19); };
        const auto suffix = [](const std::string& s) { return s.size() > 20 ? std::stoi(s.substr(20)) : 1; };
        return base(x) != base(y) ? base(x) < base(y) : suffix(x) < suffix(y);
    });
    return out;
}

CardBackups::Result CardBackups::before_change(std::chrono::system_clock::time_point now,
                                               std::chrono::steady_clock::time_point mono) {
    Result r;
    const bool grouped = last_change_ && mono - *last_change_ < group_window_;
    last_change_ = mono;
    if (keep_ == 0) {
        r.skipped = "backups off (card_backups=0)";
        return r;
    }
    if (grouped) {
        r.skipped = "the same save";
        return r;
    }
    const auto files = card_folder_files(save_directory_);
    if (files.empty()) {
        r.skipped = "no save files yet";
        return r;
    }
    auto list = backups();
    if (!list.empty() && card_folder_files(list.back() / save_directory_.filename()) == files) {
        r.skipped = "unchanged since " + list.back().filename().string();
        return r;
    }
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::ostringstream name;
    name << std::put_time(&local, "%Y%m%d-%H%M%S") << '-' << std::setw(3) << std::setfill('0') << ms;
    fs::path folder = root_ / name.str();
    for (int k = 2; fs::exists(folder); ++k) folder = root_ / (name.str() + "-" + std::to_string(k));
    const fs::path target = folder / save_directory_.filename();
    fs::create_directories(target);
    for (const auto& [file, bytes] : files) {
        std::ofstream out(target / file, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("Memory card backup: cannot write " + (target / file).string());
    }
    r.made = folder;
    list.push_back(folder);
    while (list.size() > keep_) {
        std::error_code ec;
        fs::remove_all(list.front(), ec);
        r.pruned.push_back(list.front());
        list.erase(list.begin());
    }
    return r;
}

}  // namespace omega
