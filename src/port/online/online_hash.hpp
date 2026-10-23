#pragma once
// PORT-ONLY (not in the original; checkpoint 132on1, research/ONLINE.md §5.2): the password hashing of omega_server's
// account store. The original Medius servers are gone and their storage was never on the disc, so this is port-made:
// SHA-256 (FIPS 180-4), HMAC-SHA-256 (RFC 2104) and PBKDF2-HMAC-SHA-256 (RFC 8018), self-contained so the server builds
// on Windows and Linux without a crypto library. Passwords are stored only as salted PBKDF2 digests, never as text.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

using Sha256Digest = std::array<std::uint8_t, 32>;

Sha256Digest sha256(std::span<const std::uint8_t> data);
Sha256Digest sha256(std::string_view text);
Sha256Digest hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data);
std::vector<std::uint8_t> pbkdf2_sha256(std::string_view password, std::span<const std::uint8_t> salt,
                                        std::uint32_t iterations, std::size_t length);

std::string to_hex(std::span<const std::uint8_t> bytes);
// Throws std::runtime_error on an odd length or a non-hex digit.
std::vector<std::uint8_t> from_hex(std::string_view text);

// Constant-time comparison (the result does not depend on where the first difference is).
bool equal_digest(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);

// n random bytes from std::random_device (salts, session keys).
std::vector<std::uint8_t> random_bytes(std::size_t n);

}  // namespace omega::online
