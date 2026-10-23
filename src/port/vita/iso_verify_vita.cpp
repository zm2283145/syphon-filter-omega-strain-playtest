// Port-only (checkpoint 132vt1, research/VITA.md "Storage"): windows_hash.hpp's ISO checks on the PS Vita. The same
// contract as the Windows build's windows_hash.cpp (the size, then the SHA-256 of research/REQUIRED_DISC.md, a file
// verified once per process is not hashed again), with a SceIo stream (vita_file.hpp) and a self-contained SHA-256
// (FIPS 180-4; the Vita has no bcrypt). Hashing the 3.46 GB image takes minutes on the handheld, so a verified image
// is also remembered across runs in ux0:data/omega_strain/iso_verified.txt (its path, size, modification time and
// digest; any change hashes it again), as the PC launcher's iso_verified= setting does. The boot uses the quick check
// (iso_verify_vita.hpp, open_checked_iso); the full hash is an option there.

#include "iso_verify_vita.hpp"
#include "windows_hash.hpp"

#include <psp2/io/stat.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace omega {
namespace {

class Sha256 {
public:
    void update(const std::uint8_t* data, std::size_t n) {
        total_ += n;
        while (n > 0) {
            const std::size_t take = std::min(n, std::size_t(64) - used_);
            std::memcpy(block_.data() + used_, data, take);
            used_ += take;
            data += take;
            n -= take;
            if (used_ == 64) {
                compress(block_.data());
                used_ = 0;
            }
        }
    }
    std::array<std::uint8_t, 32> finish() {
        const std::uint64_t bits = total_ * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (used_ != 56) update(&zero, 1);
        std::uint8_t length[8];
        for (int k = 0; k < 8; ++k) length[k] = std::uint8_t(bits >> (56 - 8 * k));
        update(length, 8);
        std::array<std::uint8_t, 32> out{};
        for (int k = 0; k < 8; ++k)
            for (int b = 0; b < 4; ++b) out[std::size_t(4 * k + b)] = std::uint8_t(h_[std::size_t(k)] >> (24 - 8 * b));
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const std::uint8_t* p) {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
            0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
            0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
            0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
            0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
            0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
            0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = std::uint32_t(p[4 * i]) << 24 | std::uint32_t(p[4 * i + 1]) << 16 | std::uint32_t(p[4 * i + 2]) << 8 |
                   std::uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
        h_[5] += f;
        h_[6] += g;
        h_[7] += h;
    }
    std::array<std::uint32_t, 8> h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> block_{};
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

std::string hex(const std::array<std::uint8_t, 32>& digest) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

std::string key_text(const std::string& path, std::uint64_t size, const std::string& sha256) {
    SceIoStat stat{};
    if (sceIoGetstat(path.c_str(), &stat) < 0) return {};
    char time[64];
    std::snprintf(time, sizeof time, "%04d-%02d-%02d %02d:%02d:%02d.%06u", stat.st_mtime.year, stat.st_mtime.month,
                  stat.st_mtime.day, stat.st_mtime.hour, stat.st_mtime.minute, stat.st_mtime.second,
                  unsigned(stat.st_mtime.microsecond));
    return path + "|" + std::to_string(size) + "|" + time + "|" + sha256;
}

std::string cache_path() { return std::string(vita_data_root) + "/iso_verified.txt"; }

std::mutex verified_mutex;
std::set<std::string> verified;

}  // namespace

std::string sha256_stream(std::istream& input) {
    Sha256 hash;
    std::vector<char> buffer(4 * 1024 * 1024);
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(count));
        if (input.bad() || (input.fail() && !input.eof())) throw std::runtime_error("Failed while reading ISO for SHA-256");
        if (input.eof()) break;
    }
    return hex(hash.finish());
}

IsoStream open_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size,
                            const std::string& expected_sha256) {
    const std::string file = path.string();
    if (vita_file_size(file) != static_cast<std::int64_t>(expected_size))
        throw std::runtime_error("Unsupported ISO size (" + file + "); see research/REQUIRED_DISC.md and research/VITA.md");
    VitaFileStream stream(file);
    if (!stream) throw std::runtime_error("Cannot open the supplied ISO " + file);
    const std::string key = key_text(file, expected_size, expected_sha256);
    bool known;
    {
        std::lock_guard lock(verified_mutex);
        known = verified.count(key) != 0;
    }
    if (!known) {
        std::ifstream cache(cache_path());
        std::string line;
        while (!known && std::getline(cache, line)) known = !key.empty() && line == key;
    }
    if (!known) {
        if (sha256_stream(stream) != expected_sha256)
            throw std::runtime_error("Unsupported ISO SHA-256; see research/REQUIRED_DISC.md");
        std::ofstream(cache_path(), std::ios::app) << key << "\n";
    }
    {
        std::lock_guard lock(verified_mutex);
        verified.insert(key);
    }
    stream.clear();
    stream.seekg(0);
    if (!stream) throw std::runtime_error("Cannot rewind the verified ISO");
    return stream;
}

std::string iso_quick_sha256(std::istream& input, std::uint64_t size) {
    constexpr std::uint64_t mib = 1024 * 1024;
    if (size < 2 * mib) throw std::runtime_error("The disc image is too small for the quick check");
    Sha256 hash;
    std::vector<char> buffer(mib);
    const auto read_at = [&](std::uint64_t at, std::size_t n) {
        input.clear();
        input.seekg(static_cast<std::streamoff>(at));
        input.read(buffer.data(), static_cast<std::streamsize>(n));
        if (static_cast<std::size_t>(input.gcount()) != n) throw std::runtime_error("Failed while reading the disc image");
    };
    read_at(0, mib);
    hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), mib);
    // ISO 9660 primary volume descriptor (sector 16): type 1, "CD001"; the root directory record at +156 (its extent's
    // LBA at +2, its length at +10, both little-endian).
    const auto* pvd = reinterpret_cast<const std::uint8_t*>(buffer.data()) + 16 * 2048;
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
        throw std::runtime_error("The disc image has no ISO 9660 volume descriptor");
    const auto le32 = [](const std::uint8_t* q) {
        return std::uint32_t(q[0]) | std::uint32_t(q[1]) << 8 | std::uint32_t(q[2]) << 16 | std::uint32_t(q[3]) << 24;
    };
    const std::uint64_t root_at = std::uint64_t(le32(pvd + 156 + 2)) * 2048;
    const std::size_t root_bytes = std::min<std::size_t>(le32(pvd + 156 + 10), 64 * 1024);
    if (root_at + root_bytes > size) throw std::runtime_error("The disc image's root directory is past its end");
    read_at(size - mib, mib);
    hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), mib);
    read_at(root_at, root_bytes);
    hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), root_bytes);
    return hex(hash.finish());
}

IsoStream open_checked_iso(const std::string& file, std::uint64_t expected_size, const std::string& expected_sha256,
                           const std::string& expected_quick_sha256, bool full,
                           const std::function<void(std::uint64_t done, std::uint64_t total)>& progress, std::string& how) {
    if (vita_file_size(file) != static_cast<std::int64_t>(expected_size))
        throw std::runtime_error("Unsupported ISO size (" + file + "); see research/REQUIRED_DISC.md and research/VITA.md");
    VitaFileStream stream(file);
    if (!stream) throw std::runtime_error("Cannot open the supplied ISO " + file);
    const std::string full_key = key_text(file, expected_size, expected_sha256);
    const std::string quick_key = full_key.empty() ? std::string() : "quick|" + key_text(file, expected_size, expected_quick_sha256);
    bool full_known = false, quick_known = false;
    {
        std::ifstream cache(cache_path());
        std::string line;
        while (std::getline(cache, line)) {
            full_known = full_known || (!full_key.empty() && line == full_key);
            quick_known = quick_known || (!quick_key.empty() && line == quick_key);
        }
    }
    if (full && !full_known) {
        Sha256 hash;
        std::vector<char> buffer(4 * 1024 * 1024);
        std::uint64_t done = 0;
        stream.clear();
        stream.seekg(0);
        for (;;) {
            stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = stream.gcount();
            if (count > 0) hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(count));
            done += static_cast<std::uint64_t>(std::max<std::streamsize>(count, 0));
            if (progress) progress(done, expected_size);
            if (stream.bad() || (stream.fail() && !stream.eof())) throw std::runtime_error("Failed while reading ISO for SHA-256");
            if (stream.eof()) break;
        }
        if (hex(hash.finish()) != expected_sha256) throw std::runtime_error("Unsupported ISO SHA-256; see research/REQUIRED_DISC.md");
        std::ofstream(cache_path(), std::ios::app) << full_key << "\n";
        full_known = true;
        how = "full";
    } else if (full_known) {
        how = "cached (full)";
    } else if (quick_known) {
        how = "cached (quick)";
    } else {
        if (iso_quick_sha256(stream, expected_size) != expected_quick_sha256)
            throw std::runtime_error("Unsupported disc image (its quick SHA-256 differs); see research/REQUIRED_DISC.md");
        if (!quick_key.empty()) std::ofstream(cache_path(), std::ios::app) << quick_key << "\n";
        how = "quick";
    }
    {
        std::lock_guard lock(verified_mutex);
        verified.insert(full_key);
    }
    stream.clear();
    stream.seekg(0);
    if (!stream) throw std::runtime_error("Cannot rewind the checked ISO");
    return stream;
}

void remember_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size, const std::string& expected_sha256) {
    const std::string file = path.string();
    if (vita_file_size(file) != static_cast<std::int64_t>(expected_size))
        throw std::runtime_error("Unsupported ISO size; see research/REQUIRED_DISC.md");
    std::lock_guard lock(verified_mutex);
    verified.insert(key_text(file, expected_size, expected_sha256));
}

}  // namespace omega
