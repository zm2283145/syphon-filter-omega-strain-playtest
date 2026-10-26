#pragma once
// Port-only (132ps5c, research/PS5.md "Paths"): the PS5's check of the user's disc image, behind windows_hash.hpp
// (iso_verify_ps5.cpp implements sha256_stream / open_verified_iso / remember_verified_iso for the shared code).
// The size, then the full SHA-256 of research/REQUIRED_DISC.md once: a verified image (path, size, modification time,
// digest) is remembered in /download0/iso_verified.txt, so later boots only compare that key.

#include "windows_hash.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace omega {

// Opens the image after the check; `how` says what was done ("cached", "hashed"). progress(done, total) is called
// while hashing (bytes).
IsoStream open_checked_iso_ps5(const std::string& path, std::uint64_t expected_size, const std::string& expected_sha256,
                               const std::function<void(std::uint64_t done, std::uint64_t total)>& progress,
                               std::string& how);

}  // namespace omega