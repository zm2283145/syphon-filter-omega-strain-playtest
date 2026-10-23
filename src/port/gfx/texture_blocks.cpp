#include "texture_blocks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega::gfx {
namespace {

using Rgb = std::array<int, 3>;

Rgb expand565(std::uint16_t c) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

std::uint16_t quantize565(const std::array<float, 3>& c) {
    const auto q = [](float v, int max) {
        return static_cast<unsigned>(std::clamp(static_cast<int>(std::lround(std::clamp(v, 0.0f, 255.0f) * max / 255.0f)), 0, max));
    };
    return static_cast<std::uint16_t>(q(c[0], 31) << 11 | q(c[1], 63) << 5 | q(c[2], 31));
}

// The four (or, a BC1 block with c0 <= c1, three + transparent) colours of a colour block; a BC3 block's colour block is
// always four-colour (`four`).
std::array<Rgb, 4> palette(std::uint16_t c0, std::uint16_t c1, bool four) {
    const auto a = expand565(c0), b = expand565(c1);
    std::array<Rgb, 4> p{a, b, {}, {}};
    for (int k = 0; k < 3; ++k) {
        if (four || c0 > c1) {
            p[2][k] = (2 * a[k] + b[k]) / 3;
            p[3][k] = (a[k] + 2 * b[k]) / 3;
        } else {
            p[2][k] = (a[k] + b[k]) / 2;
            p[3][k] = 0;
        }
    }
    return p;
}

int distance(const Rgb& a, const Rgb& b) {
    int sum = 0;
    for (int k = 0; k < 3; ++k) sum += (a[k] - b[k]) * (a[k] - b[k]);
    return sum;
}

struct ColourFit {
    std::uint16_t c0 = 0, c1 = 0;
    std::array<std::uint8_t, 16> index{};
    long long error = std::numeric_limits<long long>::max();
};

// The opaque (four-colour) block for end points a, b: c0 > c1 (swapped as needed); equal ones take index 0 throughout.
ColourFit fit(const std::array<Rgb, 16>& texels, std::uint16_t a, std::uint16_t b) {
    ColourFit f;
    if (a < b) std::swap(a, b);
    f.c0 = a;
    f.c1 = b;
    f.error = 0;
    const auto p = palette(a, b, true);
    for (int i = 0; i < 16; ++i) {
        int best = 0, best_error = distance(texels[i], p[0]);
        if (a != b)
            for (int k = 1; k < 4; ++k)
                if (const int e = distance(texels[i], p[k]); e < best_error) {
                    best = k;
                    best_error = e;
                }
        f.index[i] = static_cast<std::uint8_t>(best);
        f.error += best_error;
    }
    return f;
}

ColourFit encode_colour(const std::array<Rgb, 16>& texels) {
    std::array<float, 3> mean{};
    for (const auto& t : texels)
        for (int k = 0; k < 3; ++k) mean[k] += static_cast<float>(t[k]) / 16.0f;
    float cov[3][3]{};
    for (const auto& t : texels)
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) cov[r][c] += (t[r] - mean[r]) * (t[c] - mean[c]);
    std::array<float, 3> axis{1.0f, 1.0f, 1.0f};
    for (int iteration = 0; iteration < 8; ++iteration) {
        std::array<float, 3> next{};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) next[r] += cov[r][c] * axis[c];
        const float length = std::max({std::fabs(next[0]), std::fabs(next[1]), std::fabs(next[2])});
        if (length < 1e-6f) break;
        for (int k = 0; k < 3; ++k) axis[k] = next[k] / length;
    }
    float low = std::numeric_limits<float>::max(), high = -low;
    for (const auto& t : texels) {
        const float d = (t[0] - mean[0]) * axis[0] + (t[1] - mean[1]) * axis[1] + (t[2] - mean[2]) * axis[2];
        low = std::min(low, d);
        high = std::max(high, d);
    }
    const float norm = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
    std::array<float, 3> end0{}, end1{};
    for (int k = 0; k < 3; ++k) {
        end0[k] = mean[k] + axis[k] * high / norm;
        end1[k] = mean[k] + axis[k] * low / norm;
    }
    auto best = fit(texels, quantize565(end0), quantize565(end1));
    // Least-squares refits of the end points from the chosen indices (texel ~ w * c0 + (1 - w) * c1).
    for (int round = 0; round < 2 && best.error > 0 && best.c0 != best.c1; ++round) {
        static constexpr float weight[4]{1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
        float aa = 0, ab = 0, bb = 0;
        std::array<float, 3> ax{}, bx{};
        for (int i = 0; i < 16; ++i) {
            const float w = weight[best.index[i]], v = 1.0f - w;
            aa += w * w;
            ab += w * v;
            bb += v * v;
            for (int k = 0; k < 3; ++k) {
                ax[k] += w * texels[i][k];
                bx[k] += v * texels[i][k];
            }
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        std::array<float, 3> a{}, b{};
        for (int k = 0; k < 3; ++k) {
            a[k] = (ax[k] * bb - bx[k] * ab) / det;
            b[k] = (bx[k] * aa - ax[k] * ab) / det;
        }
        const auto refit = fit(texels, quantize565(a), quantize565(b));
        if (refit.error >= best.error) break;
        best = refit;
    }
    return best;
}

void write_colour(std::uint8_t* out, const ColourFit& f) {
    out[0] = static_cast<std::uint8_t>(f.c0);
    out[1] = static_cast<std::uint8_t>(f.c0 >> 8);
    out[2] = static_cast<std::uint8_t>(f.c1);
    out[3] = static_cast<std::uint8_t>(f.c1 >> 8);
    std::uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= std::uint32_t(f.index[i]) << (2 * i);
    for (int k = 0; k < 4; ++k) out[4 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
}

std::array<int, 8> alpha_palette(int a0, int a1) {
    std::array<int, 8> p{a0, a1};
    if (a0 > a1) {
        for (int k = 2; k < 8; ++k) p[k] = ((8 - k) * a0 + (k - 1) * a1) / 7;
    } else {
        for (int k = 2; k < 6; ++k) p[k] = ((6 - k) * a0 + (k - 1) * a1) / 5;
        p[6] = 0;
        p[7] = 255;
    }
    return p;
}

struct AlphaFit {
    int a0 = 0, a1 = 0;
    std::array<std::uint8_t, 16> index{};
    long long error = std::numeric_limits<long long>::max();
};

AlphaFit fit_alpha(const std::array<int, 16>& alpha, int a0, int a1) {
    AlphaFit f{a0, a1, {}, 0};
    const auto p = alpha_palette(a0, a1);
    for (int i = 0; i < 16; ++i) {
        int best = 0, best_error = std::abs(alpha[i] - p[0]);
        for (int k = 1; k < 8; ++k)
            if (const int e = std::abs(alpha[i] - p[k]); e < best_error) {
                best = k;
                best_error = e;
            }
        f.index[i] = static_cast<std::uint8_t>(best);
        f.error += static_cast<long long>(best_error) * best_error;
    }
    return f;
}

void encode_alpha(std::uint8_t* out, const std::array<int, 16>& alpha) {
    const auto [low_it, high_it] = std::minmax_element(alpha.begin(), alpha.end());
    const int low = *low_it, high = *high_it;
    AlphaFit best = low == high ? fit_alpha(alpha, low, low) : fit_alpha(alpha, high, low);
    if (low != high && (low == 0 || high == 255)) {
        // The 6-value mode keeps exact 0 and 255 and spends its ramp on the texels between them.
        int inner_low = 255, inner_high = 0;
        for (const int a : alpha)
            if (a != 0 && a != 255) {
                inner_low = std::min(inner_low, a);
                inner_high = std::max(inner_high, a);
            }
        if (inner_low > inner_high) inner_low = inner_high = 0;
        if (const auto six = fit_alpha(alpha, inner_low, inner_high); six.error < best.error) best = six;
    }
    out[0] = static_cast<std::uint8_t>(best.a0);
    out[1] = static_cast<std::uint8_t>(best.a1);
    std::uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= std::uint64_t(best.index[i]) << (3 * i);
    for (int k = 0; k < 6; ++k) out[2 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
}

void check_block_bytes(unsigned block_bytes) {
    if (block_bytes != bc1_block_bytes && block_bytes != bc3_block_bytes)
        throw std::runtime_error("Block-compressed texture: " + std::to_string(block_bytes) + " bytes a block (BC1 8, BC3 16)");
}

}  // namespace

std::size_t block_level_bytes(unsigned width, unsigned height, unsigned block_bytes) {
    return std::size_t((width + 3) / 4) * ((height + 3) / 4) * block_bytes;
}

unsigned block_row_pitch(unsigned width, unsigned block_bytes) { return (width + 3) / 4 * block_bytes; }

std::vector<std::uint8_t> encode_blocks(std::span<const std::uint8_t> rgba, unsigned width, unsigned height, unsigned block_bytes) {
    check_block_bytes(block_bytes);
    if (width == 0 || height == 0 || rgba.size() != std::size_t(width) * height * 4)
        throw std::runtime_error("Block compression: the image is not width * height RGBA8 texels");
    std::vector<std::uint8_t> out(block_level_bytes(width, height, block_bytes));
    std::uint8_t* block = out.data();
    for (unsigned by = 0; by < height; by += 4)
        for (unsigned bx = 0; bx < width; bx += 4, block += block_bytes) {
            std::array<Rgb, 16> texels{};
            std::array<int, 16> alpha{};
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x) {
                    const auto* p = rgba.data() + (std::size_t(std::min(by + y, height - 1)) * width + std::min(bx + x, width - 1)) * 4;
                    texels[y * 4 + x] = {p[0], p[1], p[2]};
                    alpha[y * 4 + x] = p[3];
                }
            if (block_bytes == bc3_block_bytes) encode_alpha(block, alpha);
            write_colour(block + block_bytes - bc1_block_bytes, encode_colour(texels));
        }
    return out;
}

std::vector<std::uint8_t> decode_blocks(std::span<const std::uint8_t> blocks, unsigned width, unsigned height, unsigned block_bytes) {
    check_block_bytes(block_bytes);
    if (width == 0 || height == 0) throw std::runtime_error("Block-compressed texture of no size");
    if (blocks.size() < block_level_bytes(width, height, block_bytes)) throw std::runtime_error("Block-compressed level truncated");
    std::vector<std::uint8_t> rgba(std::size_t(width) * height * 4);
    const std::uint8_t* block = blocks.data();
    for (unsigned by = 0; by < height; by += 4)
        for (unsigned bx = 0; bx < width; bx += 4, block += block_bytes) {
            const std::uint8_t* colour = block + block_bytes - bc1_block_bytes;
            const auto c0 = static_cast<std::uint16_t>(colour[0] | colour[1] << 8), c1 = static_cast<std::uint16_t>(colour[2] | colour[3] << 8);
            const auto p = palette(c0, c1, block_bytes == bc3_block_bytes);
            const std::uint32_t bits = colour[4] | colour[5] << 8 | colour[6] << 16 | std::uint32_t(colour[7]) << 24;
            std::array<int, 8> a{};
            std::uint64_t alpha_bits = 0;
            if (block_bytes == bc3_block_bytes) {
                a = alpha_palette(block[0], block[1]);
                for (int k = 0; k < 6; ++k) alpha_bits |= std::uint64_t(block[2 + k]) << (8 * k);
            }
            for (unsigned y = 0; y < 4 && by + y < height; ++y)
                for (unsigned x = 0; x < 4 && bx + x < width; ++x) {
                    const unsigned i = y * 4 + x, k = (bits >> (2 * i)) & 3;
                    auto* out = rgba.data() + (std::size_t(by + y) * width + bx + x) * 4;
                    for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(p[k][c]);
                    if (block_bytes == bc3_block_bytes) out[3] = static_cast<std::uint8_t>(a[(alpha_bits >> (3 * i)) & 7]);
                    else out[3] = c0 <= c1 && k == 3 ? 0 : 255;
                }
        }
    return rgba;
}

UploadLevels upload_levels(const Device* device, const omega::Texture& texture, bool with_mips) {
    UploadLevels out;
    out.desc.width = texture.width;
    out.desc.height = texture.height;
    out.desc.usage = Usage::immutable;
    out.desc.bind = bind_sampled;
    out.desc.format = Format::rgba8;
    if (texture.width == 0 || texture.height == 0) throw std::runtime_error("Texture upload of no size");
    const auto level_width = [&](std::size_t level) { return std::max(1U, texture.width >> level); };
    const auto level_height = [&](std::size_t level) { return std::max(1U, texture.height >> level); };
    if (texture.block_bytes) {
        check_block_bytes(texture.block_bytes);
        if (texture.blocks.empty()) throw std::runtime_error("Block-compressed texture without levels");
        const std::size_t count = with_mips ? texture.blocks.size() : 1;
        for (std::size_t level = 0; level < count; ++level)
            if (texture.blocks[level].size() != block_level_bytes(level_width(level), level_height(level), texture.block_bytes))
                throw std::runtime_error("Block-compressed level size mismatch");
        out.desc.mips = static_cast<unsigned>(count);
        // Direct3D needs the base level's sides to be multiples of 4 (whole blocks); other images are decoded.
        if (block_textures(device) && texture.width % 4 == 0 && texture.height % 4 == 0) {
            out.desc.format = texture.block_bytes == bc1_block_bytes ? Format::bc1 : Format::bc3;
            for (std::size_t level = 0; level < count; ++level)
                out.levels.push_back({texture.blocks[level].data(), block_row_pitch(level_width(level), texture.block_bytes)});
            return out;
        }
        out.decoded.reserve(count);
        for (std::size_t level = 0; level < count; ++level) {
            out.decoded.push_back(decode_blocks(texture.blocks[level], level_width(level), level_height(level), texture.block_bytes));
            out.levels.push_back({out.decoded.back().data(), level_width(level) * 4});
        }
        return out;
    }
    const std::size_t count = with_mips ? 1 + texture.mips.size() : 1;
    out.desc.mips = static_cast<unsigned>(count);
    out.levels.push_back({texture.rgba.data(), texture.width * 4});
    for (std::size_t level = 1; level < count; ++level) {
        if (texture.mips[level - 1].size() != std::size_t(level_width(level)) * level_height(level) * 4)
            throw std::runtime_error("Decoded mip level size mismatch");
        out.levels.push_back({texture.mips[level - 1].data(), level_width(level) * 4});
    }
    return out;
}

}  // namespace omega::gfx
