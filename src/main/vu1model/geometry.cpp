#include "geometry.hpp"
#include "binary.hpp"
#include "zone_profile.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
using Word = std::uint32_t;

float read_float(std::span<const std::uint8_t> bytes, std::size_t offset) {
    const float value = std::bit_cast<float>(read_u32(bytes, offset));
    if (!std::isfinite(value)) throw std::runtime_error("Nonfinite model value");
    return value;
}

Vec3 read_vec3(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return {read_float(bytes, offset), read_float(bytes, offset + 4), read_float(bytes, offset + 8)};
}

void marker(std::span<const std::uint8_t> bytes, std::size_t offset, Word value) {
    if (read_u32(bytes, offset) != value) throw std::runtime_error("Invalid model chunk marker at " + std::to_string(offset));
}

bool degenerate(const MeshVertex& a, const MeshVertex& b, const MeshVertex& c) {
    const double ux = double(b.position.x) - a.position.x;
    const double uy = double(b.position.y) - a.position.y;
    const double uz = double(b.position.z) - a.position.z;
    const double vx = double(c.position.x) - a.position.x;
    const double vy = double(c.position.y) - a.position.y;
    const double vz = double(c.position.z) - a.position.z;
    return uy * vz - uz * vy == 0 && uz * vx - ux * vz == 0 && ux * vy - uy * vx == 0;
}

class VifBatch {
public:
    VifBatch(std::span<const std::uint8_t> bytes, std::size_t begin, std::size_t end)
        : bytes_(bytes), cursor_(begin), end_(end) {
        if (begin > end) throw std::runtime_error("Invalid VIF range");
        checked_bytes(bytes, begin, end - begin);
    }

    void expect(Word value) {
        if (cursor_ > end_ || end_ - cursor_ < 4 || read_u32(bytes_, cursor_) != value)
            throw std::runtime_error("Unsupported VIF command at byte " + std::to_string(cursor_));
        cursor_ += 4;
    }

    std::span<const std::uint8_t> take(std::size_t size) {
        if (cursor_ > end_ || size > end_ - cursor_) throw std::runtime_error("Truncated VIF data");
        const auto data = bytes_.subspan(cursor_, size);
        cursor_ += size;
        return data;
    }

    std::size_t attribute(Word count, Word number, Word mode, Word mask,
                          const std::array<Word, 4>& row, Word opcode, std::size_t size) {
        expect(0x05000000 | mode);
        expect(0x20000000);
        expect(mask);
        expect(0x30000000);
        for (const auto word : row) expect(word);
        expect((opcode << 24) | ((count & 255) << 16) | 0xc000 | (12 + number * count));
        const auto offset = cursor_;
        take(size);
        const auto padding = (4 - cursor_ % 4) % 4;
        take(padding);
        return offset;
    }

    std::size_t cursor() const { return cursor_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t cursor_;
    std::size_t end_;
};

struct ColorSpan { std::size_t offset; Word count, first_vertex; };

std::size_t append_batch(Mesh& mesh, std::span<const std::uint8_t> bytes, std::size_t begin,
                         std::size_t end, Word count, Word colors, Word call, const Vec3& bias,
                         Word expected_total, std::vector<ColorSpan>& color_spans) {
    if (count < 3 || count > 253 || mesh.vertices.size() > expected_total
        || count > expected_total - mesh.vertices.size())
        throw std::runtime_error("Unsupported VUM vertex batch count");
    VifBatch input(bytes, begin, end);
    input.expect(0x6c068006);
    const auto header = input.take(96);
    if (read_u64(header, 80) != (0x302e400000008000ULL | count)
        || read_u64(header, 88) != 0x700000412ULL)
        throw std::runtime_error("Unsupported GIF primitive/register layout");
    std::vector<bool> skipped(count, false);
    skipped[0] = skipped[1] = true;
    for (std::size_t offset = 32; offset < 80; offset += 4) {
        const auto value = read_u32(header, offset);
        if (value < 2 || (value - 2) % 3) throw std::runtime_error("Invalid XYZ restart marker");
        const auto vertex = (value - 2) / 3;
        if (vertex + 1 >= count) throw std::runtime_error("Strip restart outside vertex batch");
        skipped[vertex] = skipped[vertex + 1] = true;
    }
    const auto positions = input.attribute(count, 0, 1, 0x40404040,
        {0x47000000, 0x47000000, 0x47000000, 0x3f800000}, 0x79, count * 6);
    const auto normals = input.attribute(count, 1, 1, 0x40404040,
        {0x47800000, 0x47800000, 0x47800000, 0}, 0x7a, count * 3);
    const auto color_data = input.attribute(count, 2, 0, 0x40404040,
        {0, 0, 0, 128}, 0x7a, count * 3);
    const auto uvs = input.attribute(count, 3, 1, 0x50505050,
        {0x45000000, 0x45000000, 0x3f800000, 0}, 0x75, count * 4);
    if (color_data != colors) throw std::runtime_error("VIF color pointer differs from strip metadata");
    input.expect(0x05000000);
    if (input.cursor() != call) throw std::runtime_error("VIF call pointer differs from strip metadata");
    input.expect(0x4c43534d);

    const auto base = static_cast<Word>(mesh.vertices.size());
    color_spans.push_back({color_data, count, base});
    MeshBatch batch{base, count, {}, {}};
    for (std::size_t lane = 0; lane < 4; ++lane) {
        batch.header_minimum[lane] = std::bit_cast<float>(read_u32(header, lane * 4));
        batch.header_maximum[lane] = std::bit_cast<float>(read_u32(header, 16 + lane * 4));
    }
    mesh.batch_spans.push_back(batch);
    // 132vl1 (research/VITA.md "Load time (132vl1)"): the attribute ranges were bounds-checked by input.attribute
    // (take), so the vertices read them unchecked; the arithmetic is unchanged.
    const std::uint8_t* const b = bytes.data();
    const auto u16 = [b](std::size_t at) { return Word(b[at]) | (Word(b[at + 1]) << 8); };
    for (Word i = 0; i < count; ++i) {
        const auto p = positions + std::size_t(i) * 6;
        MeshVertex vertex;
        vertex.position = {
            std::bit_cast<float>(0x47000000U + u16(p)) + bias.x,
            std::bit_cast<float>(0x47000000U + u16(p + 2)) + bias.y,
            std::bit_cast<float>(0x47000000U + u16(p + 4)) + bias.z
        };
        const auto n = normals + std::size_t(i) * 3;
        vertex.normal = {(float(b[n]) - 128) / 128, (float(b[n + 1]) - 128) / 128,
                         (float(b[n + 2]) - 128) / 128};
        const auto uv = uvs + std::size_t(i) * 4;
        vertex.uv = {std::bit_cast<float>(0x45000000U + u16(uv)) - 2048,
                     std::bit_cast<float>(0x45000000U + u16(uv + 2)) - 2048};
        const auto c = colors + std::size_t(i) * 3;
        vertex.color = {float(b[c]) / 128, float(b[c + 1]) / 128, float(b[c + 2]) / 128, 1};
        mesh.vertices.push_back(vertex);
    }
    for (Word i = 2; i < count; ++i) {
        if (skipped[i]) continue;
        // ADC suppresses kicks, not GS strip parity; an odd restart must not reset winding.
        const std::array<Word, 3> triangle = i % 2 == 0
            ? std::array<Word, 3>{base + i - 2, base + i - 1, base + i}
            : std::array<Word, 3>{base + i - 1, base + i - 2, base + i};
        if (!degenerate(mesh.vertices[triangle[0]], mesh.vertices[triangle[1]], mesh.vertices[triangle[2]]))
            mesh.indices.insert(mesh.indices.end(), triangle.begin(), triangle.end());
    }
    ++mesh.batches;
    return input.cursor();
}

struct Reference { std::size_t begin{}, end{}, next{}; std::optional<std::uint64_t> alpha; };

Reference dma_reference(std::span<const std::uint8_t> bytes, std::size_t offset,
                         std::size_t chain_begin, std::size_t chain_end) {
    if (offset < chain_begin || offset > chain_end || chain_end - offset < 16)
        throw std::runtime_error("DMA reference outside model command chain");
    auto tag = read_u32(bytes, offset);
    std::optional<std::uint64_t> alpha;
    if (tag >> 28 == 1) {
        // CNT of 6 qwords unpacked to VU1 address 0: two data qwords, then
        // GIF tag NLOOP 3 EOP A+D with TEX1_1 (0x14), TEX0_1 (0x06, patched
        // at load from texture +0x50) and ALPHA_1 (0x42).
        if (tag != 0x10000006 || read_u32(bytes, offset + 12) != 0x6c068000
            || chain_end - offset < 128 || read_u64(bytes, offset + 48) != 0x1000000000000003ULL
            || read_u64(bytes, offset + 56) != 0xe || read_u64(bytes, offset + 72) != 0x14
            || read_u64(bytes, offset + 104) != 0x42)
            throw std::runtime_error("Unsupported model render-state DMA packet");
        alpha = read_u64(bytes, offset + 96);
        offset += 112;
        tag = read_u32(bytes, offset);
    }
    if (chain_end - offset < 16 || tag >> 16 != 0x3000)
        throw std::runtime_error("Expected model DMA REF");
    const auto begin = read_u32(bytes, offset + 4);
    const std::size_t size = (tag & 65535) * 16;
    if (begin < chain_end + 16 || size == 0) throw std::runtime_error("DMA payload overlaps model metadata");
    checked_bytes(bytes, begin, size);
    return {begin, begin + size, offset + 16, alpha};
}

// The strip's later passes (MHSM meshes): one CNT after its two batch packets
// whose VIF stream holds a 0xd8-byte section per pass k >= 1 at +8 + (k-1)*0xd8,
// the stride of 00377270's pass loop (00377430). Each section repeats, for the
// two batch halves (+0 / +0x68), a GIF tag NLOOP 3 EOP A+D with TEX1_1, TEX0_1
// (patched at load from the pass texture +0x50) and ALPHA_1, then the MSCAL that
// 00377270 patches from the program table at 00538080 for the pass shader.
std::vector<std::uint64_t> later_pass_alphas(std::span<const std::uint8_t> bytes, std::size_t offset,
                                             std::size_t chain_end, std::size_t passes) {
    const std::size_t qwords = (8 + 0xd8 * (passes - 1) + 15) / 16 - 1;
    if (offset > chain_end || chain_end - offset < 16 * (qwords + 1) || read_u32(bytes, offset) != (0x10000000 | qwords))
        throw std::runtime_error("Expected the later-pass render-state DMA packet");
    std::vector<std::uint64_t> alphas;
    for (std::size_t k = 1; k < passes; ++k) {
        const auto at = offset + 8 + (k - 1) * 0xd8;
        std::optional<std::uint64_t> alpha;
        for (const std::size_t half : {std::size_t{0}, std::size_t{0x68}}) {
            const auto s = at + half;
            if (read_u64(bytes, s + 0x28) != 0x1000000000000003ULL || read_u64(bytes, s + 0x30) != 0xe
                || read_u64(bytes, s + 0x40) != 0x14 || read_u64(bytes, s + 0x60) != 0x42)
                throw std::runtime_error("Unsupported later-pass render-state section");
            const auto value = read_u64(bytes, s + 0x58);
            if (alpha && *alpha != value) throw std::runtime_error("Later-pass batch halves carry different GS ALPHA");
            alpha = value;
        }
        alphas.push_back(*alpha);
    }
    return alphas;
}

void zeros(std::span<const std::uint8_t> bytes, std::size_t begin, std::size_t end) {
    if (begin > end) throw std::runtime_error("VIF batch exceeds DMA payload");
    const auto padding = checked_bytes(bytes, begin, end - begin);
    if (std::any_of(padding.begin(), padding.end(), [](auto value) { return value != 0; }))
        throw std::runtime_error("Unexpected data after VIF batch");
}

void write_vec(std::ostream& output, const Vec3& value) {
    output << '[' << value.x << ',' << value.y << ',' << value.z << ']';
}

}

Mesh decode_vum(std::span<const std::uint8_t> bytes) {
    OMEGA_ZONE("load.decode_vum");  // port-only (132vt2e): the PS Vita's load-time zones
    checked_bytes(bytes, 0, 112);
    marker(bytes, 0, 0x534d5556);
    if (read_u32(bytes, 100) != 0)
        throw std::runtime_error("Relocated VUMS models are unsupported");
    const auto expected_vertices = read_u32(bytes, 8);
    const auto mesh_count = read_u32(bytes, 16);
    const auto texture_count = read_u32(bytes, 20);
    const auto material_count = read_u32(bytes, 24);
    // VUM init 00377920 (and release 00377780) skip a model whose mesh count (+0x10) is zero whatever its
    // +4 flags; TORONTO2 TORONTO_SEWERS_A53.VUM is such a model with flags 0.
    const bool empty = expected_vertices == 0 && mesh_count == 0;
    if ((!empty && (!expected_vertices || !mesh_count)) || expected_vertices > 1000000 || mesh_count > 65536
        || !texture_count || texture_count > 4096 || !material_count || material_count > 4096)
        throw std::runtime_error("VUMS count exceeds supported limits (vertices " + std::to_string(expected_vertices) + ", meshes " +
                                 std::to_string(mesh_count) + ", textures " + std::to_string(texture_count) + ", materials " +
                                 std::to_string(material_count) + ")");
    const auto materials_at = read_u32(bytes, 80);
    const auto meshes_at = read_u32(bytes, 84);
    const auto chain_begin = read_u32(bytes, 92);
    const auto chain_end = read_u32(bytes, 96);
    checked_bytes(bytes, materials_at, std::size_t(material_count) * 92);
    checked_bytes(bytes, meshes_at, std::size_t(mesh_count) * 16);
    if (chain_begin > chain_end || (chain_begin == chain_end && !empty) || read_u32(bytes, chain_end) != 0x70000000)
        throw std::runtime_error("Unsupported VUMS DMA boundary");
    const auto bias = read_vec3(bytes, 32);
    Mesh mesh;
    // 132vl1: room for the declared vertices up front (each takes at least 16 input bytes), not regrown per batch.
    mesh.vertices.reserve(std::min<std::size_t>(expected_vertices, bytes.size() / 16));
    std::vector<ColorSpan> color_spans;
    mesh.declared_bounds = {read_vec3(bytes, 48), read_vec3(bytes, 64)};
    mesh.header_flags = read_u32(bytes, 4);
    mesh.declared_bounds_w = {std::bit_cast<float>(read_u32(bytes, 60)), std::bit_cast<float>(read_u32(bytes, 76))};
    std::size_t text = 112;
    for (Word i = 0; i < texture_count; ++i) {
        std::string name;
        while (text < materials_at && bytes[text]) {
            const auto value = bytes[text++];
            if (value < 32 || value >= 127 || value == '/' || value == '\\' || value == ':')
                throw std::runtime_error("Invalid model texture basename");
            name.push_back(static_cast<char>(value));
        }
        if (name.empty() || text >= materials_at) throw std::runtime_error("Unterminated model texture name");
        ++text;
        mesh.textures.push_back(std::move(name));
    }
    for (Word i = 0; i < material_count; ++i) {
        const auto offset = materials_at + std::size_t(i) * 92;
        marker(bytes, offset, 0x4c52544d);
        MeshMaterial material{read_u32(bytes, offset + 56), read_u32(bytes, offset + 4),
                              read_u32(bytes, offset + 72), {}};
        const auto passes = read_u32(bytes, offset + 88);
        // Flags are retained as metadata; accepting geometry does not implement its material.
        if ((material.flags & ~54U) || (material.shader != 2 && material.shader != 13)
            || passes == 0 || passes > 4 || material.texture >= texture_count)
            throw std::runtime_error("Unsupported material flags, shader, pass count or texture index");
        for (Word layer = 0; layer < passes; ++layer) {
            MaterialPass pass{read_u32(bytes, offset + 56 + layer * 4), read_u32(bytes, offset + 72 + layer * 4)};
            if (pass.texture >= texture_count || pass.shader >= 30)
                throw std::runtime_error("Invalid active material pass");
            material.passes.push_back(pass);
        }
        material.model_flags = mesh.header_flags;
        material.animation = {read_u32(bytes, offset + 8), read_u32(bytes, offset + 12)};
        mesh.materials.push_back(material);
    }
    for (Word i = 0; i < mesh_count; ++i) {
        const auto offset = meshes_at + std::size_t(i) * 16;
        const auto mesh_tag = read_u32(bytes, offset);
        if (mesh_tag != 0x5348534d && mesh_tag != 0x4d48534d)
            throw std::runtime_error("Invalid mesh chunk marker");
        const auto count = read_u32(bytes, offset + 4);
        const auto strips = read_u32(bytes, offset + 8);
        const auto material = read_u32(bytes, offset + 12);
        if (!count || count > 65536 || material >= material_count)
            throw std::runtime_error("Invalid strip/material count");
        if ((mesh_tag == 0x4d48534d) != (mesh.materials[material].passes.size() > 1))
            throw std::runtime_error("Mesh tag does not match material pass count");
        checked_bytes(bytes, strips, std::size_t(count) * 32);
        const auto first = static_cast<Word>(mesh.indices.size());
        for (Word strip = 0; strip < count; ++strip) {
            const auto at = strips + std::size_t(strip) * 32;
            marker(bytes, at, 0x52505453);
            auto reference = dma_reference(bytes, read_u32(bytes, at + 4), chain_begin, chain_end);
            if (reference.alpha) {
                auto& base = mesh.materials[material].gs_alpha;
                if (base && *base != *reference.alpha)
                    throw std::runtime_error("Material strips carry different GS ALPHA render states");
                base = reference.alpha;
            }
            for (Word part = 0; part < 2; ++part) {
                if (part && (strip == 0 || mesh.materials[material].passes.size() > 1))
                    reference = dma_reference(bytes, reference.next, chain_begin, chain_end);
                const auto fields = at + (part ? 20 : 8);
                const auto after = append_batch(mesh, bytes, reference.begin, reference.end,
                    read_u32(bytes, fields), read_u32(bytes, fields + 4), read_u32(bytes, fields + 8),
                    bias, expected_vertices, color_spans);
                if (part == 0 && strip > 0 && mesh.materials[material].passes.size() == 1) reference.begin = after;
                else zeros(bytes, after, reference.end);
            }
            auto& passes = mesh.materials[material].passes;
            if (passes.size() > 1) {
                const auto alphas = later_pass_alphas(bytes, reference.next, chain_end, passes.size());
                for (std::size_t k = 1; k < passes.size(); ++k) {
                    if (passes[k].gs_alpha && *passes[k].gs_alpha != alphas[k - 1])
                        throw std::runtime_error("Material strips carry different later-pass GS ALPHA");
                    passes[k].gs_alpha = alphas[k - 1];
                }
            }
        }
        mesh.surfaces.push_back({material, first, static_cast<Word>(mesh.indices.size()) - first});
    }
    const auto patch_count = read_u32(bytes, 28);
    const auto patches_at = read_u32(bytes, 88);
    if (patch_count > 65536 || (patch_count && patches_at % 4))
        throw std::runtime_error("Invalid VUMS color patch table");
    if (patch_count) checked_bytes(bytes, patches_at, std::size_t(patch_count) * 8);
    std::size_t payload_words = 0;
    for (Word i = 0; i < patch_count; ++i) {
        const auto at = patches_at + std::size_t(i) * 8;
        ColorPatchSet set{read_u32(bytes, at), {}};
        std::size_t cursor = read_u32(bytes, at + 4);
        if (cursor % 4) throw std::runtime_error("Unaligned VUMS color patch list");
        for (;;) {
            const auto target = read_u32(bytes, cursor);
            if (!target) break;
            const auto count = read_u32(bytes, cursor + 4);
            if (!count || count > 1000000 - payload_words)
                throw std::runtime_error("VUMS color patch payload exceeds supported limits");
            payload_words += count;
            checked_bytes(bytes, cursor + 8, std::size_t(count) * 4);
            std::vector<Word> colors;
            for (Word j = 0; j < count; ++j) {
                const auto color = read_u32(bytes, cursor + 8 + std::size_t(j) * 4);
                if (color >> 24) throw std::runtime_error("Unsupported VUMS patch color format");
                colors.push_back(color);
            }
            bool matched = false;
            for (const auto& span : color_spans) {
                if (target < span.offset || (target - span.offset) % 3) continue;
                const auto first = (target - span.offset) / 3;
                if (first >= span.count || count > span.count - first) continue;
                set.spans.push_back({target, span.first_vertex + static_cast<Word>(first), colors});
                matched = true;
            }
            if (!matched) throw std::runtime_error("VUMS color patch target is outside decoded RGB attributes");
            cursor += (std::size_t(count) + 2) * 4;
        }
        mesh.color_patches.push_back(std::move(set));
    }
    if (empty) return mesh;
    if (mesh.vertices.size() != expected_vertices || mesh.indices.empty())
        throw std::runtime_error("Vertex total differs from header or mesh has no triangles");
    const float infinity = std::numeric_limits<float>::infinity();
    mesh.bounds = {{infinity, infinity, infinity}, {-infinity, -infinity, -infinity}};
    for (const auto index : mesh.indices) {
        const auto& point = mesh.vertices[index].position;
        mesh.bounds.minimum.x = std::min(mesh.bounds.minimum.x, point.x);
        mesh.bounds.minimum.y = std::min(mesh.bounds.minimum.y, point.y);
        mesh.bounds.minimum.z = std::min(mesh.bounds.minimum.z, point.z);
        mesh.bounds.maximum.x = std::max(mesh.bounds.maximum.x, point.x);
        mesh.bounds.maximum.y = std::max(mesh.bounds.maximum.y, point.y);
        mesh.bounds.maximum.z = std::max(mesh.bounds.maximum.z, point.z);
    }
    constexpr float tolerance = 1.0F / 128;
    const auto& low = mesh.bounds.minimum;
    const auto& high = mesh.bounds.maximum;
    const auto& declared_low = mesh.declared_bounds.minimum;
    const auto& declared_high = mesh.declared_bounds.maximum;
    if (!std::isfinite(low.x) || !std::isfinite(low.y) || !std::isfinite(low.z)
        || !std::isfinite(high.x) || !std::isfinite(high.y) || !std::isfinite(high.z)
        || low.x < declared_low.x - tolerance || low.y < declared_low.y - tolerance || low.z < declared_low.z - tolerance
        || high.x > declared_high.x + tolerance || high.y > declared_high.y + tolerance || high.z > declared_high.z + tolerance)
        throw std::runtime_error("Decoded mesh exceeds declared bounds");
    return mesh;
}

CollisionMesh decode_col(std::span<const std::uint8_t> bytes) {
    OMEGA_ZONE("load.decode_col");  // port-only (132vt2e): the PS Vita's load-time zones
    checked_bytes(bytes, 0, 48);
    marker(bytes, 0, 0x054c4f43);
    const auto triangle_count = read_u32(bytes, 20);
    const auto triangles_at = read_u32(bytes, 24);
    const auto vertex_count = read_u32(bytes, 28);
    const auto vertices_at = read_u32(bytes, 32);
    if (triangle_count > 1000000 || vertex_count > 65536)
        throw std::runtime_error("Collision count exceeds supported limits");
    checked_bytes(bytes, triangles_at, std::size_t(triangle_count) * 16);
    checked_bytes(bytes, vertices_at, std::size_t(vertex_count) * 16);
    CollisionMesh collision;
    collision.vertices.reserve(vertex_count);      // 132vl1: both counts' bytes were checked above
    collision.triangles.reserve(triangle_count);
    for (Word i = 0; i < vertex_count; ++i)
        collision.vertices.push_back(read_vec3(bytes, vertices_at + std::size_t(i) * 16));
    for (Word i = 0; i < triangle_count; ++i) {
        const auto at = triangles_at + std::size_t(i) * 16;
        const auto coefficient = std::bit_cast<float>(read_u32(bytes, at));
        if (std::isnan(coefficient) || coefficient == -std::numeric_limits<float>::infinity())
            throw std::runtime_error("Unsupported collision coefficient");
        CollisionTriangle triangle{{read_u16(bytes, at + 4), read_u16(bytes, at + 6), read_u16(bytes, at + 8)},
                                    read_u16(bytes, at + 10), coefficient, read_u32(bytes, at + 12)};
        for (const auto index : triangle.indices) {
            if (index >= vertex_count) throw std::runtime_error("Collision index outside vertex array");
        }
        collision.triangles.push_back(triangle);
    }
    return collision;
}

bool toggle_color_patch(std::uint32_t& bits_68, const ColorPatchSet& set, std::uint32_t index, bool on,
                        std::vector<std::array<std::uint8_t, 3>>& colours, std::vector<std::uint32_t>* changed) {
    const auto bit = 1U << (index & 31U);   // srlv / sllv use the low five bits
    if (((bits_68 & bit) != 0) == on) return false;
    bits_68 ^= bit;
    for (const auto& span : set.spans) {
        if (span.first_vertex > colours.size() || span.colors.size() > colours.size() - span.first_vertex)
            throw std::runtime_error("003767e0: colour patch span outside the vertex colours");
        for (std::size_t j = 0; j < span.colors.size(); ++j) {
            auto& rgb = colours[span.first_vertex + j];
            for (std::size_t c = 0; c < 3; ++c) {
                const auto delta = static_cast<std::uint8_t>(span.colors[j] >> (8 * c));
                rgb[c] = static_cast<std::uint8_t>(on ? rgb[c] + delta : rgb[c] - delta);
            }
            if (changed) changed->push_back(static_cast<std::uint32_t>(span.first_vertex + j));
        }
    }
    return true;
}

void write_model(std::ostream& out, const std::string& name, const Mesh& mesh, const CollisionMesh& collision) {
    out << std::setprecision(std::numeric_limits<float>::max_digits10) << std::showpoint
        << "{\"model\":" << std::quoted(name) << ",\"mesh\":{\"textures\":[";
    for (std::size_t i = 0; i < mesh.textures.size(); ++i) {
        if (i) out << ',';
        out << std::quoted(mesh.textures[i]);
    }
    out << "],\"materials\":[";
    for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
        if (i) out << ',';
        const auto& material = mesh.materials[i];
        out << "{\"texture\":" << material.texture << ",\"flags\":" << material.flags << ",\"shader\":" << material.shader
            << ",\"passes\":[";
        for (std::size_t pass = 0; pass < material.passes.size(); ++pass) {
            if (pass) out << ',';
            out << "{\"texture\":" << material.passes[pass].texture << ",\"shader\":" << material.passes[pass].shader << '}';
        }
        out << "]}";
    }
    out << "],\"color_patches\":[";
    for (std::size_t i = 0; i < mesh.color_patches.size(); ++i) {
        if (i) out << ',';
        const auto& set = mesh.color_patches[i];
        out << "{\"key\":" << set.key << ",\"spans\":[";
        for (std::size_t j = 0; j < set.spans.size(); ++j) {
            if (j) out << ',';
            const auto& span = set.spans[j];
            out << "{\"source_offset\":" << span.source_offset << ",\"first_vertex\":" << span.first_vertex << ",\"colors\":[";
            for (std::size_t k = 0; k < span.colors.size(); ++k) {
                if (k) out << ',';
                out << span.colors[k];
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "],\"vertices\":[";
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (i) out << ',';
        const auto& vertex = mesh.vertices[i];
        out << '[' << vertex.position.x << ',' << vertex.position.y << ',' << vertex.position.z
            << ',' << vertex.normal.x << ',' << vertex.normal.y << ',' << vertex.normal.z
            << ',' << vertex.uv[0] << ',' << vertex.uv[1];
        for (const auto channel : vertex.color) out << ',' << channel;
        out << ']';
    }
    out << "],\"indices\":[";
    for (std::size_t i = 0; i < mesh.indices.size(); ++i) {
        if (i) out << ',';
        out << mesh.indices[i];
    }
    out << "],\"surfaces\":[";
    for (std::size_t i = 0; i < mesh.surfaces.size(); ++i) {
        if (i) out << ',';
        const auto& surface = mesh.surfaces[i];
        out << "{\"material\":" << surface.material << ",\"first_index\":" << surface.first_index
            << ",\"index_count\":" << surface.index_count << '}';
    }
    out << "],\"bounds\":[";
    write_vec(out, mesh.bounds.minimum);
    out << ',';
    write_vec(out, mesh.bounds.maximum);
    out << "],\"declared_bounds\":[";
    write_vec(out, mesh.declared_bounds.minimum);
    out << ',';
    write_vec(out, mesh.declared_bounds.maximum);
    out << "],\"batches\":" << mesh.batches << "},\"collision\":{\"vertices\":[";
    for (std::size_t i = 0; i < collision.vertices.size(); ++i) {
        if (i) out << ',';
        write_vec(out, collision.vertices[i]);
    }
    out << "],\"triangles\":[";
    for (std::size_t i = 0; i < collision.triangles.size(); ++i) {
        if (i) out << ',';
        const auto& triangle = collision.triangles[i];
        out << "{\"indices\":[" << triangle.indices[0] << ',' << triangle.indices[1] << ',' << triangle.indices[2]
            << "],\"flags\":" << triangle.flags << ",\"coefficient\":";
        if (std::isfinite(triangle.coefficient)) out << triangle.coefficient;
        else out << "null,\"coefficient_bits\":" << std::bit_cast<Word>(triangle.coefficient);
        out << ",\"reserved\":" << triangle.reserved << '}';
    }
    out << "]},\"limitation\":\"Decoded static mesh and collision triangles; not original lighting, scene placement or gameplay physics.\"}\n";
    if (!out) throw std::runtime_error("Failed to write model report");
}

}
