// PORT-ONLY (checkpoint 132on1): see online_hash.hpp.
#include "online_hash.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>

namespace omega::online {
namespace {

constexpr std::uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint8_t block[64] = {};
    std::size_t used = 0;
    std::uint64_t bits = 0;

    void compress() {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t{block[i * 4]} << 24) | (std::uint32_t{block[i * 4 + 1]} << 16) |
                   (std::uint32_t{block[i * 4 + 2]} << 8) | std::uint32_t{block[i * 4 + 3]};
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kK[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(std::span<const std::uint8_t> data) {
        for (std::uint8_t byte : data) {
            block[used++] = byte;
            if (used == 64) {
                compress();
                used = 0;
            }
        }
        bits += std::uint64_t{data.size()} * 8;
    }
    Sha256Digest finish() {
        const std::uint64_t total = bits;
        const std::uint8_t pad = 0x80;
        update({&pad, 1});
        const std::uint8_t zero = 0;
        while (used != 56) update({&zero, 1});
        std::uint8_t len[8];
        for (int i = 0; i < 8; ++i) len[i] = static_cast<std::uint8_t>(total >> (56 - 8 * i));
        update(len);
        Sha256Digest out{};
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
        return out;
    }
};

}  // namespace

Sha256Digest sha256(std::span<const std::uint8_t> data) {
    Sha256 s;
    s.update(data);
    return s.finish();
}

Sha256Digest sha256(std::string_view text) {
    return sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

Sha256Digest hmac_sha256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    std::uint8_t k[64] = {};
    if (key.size() > 64) {
        const Sha256Digest d = sha256(key);
        std::copy(d.begin(), d.end(), k);
    } else {
        std::copy(key.begin(), key.end(), k);
    }
    std::uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
    }
    Sha256 inner;
    inner.update(ipad);
    inner.update(data);
    const Sha256Digest ih = inner.finish();
    Sha256 outer;
    outer.update(opad);
    outer.update(ih);
    return outer.finish();
}

std::vector<std::uint8_t> pbkdf2_sha256(std::string_view password, std::span<const std::uint8_t> salt,
                                        std::uint32_t iterations, std::size_t length) {
    if (iterations == 0) throw std::runtime_error("pbkdf2: zero iterations");
    const std::span<const std::uint8_t> key(reinterpret_cast<const std::uint8_t*>(password.data()), password.size());
    // 132on2h (the load test's hot spot): the HMAC key pads are hashed once, and each later iteration is two
    // single-block compressions on a prepared block (the 32-byte U, the padding and the 96-byte length) instead of
    // two full HMACs (four compressions plus the byte-wise padding). The result is the same (RFC 6070 / 7914 vectors).
    std::uint8_t k[64] = {};
    if (key.size() > 64) {
        const Sha256Digest d = sha256(key);
        std::copy(d.begin(), d.end(), k);
    } else {
        std::copy(key.begin(), key.end(), k);
    }
    Sha256 inner_base, outer_base;
    for (int i = 0; i < 64; ++i) {
        inner_base.block[i] = static_cast<std::uint8_t>(k[i] ^ 0x36);
        outer_base.block[i] = static_cast<std::uint8_t>(k[i] ^ 0x5c);
    }
    inner_base.compress();
    outer_base.compress();
    inner_base.bits = outer_base.bits = 512;
    // The second block of HMAC(key, 32-byte U): U, 0x80, zeros, the bit length 64*8 + 32*8 = 768.
    std::uint8_t tail[64] = {};
    tail[32] = 0x80;
    tail[62] = 0x03;
    auto one_block = [&](const Sha256& base, const std::uint8_t* u32, std::uint8_t* out32) {
        Sha256 s = base;
        std::copy(u32, u32 + 32, s.block);
        std::copy(tail + 32, tail + 64, s.block + 32);
        s.compress();
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out32[i * 4 + j] = static_cast<std::uint8_t>(s.h[i] >> (24 - 8 * j));
    };
    std::vector<std::uint8_t> out;
    for (std::uint32_t block = 1; out.size() < length; ++block) {
        std::vector<std::uint8_t> msg(salt.begin(), salt.end());
        msg.push_back(static_cast<std::uint8_t>(block >> 24));
        msg.push_back(static_cast<std::uint8_t>(block >> 16));
        msg.push_back(static_cast<std::uint8_t>(block >> 8));
        msg.push_back(static_cast<std::uint8_t>(block));
        Sha256 first = inner_base;
        first.update(msg);
        const Sha256Digest ih = first.finish();
        Sha256 first_outer = outer_base;
        first_outer.update(ih);
        Sha256Digest u = first_outer.finish();
        Sha256Digest t = u;
        std::uint8_t mid[32];
        for (std::uint32_t i = 1; i < iterations; ++i) {
            one_block(inner_base, u.data(), mid);
            one_block(outer_base, mid, u.data());
            for (std::size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        for (std::size_t j = 0; j < t.size() && out.size() < length; ++j) out.push_back(t[j]);
    }
    return out;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

std::vector<std::uint8_t> from_hex(std::string_view text) {
    if (text.size() % 2) throw std::runtime_error("hex: odd length");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw std::runtime_error("hex: bad digit");
    };
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < text.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>(nibble(text[i]) * 16 + nibble(text[i + 1])));
    return out;
}

bool equal_digest(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

std::vector<std::uint8_t> random_bytes(std::size_t n) {
    std::random_device rd;
    std::vector<std::uint8_t> out(n);
    for (auto& b : out) b = static_cast<std::uint8_t>(rd() & 0xff);
    return out;
}

}  // namespace omega::online
