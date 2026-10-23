// Port-only (checkpoint 132md1, research/MODDING.md): PNG / DDS readers and a PNG writer for mod textures.
#include "image_files.hpp"
#include "mod_archive.hpp"
#include "texture_blocks.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace omega::mods {
namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t at) {
    if (at + 4 > b.size()) fail("Truncated image file");
    return std::uint32_t(b[at]) << 24 | std::uint32_t(b[at + 1]) << 16 | std::uint32_t(b[at + 2]) << 8 | b[at + 3];
}
std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    if (at + 4 > b.size()) fail("Truncated image file");
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8 | std::uint32_t(b[at + 2]) << 16 | std::uint32_t(b[at + 3]) << 24;
}

// RFC 1951 inflate. Port-only (checkpoint 132md8, research/MODDING.md "Speed"): the bits come from a 64-bit buffer and
// a code of up to fast_bits bits decodes by one table lookup (longer ones, and incomplete codes' gaps, by the canonical
// walk); the output is written in place. The same bytes and failures as the bit-by-bit reader it replaced.
class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}
    // At least min(n, the bits left) bits in the buffer (n <= 57).
    void refill() {
        while (count_ <= 56 && pos_ < data_.size()) {
            buffer_ |= std::uint64_t(data_[pos_++]) << count_;
            count_ += 8;
        }
    }
    unsigned available() const { return count_; }
    unsigned peek() const { return static_cast<unsigned>(buffer_); }
    void consume(unsigned n) {
        buffer_ >>= n;
        count_ -= n;
    }
    unsigned bit() { return bits(1); }
    unsigned bits(unsigned n) {
        if (n == 0) return 0;
        if (count_ < n) refill();
        if (count_ < n) fail("Truncated deflate stream");
        const auto v = static_cast<unsigned>(buffer_ & ((std::uint64_t(1) << n) - 1));
        consume(n);
        return v;
    }
    void align() { consume(count_ % 8); }
    std::uint8_t byte() { return static_cast<std::uint8_t>(bits(8)); }
    // `n` bytes after align() appended to `out`.
    void copy(std::vector<std::uint8_t>& out, std::size_t n) {
        for (; n && count_ >= 8; --n) out.push_back(byte());
        if (n > data_.size() - pos_) fail("Truncated deflate stream");
        out.insert(out.end(), data_.begin() + static_cast<std::ptrdiff_t>(pos_), data_.begin() + static_cast<std::ptrdiff_t>(pos_ + n));
        pos_ += n;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    std::uint64_t buffer_ = 0;
    unsigned count_ = 0;
};

constexpr unsigned fast_bits = 10;

struct Huffman {
    std::array<std::uint16_t, 16> counts{};
    std::vector<std::uint16_t> symbols;
    // Indexed by the next fast_bits input bits: (code length << 9) | symbol; 0: the walk decides.
    std::array<std::uint16_t, std::size_t(1) << fast_bits> fast{};
    void build(const std::uint8_t* lengths, std::size_t n) {
        counts.fill(0);
        for (std::size_t i = 0; i < n; ++i) ++counts[lengths[i]];
        counts[0] = 0;
        std::array<std::uint16_t, 16> offsets{};
        for (std::size_t i = 1; i < 16; ++i) offsets[i] = static_cast<std::uint16_t>(offsets[i - 1] + counts[i - 1]);
        symbols.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            if (lengths[i]) symbols[offsets[lengths[i]]++] = static_cast<std::uint16_t>(i);
        fast.fill(0);
        // The canonical codes, as the walk reads them; an over-subscribed set keeps the walk alone.
        int code = 0;
        std::size_t index = 0;
        for (unsigned len = 1; len < 16; ++len) {
            const int count = counts[len];
            if (code + count > (1 << len)) {
                fast.fill(0);
                return;
            }
            for (int k = 0; k < count; ++k, ++code, ++index)
                if (len <= fast_bits) {
                    unsigned reversed = 0;
                    for (unsigned b = 0; b < len; ++b) reversed |= ((static_cast<unsigned>(code) >> b) & 1U) << (len - 1 - b);
                    for (unsigned fill = reversed; fill < fast.size(); fill += 1U << len)
                        fast[fill] = static_cast<std::uint16_t>(len << 9 | symbols[index]);
                }
            code <<= 1;
        }
    }
    unsigned decode(BitReader& in) const {
        if (in.available() < fast_bits) in.refill();
        if (in.available() >= fast_bits) {
            const auto entry = fast[in.peek() & ((1U << fast_bits) - 1)];
            if (entry) {
                in.consume(entry >> 9);
                return entry & 0x1FFU;
            }
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(in.bit());
            const int count = counts[static_cast<std::size_t>(len)];
            if (code - count < first) return symbols.at(static_cast<std::size_t>(index + (code - first)));
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        fail("Invalid deflate Huffman code");
    }
};

constexpr std::uint16_t length_base[]{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t length_extra[]{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t distance_base[]{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t distance_extra[]{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void inflate_block(BitReader& in, std::vector<std::uint8_t>& out, const Huffman& lit, const Huffman& dist, std::size_t limit) {
    for (;;) {
        const unsigned symbol = lit.decode(in);
        if (symbol < 256) {
            if (out.size() >= limit) fail("Inflated data exceeds its limit");
            out.push_back(static_cast<std::uint8_t>(symbol));
        } else if (symbol == 256) {
            return;
        } else {
            const unsigned l = symbol - 257;
            if (l >= 29) fail("Invalid deflate length code");
            const std::size_t length = length_base[l] + in.bits(length_extra[l]);
            const unsigned d = dist.decode(in);
            if (d >= 30) fail("Invalid deflate distance code");
            const std::size_t distance = distance_base[d] + in.bits(distance_extra[d]);
            if (distance > out.size()) fail("Deflate distance before the output start");
            if (out.size() + length > limit) fail("Inflated data exceeds its limit");
            const std::size_t at = out.size();
            out.resize(at + length);
            std::uint8_t* to = out.data() + at;
            const std::uint8_t* from = to - distance;
            for (std::size_t i = 0; i < length; ++i) to[i] = from[i];   // overlapping: byte by byte
        }
    }
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            t[n] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (const auto b : bytes) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::vector<std::uint8_t> zlib_inflate(std::span<const std::uint8_t> stream, std::size_t limit) {
    if (stream.size() < 6) fail("Truncated zlib stream");
    if ((stream[0] & 0x0F) != 8 || ((unsigned(stream[0]) << 8 | stream[1]) % 31) != 0 || (stream[1] & 0x20))
        fail("Unsupported zlib header");
    return inflate_raw(stream.subspan(2), limit);
}

std::vector<std::uint8_t> inflate_raw(std::span<const std::uint8_t> stream, std::size_t limit) {
    BitReader in(stream);
    std::vector<std::uint8_t> out;
    out.reserve(std::min<std::size_t>(limit, std::size_t(64) << 20));
    for (bool last = false; !last;) {
        last = in.bit() != 0;
        const unsigned type = in.bits(2);
        if (type == 0) {
            in.align();
            const unsigned len = in.byte() | unsigned(in.byte()) << 8;
            const unsigned nlen = in.byte() | unsigned(in.byte()) << 8;
            if ((len ^ 0xFFFFU) != nlen) fail("Invalid stored deflate block");
            if (out.size() + len > limit) fail("Inflated data exceeds its limit");
            in.copy(out, len);
        } else if (type == 1) {
            static const auto fixed = [] {
                std::array<std::uint8_t, 288> l{};
                std::fill(l.begin(), l.begin() + 144, std::uint8_t(8));
                std::fill(l.begin() + 144, l.begin() + 256, std::uint8_t(9));
                std::fill(l.begin() + 256, l.begin() + 280, std::uint8_t(7));
                std::fill(l.begin() + 280, l.end(), std::uint8_t(8));
                std::array<std::uint8_t, 30> d{};
                d.fill(5);
                std::pair<Huffman, Huffman> h;
                h.first.build(l.data(), l.size());
                h.second.build(d.data(), d.size());
                return h;
            }();
            inflate_block(in, out, fixed.first, fixed.second, limit);
        } else if (type == 2) {
            const unsigned hlit = in.bits(5) + 257, hdist = in.bits(5) + 1, hclen = in.bits(4) + 4;
            static constexpr std::uint8_t order[]{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::array<std::uint8_t, 19> code_lengths{};
            for (unsigned i = 0; i < hclen; ++i) code_lengths[order[i]] = static_cast<std::uint8_t>(in.bits(3));
            Huffman code;
            code.build(code_lengths.data(), code_lengths.size());
            std::array<std::uint8_t, 320> lengths{};
            for (unsigned i = 0; i < hlit + hdist;) {
                const unsigned sym = code.decode(in);
                if (sym < 16) lengths[i++] = static_cast<std::uint8_t>(sym);
                else {
                    std::uint8_t value = 0;
                    unsigned repeat = 0;
                    if (sym == 16) {
                        if (i == 0) fail("Deflate repeat with no previous length");
                        value = lengths[i - 1];
                        repeat = 3 + in.bits(2);
                    } else if (sym == 17) repeat = 3 + in.bits(3);
                    else repeat = 11 + in.bits(7);
                    if (i + repeat > hlit + hdist) fail("Deflate code lengths overrun");
                    while (repeat--) lengths[i++] = value;
                }
            }
            Huffman lit, dist;
            lit.build(lengths.data(), hlit);
            dist.build(lengths.data() + hlit, hdist);
            inflate_block(in, out, lit, dist, limit);
        } else {
            fail("Invalid deflate block type");
        }
    }
    return out;
}

RgbaImage decode_png(std::span<const std::uint8_t> bytes) {
    static constexpr std::uint8_t signature[]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (bytes.size() < 8 || !std::equal(std::begin(signature), std::end(signature), bytes.begin())) fail("Not a PNG file");
    std::uint32_t width = 0, height = 0;
    unsigned depth = 0, colour = 0;
    bool have_header = false;
    std::vector<std::uint8_t> idat, palette, trns;
    for (std::size_t at = 8;;) {
        const auto length = be32(bytes, at);
        if (length > bytes.size() || at + 12 + std::size_t(length) > bytes.size()) fail("Truncated PNG chunk");
        const auto type = std::string(reinterpret_cast<const char*>(bytes.data() + at + 4), 4);
        const auto data = bytes.subspan(at + 8, length);
        if (crc32(bytes.subspan(at + 4, std::size_t(length) + 4)) != be32(bytes, at + 8 + length)) fail("PNG chunk CRC mismatch: " + type);
        if (type == "IHDR") {
            if (length != 13) fail("Invalid PNG header");
            width = be32(data, 0);
            height = be32(data, 4);
            depth = data[8];
            colour = data[9];
            if (data[10] != 0 || data[11] != 0) fail("Unsupported PNG compression or filter method");
            if (data[12] != 0) fail("Interlaced PNG is not supported; save it without interlacing");
            have_header = true;
        } else if (type == "PLTE") palette.assign(data.begin(), data.end());
        else if (type == "tRNS") trns.assign(data.begin(), data.end());
        else if (type == "IDAT") idat.insert(idat.end(), data.begin(), data.end());
        else if (type == "IEND") break;
        at += 12 + std::size_t(length);
    }
    if (!have_header) fail("PNG without IHDR");
    if (width == 0 || height == 0 || width > image_size_limit || height > image_size_limit)
        fail("PNG size " + std::to_string(width) + " x " + std::to_string(height) + " outside 1.." + std::to_string(image_size_limit));
    unsigned channels = 0;
    switch (colour) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: fail("Invalid PNG colour type");
    }
    const bool depth_ok = colour == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
        : colour == 0 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16) : (depth == 8 || depth == 16);
    if (!depth_ok) fail("Unsupported PNG bit depth");
    if (colour == 3 && (palette.empty() || palette.size() % 3)) fail("PNG palette missing");
    const std::size_t bits_per_pixel = std::size_t(channels) * depth;
    const std::size_t stride = (std::size_t(width) * bits_per_pixel + 7) / 8;
    const std::size_t bpp = std::max<std::size_t>(1, bits_per_pixel / 8);
    const auto raw = zlib_inflate(idat, (stride + 1) * height);
    if (raw.size() != (stride + 1) * height) fail("PNG image data size mismatch");
    std::vector<std::uint8_t> rows(stride * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto filter = raw[y * (stride + 1)];
        const std::uint8_t* in = raw.data() + y * (stride + 1) + 1;
        std::uint8_t* out = rows.data() + y * stride;
        const std::uint8_t* prior = y ? rows.data() + (y - 1) * stride : nullptr;
        // 132md8: one loop per filter (the bytes as before); the first row's prior is 0.
        const auto up = [&](std::size_t x) -> int { return prior ? prior[x] : 0; };
        const auto left = [&](std::size_t x) -> int { return x >= bpp ? out[x - bpp] : 0; };
        switch (filter) {
        case 0: std::memcpy(out, in, stride); break;
        case 1:
            for (std::size_t x = 0; x < stride; ++x) out[x] = static_cast<std::uint8_t>(in[x] + left(x));
            break;
        case 2:
            if (!prior) std::memcpy(out, in, stride);
            else for (std::size_t x = 0; x < stride; ++x) out[x] = static_cast<std::uint8_t>(in[x] + prior[x]);
            break;
        case 3:
            for (std::size_t x = 0; x < stride; ++x) out[x] = static_cast<std::uint8_t>(in[x] + (left(x) + up(x)) / 2);
            break;
        case 4:
            for (std::size_t x = 0; x < stride; ++x) {
                const int a = left(x), b = up(x), c = (prior && x >= bpp) ? prior[x - bpp] : 0;
                const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                out[x] = static_cast<std::uint8_t>(in[x] + ((pa <= pb && pa <= pc) ? a : pb <= pc ? b : c));
            }
            break;
        default: fail("Invalid PNG row filter");
        }
    }
    RgbaImage image{width, height, std::vector<std::uint8_t>(std::size_t(width) * height * 4)};
    const auto sample = [&](const std::uint8_t* row, std::size_t index) -> unsigned {
        // Sample `index` of a row (a channel value at the PNG depth, scaled to 8 bits for grey).
        if (depth == 8) return row[index];
        if (depth == 16) return row[index * 2];
        const std::size_t bit = index * depth;
        return (row[bit / 8] >> (8 - depth - bit % 8)) & ((1U << depth) - 1);
    };
    if (colour == 6 && depth == 8) {   // RGBA8: the rows are the image
        std::memcpy(image.rgba.data(), rows.data(), image.rgba.size());
        return image;
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* row = rows.data() + y * stride;
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* p = image.rgba.data() + (std::size_t(y) * width + x) * 4;
            if (colour == 3) {
                const auto i = sample(row, x);
                if (std::size_t(i) * 3 + 2 >= palette.size()) fail("PNG palette index out of range");
                p[0] = palette[i * 3];
                p[1] = palette[i * 3 + 1];
                p[2] = palette[i * 3 + 2];
                p[3] = i < trns.size() ? trns[i] : 255;
            } else if (colour == 0 || colour == 4) {
                unsigned g = sample(row, std::size_t(x) * channels);
                if (depth < 8) g = g * 255 / ((1U << depth) - 1);
                p[0] = p[1] = p[2] = static_cast<std::uint8_t>(g);
                p[3] = colour == 4 ? static_cast<std::uint8_t>(sample(row, std::size_t(x) * 2 + 1)) : 255;
                if (colour == 0 && trns.size() >= 2) {
                    const unsigned key = depth == 16 ? trns[0] : (unsigned(trns[0]) << 8 | trns[1]);
                    if (sample(row, x) == (depth == 16 ? key : key & ((1U << depth) - 1))) p[3] = 0;
                }
            } else {
                for (unsigned c = 0; c < 3; ++c) p[c] = static_cast<std::uint8_t>(sample(row, std::size_t(x) * channels + c));
                p[3] = colour == 6 ? static_cast<std::uint8_t>(sample(row, std::size_t(x) * 4 + 3)) : 255;
                if (colour == 2 && depth == 8 && trns.size() >= 6 && p[0] == trns[1] && p[1] == trns[3] && p[2] == trns[5]) p[3] = 0;
            }
        }
    }
    return image;
}

namespace {

constexpr std::uint32_t fourcc_dxt1 = 0x31545844U, fourcc_dxt5 = 0x35545844U, fourcc_dx10 = 0x30315844U;

// 8 / 16 for a BC1 / BC3 header (legacy fourcc or DX10 DXGI format), else 0. `bytes` holds at least the 128-byte header.
std::uint32_t dds_block_bytes(std::span<const std::uint8_t> bytes) {
    constexpr std::uint32_t DDPF_FOURCC = 0x4;
    if (!(le32(bytes, 80) & DDPF_FOURCC)) return 0;
    const auto fourcc = le32(bytes, 84);
    if (fourcc == fourcc_dxt1) return 8;
    if (fourcc == fourcc_dxt5) return 16;
    if (fourcc != fourcc_dx10 || bytes.size() < 132) return 0;
    const auto format = le32(bytes, 128);
    if (format == 71 || format == 72) return 8;    // DXGI_FORMAT_BC1_UNORM(_SRGB)
    if (format == 77 || format == 78) return 16;   // DXGI_FORMAT_BC3_UNORM(_SRGB)
    return 0;
}

}  // namespace

std::optional<BlockImage> dds_blocks(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "DDS ", 4) != 0) return std::nullopt;
    if (bytes.size() < 128) fail("Truncated DDS header");
    if (le32(bytes, 4) != 124) fail("Invalid DDS header size");
    const auto block_bytes = dds_block_bytes(bytes);
    if (!block_bytes) return std::nullopt;
    BlockImage image{le32(bytes, 16), le32(bytes, 12), block_bytes, {}};
    if (image.width == 0 || image.height == 0 || image.width > image_size_limit || image.height > image_size_limit)
        fail("DDS size outside 1.." + std::to_string(image_size_limit));
    constexpr std::uint32_t DDSD_MIPMAPCOUNT = 0x20000;
    std::uint32_t mips = (le32(bytes, 8) & DDSD_MIPMAPCOUNT) ? le32(bytes, 28) : 1;
    mips = std::max(1U, mips);
    std::size_t at = le32(bytes, 84) == fourcc_dx10 ? 148 : 128;
    for (std::uint32_t level = 0; level < mips; ++level) {
        const auto w = std::max(1U, image.width >> level), h = std::max(1U, image.height >> level);
        const auto size = gfx::block_level_bytes(w, h, block_bytes);
        if (bytes.size() < at || bytes.size() - at < size) fail("Truncated DDS image data");
        image.levels.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        if (w == 1 && h == 1) break;
    }
    return image;
}

std::vector<std::uint8_t> encode_dds(const BlockImage& image) {
    if (image.block_bytes != 8 && image.block_bytes != 16) fail("A DDS holds BC1 (8) or BC3 (16) blocks");
    if (image.width == 0 || image.height == 0 || image.levels.empty()) fail("Invalid image for DDS");
    std::vector<std::uint8_t> out(128);
    const auto put = [&](std::size_t at, std::uint32_t v) {
        for (int k = 0; k < 4; ++k) out[at + k] = static_cast<std::uint8_t>(v >> (8 * k));
    };
    std::memcpy(out.data(), "DDS ", 4);
    put(4, 124);
    // DDSD_CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT | LINEARSIZE
    put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000);
    put(12, image.height);
    put(16, image.width);
    put(20, static_cast<std::uint32_t>(gfx::block_level_bytes(image.width, image.height, image.block_bytes)));
    put(28, static_cast<std::uint32_t>(image.levels.size()));
    put(76, 32);
    put(80, 0x4);   // DDPF_FOURCC
    put(84, image.block_bytes == 8 ? fourcc_dxt1 : fourcc_dxt5);
    // DDSCAPS_TEXTURE, plus COMPLEX | MIPMAP with more than one level
    put(108, 0x1000 | (image.levels.size() > 1 ? 0x8 | 0x400000 : 0));
    for (std::size_t level = 0; level < image.levels.size(); ++level) {
        const auto w = std::max(1U, image.width >> level), h = std::max(1U, image.height >> level);
        if (image.levels[level].size() != gfx::block_level_bytes(w, h, image.block_bytes)) fail("DDS level size mismatch");
        out.insert(out.end(), image.levels[level].begin(), image.levels[level].end());
    }
    return out;
}

RgbaImage decode_dds(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 128 || std::memcmp(bytes.data(), "DDS ", 4) != 0) fail("Not a DDS file");
    if (const auto blocks = dds_blocks(bytes))
        return {blocks->width, blocks->height, gfx::decode_blocks(blocks->levels[0], blocks->width, blocks->height, blocks->block_bytes)};
    if (le32(bytes, 4) != 124) fail("Invalid DDS header size");
    const auto height = le32(bytes, 12), width = le32(bytes, 16);
    if (width == 0 || height == 0 || width > image_size_limit || height > image_size_limit)
        fail("DDS size outside 1.." + std::to_string(image_size_limit));
    const auto pf_flags = le32(bytes, 80), fourcc = le32(bytes, 84), bit_count = le32(bytes, 88);
    std::array<std::uint32_t, 4> masks{le32(bytes, 92), le32(bytes, 96), le32(bytes, 100), le32(bytes, 104)};
    std::size_t data = 128;
    constexpr std::uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40;
    if (pf_flags & DDPF_FOURCC) {
        if (fourcc != fourcc_dx10) fail("Unsupported DDS fourcc; use uncompressed 32-bit RGBA, BC1 (DXT1) or BC3 (DXT5)");
        if (bytes.size() < 148) fail("Truncated DDS DX10 header");
        const auto format = le32(bytes, 128);
        if (format == 28 || format == 29) masks = {0x000000FFU, 0x0000FF00U, 0x00FF0000U, 0xFF000000U};
        else if (format == 87 || format == 91) masks = {0x00FF0000U, 0x0000FF00U, 0x000000FFU, 0xFF000000U};
        else fail("Unsupported DDS DXGI format " + std::to_string(format) + "; use R8G8B8A8_UNORM, B8G8R8A8_UNORM, BC1_UNORM or BC3_UNORM");
        data = 148;
    } else {
        if (!(pf_flags & DDPF_RGB) || bit_count != 32) fail("Unsupported DDS pixel format; use uncompressed 32-bit RGBA");
        if (!(pf_flags & DDPF_ALPHAPIXELS)) masks[3] = 0;
    }
    const std::size_t size = std::size_t(width) * height * 4;
    if (bytes.size() < data + size) fail("Truncated DDS image data");
    RgbaImage image{width, height, std::vector<std::uint8_t>(size)};
    const auto channel = [](std::uint32_t pixel, std::uint32_t mask) -> std::uint8_t {
        if (!mask) return 255;
        unsigned shift = 0;
        while (!((mask >> shift) & 1U)) ++shift;
        const std::uint32_t max = mask >> shift;
        return static_cast<std::uint8_t>(((pixel & mask) >> shift) * 255U / max);
    };
    for (std::size_t i = 0; i < std::size_t(width) * height; ++i) {
        const auto pixel = le32(bytes, data + i * 4);
        for (std::size_t c = 0; c < 4; ++c) image.rgba[i * 4 + c] = channel(pixel, masks[c]);
    }
    return image;
}

RgbaImage decode_image(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0) return decode_dds(bytes);
    return decode_png(bytes);
}

std::vector<std::uint8_t> read_image_bytes(const std::filesystem::path& path) {
    if (auto archived = read_archived(path)) return std::move(*archived);
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail("Cannot open image " + path.string());
    // One read of the whole file (132md7: a byte-wise stream copy cost about half the PNG decode).
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(in.tellg()));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        fail("Cannot read image " + path.string());
    return bytes;
}

RgbaImage read_image_file(const std::filesystem::path& path) { return decode_image(read_image_bytes(path)); }

ImageSize image_size(std::span<const std::uint8_t> header) {
    static constexpr std::uint8_t png[]{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (header.size() >= 4 && std::memcmp(header.data(), "DDS ", 4) == 0) {
        if (header.size() < 20) fail("Truncated DDS header");
        return {le32(header, 16), le32(header, 12), header.size() >= 128 ? dds_block_bytes(header) : 0};
    }
    if (header.size() >= 8 && std::equal(std::begin(png), std::end(png), header.begin())) {
        if (header.size() < 24 || std::memcmp(header.data() + 12, "IHDR", 4) != 0) fail("PNG without IHDR first");
        return {be32(header, 16), be32(header, 20)};
    }
    fail("Not a PNG or DDS file");
}

ImageSize read_image_size(const std::filesystem::path& path) {
    std::string name;
    if (const auto archive = archive_holding(path, name)) {
        const auto* member = archive->find(name);
        if (!member) fail("Cannot open image " + path.string());
        return image_size(archive->read_prefix(*member, 148));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) fail("Cannot open image " + path.string());
    std::uint8_t header[148]{};
    in.read(reinterpret_cast<char*>(header), sizeof header);
    return image_size(std::span<const std::uint8_t>(header, static_cast<std::size_t>(in.gcount())));
}

std::vector<std::uint8_t> encode_png(const RgbaImage& image) {
    if (image.width == 0 || image.height == 0 || image.rgba.size() != std::size_t(image.width) * image.height * 4)
        fail("Invalid image for PNG");
    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto put32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
        for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<std::uint8_t>(x >> s));
    };
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        put32(out, static_cast<std::uint32_t>(data.size()));
        const auto start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        put32(out, crc32(std::span(out).subspan(start)));
    };
    std::vector<std::uint8_t> header;
    put32(header, image.width);
    put32(header, image.height);
    header.insert(header.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", header);
    std::vector<std::uint8_t> raw;
    raw.reserve((std::size_t(image.width) * 4 + 1) * image.height);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        raw.push_back(0);
        const auto row = image.rgba.begin() + static_cast<std::ptrdiff_t>(std::size_t(y) * image.width * 4);
        raw.insert(raw.end(), row, row + static_cast<std::ptrdiff_t>(image.width) * 4);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t at = 0; at < raw.size() || at == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        if (at == raw.size()) break;
    }
    std::uint32_t a = 1, b = 0;
    for (const auto byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

void write_png(const std::filesystem::path& path, const RgbaImage& image) {
    const auto bytes = encode_png(image);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) fail("Cannot create " + path.string());
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) fail("Cannot write " + path.string());
}

}  // namespace omega::mods
