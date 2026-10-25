#pragma once
// Port-only (checkpoint 132vt1, research/VITA.md "Storage"): PS Vita file access the shared code needs beyond the
// toolchain's libstdc++ / newlib: a read-only std::istream on SceIo with 64-bit offsets (newlib's off_t, and so
// std::ifstream's seeks and std::filesystem::file_size, are 32-bit; the game's ISO is 3.46 GB), and the data folder.
//
// Game data (the same reference mode as the PC build; nothing is packaged in the .vpk): the user's verified disc image
// at ux0:data/omega_strain/omega_strain.iso. The port writes its settings, memory card, shader cache and logs under
// ux0:data/omega_strain/ as well.

#include <cstdint>
#include <filesystem>
#include <istream>
#include <streambuf>
#include <string>
#include <vector>

namespace omega {

inline constexpr const char* vita_data_root = "ux0:data/omega_strain";

// The file's size (sceIoGetstat), or -1 when it does not exist.
std::int64_t vita_file_size(const std::string& path);

// Port-only (132vl1, research/VITA.md "Load time (132vl1)"): the SceIo reads every VitaFileBuf made (sceIoRead calls,
// bytes, seconds inside sceIoLseek + sceIoRead), for the level load's phase lines. With verify on (the test switch
// vita_test_load_verify.txt), each xsgetn's bytes are read again from a second descriptor of the same file, straight
// (no buffer), and compared: `verified` reads, `mismatches` that differed (0 expected).
struct VitaIoStats {
    std::uint64_t reads = 0, bytes = 0, verified = 0, mismatches = 0;
    double seconds = 0.0;
};
VitaIoStats vita_io_stats();
void vita_io_verify(bool on);

class VitaFileBuf final : public std::streambuf {
public:
    VitaFileBuf() = default;
    ~VitaFileBuf() override;
    VitaFileBuf(VitaFileBuf&& other) noexcept;
    VitaFileBuf& operator=(VitaFileBuf&&) = delete;
    bool open(const std::string& path);
    bool is_open() const { return fd_ >= 0; }
    void close();

protected:
    int_type underflow() override;
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, std::ios_base::openmode which) override;
    pos_type seekpos(pos_type pos, std::ios_base::openmode which) override;
    std::streamsize xsgetn(char* s, std::streamsize n) override;

private:
    int fill(std::int64_t at, std::size_t size);
    std::streamsize read_direct(std::int64_t at, char* s, std::streamsize n);
    void verify(std::int64_t at, const char* s, std::streamsize n);

    int fd_ = -1;
    int verify_fd_ = -1;
    std::string path_;
    std::int64_t buffer_start_ = 0;  // file offset of eback()
    std::int64_t read_end_ = -1;     // file offset after the last sceIoRead (a read starting there is sequential)
    std::vector<char> buffer_ = std::vector<char>(64 * 1024);
};

class VitaFileStream final : public std::istream {
public:
    VitaFileStream() : std::istream(nullptr) { rdbuf(&buf_); }
    explicit VitaFileStream(const std::string& path) : VitaFileStream() { open(path); }
    // std::ifstream's (path, mode) form, so shared code holding an IsoStream (windows_hash.hpp) opens either; read-only.
    VitaFileStream(const std::filesystem::path& path, std::ios_base::openmode) : VitaFileStream(path.string()) {}
    VitaFileStream(VitaFileStream&& other) noexcept : std::istream(nullptr), buf_(std::move(other.buf_)) {
        rdbuf(&buf_);
        clear(other.rdstate());
    }
    void open(const std::string& path) {
        if (!buf_.open(path)) setstate(std::ios_base::failbit);
        else clear();
    }
    bool is_open() const { return buf_.is_open(); }

private:
    VitaFileBuf buf_;
};

}  // namespace omega
