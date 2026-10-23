// Port-only (checkpoint 132vt1, research/VITA.md "Storage"): the PS Vita's boot check of the user's disc image.
//
// Hashing the whole 3.46 GB image takes minutes on the handheld (the first hardware run sat in it with a black
// screen), so the boot check is quick: the size, then the SHA-256 of the image's first and last MiB and the ISO 9660
// root directory (the primary volume descriptor's extent, at most 64 KiB) against OMEGA_ISO_QUICK_SHA256. A checked
// image (path, size, modification time) is remembered in ux0:data/omega_strain/iso_verified.txt, as is a fully
// verified one. The full SHA-256 of research/REQUIRED_DISC.md is an option (`full`: ux0:data/omega_strain/
// verify_full.txt exists, or TRIANGLE is held at boot) with progress reports.
#pragma once

#include "windows_hash.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace omega {

// The quick digest's input (above) of an open image of `size` bytes.
std::string iso_quick_sha256(std::istream& input, std::uint64_t size);

// Opens the image after the boot check; `how` says what was done ("cached", "quick", "full").
// progress(done, total) is called during a full verify (bytes).
IsoStream open_checked_iso(const std::string& path, std::uint64_t expected_size, const std::string& expected_sha256,
                           const std::string& expected_quick_sha256, bool full,
                           const std::function<void(std::uint64_t done, std::uint64_t total)>& progress, std::string& how);

}  // namespace omega
