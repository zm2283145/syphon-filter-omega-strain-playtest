// PORT-ONLY (checkpoint 134on5): see online_rt_crypto.hpp (research/ONLINE.md §8.7).
#include "online_rt_crypto.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>
#include <stdexcept>

namespace omega::online::rtcrypto {

// ---- SHA-1 (FIPS 180-4) -----------------------------------------------------------------------------------------

namespace {
inline std::uint32_t rol(std::uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }
}  // namespace

Sha1Digest sha1(std::span<const std::uint8_t> data) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<std::uint8_t> m(data.begin(), data.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    for (std::size_t off = 0; off < m.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<std::uint32_t>(m[off + 4 * i]) << 24 | static_cast<std::uint32_t>(m[off + 4 * i + 1]) << 16 |
                   static_cast<std::uint32_t>(m[off + 4 * i + 2]) << 8 | m[off + 4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else f = b ^ c ^ d, k = 0xCA62C1D6u;
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    Sha1Digest out{};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[4 * i + j] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * j));
    return out;
}

std::uint32_t rt_hash(std::span<const std::uint8_t> plain, std::uint8_t context) {
    const auto d = sha1(plain);
    const std::uint32_t v = d[0] | static_cast<std::uint32_t>(d[1]) << 8 | static_cast<std::uint32_t>(d[2]) << 16 |
                            static_cast<std::uint32_t>(d[3]) << 24;
    return (v & 0x1FFFFFFFu) | static_cast<std::uint32_t>(context & 7) << 29;
}

// ---- the RC cipher (RCSaltyInitialize / RCEncrypt / RCDecrypt) -------------------------------------------------

namespace {
struct RcState {
    std::uint8_t s[256];
    std::uint8_t i = 0, j = 0;
};

void rc_salty_init(RcState& st, const Block& key, std::uint32_t salt) {
    std::uint8_t* S = st.s;
    std::uint8_t i = 0;
    do {
        S[i] = static_cast<std::uint8_t>(~i);
        ++i;
    } while (i != 0);
    const std::uint8_t k4[4] = {static_cast<std::uint8_t>(salt), static_cast<std::uint8_t>(salt >> 8),
                                static_cast<std::uint8_t>(salt >> 16), static_cast<std::uint8_t>(salt >> 24)};
    std::uint8_t j = 0, n = 0;
    do {
        j = static_cast<std::uint8_t>(j + k4[n] + S[i]);
        n = (n + 1) & 3;
        std::swap(S[i], S[j]);
        i = static_cast<std::uint8_t>(i + 5);
    } while (i != 0);
    j = 0, n = 0;
    do {
        j = static_cast<std::uint8_t>(j + key[n] + S[i]);
        n = (n + 1) & 0x3F;
        std::swap(S[i], S[j]);
        i = static_cast<std::uint8_t>(i + 3);
    } while (i != 0);
    st.i = 0, st.j = 0;
}
}  // namespace

void rc_encrypt(const Block& key, std::uint32_t salt, std::span<std::uint8_t> data) {
    RcState st;
    rc_salty_init(st, key, salt);
    std::uint8_t i = st.i, j = st.j;
    for (auto& byte : data) {
        i = static_cast<std::uint8_t>(i + 5);
        j = static_cast<std::uint8_t>(j + st.s[i]);
        const std::uint8_t t = st.s[i];
        st.s[i] = st.s[j];
        st.s[j] = t;
        const std::uint8_t d = byte;
        const std::uint8_t x = st.s[static_cast<std::uint8_t>(st.s[i] + t)];
        j = static_cast<std::uint8_t>(j + st.s[d]);
        byte = d ^ x;
    }
}

void rc_decrypt(const Block& key, std::uint32_t salt, std::span<std::uint8_t> data) {
    RcState st;
    rc_salty_init(st, key, salt);
    std::uint8_t i = st.i, j = st.j;
    for (auto& byte : data) {
        i = static_cast<std::uint8_t>(i + 5);
        j = static_cast<std::uint8_t>(j + st.s[i]);
        const std::uint8_t t = st.s[i];
        st.s[i] = st.s[j];
        st.s[j] = t;
        byte ^= st.s[static_cast<std::uint8_t>(st.s[i] + t)];
        j = static_cast<std::uint8_t>(j + st.s[byte]);
    }
}

// ---- 512-bit arithmetic (Montgomery, 16 x 32-bit limbs) --------------------------------------------------------

namespace {
constexpr int L = 16;
using Limbs = std::array<std::uint32_t, L + 1>;  // one spare limb for the shift-subtract reduction

Limbs to_limbs(const Block& b) {
    Limbs r{};
    for (int i = 0; i < L; ++i)
        r[i] = b[4 * i] | static_cast<std::uint32_t>(b[4 * i + 1]) << 8 | static_cast<std::uint32_t>(b[4 * i + 2]) << 16 |
               static_cast<std::uint32_t>(b[4 * i + 3]) << 24;
    return r;
}
Block to_block(const Limbs& r) {
    Block b{};
    for (int i = 0; i < L; ++i)
        for (int k = 0; k < 4; ++k) b[4 * i + k] = static_cast<std::uint8_t>(r[i] >> (8 * k));
    return b;
}
bool geq(const Limbs& a, const Limbs& b) {
    for (int i = L; i >= 0; --i)
        if (a[i] != b[i]) return a[i] > b[i];
    return true;
}
void sub_in(Limbs& a, const Limbs& b) {
    std::uint64_t borrow = 0;
    for (int i = 0; i <= L; ++i) {
        const std::uint64_t d = static_cast<std::uint64_t>(a[i]) - b[i] - borrow;
        a[i] = static_cast<std::uint32_t>(d);
        borrow = (d >> 32) & 1;
    }
}
void shl1_in(Limbs& a, std::uint32_t bit) {
    for (int i = L; i > 0; --i) a[i] = (a[i] << 1) | (a[i - 1] >> 31);
    a[0] = (a[0] << 1) | bit;
}
bool is_zero(const Limbs& a) {
    return std::all_of(a.begin(), a.end(), [](std::uint32_t v) { return v == 0; });
}

struct Mont {
    Limbs n{};
    std::uint32_t n0inv = 0;  // -n^-1 mod 2^32
    Limbs r2{};               // 2^1024 mod n

    explicit Mont(const Limbs& modulus) : n(modulus) {
        std::uint32_t inv = 1;  // Newton: inv = n0^-1 mod 2^32
        for (int k = 0; k < 5; ++k) inv *= 2u - n[0] * inv;
        n0inv = 0u - inv;
        Limbs r{};
        r[0] = 1;
        for (int k = 0; k < 2 * 32 * L; ++k) {
            shl1_in(r, 0);
            if (geq(r, n)) sub_in(r, n);
        }
        r2 = r;
    }
    Limbs reduce(const Limbs& x) const {  // x mod n, bit-serial
        Limbs r{};
        for (int bit = 32 * L - 1; bit >= 0; --bit) {
            shl1_in(r, (x[bit / 32] >> (bit % 32)) & 1);
            if (geq(r, n)) sub_in(r, n);
        }
        return r;
    }
    Limbs mul(const Limbs& a, const Limbs& b) const {  // a*b/R mod n (CIOS), a, b < n
        std::uint32_t t[L + 2] = {};
        for (int i = 0; i < L; ++i) {
            std::uint64_t c = 0;
            for (int j = 0; j < L; ++j) {
                const std::uint64_t v = static_cast<std::uint64_t>(a[j]) * b[i] + t[j] + c;
                t[j] = static_cast<std::uint32_t>(v);
                c = v >> 32;
            }
            std::uint64_t v = static_cast<std::uint64_t>(t[L]) + c;
            t[L] = static_cast<std::uint32_t>(v);
            t[L + 1] = static_cast<std::uint32_t>(v >> 32);
            const std::uint32_t m = t[0] * n0inv;
            v = static_cast<std::uint64_t>(m) * n[0] + t[0];
            c = v >> 32;
            for (int j = 1; j < L; ++j) {
                v = static_cast<std::uint64_t>(m) * n[j] + t[j] + c;
                t[j - 1] = static_cast<std::uint32_t>(v);
                c = v >> 32;
            }
            v = static_cast<std::uint64_t>(t[L]) + c;
            t[L - 1] = static_cast<std::uint32_t>(v);
            t[L] = t[L + 1] + static_cast<std::uint32_t>(v >> 32);
        }
        Limbs r{};
        for (int k = 0; k <= L; ++k) r[k] = t[k];
        if (geq(r, n)) sub_in(r, n);
        return r;
    }
};
}  // namespace

Block modexp(const Block& x, std::span<const std::uint8_t> e, const Block& n) {
    const Limbs nl = to_limbs(n);
    if (!(nl[0] & 1)) throw std::runtime_error("RSA modulus is even");
    const Mont m(nl);
    const Limbs xm = m.mul(m.reduce(to_limbs(x)), m.r2);
    Limbs one{};
    one[0] = 1;
    Limbs acc = m.mul(one, m.r2);  // R mod n
    int top = static_cast<int>(e.size()) * 8 - 1;
    while (top >= 0 && !((e[static_cast<std::size_t>(top / 8)] >> (top % 8)) & 1)) --top;
    for (int bit = top; bit >= 0; --bit) {
        acc = m.mul(acc, acc);
        if ((e[static_cast<std::size_t>(bit / 8)] >> (bit % 8)) & 1) acc = m.mul(acc, xm);
    }
    return to_block(m.mul(acc, one));
}

Block add_mod_2_512(const Block& a, const Block& b) {
    Block r{};
    unsigned carry = 0;
    for (std::size_t i = 0; i < kBlock; ++i) {
        const unsigned v = static_cast<unsigned>(a[i]) + b[i] + carry;
        r[i] = static_cast<std::uint8_t>(v);
        carry = v >> 8;
    }
    return r;
}

bool less_than(const Block& a, const Block& b) {
    for (std::size_t i = kBlock; i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i];
    return false;
}

bool RsaKey::has_private() const {
    return std::any_of(d.begin(), d.end(), [](std::uint8_t v) { return v != 0; });
}

void rsa_encrypt17(const Block& n, std::span<std::uint8_t> data) {
    const std::uint8_t e[1] = {17};
    for (std::size_t off = 0; off + kBlock <= data.size(); off += kBlock) {
        Block x;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(off), kBlock, x.begin());
        const Block y = modexp(x, e, n);
        std::copy(y.begin(), y.end(), data.begin() + static_cast<std::ptrdiff_t>(off));
    }
}

void rsa_decrypt(const RsaKey& key, std::span<std::uint8_t> data) {
    for (std::size_t off = 0; off + kBlock <= data.size(); off += kBlock) {
        Block x;
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(off), kBlock, x.begin());
        const Block y = modexp(x, key.d, key.n);
        std::copy(y.begin(), y.end(), data.begin() + static_cast<std::ptrdiff_t>(off));
    }
}

Block parse_big(std::string_view text) {
    Limbs r{};
    auto mul_add = [&](std::uint32_t mul, std::uint32_t add) {
        std::uint64_t c = add;
        for (int i = 0; i <= L; ++i) {
            const std::uint64_t v = static_cast<std::uint64_t>(r[i]) * mul + c;
            r[i] = static_cast<std::uint32_t>(v);
            c = v >> 32;
        }
        if (c || r[L]) throw std::runtime_error("number larger than 512 bits");
    };
    bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hex) text.remove_prefix(2);
    if (text.empty()) throw std::runtime_error("empty number");
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (hex && std::isxdigit(c)) mul_add(16, std::isdigit(c) ? c - '0' : (std::tolower(c) - 'a' + 10));
        else if (!hex && std::isdigit(c)) mul_add(10, c - '0');
        else throw std::runtime_error(std::string("bad digit '") + ch + "'");
    }
    return to_block(r);
}

std::string to_hex_be(const Block& b) {
    std::string s;
    char buf[3];
    for (std::size_t i = kBlock; i-- > 0;) {
        std::snprintf(buf, sizeof buf, "%02x", b[i]);
        s += buf;
    }
    return s;
}

RsaKey parse_rsa_key(std::string_view text) {
    static const std::regex field("\"?\\b([nde])\"?\\s*[:=]\\s*\"?(0[xX][0-9a-fA-F]+|[0-9]+)\"?");
    RsaKey key;
    bool have_n = false, have_d = false;
    const std::string s(text);
    for (auto it = std::sregex_iterator(s.begin(), s.end(), field); it != std::sregex_iterator(); ++it) {
        const std::string name = (*it)[1], value = (*it)[2];
        const Block v = parse_big(value);
        if (name == "n") key.n = v, have_n = true;
        else if (name == "d") key.d = v, have_d = true;
        else {
            Block seventeen{};
            seventeen[0] = 17;
            if (v != seventeen) throw std::runtime_error("e is " + value + "; the disc's RSA uses e = 17");
        }
    }
    if (!have_n) throw std::runtime_error("no n= (the modulus) in the key file");
    if (!have_d) throw std::runtime_error("no d= (the private exponent) in the key file");
    if (!(key.n[0] & 1)) throw std::runtime_error("n is even");
    return key;
}

bool rsa_pair_consistent(const RsaKey& key) {
    if (!(key.n[0] & 1) || !key.has_private()) return false;
    const std::uint8_t e[1] = {17};
    for (const unsigned seed : {2u, 3u, 0x5au}) {
        Block m{};
        for (std::size_t i = 0; i + 1 < kBlock; ++i) m[i] = static_cast<std::uint8_t>(seed * (i + 7) + i * i);
        m[kBlock - 1] = 0;  // below any 512-bit modulus
        if (modexp(modexp(m, e, key.n), key.d, key.n) != m) return false;
    }
    return true;
}

std::optional<Block> read_disc_public_key(std::span<const std::uint8_t> elf) {
    auto u16 = [&](std::size_t o) { return static_cast<std::uint32_t>(elf[o] | elf[o + 1] << 8); };
    auto u32 = [&](std::size_t o) {
        return elf[o] | static_cast<std::uint32_t>(elf[o + 1]) << 8 | static_cast<std::uint32_t>(elf[o + 2]) << 16 |
               static_cast<std::uint32_t>(elf[o + 3]) << 24;
    };
    if (elf.size() < 0x34 || elf[0] != 0x7f || elf[1] != 'E' || elf[2] != 'L' || elf[3] != 'F' || elf[4] != 1 || elf[5] != 1)
        return std::nullopt;
    const std::uint32_t phoff = u32(0x1c), phentsize = u16(0x2a), phnum = u16(0x2c);
    for (std::uint32_t k = 0; k < phnum; ++k) {
        const std::size_t ph = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(k) * phentsize;
        if (phentsize < 0x20 || ph + 0x20 > elf.size()) return std::nullopt;
        if (u32(ph) != 1) continue;  // PT_LOAD
        const std::uint32_t offset = u32(ph + 4), vaddr = u32(ph + 8), filesz = u32(ph + 16);
        if (kMediusPublicKeyAddress < vaddr || kMediusPublicKeyAddress + kBlock > static_cast<std::uint64_t>(vaddr) + filesz)
            continue;
        const std::size_t at = static_cast<std::size_t>(offset) + (kMediusPublicKeyAddress - vaddr);
        if (at + kBlock > elf.size()) return std::nullopt;
        Block b;
        std::copy_n(elf.begin() + static_cast<std::ptrdiff_t>(at), kBlock, b.begin());
        return b;
    }
    return std::nullopt;
}

}  // namespace omega::online::rtcrypto
