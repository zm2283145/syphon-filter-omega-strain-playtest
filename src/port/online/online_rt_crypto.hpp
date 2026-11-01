#pragma once
// PORT-ONLY server side (checkpoint 134on5, research/ONLINE.md §8.7): the SCE-RT message security of the ORIGINAL
// client (the retail disc with its INI's DISABLE_SECURITY unset), so omega_server can serve an unpatched disc. The
// client side is the matched decomp of the retail executable; this file is our own C++ written from it:
//
//  * The hash: KM_Encrypt 0x002f9780 / KM_Decrypt 0x002f9c80 take the first four bytes of SHA-1 (KM_SHA1
//    0x002f7fb0) of the plaintext as a little-endian u32, clear its top three bits and put the cipher context there:
//    7 (0xE0000000) RSA to the holder of the private key, 4 (0x80000000) the software key pair, 3 (0x60000000) the
//    connection's RC key (KM_AddSymmetricKey), 1 (0x20000000) the session RC key, 0 plain. An encrypted RT message
//    is [type | 0x80][u16 length of the data][u32 hash][data] (rt_msg_client_process_recv_buf 0x002ff308: the
//    length excludes the hash; a zero-length message has none).
//  * The RC cipher (RCSaltyInitialize 0x002f3728, RCEncrypt 0x002f3810, RCDecrypt 0x002f38b8): an RC4 variant
//    whose S-box starts reversed (S[i] = ~i), first mixed with the four hash bytes (step 5), then with the 64-byte
//    key (step 3); each byte steps i by 5 and also folds the PLAINTEXT byte into j.
//  * RSA (RSAEncrypt 0x002f4cd0 / RSADecrypt 0x002f4d00): 64-byte blocks, little-endian integers, e = 17
//    (dat_00490848). After decrypting, a hash mismatch is retried with N added (KM_RSAAdjust 0x002fa0f8), for a
//    plaintext that was >= N.
//  * The disc's server key: KM_GetMediusPublicKey 0x00352c30 copies 64 bytes (D_00492BF0) into the connect
//    parameters (SFOMGCL_Connect_MAS 0x00444a4c, unless DISABLE_SECURITY). The client encrypts its first message
//    (type 0x12, its own 64-byte public key) to it with context 7. Serving that client needs the matching private
//    exponent. It is NOT in this repository: the operator supplies it (original_rt_key=, ONLINE_DEPLOY.md), and
//    the server checks it against N and, given the operator's own executable, against the disc's key.
//
// Credits: the Horizon Medius server (MIT, github.com/Horizon-Private-Server/horizon-server, RT.Cryptography)
// documents the same scheme for other SCE-RT titles; nothing of its code is copied here.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online::rtcrypto {

using Sha1Digest = std::array<std::uint8_t, 20>;
Sha1Digest sha1(std::span<const std::uint8_t> data);

constexpr std::uint8_t kCtxPlain = 0, kCtxServerSession = 1, kCtxClientSession = 3, kCtxSoftware = 4, kCtxRsa = 7;
constexpr std::size_t kBlock = 64;  // the RSA block and the RC key size
using Block = std::array<std::uint8_t, kBlock>;  // a little-endian 512-bit integer

// KM_Encrypt's header word: SHA-1 bytes 0..3 (little endian), top three bits = the context.
std::uint32_t rt_hash(std::span<const std::uint8_t> plain, std::uint8_t context);
// The client treats a header with no hash bits as plaintext (the hash bytes 0..2 zero, byte 3's low five bits zero).
inline bool hash_present(std::uint32_t h) { return (h & 0x1FFFFFFFu) != 0; }
inline std::uint8_t hash_context(std::uint32_t h) { return static_cast<std::uint8_t>(h >> 29); }

// The RC cipher with its per-message salt (the header word, as stored little endian).
void rc_encrypt(const Block& key, std::uint32_t salt, std::span<std::uint8_t> data);
void rc_decrypt(const Block& key, std::uint32_t salt, std::span<std::uint8_t> data);

// x^e mod n on 64-byte little-endian integers (n odd, non-zero). e is a little-endian integer of up to 64 bytes.
Block modexp(const Block& x, std::span<const std::uint8_t> e, const Block& n);
Block add_mod_2_512(const Block& a, const Block& b);  // KM_RSAAdjust: (a + b) truncated to 512 bits
bool less_than(const Block& a, const Block& b);

struct RsaKey {
    Block n{};  // modulus, little endian
    Block d{};  // private exponent (all zero for a public-only key)
    bool has_private() const;
};
// RSAEncrypt with e = 17: each whole 64-byte block of data in place (a short tail is left, as RSADecrypt does).
void rsa_encrypt17(const Block& n, std::span<std::uint8_t> data);
void rsa_decrypt(const RsaKey& key, std::span<std::uint8_t> data);

// Parses an operator's key file: "n=" and "d=" (and optionally "e=", which must be 17), each decimal or 0x-hex
// (big-endian digits as written), one per line; also the JSON shape {"n": "...", "e": "17", "d": "..."} of the
// Horizon server's settings. Throws std::runtime_error with the reason.
RsaKey parse_rsa_key(std::string_view text);
// pow(pow(m, 17, n), d, n) == m for a few m: the pair belongs together.
bool rsa_pair_consistent(const RsaKey& key);
// The 64 bytes KM_GetMediusPublicKey copies (D_00492BF0, EE address 0x00492bf0), read from the operator's own
// retail executable (SCUS_972.64, an ELF32 little-endian MIPS file); nullopt when the file is no such ELF or the
// address is in no loadable segment.
constexpr std::uint32_t kMediusPublicKeyAddress = 0x00492bf0;
std::optional<Block> read_disc_public_key(std::span<const std::uint8_t> elf);

// Decimal / 0x-hex text to a 64-byte little-endian integer (throws on overflow or a bad digit).
Block parse_big(std::string_view text);
std::string to_hex_be(const Block& b);  // for logs

}  // namespace omega::online::rtcrypto
