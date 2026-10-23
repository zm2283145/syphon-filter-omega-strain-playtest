#pragma once
// PORT-ONLY (checkpoint 132on2d, research/ONLINE.md §6.10): omega_server's file writes. Every store (accounts,
// statistics, the policy file) is written to "<file>.tmp" and renamed over the file, so a crash leaves the old or the
// new file, never half of one. Before the replace, the old file is kept as "<file>.bak.1" (".bak.1" -> ".bak.2" ...,
// up to `backups`), at most once per `backup_interval_seconds` (0: at every write), so the backups span time instead
// of the last few writes.
//
// Checkpoint 132on2g (ONLINE_DEPLOY.md "Crash safety"): the ".tmp" is flushed to the disk (_commit / fsync) before
// the rename, the rename is write-through (MoveFileEx MOVEFILE_WRITE_THROUGH on Windows, the directory fsynced on
// POSIX), so a power cut after a save keeps it; load_or_recover falls back to the backups when a store does not
// parse; RotatingLog is the server's size-capped log file.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace omega::online {

// Throws std::runtime_error when the file cannot be written.
void write_file_atomic(const std::filesystem::path& file, std::string_view text, int backups = 0,
                       std::uint64_t backup_interval_seconds = 0);
std::filesystem::path backup_path(const std::filesystem::path& file, int n);  // "<file>.bak.<n>"

// Runs `load` (which reads `file` and throws on a malformed one). When it throws, the file is moved aside to
// "<file>.corrupt-<unix time>" and each backup ".bak.1" ... ".bak.<backups>" is copied over the file and loaded in
// turn, the first that loads wins. Each step is reported through `log`. When no backup loads, the bad file is put
// back and the first error is rethrown (the server refuses to start rather than start empty). A leftover "<file>.tmp"
// (a write cut short: the file itself is the last complete one) is reported and removed.
void load_or_recover(const std::filesystem::path& file, int backups, const std::function<void()>& load,
                     const std::function<void(const std::string&)>& log, std::uint64_t now_unix);

// A log file capped at `max_bytes` (0: never rotated): when a line would take it past the cap, "<log>" becomes
// "<log>.1" (".1" -> ".2" ..., `keep` kept, the oldest dropped) and a fresh "<log>" is started. Each line is flushed.
class RotatingLog {
public:
    RotatingLog() = default;
    ~RotatingLog();
    RotatingLog(const RotatingLog&) = delete;
    RotatingLog& operator=(const RotatingLog&) = delete;

    bool open(const std::filesystem::path& path, std::uint64_t max_bytes, int keep);  // appends; false on failure
    void close();
    bool is_open() const { return f_ != nullptr; }
    void write_line(std::string_view line);
    std::uint64_t size() const { return size_; }
    std::uint64_t rotations() const { return rotations_; }
    static std::filesystem::path rotated_path(const std::filesystem::path& path, int n);  // "<log>.<n>"

private:
    void rotate();

    std::FILE* f_ = nullptr;
    std::filesystem::path path_;
    std::uint64_t max_bytes_ = 0;
    int keep_ = 0;
    std::uint64_t size_ = 0;
    std::uint64_t rotations_ = 0;
};

}  // namespace omega::online