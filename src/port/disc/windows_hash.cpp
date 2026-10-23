#include "windows_hash.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace omega {
namespace {

void check(NTSTATUS status, const char* operation) {
    if (status < 0) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(status));
}

struct Algorithm {
    BCRYPT_ALG_HANDLE value{};
    ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
};

struct Hash {
    BCRYPT_HASH_HANDLE value{};
    ~Hash() { if (value) BCryptDestroyHash(value); }
};

using VerifiedKey = std::tuple<std::wstring, std::uint64_t, std::filesystem::file_time_type, std::string>;
std::mutex verified_mutex;
std::set<VerifiedKey> verified;

VerifiedKey verified_key(const std::filesystem::path& path, std::uint64_t size, const std::string& sha256) {
    return {std::filesystem::absolute(path).wstring(), size, std::filesystem::last_write_time(path), sha256};
}

}

std::string sha256_stream(std::istream& input) {
    Algorithm algorithm;
    check(BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "Open SHA-256");
    DWORD object_size = 0;
    DWORD returned = 0;
    check(BCryptGetProperty(algorithm.value, BCRYPT_OBJECT_LENGTH,
                           reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &returned, 0),
          "Read SHA-256 object size");
    if (returned != sizeof(object_size)) throw std::runtime_error("Unexpected SHA-256 object-size result");
    std::vector<UCHAR> object(object_size);
    Hash hash;
    check(BCryptCreateHash(algorithm.value, &hash.value, object.data(), object_size, nullptr, 0, 0), "Create SHA-256");
    std::vector<char> buffer(8 * 1024 * 1024);
    for (;;) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0) check(BCryptHashData(hash.value, reinterpret_cast<PUCHAR>(buffer.data()),
                                          static_cast<ULONG>(count), 0), "Hash ISO data");
        if (input.bad() || (input.fail() && !input.eof())) throw std::runtime_error("Failed while reading ISO for SHA-256");
        if (input.eof()) break;
    }
    std::array<UCHAR, 32> digest{};
    check(BCryptFinishHash(hash.value, digest.data(), static_cast<ULONG>(digest.size()), 0), "Finish SHA-256");
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

IsoStream open_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size,
                            const std::string& expected_sha256) {
    if (std::filesystem::file_size(path) != expected_size)
        throw std::runtime_error("Unsupported ISO size; see research/REQUIRED_DISC.md");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot open the supplied ISO");
    // Port-only (checkpoint 132win8): a file already verified in this process (the same path, size and write time,
    // e.g. the launcher's first-run check before the play host opens it) is not hashed a second time.
    const auto key = verified_key(path, expected_size, expected_sha256);
    bool known;
    {
        std::lock_guard lock(verified_mutex);
        known = verified.count(key) != 0;
    }
    if (!known) {
        if (sha256_stream(stream) != expected_sha256)
            throw std::runtime_error("Unsupported ISO SHA-256; see research/REQUIRED_DISC.md");
        std::lock_guard lock(verified_mutex);
        verified.insert(key);
    }
    stream.clear();
    stream.seekg(0);
    if (!stream) throw std::runtime_error("Cannot rewind the verified ISO");
    return stream;
}

void remember_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size, const std::string& expected_sha256) {
    if (std::filesystem::file_size(path) != expected_size)
        throw std::runtime_error("Unsupported ISO size; see research/REQUIRED_DISC.md");
    const auto key = verified_key(path, expected_size, expected_sha256);
    std::lock_guard lock(verified_mutex);
    verified.insert(key);
}

}
