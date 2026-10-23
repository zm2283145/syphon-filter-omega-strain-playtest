#include "vita_file.hpp"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include <algorithm>
#include <cstring>

namespace omega {

std::int64_t vita_file_size(const std::string& path) {
    SceIoStat stat{};
    if (sceIoGetstat(path.c_str(), &stat) < 0) return -1;
    return stat.st_size;
}

VitaFileBuf::~VitaFileBuf() { close(); }

VitaFileBuf::VitaFileBuf(VitaFileBuf&& other) noexcept
    : fd_(other.fd_), buffer_start_(other.buffer_start_), buffer_(std::move(other.buffer_)) {
    other.fd_ = -1;
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
    buffer_start_ = 0;
    setg(buffer_.data(), buffer_.data(), buffer_.data());
    return true;
}

void VitaFileBuf::close() {
    if (fd_ >= 0) sceIoClose(fd_);
    fd_ = -1;
    setg(nullptr, nullptr, nullptr);
}

VitaFileBuf::int_type VitaFileBuf::underflow() {
    if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
    if (fd_ < 0) return traits_type::eof();
    const std::int64_t next = buffer_start_ + (egptr() - eback());
    if (sceIoLseek(fd_, next, SCE_SEEK_SET) < 0) return traits_type::eof();
    const int got = sceIoRead(fd_, buffer_.data(), static_cast<SceSize>(buffer_.size()));
    if (got <= 0) return traits_type::eof();
    buffer_start_ = next;
    setg(buffer_.data(), buffer_.data(), buffer_.data() + got);
    return traits_type::to_int_type(*gptr());
}

std::streamsize VitaFileBuf::xsgetn(char* s, std::streamsize n) {
    std::streamsize done = 0;
    // What is buffered first, then large reads straight from the file.
    const std::streamsize buffered = std::min<std::streamsize>(n, egptr() - gptr());
    if (buffered > 0) {
        std::memcpy(s, gptr(), static_cast<std::size_t>(buffered));
        gbump(static_cast<int>(buffered));
        done = buffered;
    }
    if (done < n && n - done >= static_cast<std::streamsize>(buffer_.size()) && fd_ >= 0) {
        const std::int64_t at = buffer_start_ + (gptr() - eback());
        if (sceIoLseek(fd_, at, SCE_SEEK_SET) < 0) return done;
        while (done < n) {
            const auto want = static_cast<SceSize>(std::min<std::streamsize>(n - done, 16 * 1024 * 1024));
            const int got = sceIoRead(fd_, s + done, want);
            if (got <= 0) break;
            done += got;
        }
        buffer_start_ = at + (done - buffered);
        setg(buffer_.data(), buffer_.data(), buffer_.data());
        return done;
    }
    while (done < n) {
        if (traits_type::eq_int_type(underflow(), traits_type::eof())) break;
        const std::streamsize take = std::min<std::streamsize>(n - done, egptr() - gptr());
        std::memcpy(s + done, gptr(), static_cast<std::size_t>(take));
        gbump(static_cast<int>(take));
        done += take;
    }
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
