// PORT-ONLY (checkpoint 132on2d, 132on2g): see online_files.hpp.
#include "online_files.hpp"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace omega::online {

std::filesystem::path backup_path(const std::filesystem::path& file, int n) {
    return std::filesystem::path(file.string() + ".bak." + std::to_string(n));
}

namespace {

bool backup_due(const std::filesystem::path& file, std::uint64_t interval) {
    std::error_code ec;
    const auto newest = backup_path(file, 1);
    if (interval == 0 || !std::filesystem::exists(newest, ec)) return true;
    const auto when = std::filesystem::last_write_time(newest, ec);
    if (ec) return true;
    const auto age = std::filesystem::file_time_type::clock::now() - when;
    return age >= std::chrono::seconds(static_cast<std::int64_t>(interval));
}

std::FILE* open_file(const std::filesystem::path& path, bool append) {
#ifdef _WIN32
    // Shared (_SH_DENYNO), unlike _wfopen_s: an operator (or a test) can read the log while the server writes it.
    return _wfsopen(path.wstring().c_str(), append ? L"ab" : L"wb", _SH_DENYNO);
#else
    return std::fopen(path.string().c_str(), append ? "ab" : "wb");
#endif
}

// The data reaches the disk, not only the OS cache.
bool flush_to_disk(std::FILE* f) {
    if (std::fflush(f) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

// Replaces `to` with `from`; the rename itself is made durable too.
bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
#ifdef _WIN32
    if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
#else
    std::filesystem::rename(from, to, ec);
    if (ec) return false;
    const auto dir = to.has_parent_path() ? to.parent_path() : std::filesystem::path(".");
    const int fd = ::open(dir.c_str(), O_RDONLY);
    if (fd >= 0) {
        (void)fsync(fd);
        ::close(fd);
    }
    return true;
#endif
}

}  // namespace

void write_file_atomic(const std::filesystem::path& file, std::string_view text, int backups,
                       std::uint64_t backup_interval_seconds) {
    if (file.empty()) return;
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::FILE* out = open_file(tmp, false);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        const bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size() && flush_to_disk(out);
        if (std::fclose(out) != 0 || !ok) throw std::runtime_error("cannot write " + tmp.string());
    }
    if (backups > 0 && std::filesystem::exists(file, ec) && backup_due(file, backup_interval_seconds)) {
        std::filesystem::remove(backup_path(file, backups), ec);
        for (int n = backups - 1; n >= 1; --n)
            if (std::filesystem::exists(backup_path(file, n), ec))
                std::filesystem::rename(backup_path(file, n), backup_path(file, n + 1), ec);
        std::filesystem::copy_file(file, backup_path(file, 1), std::filesystem::copy_options::overwrite_existing, ec);
        // copy_file keeps the source's time; the interval counts from the backup's making.
        std::filesystem::last_write_time(backup_path(file, 1), std::filesystem::file_time_type::clock::now(), ec);
    }
    // A virus scanner or indexer can hold the fresh file for a moment (a sharing violation on Windows): retry briefly.
    for (int attempt = 0; attempt < 20; ++attempt) {
        ec.clear();
        if (replace_file(tmp, file, ec)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (ec) throw std::runtime_error("cannot replace " + file.string() + ": " + ec.message());
}

void load_or_recover(const std::filesystem::path& file, int backups, const std::function<void()>& load,
                     const std::function<void(const std::string&)>& log, std::uint64_t now_unix) {
    const auto say = [&](const std::string& s) {
        if (log) log(s);
    };
    std::error_code ec;
    if (!file.empty()) {
        const std::filesystem::path tmp = file.string() + ".tmp";
        if (std::filesystem::exists(tmp, ec)) {
            say("store " + file.string() + ": removed " + tmp.string() + " (a write cut short; the file is the last complete save)");
            std::filesystem::remove(tmp, ec);
        }
    }
    std::exception_ptr first;
    std::string first_text;
    try {
        load();
        return;
    } catch (const std::exception& e) {
        first = std::current_exception();
        first_text = e.what();
    }
    if (file.empty() || !std::filesystem::exists(file, ec)) std::rethrow_exception(first);
    const std::filesystem::path corrupt = file.string() + ".corrupt-" + std::to_string(now_unix);
    say("store " + file.string() + " does not load (" + first_text + "): moved to " + corrupt.string());
    std::filesystem::rename(file, corrupt, ec);
    if (ec) {
        say("store " + file.string() + ": cannot move it aside (" + ec.message() + ")");
        std::rethrow_exception(first);
    }
    for (int n = 1; n <= backups; ++n) {
        const auto b = backup_path(file, n);
        if (!std::filesystem::exists(b, ec)) continue;
        std::filesystem::copy_file(b, file, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) continue;
        try {
            load();
            say("store " + file.string() + ": recovered from " + b.string());
            return;
        } catch (const std::exception& e) {
            say("store " + file.string() + ": " + b.string() + " does not load either (" + e.what() + ")");
        }
    }
    std::filesystem::remove(file, ec);
    std::filesystem::rename(corrupt, file, ec);
    say("store " + file.string() + ": no backup loads; the file is left as it was");
    std::rethrow_exception(first);
}

// ---- RotatingLog ----------------------------------------------------------------------------------------------

RotatingLog::~RotatingLog() { close(); }

std::filesystem::path RotatingLog::rotated_path(const std::filesystem::path& path, int n) {
    return std::filesystem::path(path.string() + "." + std::to_string(n));
}

bool RotatingLog::open(const std::filesystem::path& path, std::uint64_t max_bytes, int keep) {
    close();
    path_ = path;
    max_bytes_ = max_bytes;
    keep_ = keep < 0 ? 0 : keep;
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    f_ = open_file(path, true);
    if (!f_) return false;
    const auto size = std::filesystem::file_size(path, ec);
    size_ = ec ? 0 : static_cast<std::uint64_t>(size);
    return true;
}

void RotatingLog::close() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
}

void RotatingLog::rotate() {
    close();
    std::error_code ec;
    if (keep_ > 0) {
        std::filesystem::remove(rotated_path(path_, keep_), ec);
        for (int n = keep_ - 1; n >= 1; --n)
            if (std::filesystem::exists(rotated_path(path_, n), ec))
                std::filesystem::rename(rotated_path(path_, n), rotated_path(path_, n + 1), ec);
        std::filesystem::rename(path_, rotated_path(path_, 1), ec);
    }
    f_ = open_file(path_, keep_ > 0 && ec);  // a failed rename (the file held open elsewhere) keeps appending
    if (!f_) return;
    if (keep_ == 0 || !ec) size_ = 0;
    ++rotations_;
}

void RotatingLog::write_line(std::string_view line) {
    if (!f_) return;
    const std::uint64_t n = line.size() + 1;
    if (max_bytes_ != 0 && size_ != 0 && size_ + n > max_bytes_) {
        rotate();
        if (!f_) return;
    }
    std::fwrite(line.data(), 1, line.size(), f_);
    std::fputc('\n', f_);
    std::fflush(f_);
    size_ += n;
}

}  // namespace omega::online
