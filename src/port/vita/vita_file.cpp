#include "vita_file.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>

namespace omega {

namespace {

// 132vl1 (research/VITA.md "Load time (132vl1)"): a read that does not continue the last one takes exactly its bytes
// once it is this large; a smaller one fills `random_fill` bytes. Before, any read under 64 KB filled the whole 64 KB
// buffer, so the level load's 4 KB TEX.HOG images and 10 KB part meshes each cost a 64 KB card read.
constexpr std::streamsize direct_minimum = 4 * 1024;
constexpr std::size_t random_fill = 8 * 1024;

std::atomic<std::uint64_t> io_reads{0}, io_bytes{0}, io_ns{0}, io_verified{0}, io_mismatches{0};
std::atomic<bool> io_verify{false};

struct IoTime {
    std::chrono::steady_clock::time_point begun = std::chrono::steady_clock::now();
    ~IoTime() {
        io_ns += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begun).count());
    }
};

}  // namespace

std::int64_t vita_file_size(const std::string& path) {
    SceIoStat stat{};
    if (sceIoGetstat(path.c_str(), &stat) < 0) return -1;
    return stat.st_size;
}

VitaIoStats vita_io_stats() {
    VitaIoStats s;
    s.reads = io_reads.load();
    s.bytes = io_bytes.load();
    s.seconds = static_cast<double>(io_ns.load()) / 1e9;
    s.verified = io_verified.load();
    s.mismatches = io_mismatches.load();
    return s;
}

void vita_io_verify(bool on) { io_verify = on; }

VitaFileBuf::~VitaFileBuf() { close(); }

VitaFileBuf::VitaFileBuf(VitaFileBuf&& other) noexcept
    : fd_(other.fd_), verify_fd_(other.verify_fd_), path_(std::move(other.path_)), buffer_start_(other.buffer_start_),
      read_end_(other.read_end_), buffer_(std::move(other.buffer_)) {
    other.fd_ = -1;
    other.verify_fd_ = -1;
    setg(buffer_.data(), buffer_.data() + (other.gptr() - other.eback()), buffer_.data() + (other.egptr() - other.eback()));
    other.setg(nullptr, nullptr, nullptr);
}

bool VitaFileBuf::open(const std::string& path) {
    close();
    fd_ = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (fd_ < 0) {
        fd_ = -1;
        return false;
    }
    path_ = path;
    buffer_start_ = 0;
    read_end_ = -1;
    setg(buffer_.data(), buffer_.data(), buffer_.data());
    return true;
}

void VitaFileBuf::close() {
    if (fd_ >= 0) sceIoClose(fd_);
    if (verify_fd_ >= 0) sceIoClose(verify_fd_);
    fd_ = -1;
    verify_fd_ = -1;
    setg(nullptr, nullptr, nullptr);
}

int VitaFileBuf::fill(std::int64_t at, std::size_t size) {
    IoTime timed;
    if (sceIoLseek(fd_, at, SCE_SEEK_SET) < 0) return -1;
    const int got = sceIoRead(fd_, buffer_.data(), static_cast<SceSize>(std::min(size, buffer_.size())));
    ++io_reads;
    if (got <= 0) return got;
    io_bytes += static_cast<std::uint64_t>(got);
    read_end_ = at + got;
    buffer_start_ = at;
    setg(buffer_.data(), buffer_.data(), buffer_.data() + got);
    return got;
}

std::streamsize VitaFileBuf::read_direct(std::int64_t at, char* s, std::streamsize n) {
    IoTime timed;
    if (sceIoLseek(fd_, at, SCE_SEEK_SET) < 0) return 0;
    std::streamsize done = 0;
    while (done < n) {
        const auto want = static_cast<SceSize>(std::min<std::streamsize>(n - done, 16 * 1024 * 1024));
        const int got = sceIoRead(fd_, s + done, want);
        ++io_reads;
        if (got <= 0) break;
        io_bytes += static_cast<std::uint64_t>(got);
        done += got;
    }
    read_end_ = at + done;
    return done;
}

void VitaFileBuf::verify(std::int64_t at, const char* s, std::streamsize n) {
    if (verify_fd_ < 0) verify_fd_ = sceIoOpen(path_.c_str(), SCE_O_RDONLY, 0);
    std::vector<char> reference(static_cast<std::size_t>(n));
    std::streamsize done = 0;
    if (verify_fd_ >= 0 && sceIoLseek(verify_fd_, at, SCE_SEEK_SET) >= 0)
        while (done < n) {
            const int got = sceIoRead(verify_fd_, reference.data() + done, static_cast<SceSize>(n - done));
            if (got <= 0) break;
            done += got;
        }
    ++io_verified;
    if (done != n || std::memcmp(reference.data(), s, static_cast<std::size_t>(n)) != 0) ++io_mismatches;
}

VitaFileBuf::int_type VitaFileBuf::underflow() {
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    if (fd_ < 0) return traits_type::eof();
    const std::int64_t next = buffer_start_ + (egptr() - eback());
    // A read continuing the last one (a stream read in order: the movies, the disc check) fills the whole buffer.
    if (fill(next, next == read_end_ ? buffer_.size() : random_fill) <= 0) return traits_type::eof();
    return traits_type::to_int_type(*gptr());
}

std::streamsize VitaFileBuf::xsgetn(char* s, std::streamsize n) {
    const std::int64_t start = buffer_start_ + (gptr() - eback());
    std::streamsize done = 0;
    // What is buffered first; then a large read, or a smaller one elsewhere in the file, straight from the file.
    const std::streamsize buffered = std::min<std::streamsize>(n, egptr() - gptr());
    if (buffered > 0) {
        std::memcpy(s, gptr(), static_cast<std::size_t>(buffered));
        gbump(static_cast<int>(buffered));
        done = buffered;
    }
    const std::int64_t at = buffer_start_ + (gptr() - eback());
    const std::streamsize rest = n - done;
    if (rest > 0 && fd_ >= 0 &&
        (rest >= static_cast<std::streamsize>(buffer_.size()) || (rest >= direct_minimum && at != read_end_))) {
        done += read_direct(at, s + done, rest);
        buffer_start_ = at + (done - buffered);
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    } else {
        while (done < n) {
            if (traits_type::eq_int_type(underflow(), traits_type::eof())) break;
            const std::streamsize take = std::min<std::streamsize>(n - done, egptr() - gptr());
            std::memcpy(s + done, gptr(), static_cast<std::size_t>(take));
            gbump(static_cast<int>(take));
            done += take;
        }
    }
    if (done > 0 && io_verify.load(std::memory_order_relaxed)) verify(start, s, done);
    return done;
}

VitaFileBuf::pos_type VitaFileBuf::seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) {
    if (fd_ < 0 || !(which & std::ios_base::in)) return pos_type(off_type(-1));
    std::int64_t target = 0;
    if (dir == std::ios_base::beg) {
        target = off;
    } else if (dir == std::ios_base::cur) {
        target = buffer_start_ + (gptr() - eback()) + off;
    } else {
        SceIoStat stat{};
        if (sceIoGetstatByFd(fd_, &stat) < 0) return pos_type(off_type(-1));
        target = stat.st_size + off;
    }
    if (target < 0) return pos_type(off_type(-1));
    // Inside the buffer: move the get pointer only.
    if (target >= buffer_start_ && target <= buffer_start_ + (egptr() - eback()) && eback()) {
        setg(eback(), eback() + (target - buffer_start_), egptr());
    } else {
        buffer_start_ = target;
        setg(buffer_.data(), buffer_.data(), buffer_.data());
    }
    return pos_type(off_type(target));
}

VitaFileBuf::pos_type VitaFileBuf::seekpos(pos_type pos, std::ios_base::openmode which) {
    return seekoff(off_type(pos), std::ios_base::beg, which);
}

}  // namespace omega
