#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <istream>
#include <string>

#ifdef __vita__
#include "vita_file.hpp"
#endif

namespace omega {
// The opened ISO's stream: a file stream on Windows; on the PS Vita (port-only, checkpoint 132vt1) a SceIo stream with
// 64-bit offsets (the toolchain's libstdc++ file streams seek with a 32-bit off_t; the ISO is 3.46 GB), the Vita's
// implementation of these functions being vita/iso_verify_vita.cpp.
#ifdef __vita__
using IsoStream = VitaFileStream;
#else
using IsoStream = std::ifstream;
#endif
std::string sha256_stream(std::istream& input);
IsoStream open_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size,
                            const std::string& expected_sha256);
// Port-only (checkpoint 132win9): marks the file (its current path, size and write time) as verified for this process,
// so open_verified_iso does not hash it (the launcher's cached verification, launcher.hpp iso_verified=).
void remember_verified_iso(const std::filesystem::path& path, std::uint64_t expected_size, const std::string& expected_sha256);
}
