#include "audio_files.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace omega::mods {
namespace {

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

std::uint32_t u16le(std::span<const std::uint8_t> b, std::size_t at) { return b[at] | (b[at + 1] << 8); }
std::uint32_t u32le(std::span<const std::uint8_t> b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (std::uint32_t(b[at + 3]) << 24);
}

// ---------------------------------------------------------------------------------------------------------------
// WAV

float wav_sample(std::span<const std::uint8_t> b, std::size_t at, std::uint32_t format, std::uint32_t bits) {
    if (format == 3) {
        if (bits == 32) {
            float f;
            const std::uint32_t v = u32le(b, at);
            std::memcpy(&f, &v, 4);
            return f;
        }
        double d;
        const std::uint64_t v = u32le(b, at) | (std::uint64_t(u32le(b, at + 4)) << 32);
        std::memcpy(&d, &v, 8);
        return static_cast<float>(d);
    }
    switch (bits) {
    case 8: return (static_cast<int>(b[at]) - 128) / 128.0F;
    case 16: return static_cast<std::int16_t>(u16le(b, at)) / 32768.0F;
    case 24: {
        const std::int32_t v = static_cast<std::int32_t>((b[at] << 8) | (b[at + 1] << 16) | (std::uint32_t(b[at + 2]) << 24)) >> 8;
        return v / 8388608.0F;
    }
    default: return static_cast<float>(static_cast<std::int32_t>(u32le(b, at)) / 2147483648.0);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Ogg

struct Packet {
    std::vector<std::uint8_t> bytes;
    std::int64_t granule = -1;   // the page's granule position when the packet is the last one ending on it
    bool eos = false;
    bool first_granule_page = false;
};

std::uint32_t ogg_crc(std::span<const std::uint8_t> page) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t r = i << 24;
            for (int k = 0; k < 8; ++k) r = (r & 0x80000000U) ? (r << 1) ^ 0x04c11db7U : r << 1;
            t[i] = r;
        }
        return t;
    }();
    std::uint32_t crc = 0;
    for (std::size_t i = 0; i < page.size(); ++i) {
        const std::uint8_t byte = (i >= 22 && i < 26) ? 0 : page[i];
        crc = (crc << 8) ^ table[((crc >> 24) ^ byte) & 0xff];
    }
    return crc;
}

std::vector<Packet> ogg_packets(std::span<const std::uint8_t> bytes) {
    std::vector<Packet> packets;
    std::optional<std::uint32_t> serial;
    std::vector<std::uint8_t> partial;
    bool seen_granule = false;
    std::size_t at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 27 || std::memcmp(bytes.data() + at, "OggS", 4) != 0) fail("Ogg: no page at byte " + std::to_string(at));
        if (bytes[at + 4] != 0) fail("Ogg: unknown page version");
        const auto type = bytes[at + 5];
        const std::int64_t granule = static_cast<std::int64_t>(u32le(bytes, at + 6) | (std::uint64_t(u32le(bytes, at + 10)) << 32));
        const auto page_serial = u32le(bytes, at + 14);
        const std::size_t segments = bytes[at + 26];
        if (bytes.size() - at < 27 + segments) fail("Ogg: truncated page");
        std::size_t body = 0;
        for (std::size_t i = 0; i < segments; ++i) body += bytes[at + 27 + i];
        const std::size_t size = 27 + segments + body;
        if (bytes.size() - at < size) fail("Ogg: truncated page");
        const auto page = bytes.subspan(at, size);
        if (ogg_crc(page) != u32le(bytes, at + 22)) fail("Ogg: page CRC mismatch at byte " + std::to_string(at));
        at += size;
        if (!serial) {
            if (!(type & 2)) fail("Ogg: the first page does not begin a stream");
            serial = page_serial;
        }
        if (page_serial != *serial) continue;   // another logical stream: not ours
        if (!(type & 1)) partial.clear();       // a fresh packet starts on this page
        std::size_t offset = 27 + segments;
        std::optional<std::size_t> last_complete;
        for (std::size_t i = 0; i < segments; ++i) {
            const auto lacing = page[27 + i];
            partial.insert(partial.end(), page.begin() + static_cast<std::ptrdiff_t>(offset),
                           page.begin() + static_cast<std::ptrdiff_t>(offset + lacing));
            offset += lacing;
            if (lacing < 255) {
                packets.push_back({std::move(partial), -1, false, false});
                partial.clear();
                last_complete = packets.size() - 1;
            }
        }
        if (last_complete && granule != -1) {
            packets[*last_complete].granule = granule;
            packets[*last_complete].eos = (type & 4) != 0;
            packets[*last_complete].first_granule_page = !seen_granule;
            seen_granule = true;
        }
        if (type & 4) break;
    }
    return packets;
}

// ---------------------------------------------------------------------------------------------------------------
// Vorbis I (the Xiph.Org Vorbis I specification)

class Bits {
public:
    explicit Bits(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint32_t read(int count) {
        std::uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            if (pos_ >= bytes_.size() * 8) {
                eop_ = true;
                return 0;
            }
            value |= std::uint32_t((bytes_[pos_ >> 3] >> (pos_ & 7)) & 1) << i;
            ++pos_;
        }
        return value;
    }
    bool bit() { return read(1) != 0; }
    bool eop() const { return eop_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t pos_ = 0;
    bool eop_ = false;
};

int ilog(std::uint32_t x) {
    int n = 0;
    while (x) {
        ++n;
        x >>= 1;
    }
    return n;
}

float float32_unpack(std::uint32_t x) {
    double mantissa = x & 0x1fffff;
    const int exponent = static_cast<int>((x & 0x7fe00000U) >> 21);
    if (x & 0x80000000U) mantissa = -mantissa;
    return static_cast<float>(std::ldexp(mantissa, exponent - 788));
}

std::uint32_t lookup1_values(std::uint32_t entries, std::uint32_t dimensions) {
    auto r = static_cast<std::uint32_t>(std::floor(std::exp(std::log(double(entries)) / dimensions)));
    const auto power = [&](std::uint64_t base) {
        std::uint64_t p = 1;
        for (std::uint32_t i = 0; i < dimensions && p <= entries; ++i) p *= base;
        return p;
    };
    while (r > 0 && power(r) > entries) --r;
    while (power(std::uint64_t(r) + 1) <= entries) ++r;
    return r;
}

struct Codebook {
    std::uint32_t dimensions = 0, entries = 0;
    std::vector<std::array<std::int32_t, 2>> tree;   // node -> child per bit; >= 0 a node, < 0 the leaf -(entry + 1)
    std::vector<float> vectors;                       // entries * dimensions (lookup type 1 / 2), else empty

    // The entry number, or -1 at the end of the packet / an invalid codeword.
    int scalar(Bits& bits) const {
        if (tree.empty()) return -1;
        std::int32_t node = 0;
        for (;;) {
            const auto b = bits.read(1);
            if (bits.eop()) return -1;
            node = tree[static_cast<std::size_t>(node)][b];
            if (node == 0) return -1;
            if (node < 0) return -node - 1;
        }
    }
    const float* vector(Bits& bits) const {
        const int entry = scalar(bits);
        if (entry < 0 || vectors.empty()) return nullptr;
        return vectors.data() + std::size_t(entry) * dimensions;
    }
};

void build_tree(Codebook& book, const std::vector<std::uint8_t>& lengths) {
    // Codewords in entry order, each the lowest available of its length (MSB first): the specification's assignment.
    std::array<std::uint32_t, 33> available{};
    bool first = true;
    std::size_t used = 0;
    book.tree.assign(1, {0, 0});
    const auto insert = [&](std::uint32_t code, int length, std::uint32_t entry) {
        std::int32_t node = 0;
        for (int i = 0; i < length; ++i) {
            const auto b = (code >> (31 - i)) & 1;
            auto& child = book.tree[static_cast<std::size_t>(node)][b];
            if (i == length - 1) {
                if (child != 0) fail("Vorbis: overlapping Huffman codewords");
                child = -static_cast<std::int32_t>(entry) - 1;
                return;
            }
            if (child < 0) fail("Vorbis: overlapping Huffman codewords");
            if (child == 0) {
                child = static_cast<std::int32_t>(book.tree.size());
                book.tree.push_back({0, 0});
            }
            node = book.tree[static_cast<std::size_t>(node)][b];
        }
    };
    for (std::uint32_t entry = 0; entry < lengths.size(); ++entry) {
        const int length = lengths[entry];
        if (!length) continue;
        ++used;
        if (first) {
            first = false;
            insert(0, length, entry);
            for (int i = 1; i <= length; ++i) available[std::size_t(i)] = 1U << (32 - i);
            continue;
        }
        int z = length;
        while (z > 0 && !available[std::size_t(z)]) --z;
        if (z == 0) fail("Vorbis: an over-specified Huffman tree");
        const auto code = available[std::size_t(z)];
        available[std::size_t(z)] = 0;
        insert(code, length, entry);
        for (int y = length; y > z; --y) available[std::size_t(y)] = code + (1U << (32 - y));
    }
    // A single used entry: its one codeword (either bit value reaches it, as libvorbis decodes it).
    if (used == 1) {
        auto& root = book.tree[0];
        if (root[0] == 0) root[0] = root[1];
        if (root[1] == 0) root[1] = root[0];
    }
}

Codebook read_codebook(Bits& bits) {
    if (bits.read(24) != 0x564342) fail("Vorbis: bad codebook sync");
    Codebook book;
    book.dimensions = bits.read(16);
    book.entries = bits.read(24);
    if (!book.dimensions || !book.entries) fail("Vorbis: an empty codebook");
    std::vector<std::uint8_t> lengths(book.entries, 0);
    if (!bits.bit()) {
        const bool sparse = bits.bit();
        for (auto& length : lengths) {
            if (sparse && !bits.bit()) continue;
            length = static_cast<std::uint8_t>(bits.read(5) + 1);
        }
    } else {
        std::uint32_t entry = 0, length = bits.read(5) + 1;
        while (entry < book.entries) {
            const auto number = bits.read(ilog(book.entries - entry));
            if (entry + number > book.entries || length > 32) fail("Vorbis: bad ordered codebook");
            for (std::uint32_t i = 0; i < number; ++i) lengths[entry + i] = static_cast<std::uint8_t>(length);
            entry += number;
            ++length;
            if (bits.eop()) fail("Vorbis: truncated codebook");
        }
    }
    const auto lookup = bits.read(4);
    if (lookup == 1 || lookup == 2) {
        const float minimum = float32_unpack(bits.read(32)), delta = float32_unpack(bits.read(32));
        const int value_bits = static_cast<int>(bits.read(4)) + 1;
        const bool sequence = bits.bit();
        const std::uint64_t count = lookup == 1 ? lookup1_values(book.entries, book.dimensions)
                                                : std::uint64_t(book.entries) * book.dimensions;
        if (count > (1U << 24)) fail("Vorbis: an oversized codebook lookup");
        std::vector<std::uint32_t> multiplicands(count);
        for (auto& m : multiplicands) m = bits.read(value_bits);
        book.vectors.resize(std::size_t(book.entries) * book.dimensions);
        for (std::uint32_t e = 0; e < book.entries; ++e) {
            float last = 0;
            std::uint64_t divisor = 1;
            for (std::uint32_t d = 0; d < book.dimensions; ++d) {
                const auto offset = lookup == 1 ? (e / divisor) % count : std::uint64_t(e) * book.dimensions + d;
                const float value = static_cast<float>(multiplicands[offset]) * delta + minimum + last;
                if (sequence) last = value;
                book.vectors[std::size_t(e) * book.dimensions + d] = value;
                if (lookup == 1) divisor *= count;
            }
        }
    } else if (lookup != 0) {
        fail("Vorbis: unknown codebook lookup type " + std::to_string(lookup));
    }
    if (bits.eop()) fail("Vorbis: truncated setup header");
    build_tree(book, lengths);
    return book;
}

struct Floor1 {
    std::vector<int> partition_class;
    std::array<int, 16> class_dimensions{}, class_subclasses{}, class_masterbook{};
    std::array<std::array<int, 8>, 16> subclass_books{};
    int multiplier = 1;
    std::vector<int> x;          // X_list
    std::vector<int> order;      // indexes of x sorted by x
    std::vector<int> low, high;  // the neighbours of each point (>= 2)
};

struct Residue {
    int type = 0;
    std::uint32_t begin = 0, end = 0, partition_size = 1;
    int classifications = 1, classbook = 0;
    std::vector<std::array<int, 8>> books;   // -1: none
};

struct Mapping {
    std::vector<int> mux;
    std::vector<int> submap_floor, submap_residue;
    std::vector<std::pair<int, int>> coupling;   // magnitude, angle
};

struct Mode {
    bool blockflag = false;
    int mapping = 0;
};

// The DCT-IV of size m through a complex FFT of size m / 2 (the IMDCT below).
class Imdct {
public:
    explicit Imdct(std::size_t n) : n_(n) {
        const std::size_t m = n / 2, half = m / 2;
        pre_.resize(half);
        post_.resize(half);
        for (std::size_t k = 0; k < half; ++k) {
            pre_[k] = std::polar(1.0, -std::numbers::pi * double(4 * k + 1) / double(4 * m));
            post_[k] = std::polar(1.0, -std::numbers::pi * double(k) / double(m));
        }
        twiddle_.resize(half / 2 + 1);
        for (std::size_t k = 0; k < twiddle_.size(); ++k) twiddle_[k] = std::polar(1.0, -2 * std::numbers::pi * double(k) / double(half));
        work_.resize(half);
        dct_.resize(m);
    }
    // n / 2 coefficients -> n samples: y[i] = sum_k X[k] cos(2 pi / n (i + 1/2 + n/4)(k + 1/2)).
    void run(const float* in, float* out) {
        const std::size_t m = n_ / 2, half = m / 2;
        for (std::size_t k = 0; k < half; ++k) work_[k] = std::complex<double>(in[2 * k], in[m - 1 - 2 * k]) * pre_[k];
        fft();
        for (std::size_t k = 0; k < half; ++k) {
            const auto u = work_[k] * post_[k];
            dct_[2 * k] = u.real();
            dct_[m - 1 - 2 * k] = -u.imag();
        }
        for (std::size_t i = 0; i < n_; ++i) {
            const std::size_t j = i + m / 2;
            double v;
            if (j < m) v = dct_[j];
            else if (j < 2 * m) v = -dct_[2 * m - 1 - j];
            else v = -dct_[j - 2 * m];
            out[i] = static_cast<float>(v);
        }
    }

private:
    std::size_t n_;
    std::vector<std::complex<double>> pre_, post_, twiddle_, work_;
    std::vector<double> dct_;

    void fft() {
        const std::size_t size = work_.size();
        for (std::size_t i = 1, j = 0; i < size; ++i) {
            std::size_t bit = size >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(work_[i], work_[j]);
        }
        for (std::size_t len = 2; len <= size; len <<= 1) {
            const std::size_t stride = size / len;
            for (std::size_t i = 0; i < size; i += len)
                for (std::size_t k = 0; k < len / 2; ++k) {
                    const auto t = work_[i + k + len / 2] * twiddle_[k * stride];
                    work_[i + k + len / 2] = work_[i + k] - t;
                    work_[i + k] += t;
                }
        }
    }
};

// The floor 1 inverse dB table (geometric from 1.0649863e-07 to 1).
const std::array<float, 256>& inverse_db() {
    static const auto table = [] {
        std::array<float, 256> t{};
        const double first = 1.0649863e-07;
        for (int i = 0; i < 256; ++i) t[std::size_t(i)] = static_cast<float>(first * std::pow(1.0 / first, i / 255.0));
        t[255] = 1.0F;
        return t;
    }();
    return table;
}

int render_point(int x0, int y0, int x1, int y1, int x) {
    const int dy = y1 - y0, adx = x1 - x0, ady = std::abs(dy);
    const int off = ady * (x - x0) / adx;
    return dy < 0 ? y0 - off : y0 + off;
}

void render_line(int x0, int y0, int x1, int y1, std::vector<int>& v) {
    const int dy = y1 - y0, adx = x1 - x0;
    int ady = std::abs(dy);
    const int base = dy / adx;
    const int sy = dy < 0 ? base - 1 : base + 1;
    int y = y0, err = 0;
    ady -= std::abs(base) * adx;
    const int limit = static_cast<int>(v.size());
    if (x0 < limit) v[std::size_t(x0)] = y;
    for (int x = x0 + 1; x < x1 && x < limit; ++x) {
        err += ady;
        if (err >= adx) {
            err -= adx;
            y += sy;
        } else {
            y += base;
        }
        v[std::size_t(x)] = y;
    }
}

class VorbisDecoder {
public:
    void identification(std::span<const std::uint8_t> packet) {
        if (packet.size() < 30 || packet[0] != 1 || std::memcmp(packet.data() + 1, "vorbis", 6) != 0)
            fail("Vorbis: no identification header");
        if (u32le(packet, 7) != 0) fail("Vorbis: unknown version");
        channels_ = packet[11];
        rate_ = u32le(packet, 12);
        block_[0] = std::size_t(1) << (packet[28] & 15);
        block_[1] = std::size_t(1) << (packet[28] >> 4);
        if (!channels_ || !rate_) fail("Vorbis: no channels or rate");
        if (block_[0] < 64 || block_[1] > 8192 || block_[0] > block_[1]) fail("Vorbis: bad block sizes");
        if (!(packet[29] & 1)) fail("Vorbis: identification header framing");
    }
    void setup(std::span<const std::uint8_t> packet) {
        if (packet.size() < 7 || packet[0] != 5 || std::memcmp(packet.data() + 1, "vorbis", 6) != 0)
            fail("Vorbis: no setup header");
        Bits bits(packet.subspan(7));
        const auto books = bits.read(8) + 1;
        for (std::uint32_t i = 0; i < books; ++i) books_.push_back(read_codebook(bits));
        const auto times = bits.read(6) + 1;
        for (std::uint32_t i = 0; i < times; ++i)
            if (bits.read(16) != 0) fail("Vorbis: unknown time domain transform");
        const auto floors = bits.read(6) + 1;
        for (std::uint32_t i = 0; i < floors; ++i) {
            const auto type = bits.read(16);
            if (type == 0) fail("Vorbis: floor 0 is not supported (re-encode with a current Vorbis encoder)");
            if (type != 1) fail("Vorbis: unknown floor type");
            floors_.push_back(read_floor1(bits));
        }
        const auto residues = bits.read(6) + 1;
        for (std::uint32_t i = 0; i < residues; ++i) residues_.push_back(read_residue(bits));
        const auto mappings = bits.read(6) + 1;
        for (std::uint32_t i = 0; i < mappings; ++i) mappings_.push_back(read_mapping(bits));
        const auto modes = bits.read(6) + 1;
        for (std::uint32_t i = 0; i < modes; ++i) {
            Mode mode;
            mode.blockflag = bits.bit();
            if (bits.read(16) != 0 || bits.read(16) != 0) fail("Vorbis: unknown window / transform type");
            mode.mapping = static_cast<int>(bits.read(8));
            if (mode.mapping >= static_cast<int>(mappings_.size())) fail("Vorbis: bad mode mapping");
            modes_.push_back(mode);
        }
        if (!bits.bit() || bits.eop()) fail("Vorbis: setup header framing");
        for (int b = 0; b < 2; ++b) {
            imdct_[b] = std::make_unique<Imdct>(block_[b]);
            slope_[b].resize(block_[b] / 2);
            for (std::size_t i = 0; i < slope_[b].size(); ++i) {
                const double s = std::sin((double(i) + 0.5) / double(slope_[b].size()) * std::numbers::pi / 2);
                slope_[b][i] = static_cast<float>(std::sin(std::numbers::pi / 2 * s * s));
            }
        }
    }

    std::uint32_t rate() const { return rate_; }
    std::uint32_t channels() const { return channels_; }

    // The block size audio() decodes this packet with; 0 when audio() skips it (not an audio packet).
    std::size_t block_size(std::span<const std::uint8_t> packet) const {
        Bits bits(packet);
        if (packet.empty() || bits.bit()) return 0;
        const auto mode_number = bits.read(ilog(static_cast<std::uint32_t>(modes_.size() - 1)));
        if (bits.eop() || mode_number >= modes_.size()) return 0;
        return block_[modes_[mode_number].blockflag ? 1 : 0];
    }

    // Decodes one audio packet; appends the finished frames (interleaved) to out; returns their count.
    std::size_t audio(std::span<const std::uint8_t> packet, std::vector<float>& out) {
        Bits bits(packet);
        if (packet.empty() || bits.bit()) return 0;   // not an audio packet: ignored
        const auto mode_number = bits.read(ilog(static_cast<std::uint32_t>(modes_.size() - 1)));
        if (bits.eop() || mode_number >= modes_.size()) return 0;
        const auto& mode = modes_[mode_number];
        const auto& mapping = mappings_[std::size_t(mode.mapping)];
        const std::size_t n = block_[mode.blockflag ? 1 : 0], half = n / 2;
        bool prev_long = true, next_long = true;
        if (mode.blockflag) {
            prev_long = bits.bit();
            next_long = bits.bit();
        }
        // Floors.
        std::vector<std::vector<int>> floor_y(channels_);
        std::vector<bool> unused(channels_, false);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const auto submap = std::size_t(mapping.mux[c]);
            unused[c] = !decode_floor1(floors_[std::size_t(mapping.submap_floor[submap])], bits, floor_y[c]);
        }
        std::vector<bool> no_residue = unused;
        for (const auto& [m, a] : mapping.coupling)
            if (!no_residue[std::size_t(m)] || !no_residue[std::size_t(a)]) no_residue[std::size_t(m)] = no_residue[std::size_t(a)] = false;
        // Residues.
        std::vector<std::vector<float>> spectrum(channels_, std::vector<float>(half, 0.0F));
        for (std::size_t s = 0; s < mapping.submap_floor.size(); ++s) {
            std::vector<std::vector<float>*> vectors;
            std::vector<bool> skip;
            for (std::uint32_t c = 0; c < channels_; ++c)
                if (std::size_t(mapping.mux[c]) == s) {
                    vectors.push_back(&spectrum[c]);
                    skip.push_back(no_residue[c]);
                }
            decode_residue(residues_[std::size_t(mapping.submap_residue[s])], bits, vectors, skip, half);
        }
        // Inverse coupling.
        for (auto it = mapping.coupling.rbegin(); it != mapping.coupling.rend(); ++it) {
            auto& mag = spectrum[std::size_t(it->first)];
            auto& ang = spectrum[std::size_t(it->second)];
            for (std::size_t i = 0; i < half; ++i) {
                const float m = mag[i], a = ang[i];
                float nm, na;
                if (m > 0) {
                    if (a > 0) { nm = m; na = m - a; }
                    else { na = m; nm = m + a; }
                } else {
                    if (a > 0) { nm = m; na = m + a; }
                    else { na = m; nm = m - a; }
                }
                mag[i] = nm;
                ang[i] = na;
            }
        }
        // Floor curves, the IMDCT and the window.
        const std::size_t flag = mode.blockflag ? 1 : 0;
        std::vector<std::vector<float>> current(channels_, std::vector<float>(n, 0.0F));
        for (std::uint32_t c = 0; c < channels_; ++c) {
            if (unused[c]) continue;
            const auto submap = std::size_t(mapping.mux[c]);
            const auto curve = floor1_curve(floors_[std::size_t(mapping.submap_floor[submap])], floor_y[c], half);
            for (std::size_t i = 0; i < half; ++i) spectrum[c][i] *= curve[i];
            imdct_[flag]->run(spectrum[c].data(), current[c].data());
            apply_window(current[c], n, mode.blockflag, prev_long, next_long);
        }
        // Overlap-add: from the previous window's centre to this one's.
        std::size_t produced = 0;
        if (!previous_.empty()) {
            const std::size_t pn = previous_n_;
            produced = pn / 4 + n / 4;
            const std::ptrdiff_t start = std::ptrdiff_t(n / 4) - std::ptrdiff_t(pn / 4);
            const std::size_t base = out.size();
            out.resize(base + produced * channels_, 0.0F);
            for (std::uint32_t c = 0; c < channels_; ++c)
                for (std::size_t k = 0; k < produced; ++k) {
                    float v = 0;
                    const std::ptrdiff_t x = start + std::ptrdiff_t(k);
                    if (x >= 0 && std::size_t(x) < n) v += current[c][std::size_t(x)];
                    const std::size_t p = pn / 2 + k;
                    if (p < pn) v += previous_[c][p];
                    out[base + k * channels_ + c] = v;
                }
        }
        previous_ = std::move(current);
        previous_n_ = n;
        return produced;
    }

private:
    std::uint32_t channels_ = 0, rate_ = 0;
    std::array<std::size_t, 2> block_{};
    std::vector<Codebook> books_;
    std::vector<Floor1> floors_;
    std::vector<Residue> residues_;
    std::vector<Mapping> mappings_;
    std::vector<Mode> modes_;
    std::array<std::unique_ptr<Imdct>, 2> imdct_;
    std::array<std::vector<float>, 2> slope_;
    std::vector<std::vector<float>> previous_;
    std::size_t previous_n_ = 0;

    int book_index(std::uint32_t value) const {
        if (value >= books_.size()) fail("Vorbis: a codebook number past the codebooks");
        return static_cast<int>(value);
    }

    Floor1 read_floor1(Bits& bits) {
        Floor1 f;
        const auto partitions = bits.read(5);
        int max_class = -1;
        for (std::uint32_t i = 0; i < partitions; ++i) {
            f.partition_class.push_back(static_cast<int>(bits.read(4)));
            max_class = std::max(max_class, f.partition_class.back());
        }
        for (int i = 0; i <= max_class; ++i) {
            const auto c = std::size_t(i);
            f.class_dimensions[c] = static_cast<int>(bits.read(3)) + 1;
            f.class_subclasses[c] = static_cast<int>(bits.read(2));
            if (f.class_subclasses[c]) f.class_masterbook[c] = book_index(bits.read(8));
            for (int j = 0; j < (1 << f.class_subclasses[c]); ++j) {
                const int book = static_cast<int>(bits.read(8)) - 1;
                f.subclass_books[c][std::size_t(j)] = book < 0 ? -1 : book_index(static_cast<std::uint32_t>(book));
            }
        }
        f.multiplier = static_cast<int>(bits.read(2)) + 1;
        const int range_bits = static_cast<int>(bits.read(4));
        f.x = {0, 1 << range_bits};
        for (const auto c : f.partition_class)
            for (int j = 0; j < f.class_dimensions[std::size_t(c)]; ++j) f.x.push_back(static_cast<int>(bits.read(range_bits)));
        if (f.x.size() > 65) fail("Vorbis: too many floor 1 points");
        f.order.resize(f.x.size());
        for (std::size_t i = 0; i < f.order.size(); ++i) f.order[i] = static_cast<int>(i);
        std::stable_sort(f.order.begin(), f.order.end(), [&](int a, int b) { return f.x[std::size_t(a)] < f.x[std::size_t(b)]; });
        for (std::size_t i = 1; i < f.order.size(); ++i)
            if (f.x[std::size_t(f.order[i])] == f.x[std::size_t(f.order[i - 1])]) fail("Vorbis: repeated floor 1 point");
        f.low.assign(f.x.size(), 0);
        f.high.assign(f.x.size(), 1);
        for (std::size_t i = 2; i < f.x.size(); ++i) {
            int lo = -1, hi = -1;
            for (std::size_t j = 0; j < i; ++j) {
                if (f.x[j] < f.x[i] && (lo < 0 || f.x[j] > f.x[std::size_t(lo)])) lo = static_cast<int>(j);
                if (f.x[j] > f.x[i] && (hi < 0 || f.x[j] < f.x[std::size_t(hi)])) hi = static_cast<int>(j);
            }
            f.low[i] = lo;
            f.high[i] = hi;
        }
        return f;
    }

    Residue read_residue(Bits& bits) {
        Residue r;
        r.type = static_cast<int>(bits.read(16));
        if (r.type > 2) fail("Vorbis: unknown residue type");
        r.begin = bits.read(24);
        r.end = bits.read(24);
        r.partition_size = bits.read(24) + 1;
        r.classifications = static_cast<int>(bits.read(6)) + 1;
        r.classbook = book_index(bits.read(8));
        std::vector<int> cascade;
        for (int i = 0; i < r.classifications; ++i) {
            int low = static_cast<int>(bits.read(3)), high = 0;
            if (bits.bit()) high = static_cast<int>(bits.read(5));
            cascade.push_back(high * 8 + low);
        }
        for (const auto c : cascade) {
            std::array<int, 8> books{};
            for (int j = 0; j < 8; ++j) books[std::size_t(j)] = (c & (1 << j)) ? book_index(bits.read(8)) : -1;
            r.books.push_back(books);
        }
        return r;
    }

    Mapping read_mapping(Bits& bits) {
        if (bits.read(16) != 0) fail("Vorbis: unknown mapping type");
        Mapping m;
        const std::uint32_t submaps = bits.bit() ? bits.read(4) + 1 : 1;
        if (bits.bit()) {
            const auto steps = bits.read(8) + 1;
            const int width = ilog(channels_ - 1);
            for (std::uint32_t i = 0; i < steps; ++i) {
                const auto mag = static_cast<int>(bits.read(width)), ang = static_cast<int>(bits.read(width));
                if (mag == ang || mag >= static_cast<int>(channels_) || ang >= static_cast<int>(channels_))
                    fail("Vorbis: bad channel coupling");
                m.coupling.push_back({mag, ang});
            }
        }
        if (bits.read(2) != 0) fail("Vorbis: mapping reserved bits");
        m.mux.assign(channels_, 0);
        if (submaps > 1)
            for (auto& mux : m.mux) {
                mux = static_cast<int>(bits.read(4));
                if (mux >= static_cast<int>(submaps)) fail("Vorbis: bad mapping mux");
            }
        for (std::uint32_t i = 0; i < submaps; ++i) {
            bits.read(8);
            const auto floor = bits.read(8), residue = bits.read(8);
            if (floor >= floors_.size() || residue >= residues_.size()) fail("Vorbis: bad submap");
            m.submap_floor.push_back(static_cast<int>(floor));
            m.submap_residue.push_back(static_cast<int>(residue));
        }
        return m;
    }

    bool decode_floor1(const Floor1& f, Bits& bits, std::vector<int>& y) const {
        if (!bits.bit()) return false;
        static constexpr std::array<int, 4> ranges{256, 128, 86, 64};
        const int range = ranges[std::size_t(f.multiplier - 1)];
        const int width = ilog(static_cast<std::uint32_t>(range - 1));
        y.assign(f.x.size(), 0);
        y[0] = static_cast<int>(bits.read(width));
        y[1] = static_cast<int>(bits.read(width));
        std::size_t offset = 2;
        for (const auto c : f.partition_class) {
            const auto cls = std::size_t(c);
            const int cdim = f.class_dimensions[cls], cbits = f.class_subclasses[cls], csub = (1 << cbits) - 1;
            int cval = 0;
            if (cbits) {
                cval = books_[std::size_t(f.class_masterbook[cls])].scalar(bits);
                if (cval < 0) return false;
            }
            for (int j = 0; j < cdim; ++j) {
                const int book = f.subclass_books[cls][std::size_t(cval & csub)];
                cval >>= cbits;
                if (book >= 0) {
                    const int v = books_[std::size_t(book)].scalar(bits);
                    if (v < 0) return false;
                    y[offset + std::size_t(j)] = v;
                } else {
                    y[offset + std::size_t(j)] = 0;
                }
            }
            offset += std::size_t(cdim);
        }
        return !bits.eop();
    }

    std::vector<float> floor1_curve(const Floor1& f, const std::vector<int>& y, std::size_t half) const {
        static constexpr std::array<int, 4> ranges{256, 128, 86, 64};
        const int range = ranges[std::size_t(f.multiplier - 1)];
        const std::size_t count = f.x.size();
        std::vector<int> final_y(count);
        std::vector<bool> step2(count, false);
        step2[0] = step2[1] = true;
        final_y[0] = y[0];
        final_y[1] = y[1];
        for (std::size_t i = 2; i < count; ++i) {
            const auto lo = std::size_t(f.low[i]), hi = std::size_t(f.high[i]);
            const int predicted = render_point(f.x[lo], final_y[lo], f.x[hi], final_y[hi], f.x[i]);
            const int val = y[i], highroom = range - predicted, lowroom = predicted;
            const int room = (highroom < lowroom ? highroom : lowroom) * 2;
            if (val) {
                step2[lo] = step2[hi] = step2[i] = true;
                if (val >= room) final_y[i] = highroom > lowroom ? val - lowroom + predicted : predicted - val + highroom - 1;
                else final_y[i] = (val & 1) ? predicted - (val + 1) / 2 : predicted + val / 2;
            } else {
                final_y[i] = predicted;
            }
        }
        std::vector<int> indexes(half, 0);
        int lx = 0, ly = final_y[std::size_t(f.order[0])] * f.multiplier, hx = 0, hy = ly;
        for (std::size_t s = 1; s < count; ++s) {
            const auto i = std::size_t(f.order[s]);
            if (!step2[i]) continue;
            hy = final_y[i] * f.multiplier;
            hx = f.x[i];
            render_line(lx, ly, hx, hy, indexes);
            lx = hx;
            ly = hy;
        }
        if (hx < static_cast<int>(half)) render_line(hx, hy, static_cast<int>(half), hy, indexes);
        std::vector<float> curve(half);
        const auto& db = inverse_db();
        for (std::size_t i = 0; i < half; ++i) curve[i] = db[std::size_t(std::clamp(indexes[i], 0, 255))];
        return curve;
    }

    void decode_residue(const Residue& r, Bits& bits, const std::vector<std::vector<float>*>& vectors,
                        const std::vector<bool>& skip, std::size_t half) const {
        if (vectors.empty()) return;
        if (r.type == 2) {
            if (std::all_of(skip.begin(), skip.end(), [](bool s) { return s; })) return;
            const std::size_t ch = vectors.size();
            std::vector<float> joined(half * ch, 0.0F);
            std::vector<float*> one{joined.data()};
            decode_partitions(r, bits, one, {false}, half * ch, 2);
            for (std::size_t i = 0; i < half; ++i)
                for (std::size_t c = 0; c < ch; ++c) (*vectors[c])[i] += joined[i * ch + c];
            return;
        }
        std::vector<float*> raw;
        for (auto* v : vectors) raw.push_back(v->data());
        decode_partitions(r, bits, raw, skip, half, r.type);
    }

    void decode_partitions(const Residue& r, Bits& bits, const std::vector<float*>& vectors,
                           const std::vector<bool>& skip, std::size_t size, int type) const {
        const std::size_t begin = std::min<std::size_t>(r.begin, size), end = std::min<std::size_t>(r.end, size);
        if (end <= begin) return;
        const std::size_t partitions = (end - begin) / r.partition_size;
        if (!partitions) return;
        const auto& classbook = books_[std::size_t(r.classbook)];
        const std::size_t per_word = classbook.dimensions;
        std::vector<std::vector<int>> classes(vectors.size(), std::vector<int>(partitions + per_word, 0));
        for (int pass = 0; pass < 8; ++pass) {
            std::size_t partition = 0;
            while (partition < partitions) {
                if (pass == 0)
                    for (std::size_t j = 0; j < vectors.size(); ++j) {
                        if (skip[j]) continue;
                        int temp = classbook.scalar(bits);
                        if (temp < 0) return;
                        for (std::size_t i = per_word; i-- > 0;) {
                            classes[j][i + partition] = temp % r.classifications;
                            temp /= r.classifications;
                        }
                    }
                for (std::size_t i = 0; i < per_word && partition < partitions; ++i, ++partition)
                    for (std::size_t j = 0; j < vectors.size(); ++j) {
                        if (skip[j]) continue;
                        const int book = r.books[std::size_t(classes[j][partition])][std::size_t(pass)];
                        if (book < 0) continue;
                        const auto& cb = books_[std::size_t(book)];
                        if (cb.vectors.empty()) fail("Vorbis: a residue book without vectors");
                        const std::size_t offset = begin + partition * r.partition_size;
                        float* v = vectors[j] + offset;
                        const std::size_t dims = cb.dimensions;
                        if (type == 0) {
                            const std::size_t step = r.partition_size / dims;
                            for (std::size_t k = 0; k < step; ++k) {
                                const float* e = cb.vector(bits);
                                if (!e) return;
                                for (std::size_t d = 0; d < dims; ++d) v[k + d * step] += e[d];
                            }
                        } else {
                            for (std::size_t k = 0; k < r.partition_size;) {
                                const float* e = cb.vector(bits);
                                if (!e) return;
                                for (std::size_t d = 0; d < dims && k < r.partition_size; ++d, ++k) v[k] += e[d];
                            }
                        }
                    }
            }
        }
    }

    void apply_window(std::vector<float>& v, std::size_t n, bool long_block, bool prev_long, bool next_long) const {
        const std::size_t s0 = block_[0];
        std::size_t left_start = 0, left_n = n / 2, right_start = n / 2, right_n = n / 2;
        if (long_block && !prev_long) {
            left_start = n / 4 - s0 / 4;
            left_n = s0 / 2;
        }
        if (long_block && !next_long) {
            right_start = n * 3 / 4 - s0 / 4;
            right_n = s0 / 2;
        }
        const auto& left = slope_[left_n == slope_[1].size() ? 1 : 0];
        const auto& right = slope_[right_n == slope_[1].size() ? 1 : 0];
        for (std::size_t i = 0; i < left_start; ++i) v[i] = 0;
        for (std::size_t i = 0; i < left_n; ++i) v[left_start + i] *= left[i];
        for (std::size_t i = 0; i < right_n; ++i) v[right_start + i] *= right[right_n - 1 - i];
        for (std::size_t i = right_start + right_n; i < n; ++i) v[i] = 0;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Sources: a file's frames a piece at a time (decode_wav / decode_ogg_vorbis read every piece; FittedAudio as the mix
// reaches them). The constructor reads the headers and knows the length; nothing past them is decoded.

}  // namespace

class AudioSource {
public:
    virtual ~AudioSource() = default;
    std::uint32_t rate() const { return rate_; }
    std::uint32_t channels() const { return channels_; }
    std::size_t frames() const { return frames_; }
    virtual bool done() const = 0;
    // Appends the next frames (interleaved floats) to out; returns their count (0 is possible before the end).
    virtual std::size_t decode_next(std::vector<float>& out) = 0;

protected:
    std::uint32_t rate_ = 0, channels_ = 0;
    std::size_t frames_ = 0;
};

namespace {

class WavSource final : public AudioSource {
public:
    explicit WavSource(std::span<const std::uint8_t> bytes) {
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0)
            fail("WAV: not a RIFF WAVE file");
        std::uint32_t align = 0;
        std::optional<std::span<const std::uint8_t>> data;
        std::size_t at = 12;
        while (at + 8 <= bytes.size()) {
            const auto size = u32le(bytes, at + 4);
            const std::size_t body = at + 8;
            const std::size_t available = std::min<std::size_t>(size, bytes.size() - body);
            if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0) {
                if (available < 16) fail("WAV: short fmt chunk");
                format_ = u16le(bytes, body);
                channels_ = u16le(bytes, body + 2);
                rate_ = u32le(bytes, body + 4);
                align = u16le(bytes, body + 12);
                bits_ = u16le(bytes, body + 14);
                if (format_ == 0xfffe) {
                    if (available < 26) fail("WAV: short extensible fmt chunk");
                    format_ = u16le(bytes, body + 24);
                }
            } else if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
                data = bytes.subspan(body, available);   // a truncated data chunk keeps its whole frames
            }
            at = body + size + (size & 1);
        }
        if (!format_) fail("WAV: no fmt chunk");
        if (!data) fail("WAV: no data chunk");
        if (!channels_ || !rate_) fail("WAV: no channels or rate");
        const bool integer = format_ == 1 && (bits_ == 8 || bits_ == 16 || bits_ == 24 || bits_ == 32);
        const bool floating = format_ == 3 && (bits_ == 32 || bits_ == 64);
        if (!integer && !floating)
            fail("WAV: unsupported sample format " + std::to_string(format_) + " / " + std::to_string(bits_) + " bit");
        if (align != bits_ / 8 * channels_) fail("WAV: bad block alignment");
        data_ = *data;
        frames_ = data_.size() / align;
    }
    bool done() const override { return next_ >= frames_; }
    std::size_t decode_next(std::vector<float>& out) override {
        const std::size_t count = std::min<std::size_t>(4096, frames_ - next_), width = bits_ / 8;
        const std::size_t base = out.size();
        out.resize(base + count * channels_);
        for (std::size_t i = 0; i < count * channels_; ++i)
            out[base + i] = wav_sample(data_, (next_ * channels_ + i) * width, format_, bits_);
        next_ += count;
        return count;
    }

private:
    std::uint32_t format_ = 0, bits_ = 0;
    std::span<const std::uint8_t> data_;
    std::size_t next_ = 0;
};

class OggVorbisSource final : public AudioSource {
public:
    explicit OggVorbisSource(std::span<const std::uint8_t> bytes) : packets_(ogg_packets(bytes)) {
        if (packets_.size() < 3) fail("Ogg: fewer than the three Vorbis headers");
        decoder_.identification(packets_[0].bytes);
        if (packets_[1].bytes.size() < 7 || packets_[1].bytes[0] != 3) fail("Vorbis: no comment header");
        decoder_.setup(packets_[2].bytes);
        rate_ = decoder_.rate();
        channels_ = decoder_.channels();
        // The length from the block sizes and granule positions alone: each audio packet ends the previous block's
        // second half and begins its own (a quarter of each); the first granule page's position trims the start
        // (libvorbis's leading trim), the last page's the end.
        std::size_t total = 0, previous = 0;
        end_ = packets_.size();
        for (std::size_t i = 3; i < packets_.size(); ++i) {
            if (const auto n = decoder_.block_size(packets_[i].bytes)) {
                if (previous) total += previous / 4 + n / 4;
                previous = n;
            }
            const auto granule = packets_[i].granule;
            if (granule < 0) continue;
            const auto g = static_cast<std::size_t>(granule);
            if (packets_[i].eos) {
                if (g < total) total = g;
                end_ = i + 1;
                break;
            }
            if (packets_[i].first_granule_page && g < total) {
                lead_ += total - g;
                total = g;
            }
        }
        frames_ = total;
    }
    bool done() const override { return emitted_ >= frames_ || next_ >= end_; }
    std::size_t decode_next(std::vector<float>& out) override {
        if (done()) return 0;
        scratch_.clear();
        const std::size_t produced = decoder_.audio(packets_[next_++].bytes, scratch_);
        std::size_t from = 0;
        if (skipped_ < lead_) {
            from = std::min(produced, lead_ - skipped_);
            skipped_ += from;
        }
        const std::size_t keep = std::min(produced - from, frames_ - emitted_);
        out.insert(out.end(), scratch_.begin() + static_cast<std::ptrdiff_t>(from * channels_),
                   scratch_.begin() + static_cast<std::ptrdiff_t>((from + keep) * channels_));
        emitted_ += keep;
        return keep;
    }

private:
    std::vector<Packet> packets_;
    VorbisDecoder decoder_;
    std::vector<float> scratch_;
    std::size_t next_ = 3, end_ = 0, lead_ = 0, skipped_ = 0, emitted_ = 0;
};

std::unique_ptr<AudioSource> open_source(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) return std::make_unique<OggVorbisSource>(bytes);
    if (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0) return std::make_unique<WavSource>(bytes);
    fail("not a WAV or Ogg Vorbis file");
}

DecodedAudio decode_all(AudioSource& source) {
    DecodedAudio out;
    out.rate = source.rate();
    out.channels = source.channels();
    out.samples.reserve(source.frames() * source.channels());
    while (!source.done()) source.decode_next(out.samples);
    return out;
}

// fit_audio's channel conversion of one source frame.
void convert_frame(const float* src, std::uint32_t in_channels, std::uint32_t channels, float* dst) {
    if (channels == 1) {
        float sum = 0;
        for (std::uint32_t c = 0; c < in_channels; ++c) sum += src[c];
        dst[0] = sum / static_cast<float>(in_channels);
    } else {
        for (std::uint32_t c = 0; c < channels; ++c) dst[c] = src[std::min(c, in_channels - 1)];
    }
}

std::int16_t to_pcm16(float v) { return static_cast<std::int16_t>(std::clamp(std::lround(v * 32768.0F), -32768L, 32767L)); }

std::size_t natural_frames(std::size_t in_frames, std::uint32_t in_rate, std::uint32_t rate) {
    return in_rate == rate ? in_frames : static_cast<std::size_t>((std::uint64_t(in_frames) * rate + in_rate / 2) / in_rate);
}

}  // namespace

DecodedAudio decode_wav(std::span<const std::uint8_t> bytes) {
    WavSource source(bytes);
    return decode_all(source);
}

DecodedAudio decode_ogg_vorbis(std::span<const std::uint8_t> bytes) {
    OggVorbisSource source(bytes);
    return decode_all(source);
}

DecodedAudio decode_audio_file(std::span<const std::uint8_t> bytes) { return decode_all(*open_source(bytes)); }

audio::PcmClip fit_audio(const DecodedAudio& in, std::uint32_t rate, std::uint32_t channels, std::optional<std::size_t> frames,
                         float gain) {
    if (!in.channels || !in.rate || !rate || !channels) fail("audio: no channels or rate");
    const std::size_t in_frames = in.frames();
    std::vector<float> mixed(in_frames * channels);
    for (std::size_t f = 0; f < in_frames; ++f) convert_frame(in.samples.data() + f * in.channels, in.channels, channels, &mixed[f * channels]);
    const std::size_t natural = natural_frames(in_frames, in.rate, rate);
    const std::size_t out_frames = frames ? *frames : natural;
    audio::PcmClip clip;
    clip.rate = rate;
    clip.channels = channels;
    clip.samples.assign(out_frames * channels, 0);
    const double step = double(in.rate) / double(rate);
    for (std::size_t f = 0; f < std::min(out_frames, natural) && in_frames; ++f) {
        const double pos = in.rate == rate ? double(f) : double(f) * step;
        const auto i0 = std::min(static_cast<std::size_t>(pos), in_frames - 1);
        const auto i1 = std::min(i0 + 1, in_frames - 1);
        const float t = static_cast<float>(pos - double(i0));
        for (std::uint32_t c = 0; c < channels; ++c)
            clip.samples[f * channels + c] = to_pcm16((mixed[i0 * channels + c] * (1 - t) + mixed[i1 * channels + c] * t) * gain);
    }
    return clip;
}

FittedAudio::FittedAudio(std::vector<std::uint8_t> file, std::uint32_t rate, std::uint32_t channels,
                         std::optional<std::size_t> frames, float gain)
    : file_(std::move(file)), rate_(rate), channels_(channels), gain_(gain) {
    if (!rate || !channels) fail("audio: no channels or rate");
    source_ = open_source(file_);
    if (!source_->channels() || !source_->rate()) fail("audio: no channels or rate");
    const std::size_t natural = natural_frames(source_->frames(), source_->rate(), rate);
    const std::size_t out_frames = frames ? *frames : natural;
    limit_ = source_->frames() ? std::min(out_frames, natural) : 0;
    step_ = double(source_->rate()) / double(rate);
    clip_ = std::make_shared<audio::PcmClip>();
    clip_->rate = rate;
    clip_->channels = channels;
    clip_->samples.assign(out_frames * channels, 0);
}

FittedAudio::~FittedAudio() = default;

std::uint32_t FittedAudio::source_rate() const { return source_->rate(); }
std::uint32_t FittedAudio::source_channels() const { return source_->channels(); }
std::size_t FittedAudio::source_frames() const { return source_->frames(); }

void FittedAudio::decode_to(std::size_t frames) {
    const std::size_t target = std::min(frames, limit_), in_frames = source_->frames();
    const std::uint32_t in_channels = source_->channels();
    const auto position = [&](std::size_t f) { return source_->rate() == rate_ ? double(f) : double(f) * step_; };
    while (done_ < target) {
        const double pos = position(done_);
        const auto i0 = std::min(static_cast<std::size_t>(pos), in_frames - 1);
        const auto i1 = std::min(i0 + 1, in_frames - 1);
        while (mixed_base_ + mixed_.size() / channels_ <= i1) {
            if (source_->done()) fail("audio: the file ended before its declared length");
            raw_.clear();
            const auto count = source_->decode_next(raw_);
            const std::size_t base = mixed_.size();
            mixed_.resize(base + count * channels_);
            for (std::size_t f = 0; f < count; ++f)
                convert_frame(raw_.data() + f * in_channels, in_channels, channels_, &mixed_[base + f * channels_]);
        }
        const float t = static_cast<float>(pos - double(i0));
        const float* a = &mixed_[(i0 - mixed_base_) * channels_];
        const float* b = &mixed_[(i1 - mixed_base_) * channels_];
        for (std::uint32_t c = 0; c < channels_; ++c) clip_->samples[done_ * channels_ + c] = to_pcm16((a[c] * (1 - t) + b[c] * t) * gain_);
        ++done_;
    }
    // The source frames before the next output frame's are not read again.
    if (done_ < limit_) {
        const auto first = std::min(static_cast<std::size_t>(position(done_)), in_frames - 1);
        if (first > mixed_base_) {
            const auto drop = std::min(first - mixed_base_, mixed_.size() / channels_);
            mixed_.erase(mixed_.begin(), mixed_.begin() + static_cast<std::ptrdiff_t>(drop * channels_));
            mixed_base_ += drop;
        }
    }
}

std::vector<std::uint8_t> convert_audio_file(std::span<const std::uint8_t> file, std::uint32_t rate, std::uint32_t channels,
                                             std::uint32_t max_rate, std::uint32_t max_channels) {
    const auto decoded = decode_audio_file(file);
    if (!rate) rate = decoded.rate;
    if (!channels) channels = decoded.channels;
    return encode_wav(fit_audio(decoded, std::min(rate, max_rate), std::min(channels, max_channels)));
}

std::vector<std::uint8_t> encode_wav(const audio::PcmClip& clip) {
    std::vector<std::uint8_t> out;
    const auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    const auto put16 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    const auto data_bytes = static_cast<std::uint32_t>(clip.samples.size() * 2);
    tag("RIFF");
    put32(36 + data_bytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);
    put16(clip.channels);
    put32(clip.rate);
    put32(clip.rate * clip.channels * 2);
    put16(clip.channels * 2);
    put16(16);
    tag("data");
    put32(data_bytes);
    for (const auto s : clip.samples) put16(static_cast<std::uint16_t>(s));
    return out;
}

}  // namespace omega::mods
