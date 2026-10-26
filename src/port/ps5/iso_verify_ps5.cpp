// Port-only (132ps5c, research/PS5.md "Paths"): windows_hash.hpp's ISO checks on the PS5. The same contract as the
// Windows build's windows_hash.cpp (the size, then the SHA-256 of research/REQUIRED_DISC.md; a file verified once per
// process is not hashed again), with a std::ifstream (the PS5's libc++ seeks with 64-bit offsets) and a self-contained
// SHA-256 (FIPS 180-4). A verified image is also remembered across runs in /download0/iso_verified.txt (its path, size,
// modification time and digest; any change hashes it again), as the PC launcher's iso_verified= setting does.

#include "iso_verify_ps5.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
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
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) return {};
    char time[64];
    std::snprintf(time, sizeof time, "%lld.%09ld", static_cast<long long>(st.st_mtim.tv_sec), long(st.st_mtim.tv_nsec));
    return path + "|" + std::to_string(size) + "|" + time + "|" + sha256;
}

std::int64_t file_size(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? static_cast<std::int64_t>(st.st_size) : -1;
}

constexpr const char* cache_path = "/download0/iso_verified.txt";

std::mutex verified_mutex;
std::set<std::string> verified;

bool cached(const std::string& key) {
    if (key.empty()) return false;
    {
        std::lock_guard lock(verified_mutex);
        if (verified.count(key)) return true;
    }
    std::ifstream cache(cache_path);
    for (std::string line; std::getline(cache, line);)
        if (line == key) return true;
    return false;
}

std::string hash_all(std::istream& input, std::uint64_t total,
                     const std::function<void(std::uint64_t done, std::uint64_t total)>& progress) {
    Sha256 hash;
    std::vector<char> buffer(4 * 1024 * 1024);
    std::uint64_t done = 0;
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) hash.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(count));
        done += static_cast<std::uint64_t>(std::max<std::streamsize>(count, 0));
        if (progress) progress(done, total);
        if (input.bad() || (input.fail() && !input.eof())) throw std::runtime_error("Failed while reading ISO for SHA-256");
        if (input.eof()) break;
    }
    return hex(hash.finish());
}

}  // namespace

std::string sha256_stream(std::istream& input) { return hash_all(input, 0, {}); }

IsoStream open_checked_iso_ps5(const std::string& file, std::uint64_t expected_size, const std::string& expected_sha256,
                               const std::function<void(std::uint64_t done, std::uint64_t total)>& progress,
                               std::string& how) {
    if (file_size(file) != static_cast<std::int64_t>(expected_size))
        throw std::runtime_error("Unsupported ISO size (" + file + "); see research/REQUIRED_DISC.md and research/PS5.md");
    IsoStream stream(file, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open the supplied ISO " + file);
    const std::string key = key_text(file, expected_size, expected_sha256);
    if (cached(key)) {
        how = "cached";
    } else {
        if (hash_all(stream, expected_size, progress) != expected_sha256)
            throw std::runtime_error("Unsupported ISO SHA-256; see research/REQUIRED_DISC.md");
        if (!key.empty()) std::ofstream(cache_path, std::ios::app) << key << "\n";
        how = "hashed";
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

IsoStream open_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size,
                            const std::string& expected_sha256) {
    std::string how;
    return open_checked_iso_ps5(path.string(), expected_size, expected_sha256, {}, how);
}

void remember_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size, const std::string& expected_sha256) {
    const std::string file = path.string();
    if (file_size(file) != static_cast<std::int64_t>(expected_size))
        throw std::runtime_error("Unsupported ISO size; see research/REQUIRED_DISC.md");
    std::lock_guard lock(verified_mutex);
    verified.insert(key_text(file, expected_size, expected_sha256));
}

}  // namespace omega